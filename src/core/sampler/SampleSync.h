#pragma once

#include <cmath>

#include "SamplerTypes.h"

namespace onda::sampler {

/// Beat-alignment math (task §11/§12). Pure functions — both the RT
/// engine (trigger time) and the domain layer (UI display) share them.
struct SampleSync {

    /// Playback-speed multiplier to lock a sample of `sampleBpm` onto
    /// `masterBpm`. 1.0 when either side is unknown/zero.
    static double speedFor(double sampleBpm, double masterBpm) noexcept {
        if (sampleBpm <= 0.0 || masterBpm <= 0.0) return 1.0;
        return masterBpm / sampleBpm;
    }

    /// Frames from `beatPos` (fractional, on the master grid) to the
    /// next `quantumBeats` boundary. 0 when quantization is off or no
    /// clock is running.
    static int64_t framesToBoundary(double beatPos, double quantumBeats,
                                    double bpm, double sampleRate) noexcept {
        if (quantumBeats <= 0.0 || bpm <= 0.0 || sampleRate <= 0.0)
            return 0;
        const double inQ = std::fmod(beatPos, quantumBeats);
        const double toNext = inQ < 0.0 ? -inQ : quantumBeats - inQ;
        const double fpBeat = sampleRate * 60.0 / bpm;
        const int64_t d = (int64_t)(toNext * fpBeat + 0.5);
        return d < 0 ? 0 : d;
    }
};

} // namespace onda::sampler
