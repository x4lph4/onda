#pragma once

#include <cstdint>

namespace onda {

/// Phase 35 §8/§23/§2/§41-42 — spec-shaped POD snapshots and the
/// performance command vocabulary. These mirror live state that
/// already lives in DeckCtx/DeckStats/sync — they are VIEWS, never
/// new owners of truth, so they stay consistent by construction.

/// §8 — the deck's live loop as one POD (built by
/// PerformanceEngine::loopState).
struct LoopState {
    bool     active      { false };  // RT loop flag echo
    int64_t  start       { -1 };     // loop-in frame (-1 = none)
    int64_t  end         { -1 };     // loop-out frame
    int64_t  length      { 0 };      // end - start (frames)
    bool     quantized   { false };  // created under deck quantize
    double   beatLength  { 0.0 };    // musical length through the grid
    int      mode        { 0 };      // 0 auto/sized, 1 manual armed,
                                     // 2 roll active
};

/// §23 — slip bookkeeping snapshot. `virtualSec` is the shadow
/// position echo the RT deck publishes (DeckStats::slipShadowSec);
/// `originalFrame` is where the shadow anchored on activation.
struct SlipState {
    bool     active          { false };
    int64_t  originalFrame   { -1 };  // playhead when slip engaged
    double   virtualSec      { -1.0 };// shadow position (-1 when off)
    uint32_t slipStartMs     { 0 };   // injected-clock timestamp
};

/// §2 — read-only view over the session BeatClock (ClockSample).
/// Performance actions never consult an independent BPM source —
/// this is literally the master clock repacked for performance use.
struct PerformanceClock {
    bool     valid       { false };
    double   bpm         { 0.0 };
    double   beatPos     { -1.0 };  // continuous beat on master grid
    int      bar         { 0 };
    double   phase       { 0.0 };   // 0..1 inside the beat
    int      beatsPerBar { 4 };
};

/// §41 — the performance command vocabulary. Mirrors existing
/// façade entry points; kept as a flat enum so controllers, MIDI
/// learn and tests can name actions without side effects.
enum class PerfCmd : int {
    LoopIn = 0, LoopOut, LoopOn, LoopOff,
    LoopDouble, LoopHalf, LoopMove,
    BeatJump, BeatRoll,
    SlipOn, SlipOff,
    QuantizeOn, QuantizeOff,
    CueJump, Count
};

/// §41-42 — one ordered performance command. `timestampMs` comes
/// from an injected clock (PerformanceEngine::setNowFn) — precise
/// event ordering without depending on wall time.
struct PerformanceCommand {
    PerfCmd  type        { PerfCmd::LoopIn };
    int      deck        { 0 };
    double   value       { 0.0 };   // beats / index / slot
    uint32_t timestampMs { 0 };
};

} // namespace onda
