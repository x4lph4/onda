#pragma once

#include <atomic>
#include <cstdint>

namespace onda::rt {

/// Metrics written by the RT audio thread, read by UI/diagnostics.
/// Every field is atomic so publishing never takes a lock.
struct AudioStats {
    std::atomic<uint64_t> callbackCount{0};
    std::atomic<uint64_t> xrunCount{0};       // detected underruns / scheduling gaps
    std::atomic<float>    lastCallbackMs{0.0f};
    std::atomic<float>    cpuLoad{0.0f};      // smoothed fraction of period consumed
    std::atomic<double>   sampleRate{0.0};
    std::atomic<int>      bufferSize{0};
    std::atomic<bool>     running{false};
};

} // namespace onda::rt
