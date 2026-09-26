#pragma once

#include <cmath>
#include <cstdint>

#include "../analysis/BeatGrid.h"

namespace onda {

/// Beat-jump target math. `beats` is signed (±1/32 … ±16 etc.).
/// Without quantize the jump preserves the beat phase exactly
/// (land on the same offset inside the beat); with quantize it lands
/// on a beat boundary.
struct BeatJumpEngine {
    static int64_t target(const BeatGrid& g, int64_t curFrame,
                          double beats, bool quantizeOn) noexcept {
        if (g.empty()) return curFrame;
        const double cur = g.frameToBeat(curFrame);
        const double dst = quantizeOn ? std::floor(cur + 0.5) + beats
                                      : cur + beats;
        const int64_t f = g.beatToFrame(std::max(dst, 0.0));
        return f >= 0 ? f : curFrame;
    }
};

} // namespace onda
