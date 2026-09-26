#pragma once

#include <atomic>
#include <algorithm>
#include <cmath>

namespace onda::mixer {

/// Telemetry for one channel or bus. Written by the RT thread (single
/// writer), read by UI/diagnostics — atomics only, no locking.
struct ChannelMeters {
    std::atomic<float>    peakL { 0.0f }, peakR { 0.0f }; // instant block peak
    std::atomic<float>    rmsL  { 0.0f }, rmsR  { 0.0f }; // block RMS
    std::atomic<float>    holdL { 0.0f }, holdR { 0.0f }; // peak-hold w/ decay
    std::atomic<uint64_t> clips { 0 };                    // over-FS samples
    std::atomic<bool>     clip  { false };
    std::atomic<float>    limiterGain { 1.0f };           // master bus only
};

/// Block-level meter calculator. Peak/RMS are computed over the block;
/// hold decays toward the running peak (~6 dB/s) so the UI sees a
/// smooth needle without polling at audio rate. All RT-safe: no
/// allocations, no locks.
class MeteringEngine {
public:
    void prepare(double sampleRate, double holdDecayDbPerSec = 6.0,
                 double clipThreshold = 1.0) noexcept {
        clipTh_ = (float)clipThreshold;
        // decay expressed as a per-block linear factor on amplitude
        holdDecayPerSec_ = 1.0f - (float)std::pow(10.0, -holdDecayDbPerSec / 20.0);
        rate_ = sampleRate;
    }

    void reset(ChannelMeters& m) noexcept {
        m.peakL = 0.0f; m.peakR = 0.0f;
        m.rmsL = 0.0f;  m.rmsR = 0.0f;
        m.holdL = 0.0f; m.holdR = 0.0f;
        m.clip = false; // clips count kept cumulative
    }

    /// RT: scan a stereo block, publish peak/rms/hold/clip into `m`.
    void update(const float* l, const float* r, int n,
                ChannelMeters& m) const noexcept {
        float peakL = 0.0f, peakR = 0.0f, sumL = 0.0f, sumR = 0.0f;
        uint64_t clipped = 0;
        for (int i = 0; i < n; ++i) {
            const float al = std::fabs(l[i]), ar = std::fabs(r[i]);
            peakL = std::max(peakL, al);
            peakR = std::max(peakR, ar);
            sumL += l[i] * l[i];
            sumR += r[i] * r[i];
            if (al > clipTh_ || ar > clipTh_) ++clipped;
        }
        const float rmsL = n > 0 ? std::sqrt(sumL / (float)n) : 0.0f;
        const float rmsR = n > 0 ? std::sqrt(sumR / (float)n) : 0.0f;
        m.peakL.store(peakL, std::memory_order_relaxed);
        m.peakR.store(peakR, std::memory_order_relaxed);
        m.rmsL.store(rmsL, std::memory_order_relaxed);
        m.rmsR.store(rmsR, std::memory_order_relaxed);
        if (clipped > 0) {
            m.clip.store(true, std::memory_order_relaxed);
            m.clips.fetch_add(clipped, std::memory_order_relaxed);
        } else {
            m.clip.store(false, std::memory_order_relaxed);
        }
        // peak-hold decay (~holdDecayPerSec_ of amplitude per second)
        const float decay = holdDecayPerSec_ * ((float)n / (float)rate_);
        const float hL = std::max(peakL, m.holdL.load(std::memory_order_relaxed) * (1.0f - decay));
        const float hR = std::max(peakR, m.holdR.load(std::memory_order_relaxed) * (1.0f - decay));
        m.holdL.store(hL, std::memory_order_relaxed);
        m.holdR.store(hR, std::memory_order_relaxed);
    }

private:
    double rate_            { 48000.0 };
    float  clipTh_          { 1.0f };
    float  holdDecayPerSec_ { 0.5f };
};

} // namespace onda::mixer
