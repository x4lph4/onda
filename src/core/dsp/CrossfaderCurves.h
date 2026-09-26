#pragma once

#include <algorithm>
#include <cmath>

namespace onda {

/// Crossfader curve family. x in [-1, +1]: -1 = full side A, +1 = full
/// side B. Gain functions return the *gain for the given side*.
enum class XfCurve : int { Linear = 0, EqualPower = 1, Cut = 2 };

inline float xfGain(XfCurve c, float x, bool sideA) noexcept {
    x = std::clamp(x, -1.0f, 1.0f);
    switch (c) {
        case XfCurve::Linear:
            return sideA ? (1.0f - x) * 0.5f : (1.0f + x) * 0.5f;
        case XfCurve::EqualPower: {
            const float t = (x + 1.0f) * 0.25f * 3.14159265f; // 0..pi/2
            return sideA ? std::cos(t) : std::sin(t);
        }
        case XfCurve::Cut:
            // Full through most of the travel, sharp drop near the far rail.
            return sideA ? std::clamp(1.0f - std::max(0.0f, x - 0.85f) / 0.15f, 0.0f, 1.0f)
                         : std::clamp(1.0f + std::min(0.0f, x + 0.85f) / 0.15f, 0.0f, 1.0f);
    }
    return 1.0f;
}

} // namespace onda
