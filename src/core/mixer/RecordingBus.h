#pragma once

#include <atomic>

#include "../rec/RecordingBuffer.h"

namespace onda::mixer {

/// §34 — recording bus. The named architectural seam between the
/// mixer program and the recording engine: after the master bus has
/// processed the block, the mixer's render loop feeds the post-master
/// program through here into the recorder's bounded SPSC ring.
///
/// No processing is duplicated — the bus copies nothing on its own;
/// RecorderRt::pushPostMaster does the single interleaved-ring write.
/// When no recorder is attached the tap is a relaxed-atomic null
/// check, ~free.
///
/// Future expansion seam: additional taps (pre-fader program, per-bus
/// stems, broadcast stream) attach the same way — the bus fans the
/// program out, each sink stays lock-free.
class RecordingBus {
public:
    /// Attach/detach the recorder sink. Non-RT callers only; the
    /// pointer is published atomically so the audio thread sees the
    /// change between blocks, never mid-block.
    void attach(rec::RecorderRt* r) noexcept {
        rec_.store(r, std::memory_order_release);
    }
    rec::RecorderRt* attached() const noexcept {
        return rec_.load(std::memory_order_acquire);
    }

    /// RT: push one block of post-master program into the sink.
    void push(const float* l, const float* r, int n) noexcept {
        if (auto* rec = rec_.load(std::memory_order_acquire))
            rec->pushPostMaster(l, r, n);
    }

private:
    std::atomic<rec::RecorderRt*> rec_ { nullptr };
};

} // namespace onda::mixer
