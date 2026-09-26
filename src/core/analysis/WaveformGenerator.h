#pragma once

#include <cstdint>

#include "WaveformData.h"

namespace onda {

/// Builds a WaveformData (overview + detail) from decoded PCM.
/// Pure function over a buffer — fully decoupled from the decoder, so
/// any PCM source can feed it. Worker thread only.
class WaveformGenerator {
public:
    /// samples: interleaved PCM, `channels` 1|2.
    WaveformData generate(const float* samples, int64_t numFrames,
                          int channels, double sampleRate);
};

} // namespace onda
