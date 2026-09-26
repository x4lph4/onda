#pragma once

#include <cstdint>
#include <string>

#include "../performance/PerformanceTypes.h"

namespace onda::sync {

/// Who owns the session's musical clock (§4-5). `DeckX` values are
/// explicit pins; `Auto` follows documented selection rules and never
/// surprise-switches while the current master is valid.
enum class MasterSource : int {
    Auto     = -1,
    DeckA    = 0, DeckB = 1, DeckC = 2, DeckD = 3,
    External = 4,   // future: external clock feed — no deck master
    None     = 5,   // structurally no master — sync cannot engage
};

/// What the master clock does when the master deck pauses (§22).
/// Chosen default is ClockHolds: the session clock freezes on the last
/// master position and followers hold their converged tempo —
/// deterministic, no surprise re-masters mid-set.
enum class MasterPausePolicy : int {
    ClockHolds      = 0,
    ClockStops      = 1,  // clock invalid → followers free-run
    FollowOtherDeck = 2,  // promote another playing synced deck
};

/// How a manual pitch-fader move interacts with an engaged sync (§26).
enum class PitchPolicy : int {
    Override      = 0,  // default: fader ignored while synced (Rekordbox-style)
    Disable       = 1,  // touching pitch disables sync on that deck
    ManualAsBend  = 2,  // fader acts as a tempo bend on top of sync
};

/// Phase-error classification shared by the RT corrector and the
/// control-side diagnostics (§14-16).
enum class CorrectionKind : int {
    Deadband   = 0,   // inside the no-op zone — no correction
    Chase      = 1,   // small error → spread across the block
    Reposition = 2,   // large error → single faded snap
};

/// Per-deck sync snapshot (§6) — plain data for UI/diagnostics/tests.
struct DeckSyncState {
    bool      enabled    { false };
    bool      isMaster   { false };
    SyncMode  mode       { SyncMode::Off };
    SyncStatus status    { SyncStatus::Off };
    double    bpm        { 0.0 };   // current effective BPM
    double    targetBpm  { 0.0 };   // master BPM the deck chases
    double    beatPos    { -1.0 };  // continuous beat position
    double    phase      { 0.0 };   // position inside current beat 0..1
    bool      tempoLock  { false }; // required tempo out of pitch range
    bool      syncLock   { false }; // survives track changes (§24)
    float     confidence { 0.0f };  // 0..1 (§30)
};

} // namespace onda::sync
