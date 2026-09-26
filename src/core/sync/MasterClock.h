#pragma once

#include <functional>

#include "SyncTypes.h"

namespace onda::sync {

/// The session's musical clock (§2-3). Samples come from the audio
/// engine's atomic stats — never from UI timers. `timestampSec` is the
/// master's track position; the clock's precision is the audio clock's.
struct ClockSample {
    bool   valid       { false };
    double bpm         { 0.0 };   // effective master BPM
    double beatPos     { -1.0 };  // continuous beat position
    int    bar         { 0 };     // 1-based bar
    double phase       { 0.0 };   // 0..1 inside the beat
    double beatDurSec  { 0.0 };   // 60/bpm at current tempo
    double barDurSec   { 0.0 };   // beatsPerBar * beatDurSec
    int    beatsPerBar { 4 };
    double timestampSec{ 0.0 };   // master playhead, audio-clock domain
    bool   external    { false }; // came from a ClockSource, not a deck
};

/// Clock source seam (§5): a deck supplies samples today; an external
/// feed (Ableton-Link-style, future) plugs in through the same shape
/// without touching followers.
using ClockSource = std::function<ClockSample()>;

/// Master-clock view the engine exposes. `stopped` distinguishes
/// "no clock" from "clock frozen" under ClockHolds.
struct MasterClockView {
    ClockSample sample;
    bool        stopped { false };
    int         deck    { -1 };   // deck feeding the clock, or -1
};

} // namespace onda::sync
