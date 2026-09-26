#pragma once

#include <cstdint>

namespace onda {

/// Catmull-Rom cubic interpolation between y1 and y2 (t in [0,1)).
inline float catmullRom(float y0, float y1, float y2, float y3, float t) noexcept {
    const float t2 = t * t, t3 = t2 * t;
    return 0.5f * ((2.0f * y1)
        + (-y0 + y2) * t
        + (2.0f * y0 - 5.0f * y1 + 4.0f * y2 - y3) * t2
        + (-y0 + 3.0f * y1 - 3.0f * y2 + y3) * t3);
}

/// Interpolated stereo read from an interleaved buffer at fractional
/// position `pos` (in frames). Indices are clamped at the edges —
/// callers guarantee pos stays inside [0, frames).
inline void readCubicStereo(const float* s, int64_t frames, double pos,
                            float& l, float& r) noexcept {
    int64_t i = (int64_t)pos;
    const float t = (float)(pos - (double)i);
    if (i < 0) { i = 0; }
    if (i > frames - 1) i = frames - 1;

    const int64_t i0 = i > 0 ? i - 1 : 0;
    const int64_t i2 = i + 1 < frames ? i + 1 : frames - 1;
    const int64_t i3 = i + 2 < frames ? i + 2 : frames - 1;

    l = catmullRom(s[i0 * 2],     s[i * 2],     s[i2 * 2],     s[i3 * 2],     t);
    r = catmullRom(s[i0 * 2 + 1], s[i * 2 + 1], s[i2 * 2 + 1], s[i3 * 2 + 1], t);
}

} // namespace onda
