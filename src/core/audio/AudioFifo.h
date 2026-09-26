#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

namespace onda {

/// Single-writer single-reader ring of interleaved float samples.
/// Writer: disk streamer thread. Reader: RT audio thread.
/// All allocation happens at construction; read/write never block.
class AudioFifo {
public:
    explicit AudioFifo(size_t capacityFrames, int channels = 2);

    /// Producer side. Returns frames actually written.
    size_t write(const float* interleaved, size_t frames) noexcept;

    /// Consumer (RT) side. Returns frames actually read.
    size_t read(float* interleavedDest, size_t frames) noexcept;

    /// Producer only: drop queued audio (used when loading a new track).
    void reset() noexcept;

    size_t availableToRead()  const noexcept;
    size_t availableToWrite() const noexcept;
    size_t capacityFrames()   const noexcept { return capacityFrames_; }
    int    channels()         const noexcept { return channels_; }

private:
    const size_t capacityFrames_;
    const int    channels_;
    std::vector<float> buffer_;
    // Monotonic frame counters; indices are value % capacityFrames_.
    std::atomic<size_t> head_{0}; // write position (producer only)
    std::atomic<size_t> tail_{0}; // read position  (consumer only)
};

} // namespace onda
