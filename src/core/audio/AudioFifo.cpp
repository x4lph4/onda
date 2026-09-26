#include "AudioFifo.h"

#include <algorithm>
#include <cstring>

namespace onda {

AudioFifo::AudioFifo(size_t capacityFrames, int channels)
    : capacityFrames_(capacityFrames),
      channels_(channels),
      buffer_(capacityFrames * (size_t)channels, 0.0f) {}

size_t AudioFifo::write(const float* src, size_t frames) noexcept {
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t tail = tail_.load(std::memory_order_acquire);
    const size_t freeSpace = capacityFrames_ - (head - tail);
    const size_t n = std::min(frames, freeSpace);

    for (size_t off = 0; off < n;) {
        const size_t pos = (head + off) % capacityFrames_;
        const size_t chunk = std::min(n - off, capacityFrames_ - pos);
        std::memcpy(buffer_.data() + pos * (size_t)channels_,
                    src + off * (size_t)channels_,
                    chunk * (size_t)channels_ * sizeof(float));
        off += chunk;
    }

    head_.store(head + n, std::memory_order_release);
    return n;
}

size_t AudioFifo::read(float* dest, size_t frames) noexcept {
    const size_t tail = tail_.load(std::memory_order_relaxed);
    const size_t head = head_.load(std::memory_order_acquire);
    const size_t avail = head - tail;
    const size_t n = std::min(frames, avail);

    for (size_t off = 0; off < n;) {
        const size_t pos = (tail + off) % capacityFrames_;
        const size_t chunk = std::min(n - off, capacityFrames_ - pos);
        std::memcpy(dest + off * (size_t)channels_,
                    buffer_.data() + pos * (size_t)channels_,
                    chunk * (size_t)channels_ * sizeof(float));
        off += chunk;
    }

    tail_.store(tail + n, std::memory_order_release);
    return n;
}

void AudioFifo::reset() noexcept {
    // Producer-side only; RT must not be consuming mid-reset.
    tail_.store(head_.load(std::memory_order_relaxed), std::memory_order_release);
}

size_t AudioFifo::availableToRead() const noexcept {
    return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
}

size_t AudioFifo::availableToWrite() const noexcept {
    return capacityFrames_ - availableToRead();
}

} // namespace onda
