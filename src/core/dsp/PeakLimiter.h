#pragma once

#include <algorithm>
#include <cmath>

namespace onda {

/// Simple zero-lookahead peak limiter for the master bus.
/// Instant attack (envelope follower), ~50 ms release. A real lookahead
/// limiter replaces this when the FX engine lands — the interface stays.
class PeakLimiter {
public:
    void prepare(double sampleRate) noexcept {
        releaseCoeff_ = std::exp(-1.0f / (float)(0.05 * sampleRate)); // 50 ms
        reset();
    }

    void reset() noexcept { env_ = 0.0f; gainReduction_ = 1.0f; }
    void setThreshold(float t) noexcept { threshold_ = t; }

    /// Returns per-sample gain reduction factor for metering.
    float lastGainReduction() const noexcept { return gainReduction_; }

    void process(float* l, float* r, int n) noexcept {
        for (int i = 0; i < n; ++i) {
            const float peak = std::max(std::fabs(l[i]), std::fabs(r[i]));
            env_ = std::max(peak, env_ * releaseCoeff_);
            const float want = env_ > threshold_ ? threshold_ / env_ : 1.0f;
            gainReduction_ = std::min(gainReduction_, want);              // instant attack
            gainReduction_ += (1.0f - releaseCoeff_) * (want - gainReduction_); // release
            l[i] *= gainReduction_;
            r[i] *= gainReduction_;
        }
    }

private:
    float threshold_       { 0.891f }; // ~ -1 dBFS
    float releaseCoeff_    { 0.9995f };
    float env_             { 0.0f };
    float gainReduction_   { 1.0f };
};

} // namespace onda
