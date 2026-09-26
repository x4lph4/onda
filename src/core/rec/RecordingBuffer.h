#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <vector>

#include "core/rec/RecordingTypes.h"

namespace onda::rec {

/// Single-producer (audio thread) / single-consumer (writer thread)
/// ring of interleaved stereo float frames. No locks, no allocation
/// after construction; power-of-two capacity for mask indexing.
struct SpscRing {
    explicit SpscRing(size_t framesPow2) { buf_.resize(framesPow2 * 2); }

    size_t capacity() const noexcept { return buf_.size() / 2; }

    /// Producer — audio thread. Writes up to n frames; returns frames
    /// actually written (short write = overflow, count as dropped).
    size_t push(const float* l, const float* r, size_t n) noexcept {
        const size_t cap   = capacity();
        const size_t head  = head_.load(std::memory_order_relaxed);
        const size_t tail  = tail_.load(std::memory_order_acquire);
        size_t space = cap - (head - tail);
        if (space == 0) return 0;
        const size_t avail = space - 1;              // keep slot empty
        if (n > avail) n = avail;
        if (n == 0) return 0;
        const size_t mask = cap - 1;
        for (size_t i = 0; i < n; ++i) {
            const size_t p = ((head + i) & mask) * 2;
            buf_[p]     = l[i];
            buf_[p + 1] = r[i];
        }
        head_.store(head + n, std::memory_order_release);
        return n;
    }

    /// Consumer — writer thread. Pops up to n frames as interleaved
    /// stereo into `out`; returns frames popped.
    size_t pop(float* out, size_t n) noexcept {
        const size_t cap  = capacity();
        const size_t head = head_.load(std::memory_order_acquire);
        const size_t tail = tail_.load(std::memory_order_relaxed);
        size_t avail = head - tail;
        if (n > avail) n = avail;
        const size_t mask = cap - 1;
        for (size_t i = 0; i < n; ++i) {
            const size_t p = ((tail + i) & mask) * 2;
            out[i * 2]     = buf_[p];
            out[i * 2 + 1] = buf_[p + 1];
        }
        tail_.store(tail + n, std::memory_order_release);
        return n;
    }

    /// Occupancy 0-100 — polled by either side (approximate under race).
    int fillPct() const noexcept {
        const size_t cap  = capacity();
        const size_t used = head_.load(std::memory_order_acquire)
                          - tail_.load(std::memory_order_acquire);
        return cap ? int(used * 100 / cap) : 0;
    }
    void clear() noexcept {
        head_.store(0); tail_.store(0);
    }

private:
    std::vector<float>   buf_;
    std::atomic<size_t>  head_{0};
    std::atomic<size_t>  tail_{0};
};

/// Clip detection threshold — real "overs" from the post-limiter bus.
inline constexpr float kClipThresh = 0.9995f;

/// Audio-thread recording tap. Fed post-master inside the render
/// callback: computes per-block peaks, session peak/RMS, clip events,
/// and pushes frames into the ring. Never allocates, locks, or touches
/// the filesystem — when `capturing` is off it returns in one load.
class RecorderRt {
public:
    /// ~8s @48k stereo (~3 MB) — amortises slow writes.
    RecorderRt() : ring_(1 << 20) {}   // 1,048,576 frames = pow2

    // ---- RT side (audio thread) ----
    void pushPostMaster(const float* l, const float* r, int n) noexcept {
        if (!capturing_.load(std::memory_order_relaxed)) return;
        float pl = 0.0f, pr = 0.0f;
        double sql = 0.0, sqr = 0.0;
        bool clipped = false;
        for (int i = 0; i < n; ++i) {
            const float al = std::fabs(l[i]), ar = std::fabs(r[i]);
            if (al > pl) pl = al;
            if (ar > pr) pr = ar;
            sql += double(al) * al; sqr += double(ar) * ar;
            if (al >= kClipThresh || ar >= kClipThresh) clipped = true;
        }
        blockPeakL_.store(pl, std::memory_order_relaxed);
        blockPeakR_.store(pr, std::memory_order_relaxed);
        for (float m = maxPeakL_.load(std::memory_order_relaxed);
             pl > m && !maxPeakL_.compare_exchange_weak(m, pl);) {}
        for (float m = maxPeakR_.load(std::memory_order_relaxed);
             pr > m && !maxPeakR_.compare_exchange_weak(m, pr);) {}
        sumSqL_.store(sumSqL_.load(std::memory_order_relaxed) + sql,
                      std::memory_order_relaxed);
        sumSqR_.store(sumSqR_.load(std::memory_order_relaxed) + sqr,
                      std::memory_order_relaxed);
        sqFrames_.fetch_add(n, std::memory_order_relaxed);
        framesIn_.fetch_add(n, std::memory_order_relaxed);
        if (clipped) {
            clipCount_.fetch_add(1, std::memory_order_relaxed);
            const float cp = std::max(pl, pr);
            for (float m = clipPeak_.load(std::memory_order_relaxed);
                 cp > m && !clipPeak_.compare_exchange_weak(m, cp);) {}
            // bounded clip-event queue (drop if full)
            const int w = clipW_.load(std::memory_order_relaxed);
            const int r_ = clipR_.load(std::memory_order_acquire);
            if (w - r_ < (int)std::size(clipQ_)) {
                ClipEvent& e = clipQ_[(size_t)w % std::size(clipQ_)];
                e.frame   = framesIn_.load(std::memory_order_relaxed) - n;
                e.channel = pl >= pr ? 0 : 1;
                e.peak    = cp;
                clipW_.store(w + 1, std::memory_order_release);
            }
        }
        const size_t want = (size_t)n;
        const size_t wrote = ring_.push(l, r, want);
        if (wrote < want)
            dropped_.fetch_add((int64_t)(want - wrote),
                               std::memory_order_relaxed);
    }

    // ---- control (app thread; safe any time) ----
    void arm(bool on)        noexcept { capturing_.store(on); }
    bool capturing()   const noexcept {
        return capturing_.load(std::memory_order_acquire); }
    void resetMeters() noexcept {
        blockPeakL_ = blockPeakR_ = maxPeakL_ = maxPeakR_ = clipPeak_ = 0.0f;
        sumSqL_ = sumSqR_ = 0.0; sqFrames_ = framesIn_ = dropped_ = 0;
        clipCount_ = 0; clipW_ = clipR_ = 0;
        ring_.clear();
    }

    // ---- drain side (writer thread) ----
    SpscRing& ring() noexcept { return ring_; }

    // ---- telemetry (app thread) ----
    Meter meter(double rate) const {
        Meter m;
        m.peakL = blockPeakL_; m.peakR = blockPeakR_;
        m.maxPeakL = maxPeakL_; m.maxPeakR = maxPeakR_;
        const int64_t f = sqFrames_.load();
        if (f > 0) {
            m.rmsL = (float)std::sqrt(sumSqL_.load() / (double)f);
            m.rmsR = (float)std::sqrt(sumSqR_.load() / (double)f);
        }
        m.frames = framesIn_.load();
        m.durationSec = rate > 0.0 ? (double)m.frames / rate : 0.0;
        m.clipCount = clipCount_; m.clipPeak = clipPeak_;
        m.droppedFrames = dropped_; m.fillPct = ring_.fillPct();
        return m;
    }
    bool popClip(ClipEvent& e) noexcept {
        const int r = clipR_.load(std::memory_order_relaxed);
        if (r >= clipW_.load(std::memory_order_acquire)) return false;
        e = clipQ_[(size_t)r % std::size(clipQ_)];
        clipR_.store(r + 1, std::memory_order_release);
        return true;
    }

private:
    SpscRing           ring_;
    std::atomic<bool>  capturing_{false};
    std::atomic<float> blockPeakL_{0}, blockPeakR_{0};
    std::atomic<float> maxPeakL_{0}, maxPeakR_{0};
    std::atomic<float> clipPeak_{0};
    std::atomic<double> sumSqL_{0}, sumSqR_{0};
    std::atomic<int64_t> sqFrames_{0}, framesIn_{0}, dropped_{0};
    std::atomic<uint32_t> clipCount_{0};
    ClipEvent          clipQ_[64];
    std::atomic<int>   clipW_{0}, clipR_{0};
};

} // namespace onda::rec
