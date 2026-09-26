#pragma once

#include <algorithm>
#include <cmath>

namespace onda::mixer {

/// Per-sample one-pole parameter smoother. Set the target (RT), call
/// `tick()` per sample or `tickBlock(n)` to advance N samples. Time
/// constant in milliseconds — kills zipper noise on every user control.
class GainSmoother {
public:
    void prepare(double sampleRate, float smoothMs) noexcept {
        coeff_ = std::exp(-1.0f / (float)(0.001 * smoothMs * sampleRate));
    }
    void setTarget(float v) noexcept { target_ = v; }
    void snap(float v) noexcept      { target_ = v; cur_ = v; }
    float tick() noexcept {
        cur_ += (1.0f - coeff_) * (target_ - cur_);
        return cur_;
    }
    float current() const noexcept { return cur_; }
    float target()  const noexcept { return target_; }
    bool  settled() const noexcept {
        return std::fabs(target_ - cur_) < 1e-5f;
    }
private:
    float coeff_ { 0.999f };
    float target_{ 0.0f };
    float cur_   { 0.0f };
};

} // namespace onda::mixer
