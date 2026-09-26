#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace onda {

/// Sync operating modes — independent, not cumulative states.
/// PhraseSync currently aligns to bar boundaries (phrase detection can
/// plug in later without changing the transport contract).
enum class SyncMode : int { Off = 0, Bpm = 1, Beat = 2, Phrase = 3 };

/// Deterministic sync status surfaced to UI/controllers — failures are
/// reported, never silently ignored.
enum class SyncStatus {
    Off, Synced, Syncing, NoMaster, NoGrid, NoGridMaster,
    UnreliableMaster, OutOfRange, DeckStopped
};

enum class CueType : int { Hot = 0, Memory = 1, Loop = 2 };

/// One stored cue. `frame` is the authored position; `beat` is its
/// musical position (for quantize re-resolution after grid edits).
/// Loop cues also carry `frame2`/`beat2` = loop-out point.
struct Cue {
    int         slot     { 0 };
    CueType     type     { CueType::Hot };
    int64_t     frame    { 0 };
    int64_t     frame2   { -1 };   // loop-out frame (CueType::Loop)
    double      beat     { 0.0 };
    double      beat2    { -1.0 };
    uint32_t    argb     { 0xFFFF4444 };
    std::string label;
    int64_t     createdAt{ 0 };
};

struct SavedLoop {
    int      slot     { 0 };
    int64_t  inFrame  { 0 };
    int64_t  outFrame { 0 };
    double   inBeat   { 0.0 };
    double   outBeat  { 0.0 };
    uint32_t argb     { 0xFF4488FF };
    std::string label;
};

/// Quantize resolution in beats (§17). `Bar` resolves against the
/// grid's beatsPerBar — pass a real bpb when available.
enum class Quantize : int {
    Beat1 = 0, Half, Quarter, Eighth, Sixteenth, ThirtySecond, Bar
};
inline double quantizeBeats(Quantize q, int beatsPerBar = 4) noexcept {
    switch (q) {
        case Quantize::Half:         return 0.5;
        case Quantize::Quarter:      return 0.25;
        case Quantize::Eighth:       return 0.125;
        case Quantize::Sixteenth:    return 0.0625;
        case Quantize::ThirtySecond: return 0.03125;
        case Quantize::Bar:          return beatsPerBar > 0 ? beatsPerBar : 4.0;
        default:                     return 1.0;
    }
}

/// Events emitted by PerformanceEngine (non-RT thread — marshal to UI).
enum class PerfEventType {
    SyncStateChanged, MasterDeckChanged, BeatChanged, BarChanged,
    CueCreated, CueDeleted, CueTriggered,
    LoopEntered, LoopExited, LoopSaved,
    BeatJumped, SlipModeChanged, QuantizeChanged, GridAttached
};
struct PerfEvent {
    PerfEventType type;
    int    deck  { -1 };
    int    slot  { -1 };
    double value { 0.0 };
};

} // namespace onda
