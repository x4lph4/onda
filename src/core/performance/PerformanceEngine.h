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
#include "../sync/MasterClock.h"
#include "../sync/SyncDiagnostics.h"
#include "../sync/SyncTypes.h"
#include "../sync/TempoMaster.h"
#include "BeatJumpEngine.h"
#include "CueEngine.h"
#include "HotCueManager.h"
#include "LoopSizeController.h"
#include "MemoryCueManager.h"
#include "PerformanceDiagnostics.h"
#include "PerformanceScheduler.h"
#include "PerformanceTypes.h"
#include "QuantizeEngine.h"
#include "ReverseEngine.h"
#include "SlipEngine.h"
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

    /// Musical master-clock snapshot (non-RT view) — Phase 15 type.
    using ClockSample = sync::ClockSample;

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
    /// `confidence` (0..1) feeds syncConfidence — pass the analysis
    /// SyncReference confidence when known.
    void attachGrid(int deck, std::shared_ptr<const BeatGrid> grid,
                    int64_t trackId = -1, float confidence = 0.7f);
    void detachDeck(int deck);
    const BeatGrid* grid(int deck) const;
    bool hasGrid(int deck) const { return grid(deck) != nullptr; }

    // ---- sync ----
    void setMasterDeck(int deck);          // -1 releases the role
    int  masterDeck() const { return master_; }
    bool isMaster(int deck) const { return deck >= 0 && deck == master_; }
    void setSyncMode(int deck, SyncMode mode);
    void enableSync(int deck, SyncMode mode = SyncMode::Beat);
    void disableSync(int deck);
    void syncDeck(int deck, SyncMode mode = SyncMode::Beat); // toggle
    SyncMode   syncMode(int deck) const;
    SyncStatus syncStatus(int deck) const;
    ClockSample masterClock() const;
    /// Clock + provenance + stopped flag (§2-3, §22).
    sync::MasterClockView masterClockView() const;

    // ---- tempo master (§4-5, §22-23) ----
    /// Who feeds the clock: Auto (documented rules), pinned deck,
    /// External (clock via setExternalClock), None (sync refused).
    void setMasterSource(sync::MasterSource s);
    sync::MasterSource masterSource() const { return masterSel_.source; }
    /// What happens when the master deck pauses — ClockHolds default.
    void setMasterPausePolicy(sync::MasterPausePolicy p);
    sync::MasterPausePolicy masterPausePolicy() const { return masterSel_.pause; }
    /// Cycle Auto → A → B → C → D → Auto (hardware-friendly step).
    void cycleMaster();
    /// Future external clock feed (Link-style); only consulted when
    /// masterSource == External.
    void setExternalClock(sync::ClockSource src);
    void clearExternalClock() { externalClock_ = nullptr; }

    // ---- per-deck sync state, confidence, diagnostics (§6,§30,§39) ----
    sync::DeckSyncState   deckSyncState(int deck) const;
    float                 syncConfidence(int deck) const;
    sync::SyncDiagnostics syncDiagnostics(int deck) const;
    std::string           syncDiagText(int deck) const;

    // ---- sync lock (§24) ----
    /// ON (default): sync mode survives a track change — the new grid
    /// re-locks to the session clock. OFF: attaching a different
    /// track's grid drops sync to Off.
    void setSyncLock(int deck, bool on);
    bool syncLockOn(int deck) const;

    // ---- pitch interaction while synced (§26) ----
    void setPitchPolicy(sync::PitchPolicy p) { pitchPolicy_ = p; }
    sync::PitchPolicy pitchPolicy() const { return pitchPolicy_; }
    /// Unified entry for user pitch-fader moves. Routes through the
    /// configured policy — ALWAYS use this instead of sending
    /// rt::SetPitch directly from UI/HID paths.
    void onUserPitch(int deck, float pct);

    // ---- quantize ----
    void setQuantize(int deck, bool on);
    void setQuantizeResolution(int deck, Quantize q);
    void cycleQuantize(int deck);              // step through resolutions
    int  quantizeResolution(int deck) const;
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

    // ---- cue managers + field edits (Phase 20 §2-8, §38) ----
    /// Typed facades — same underlying storage, no duplication.
    HotCueManager    hot(int deck);
    MemoryCueManager memory(int deck);
    std::vector<const Cue*> hotCues(int deck) const;
    std::vector<const Cue*> memoryCues(int deck) const;
    bool moveCue(int deck, int slot, int64_t frame);
    bool renameCue(int deck, int slot, const std::string& label);
    bool recolorCue(int deck, int slot, uint32_t argb);
    bool setCueEnabled(int deck, int slot, bool on);
    bool setCueKind(int deck, int slot, HotKind k);
    bool setCueQuantized(int deck, int slot, bool on);
    bool setCueComment(int deck, int slot, std::string comment);
    /// §5 — what a hot-cue trigger does while stopped (JumpPlay/JumpCue).
    void setHotCueMode(int deck, HotCueMode m);
    HotCueMode hotCueMode(int deck) const;

    // ---- cue point + cue button (§24-26) ----
    /// Temporary navigation point — NOT a hot cue (session-only, the
    /// classic "memoryless" CUE). setCuePoint(-1) = current position.
    void setCuePoint(int deck, int64_t frame = -1);
    int64_t cuePoint(int deck) const;
    /// Device-agnostic cue button (§25): press while playing returns
    /// to the cue point and pauses; press while paused sets the point
    /// and previews while held; release returns to it.
    void cueButton(int deck, CuePhase ph);
    /// §26 — restart playback from the cue point. Never touches the
    /// sync/session clock state.
    void instantRestart(int deck);

    // ---- beat jump ----
    void beatJump(int deck, double beats);
    /// §19 — sized jump through the LoopSizeController table; sign
    /// comes from `dir` (-1 back, +1 fwd) so callers stay index-based.
    void beatJumpIdx(int deck, int idx, int dir = 1);
    void setJumpSizeIndex(int deck, int idx);
    int  jumpSizeIndex(int deck) const;
    double jumpSizeBeats(int deck) const;

    // ---- loops ----
    void loopIn(int deck);                     // mark pending in-point
    void loopOut(int deck);                    // complete the loop
    void loopBeats(int deck, double beats);    // auto beat loop
    /// §11 — auto loop through the size table (1/32 … 32 beats).
    void loopBeatsIdx(int deck, int idx);
    void setLoopSizeIndex(int deck, int idx);
    int  loopSizeIndex(int deck) const;
    double loopSizeBeats(int deck) const;
    void loopToggle(int deck);
    void reloop(int deck);
    void loopHalve(int deck);
    void loopDouble(int deck);
    void loopMove(int deck, double beats);
    void exitLoop(int deck);
    /// Loop roll: temporary loop while the shadow position advances.
    void loopRoll(int deck, double beats);
    /// §16 — sized roll via kRollSizes (1/32 … 1 beat).
    void loopRollIdx(int deck, int idx);
    void loopRollEnd(int deck);
    // ---- saved loops ----
    void saveLoop(int deck, int slot);
    void triggerSavedLoop(int deck, int slot);
    void deleteSavedLoop(int deck, int slot);
    std::vector<SavedLoop> savedLoops(int deck) const;

    // ---- slip / reverse (§17-18, §27-28) ----
    void setSlip(int deck, bool on);
    bool slipOn(int deck) const;      // user-mode toggle state
    bool slipActive(int deck) const;  // any ref held (mode/roll/momentary)
    void setReverse(int deck, bool on);
    /// §27 momentary reverse — held = reversed; release restores the
    /// latched direction. Under slip the shadow keeps advancing (§28).
    void reverseMomentary(int deck, bool held);
    bool reverseEffective(int deck) const;

    /// §36 — manual-loop in-point armed (waiting for out).
    bool loopInArmed(int deck) const {
        return deck >= 0 && deck < kMaxDecks
            && ctx(deck).pendingLoopIn >= 0;
    }
    /// §36 — roll in flight on this deck (slip Roll ref held).
    bool rollActive(int deck) const {
        return deck >= 0 && deck < kMaxDecks && ctx(deck).slip.rolling();
    }

    // ---- undo support (§41) ----
    /// Verbatim re-insert of a captured cue (undo of delete/set).
    void restoreCue(int deck, const Cue& c);
    /// Verbatim re-insert of a captured saved loop.
    void restoreLoop(int deck, const SavedLoop& l);
    /// Live-loop range snapshot for undo of loop ops.
    void loopRange(int deck, int64_t& in, int64_t& out) const;
    /// Restore a live loop range (in<0 = clear the loop).
    void restoreLoopRange(int deck, int64_t in, int64_t out, bool on);

    // ---- scheduler + diagnostics (§29-31, §43, §50) ----
    /// Deterministic ordering layer — every performance action's rt
    /// commands post here and flush in documented priority order.
    PerformanceScheduler&       scheduler()       { return sched_; }
    const PerformanceScheduler& scheduler() const { return sched_; }
    const PerformanceDiagnostics& diag()    const { return diag_; }
    std::string perfDiagText(int deck) const { return diag_.text(deck); }
    /// Flush pending scheduler commands to the sink. Public mutators
    /// call it themselves; exposed for the safety flush in poll().
    void commit();
    /// §43 — last rejection reason per deck (Ok when healthy).
    PerfError lastError(int deck) const;

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
        SlipEngine    slip;             // §17 — refcounted sources
        ReverseEngine rev;              // §27 — latch + momentary XOR
        SyncMode mode     { SyncMode::Off };
        /// Last rejected sync request — surfaced by syncStatus so a
        /// missing/unreliable grid is never silently ignored.
        SyncStatus lastError { SyncStatus::Off };
        PerfError  lastErr   { PerfError::Ok };   // §43 perf rejections
        bool   syncLock   { true };    // sync survives track change
        float  gridConf   { 0.7f };    // analysis confidence at attach
        int64_t pendingLoopIn { -1 };
        int64_t lastLoopIn { -1 }, lastLoopOut { -1 };
        int64_t cuePoint  { -1 };      // §24 — temporary cue (session)
        bool   cuePreview { false };   // §25 — cue-hold preview active
        HotCueMode hotMode { HotCueMode::JumpPlay }; // §5
        int    loopSizeIdx { 5 };      // §11 — kLoopSizes index (1 bt)
        int    jumpSizeIdx { 2 };      // §19 — kJumpSizes index (4 bt)
        // poll() edge detection
        int    lastBeatInt { -1 };
        int    lastBarInt  { -1 };
        bool   lastLoopOn  { false };
        bool   lastSlip    { false };
    };

    /// Post through the scheduler — deterministic priority ordering
    /// (§29-31). `send()` is post-only; public methods call commit()
    /// before returning so a whole action flushes in policy order.
    void   send(const rt::Command& c);
    void   emit(PerfEventType t, int deck, int slot = -1, double v = 0.0);
    void   reject(int deck, PerfError e, int slot = -1);   // §43
    /// §43 — stats wired AND a real track present (not Empty). Actions
    /// on an empty deck reject deterministically instead of emitting
    /// commands into a deck with nothing loaded.
    bool    hasTrack(int deck) const;
    int64_t curFrame(int deck) const;
    static int deckOf(const rt::Command& c);
    DeckCtx& ctx(int deck) { return decks_[(size_t)deck]; }
    const DeckCtx& ctx(int deck) const { return decks_[(size_t)deck]; }
    void   persistCue(const DeckCtx& d, const Cue& c);
    void   persistLoopDelete(const DeckCtx& d, int slot);
    void   storeLoopRange(DeckCtx& d, int64_t in, int64_t out);

    std::function<void(const rt::Command&)> sink_;
    PerformanceStore* store_ { nullptr };
    PerformanceScheduler sched_;
    PerformanceDiagnostics diag_;
    std::array<DeckCtx, kMaxDecks> decks_;
    std::vector<Listener> listeners_;
    int master_ { -1 };
    sync::TempoMaster masterSel_;                 // source + pause policy
    sync::PitchPolicy pitchPolicy_ { sync::PitchPolicy::Override };
    sync::ClockSource externalClock_;             // §5 future feed
    bool rtMasterActive_ { false };               // ClockStops bookkeeping
};

} // namespace onda
