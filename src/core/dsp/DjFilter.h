#pragma once

#include <algorithm>
#include <cmath>

#include "Biquad.h"

namespace onda {

/// Single-knob DJ filter. Position -1..1:
///   pos < 0  -> low-pass, cutoff sweeps 20 kHz -> 200 Hz (exponential)
///   pos = 0  -> bypass
///   pos > 0  -> high-pass, cutoff sweeps 20 Hz -> 8 kHz (exponential)
/// Coefficients recompute only on parameter change.
class DjFilter {
public:
    void prepare(double sampleRate) noexcept { rate_ = sampleRate; setPosition(pos_); reset(); }
    void reset() noexcept { lpL_.reset(); lpR_.reset(); hpL_.reset(); hpR_.reset(); }

    void setPosition(float p) noexcept {
        p = std::clamp(p, -1.0f, 1.0f);
        if (p == pos_) return;
        pos_ = p;
        update();
    }

    void process(float* l, float* r, int n) noexcept {
        const int mode = pos_ < 0.0f ? -1 : (pos_ > 0.0f ? 1 : 0);
        if (mode == 0) return;
        if (mode < 0)
            for (int i = 0; i < n; ++i) { l[i] = lpL_.process(l[i]); r[i] = lpR_.process(r[i]); }
        else
            for (int i = 0; i < n; ++i) { l[i] = hpL_.process(l[i]); r[i] = hpR_.process(r[i]); }
    }

private:
    void update() noexcept {
        const float p = pos_;
        if (p < 0.0f) {
            const double f = 20000.0 * std::pow(200.0 / 20000.0, (double)-p);
            lpL_.setLowpass(rate_, f, 0.707);
            lpR_.setLowpass(rate_, f, 0.707);
        } else {
            const double f = 20.0 * std::pow(8000.0 / 20.0, (double)p);
            hpL_.setHighpass(rate_, f, 0.707);
            hpR_.setHighpass(rate_, f, 0.707);
        }
    }

    double rate_ { 48000.0 };
    float  pos_  { 0.0f };
    Biquad lpL_, lpR_, hpL_, hpR_;
};

} // namespace onda
