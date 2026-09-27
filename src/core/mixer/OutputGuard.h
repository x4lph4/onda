#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

namespace onda::mixer {

/// §40-42 — last-resort output protection on the physical outs.
/// Per mapped stereo pair it:
///   - zeroes non-finite samples (NaN/Inf) and counts them,
///   - counts samples exceeding full scale and hard-clamps to kClamp
///     (protection, never a substitute for gain staging — documented),
///   - feeds a slow one-pole DC-offset estimator published as `dc`.
/// Everything is RT-safe: plain math + atomics, no allocation.
class OutputGuard {
public:
    /// Hard ceiling written to hardware — far above full scale so the
    /// guard only engages on genuinely broken signal, never on hot
    /// program material (that is what the master limiter is for).
    static constexpr float kClamp = 4.0f;

    struct Stats {
        std::atomic<uint64_t> sanitized { 0 };  // NaN/Inf/over-clamp fixed
        std::atomic<uint64_t> overFull  { 0 };  // |x| > 1.0 detected
        std::atomic<float>    dc        { 0.0f };// |DC| estimate, slow mean
    };

    void prepare(double sampleRate) noexcept {
        // ~200 ms DC estimator — slow enough to not chase the program.
        dcCoeff_ = std::exp(-1.0f / (float)(0.2 * sampleRate));
    }
    void reset() noexcept { dcL_ = dcR_ = 0.0f; }

    const Stats& stats() const noexcept { return stats_; }

    /// Sanitize one output pair in place. Either channel may be null
    /// (unmapped leg / collapsed mono pair).
    void process(float* l, float* r, int n) noexcept {
        uint64_t bad = 0, over = 0;
        for (int i = 0; i < n; ++i) {
            if (l != nullptr) {
                float x = l[i];
                if (!std::isfinite(x))            { x = 0.0f;  ++bad; }
                else if (std::fabs(x) > kClamp)   { x = x < 0.0f ? -kClamp : kClamp; ++bad; }
                else if (std::fabs(x) > 1.0f)     ++over;
                l[i] = x;
                dcL_ += (1.0f - dcCoeff_) * (x - dcL_);
            }
            if (r != nullptr) {
                float x = r[i];
                if (!std::isfinite(x))            { x = 0.0f;  ++bad; }
                else if (std::fabs(x) > kClamp)   { x = x < 0.0f ? -kClamp : kClamp; ++bad; }
                else if (std::fabs(x) > 1.0f)     ++over;
                r[i] = x;
                dcR_ += (1.0f - dcCoeff_) * (x - dcR_);
            }
        }
        if (bad)  stats_.sanitized.fetch_add(bad,  std::memory_order_relaxed);
        if (over) stats_.overFull.fetch_add(over,  std::memory_order_relaxed);
        const float d = std::max(std::fabs(dcL_), std::fabs(dcR_));
        stats_.dc.store(d, std::memory_order_relaxed);
    }

private:
    float dcCoeff_ { 0.999f };
    float dcL_ { 0.0f }, dcR_ { 0.0f };
    Stats stats_;
};

} // namespace onda::mixer
