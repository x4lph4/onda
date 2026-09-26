#pragma once

#include <cmath>

namespace onda::sampler {

/// Replaceable pitch/speed strategy (task §10). The default is pure
/// playback-rate scaling (pitch shifts with speed — classic sampler).
/// A future time-stretch implementation (e.g. WSOLA-based) can be
/// dropped in behind the same interface without touching the engine.
struct ISamplePitchProcessor {
    virtual ~ISamplePitchProcessor() = default;
    /// playback-rate multiplier for a given semitone offset
    virtual double stepFor(float semitones) const noexcept = 0;
};

/// Default: rate = 2^(semi/12). No time-stretch — pitch follows speed.
struct SamplePitchProcessor final : ISamplePitchProcessor {
    double stepFor(float semitones) const noexcept override {
        return std::pow(2.0, (double)semitones / 12.0);
    }
};

} // namespace onda::sampler
