#pragma once

#include <array>
#include <functional>
#include <memory>
#include <vector>

#include "../analysis/BeatGrid.h"
#include "../data/PerformanceStore.h"
#include "../engine/DeckEngine.h"
#include "../model/DeckStats.h"
#include "../rt/CommandQueue.h"
#include "BeatJumpEngine.h"
#include "CueEngine.h"
#include "PerformanceTypes.h"
#include "QuantizeEngine.h"
#include "SmartLoopEngine.h"

namespace onda {

/// Non-RT performance layer between UI/controllers and the audio engine.
/// Owns per-deck musical context (beatgrid, cues, quantize, slip, sync
/// config) and translates musical actions into RT commands.
///
/// Threading: ALL methods run on the caller (UI/controller) thread.
/// Commands are pushed to the RT thread through the command queue (or
/// an injected sink in tests). poll() is the non-RT "tick" that frees
/// retired beatgrids and emits beat/bar/loop transition events —
/// call it from the UI timer.
class PerformanceEngine {
public:
    static constexpr int kMaxDecks = 4;

    /// Snapshot of the musical master clock (non-RT view).
    struct ClockSample {
        bool   valid   { false };
        double bpm     { 0.0 };   // effective master bpm
        double beatPos { -1.0 };  // continuous beat position
        int    bar     { 0 };
        double phase   { 0.0 };   // position inside current beat 0..1
    };

    PerformanceEngine() = default;

    // ---- wiring (call once) ----
    /// Provide a sink for commands. Default: none (tests inject their
    /// own; the app wires the AudioEngine's queue).
    void setCommandSink(std::function<void(const rt::Command&)> sink);
    /// Grid handoff needs the RT deck's atomic slot — supplied by the app.
    void setGridSlot(int deck, std::atomic<const BeatGrid*>* pending,
                     std::atomic<const BeatGrid*>* retired,
                     const DeckStats* stats);
    void setStore(PerformanceStore* store) noexcept { store_ = store; }

    using Listener = std::function<void(const PerfEvent&)>;
    void addListener(Listener l) { listeners_.push_back(std::move(l)); }

    // ---- deck context ----
    /// Attach a beatgrid to a deck (handed off to the RT thread).
    /// `trackId` links cues/loops persistence; -1 = session only.
    void attachGrid(int deck, std::shared_ptr<const BeatGrid> grid,
                    int64_t trackId = -1);
    void detachDeck(int deck);
    const BeatGrid* grid(int deck) const;
    bool hasGrid(int deck) const { return grid(deck) != nullptr; }

    // ---- sync ----
    void setMasterDeck(int deck);          // -1 releases the role
    int  masterDeck() const { return master_; }
    void setSyncMode(int deck, SyncMode mode);
    void enableSync(int deck, SyncMode mode = SyncMode::Beat);
    void disableSync(int deck);
    void syncDeck(int deck, SyncMode mode = SyncMode::Beat); // toggle
    SyncMode   syncMode(int deck) const;
    SyncStatus syncStatus(int deck) const;
    ClockSample masterClock() const;

    // ---- quantize ----
    void setQuantize(int deck, bool on);
    void setQuantizeResolution(int deck, Quantize q);
    bool   quantizeOn(int deck) const;
    double quantizeBeatsFor(int deck) const;

    // ---- cues ----
    const std::vector<Cue>& cues(int deck) const;
    /// frame < 0 uses the deck's current position.
    Cue setCue(int deck, int slot, CueType type, int64_t frame = -1,
               int64_t frame2 = -1, uint32_t argb = 0xFFFF4444,
               std::string label = {});
    bool deleteCue(int deck, int slot);
    bool updateCue(int deck, int slot, uint32_t argb,
                   const std::string& label);
    void clearAllCues(int deck);
    /// Hot/memory cue trigger. Hot → seek + play. Memory → seek only.
    /// Returns false when the slot is empty or the deck has no track.
    bool triggerCue(int deck, int slot);

    // ---- beat jump ----
    void beatJump(int deck, double beats);

    // ---- loops ----
    void loopIn(int deck);                     // mark pending in-point
    void loopOut(int deck);                    // complete the loop
    void loopBeats(int deck, double beats);    // auto beat loop
    void loopToggle(int deck);
    void reloop(int deck);
    void loopHalve(int deck);
    void loopDouble(int deck);
    void loopMove(int deck, double beats);
    void exitLoop(int deck);
    /// Loop roll: temporary loop while the shadow position advances.
    void loopRoll(int deck, double beats);
    void loopRollEnd(int deck);
    // ---- saved loops ----
    void saveLoop(int deck, int slot);
    void triggerSavedLoop(int deck, int slot);
    void deleteSavedLoop(int deck, int slot);
    std::vector<SavedLoop> savedLoops(int deck) const;

    // ---- slip / reverse ----
    void setSlip(int deck, bool on);
    bool slipOn(int deck) const;
    void setReverse(int deck, bool on); // honours slip shadow

    // ---- non-RT tick ----
    /// Free retired grids + emit beat/bar/loop/slip transitions.
    /// Call from the UI timer (~30 Hz). Not needed by tests that drive
    /// decks directly.
    void poll();

private:
    struct DeckCtx {
        const DeckStats* stats { nullptr };
        std::atomic<const BeatGrid*>* pending { nullptr };
        std::atomic<const BeatGrid*>* retired { nullptr };
        std::shared_ptr<const BeatGrid> grid;
        std::vector<std::shared_ptr<const BeatGrid>> retiring;
        int64_t trackId { -1 };
        CueEngine cues;
        std::vector<SavedLoop> loops;
        bool   quantizeOn { false };
        Quantize quantum  { Quantize::Beat1 };
        bool   slipOn     { false };
        SyncMode mode     { SyncMode::Off };
        /// Last rejected sync request — surfaced by syncStatus so a
        /// missing/unreliable grid is never silently ignored.
        SyncStatus lastError { SyncStatus::Off };
        int64_t pendingLoopIn { -1 };
        int64_t lastLoopIn { -1 }, lastLoopOut { -1 };
        bool   rolling { false };
        // poll() edge detection
        int    lastBeatInt { -1 };
        int    lastBarInt  { -1 };
        bool   lastLoopOn  { false };
        bool   lastSlip    { false };
    };

    void   send(const rt::Command& c);
    void   emit(PerfEventType t, int deck, int slot = -1, double v = 0.0);
    int64_t curFrame(int deck) const;
    DeckCtx& ctx(int deck) { return decks_[(size_t)deck]; }
    const DeckCtx& ctx(int deck) const { return decks_[(size_t)deck]; }
    void   persistCue(const DeckCtx& d, const Cue& c);
    void   persistLoopDelete(const DeckCtx& d, int slot);
    void   storeLoopRange(DeckCtx& d, int64_t in, int64_t out);

    std::function<void(const rt::Command&)> sink_;
    PerformanceStore* store_ { nullptr };
    std::array<DeckCtx, kMaxDecks> decks_;
    std::vector<Listener> listeners_;
    int master_ { -1 };
};

} // namespace onda
