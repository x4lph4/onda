#pragma once

#include <atomic>
#include <cstdint>

namespace onda {

/// Master/cue bus telemetry. Written by the RT thread (single writer),
/// read by UI/diagnostics.
struct MasterStats {
    std::atomic<float>    peakL{0.0f}, peakR{0.0f};   // instant block peak
    std::atomic<float>    rmsL{0.0f},  rmsR{0.0f};    // block RMS
    std::atomic<uint64_t> clipCount{0};               // samples over full scale
    std::atomic<float>    limiterGain{1.0f};          // current gain reduction
    std::atomic<bool>     cueAvailable{false};        // device has >= 4 outs
};

} // namespace onda
