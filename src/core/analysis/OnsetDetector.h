#pragma once

#include <cstdint>
#include <vector>

namespace onda {

struct Onset {
    int64_t frame { 0 };      // device-rate frame position
    float   strength { 0.0f };
};

/// Spectral-flux onset detection over mono-summed audio.
/// Worker-thread only — allocates freely, never runs on RT.
class OnsetDetector {
public:
    /// Analyzes interleaved stereo (or mono) `samples`, returns the raw
    /// onset envelope (positive-slope spectral flux, one value per hop)
    /// and converts to device-rate positions via `hop`.
    struct Result {
        std::vector<Onset> onsets;
        std::vector<float> envelope;    // flux per hop
        int hop { 0 };                  // frames per envelope value
        double rate { 0.0 };
    };

    Result detect(const float* samples, int64_t numFrames,
                  int channels, double sampleRate);

private:
    std::vector<float> mags_, prevMags_;
};

} // namespace onda
