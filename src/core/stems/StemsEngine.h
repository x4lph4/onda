#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../engine/DeckEngine.h"
#include "../model/DeckStats.h"
#include "../rt/CommandQueue.h"
#include "StemCache.h"
#include "StemDiagnostics.h"
#include "StemMixer.h"
#include "StemModelProvider.h"
#include "StemPresetManager.h"
#include "StemRouter.h"
#include "StemScheduler.h"
#include "StemStore.h"

namespace onda::stems {

/// ------------------------------------------------------------------
/// §1 — stems coordinator. NON-RT only.
///
///   StemsEngine
///   ├── StemManager        per-deck stem state (§3)
///   ├── StemScheduler      background job queue (§9)
///   ├── StemSeparationModel  via StemModelProvider (§5-7)
///   ├── StemCache          file cache, keyed/validated (§27-31)
///   ├── StemStore          stem_sets metadata rows (§29/§48)
///   ├── StemMixer          gain/mute/solo/pan model (§12-16)
///   ├── StemRouter/+FXRouter  routing plans + FX intent (§17-19)
///   ├── StemPresetManager  named stem-state snapshots
///   └── StemDiagnostics    counters/timing (§39/§52)
///
/// Threading contract: every method runs on control/UI threads;
/// resolved parameters reach decks as rt::SetStemParam / SetStemMode
/// commands through the shared CommandQueue. Audio never separates,
/// loads or syncs in the callback — a deck without a stem set just
/// renders its original buffer (§25).
/// ------------------------------------------------------------------
class StemsEngine {
public:
    static constexpr int kDecks = 4;

    /// `decoder(path) -> TrackAudio*` supplies audio to separation
    /// workers (borrowed pointer, owned by the caller's decode cache).
    /// `queue` is the app command queue (rt commands ride it).
    StemsEngine(Database& db, rt::CommandQueue& queue);
    ~StemsEngine();

    void configure(const StemConfig& cfg);
    void setModelProvider(std::unique_ptr<StemModelProvider> p);
    void setDecoder(
        std::function<const TrackAudio*(const std::string&)> d) {
        decoder_ = std::move(d);
    }

    // ---- deck binding -------------------------------------------------
    void bindDeck(int deck, DeckEngine* eng, DeckStats* stats);
    /// Call on the control thread when a deck starts loading a track.
    /// Resolves availability instantly from the store; never blocks
    /// playback — the deck plays the original until a set arrives.
    void onTrackLoading(int deck, int64_t trackId, uint64_t contentHash,
                        const std::string& path, double deviceRate);
    void onTrackUnloaded(int deck);

    // ---- separation control -------------------------------------------
    int64_t requestSeparation(int64_t trackId, uint64_t contentHash,
                              const std::string& path,
                              int quality = -1, int priority = 0);
    bool cancelSeparation(int64_t jobId);
    void cancelForDeck(int deck);
    StemJobPhase jobPhase(int64_t jobId) const;
    float        jobProgress(int64_t jobId) const;

    // ---- deck state (§3/§32) ------------------------------------------
    StemAvailability availability(int deck) const;
    const Stem&      stemState(int deck, int stem) const;
    const StemManager& manager(int deck) const { return mgr_[deck]; }
    const StemMixer&   mixer(int deck)   const { return mix_[deck]; }
    uint32_t           presentMask(int deck) const;
    int                stemMode(int deck) const { return mgr_[deck].mode(); }

    // ---- performance surface (§20/§35) ---------------------------------
    /// All setters update the StemMixer model AND post the resolved
    /// rt::SetStemParam — one call, both worlds consistent.
    void setStemMode(int deck, int mode);       // 0 original, 1 stems
    void setStemGain(int deck, int stem, float g);
    void setStemPan(int deck, int stem, float p);
    void setStemMute(int deck, int stem, bool muted);
    void setStemSolo(int deck, int stem, bool solo);
    void clearStemMutes(int deck);              // mutes + solos off
    /// Momentary kill (pad gate): held=true mutes, held=false restores.
    void stemGate(int deck, int stem, bool held);
    /// Combined action entry used by the command layer (kind =
    /// EngineCommands kinds 0..8 — mute/solo/gain±/pan±/gate/set/all).
    void stemAction(int deck, int stem, int kind, float v);

    // ---- routing / presets --------------------------------------------
    StemFXRouter&      fxRouter(int deck)  { return fxR_[deck]; }
    StemRoutingPlan    routing(int deck) const;
    StemPresetManager& presets()           { return presets_; }
    bool applyPreset(const std::string& name, int deck);

    // ---- library-facing status (§47) -----------------------------------
    /// -1 none · StemStore::Pending/Ready/Failed — best across keys.
    int trackStatus(uint64_t contentHash);

    // ---- cache / storage (§30) ------------------------------------------
    StemCache::StorageStats storageStats() const { return cache_.stats(); }
    /// Remove one set's files + row; originals/metadata untouched.
    bool clearSet(uint64_t contentHash, const std::string& key);
    /// Wipe the whole stems cache (files + stem_sets rows only).
    void clearAll();

    // ---- maintenance ----------------------------------------------------
    /// Control-thread pump: hands finished loads to decks, releases
    /// retired buffers. Call periodically (UI timer ~30 Hz).
    void poll();
    /// CPU gate (§45): true → low-priority jobs yield to playback.
    void setPlaybackActive(bool active);

    StemDiagnostics&       diagnostics() { return diag_; }
    StemScheduler&         scheduler()   { return sched_; }
    void addListener(StemScheduler::Listener l) { sched_.addListener(std::move(l)); }

private:
    struct DeckSlot {
        DeckEngine* eng { nullptr };
        DeckStats*  stats { nullptr };
        int64_t  trackId { 0 };
        uint64_t contentHash { 0 };
        std::string path;
        double   deviceRate { 48000.0 };
        int64_t  jobId { -1 };
    };
    /// Owned backing for a published view — lives until the deck
    /// retires the pointer (same protocol as TrackAudio). The set is
    /// shared read-only between decks playing the same track (§44);
    /// each deck gets its own handle/view so retire tracking stays
    /// per-deck.
    struct StemHandle {
        std::shared_ptr<const StemSet> set;
        DeckStemBuffers view;
    };

    void publishSet(int deck, std::shared_ptr<StemSet> set);
    void releaseRetired(int deck);
    void postParam(int deck, int stem);
    void postMode(int deck);
    void onJobEvent(const StemScheduler::Notification& n);
    StemError runJob(const StemJob& job, StemCancel& cancel,
                     const std::function<void(StemJobPhase, float)>& report);
    StemSetMeta metaFor(const DeckSlot& d, int quality) const;
    bool tryHandCached(int deck, const StemSetMeta& m);
    void autoEnableIfStems(int deck);

    Database&          db_;
    rt::CommandQueue&  queue_;
    StemConfig         cfg_;
    std::unique_ptr<StemModelProvider> provider_;
    std::function<const TrackAudio*(const std::string&)> decoder_;

    StemStore       store_;
    StemCache       cache_ { cfg_.cacheDir };
    StemDiagnostics diag_;
    StemPresetManager presets_;

    DeckSlot      slots_[kDecks];
    StemManager   mgr_[kDecks];
    StemMixer     mix_[kDecks];
    StemFXRouter  fxR_[kDecks];
    StemRouter    router_;

    std::mutex    mu_;
    /// views published to decks → their owning handles
    std::unordered_map<const DeckStemBuffers*,
                       std::shared_ptr<StemHandle>> owned_[kDecks];
    /// produced sets awaiting a deck publish (jobId → set)
    std::unordered_map<int64_t, std::shared_ptr<StemSet>> produced_;
    /// finished separation jobs whose decks should adopt
    std::vector<std::pair<int64_t, uint64_t>> readyForHash_;

    /// DECLARED LAST → destroyed FIRST: the worker threads join while
    /// mu_/produced_/owned_/cache_/store_/diag_/decoder_ are still
    /// alive. A job in flight at shutdown may touch all of them.
    StemScheduler   sched_ { 1 };
};

} // namespace onda::stems
