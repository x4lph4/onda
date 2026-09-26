#pragma once

#include <algorithm>
#include <cmath>

namespace onda::sampler {

/// Optional normalization (task §24). Computes a playback gain that
/// brings the sample's peak to `target` — stored as slot metadata,
/// the source file is NEVER modified.
struct SampleGain {
    /// gain factor (0..2 clamp) so peak*gain ≈ target
    static float normalizeGain(float peak, float target = 0.99f) noexcept {
        if (peak <= 0.0001f) return 1.0f;
        return std::clamp(target / peak, 0.0f, 2.0f);
    }
    /// perceived-loudness-ish estimate in dB (peak-based, cheap)
    static float peakDb(float peak) noexcept {
        return peak > 0.0f ? 20.0f * std::log10(peak) : -120.0f;
    }
};

} // namespace onda::sampler
