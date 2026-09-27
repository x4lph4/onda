#pragma once

#include <cstdint>

namespace onda {

/// Loop-roll bookkeeping (Phase 28 §20). A roll is a TEMPORARY loop:
/// while armed it holds a SlipEngine::Roll ref so the shadow position
/// keeps advancing and the deck returns to the untouched timeline on
/// release. This struct owns the non-RT roll state — the facade turns
/// transitions into SlipBegin/SetLoopFrames/SlipEnd commands; one
/// roll at a time, a second arm RESIZES the running roll.
struct LoopRollEngine {
    bool     armed   { false };
    double   beats   { 0.0 };   // current roll size
    int64_t  inFrame { -1 };    // resolved roll-in frame (for resize)
    int64_t  outFrame{ -1 };
    int      sizeIdx { 3 };     // kRollSizes index (default 1/4)

    /// Returns false when already armed (callers RESIZE instead of
    /// stacking a second roll).
    bool arm(double b, int64_t in, int64_t out) noexcept {
        if (armed) return false;
        armed = true; beats = b; inFrame = in; outFrame = out;
        return true;
    }
    /// Live-resize the running roll (§18 — size change while active).
    void resize(double b, int64_t out) noexcept {
        beats = b; outFrame = out;
    }
    bool release() noexcept {
        if (!armed) return false;
        armed = false; beats = 0.0; inFrame = outFrame = -1;
        return true;
    }
    void clear() noexcept { armed = false; beats = 0.0;
                            inFrame = outFrame = -1; }
};

} // namespace onda
