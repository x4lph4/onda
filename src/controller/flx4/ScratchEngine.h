#pragma once

#include <cstdint>

namespace onda::ctrl::flx4 {

/// Hardware-decoupled scratch model (task §7): receives platter
/// deltas + touch state and produces playback-position deltas for the
/// deck engine. No device or MIDI types reach in — the same model can
/// serve any future controller.
///
/// Model v1 (documented approximation, not a vinyl emulator):
///   - While touching: position follows platter rotation linearly,
///     delta * scratchGain seconds per tick-unit.
///   - While not touching: deltas become a jog/seek nudge at reduced
///     gain (no spin-up/backspin inertia — that needs deck-side rate
///     control and is intentionally left out; unstable scratch is
///     worse than no scratch).
///   - Velocity is estimated from delta per tick (ticks/sec), exposed
///     for callers that want a spin-off gesture later.
struct ScratchEngine {
    enum Mode : uint8_t { VINYL = 0, CDJ, SEARCH };

    Mode   mode        = VINYL;
    float  scratchGain = 0.0018f; // sec per delta unit while touching
    float  nudgeGain   = 0.04f;   // sec per delta unit free-spin
    float  searchGain  = 0.12f;   // sec per delta unit shift-search
    bool   touching    = false;
    double posSec      = 0.0;     // model position (mirrors deck)
    double velocity    = 0.0;     // delta units per update, smoothed

    void touchStart() { touching = true;  velocity = 0.0; }
    void touchEnd()   { touching = false; velocity = 0.0; }

    /// One platter delta → position delta in seconds.
    double apply(double delta) {
        velocity = velocity * 0.7 + delta * 0.3;
        double g;
        if (mode == SEARCH)      g = searchGain;
        else if (touching)       g = scratchGain;
        else                     g = nudgeGain;
        const double d = delta * g;
        posSec += d;
        return d;
    }

    void syncPosition(double sec) { posSec = sec; }
};

} // namespace onda::ctrl::flx4
