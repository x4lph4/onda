#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace onda::fx {

/// Circular mono delay line with linear-interpolated fractional reads.
/// Buffer is allocated once in prepare() — the RT path never allocates.
class DelayLine {
public:
    void prepare(int maxFrames) {
        buf_.assign((size_t)maxFrames + 4, 0.0f);
        w_ = 0;
    }
    void reset() noexcept {
        std::fill(buf_.begin(), buf_.end(), 0.0f);
        w_ = 0;
    }
    int size() const noexcept { return (int)buf_.size(); }

    /// Read `delayFrames` behind the write head (clamped to buffer).
    float read(float delayFrames) const noexcept {
        const int sz = (int)buf_.size();
        if (delayFrames < 1.0f) delayFrames = 1.0f;
        if (delayFrames > (float)(sz - 3)) delayFrames = (float)(sz - 3);
        float rp = (float)w_ - delayFrames;
        while (rp < 0.0f) rp += (float)sz;
        if (rp >= (float)sz) rp -= (float)sz; // float rounding can land on sz
        const int i0 = (int)rp;
        const int i1 = i0 + 1 < sz ? i0 + 1 : 0;
        const float f = rp - (float)i0;
        return buf_[(size_t)i0] + f * (buf_[(size_t)i1] - buf_[(size_t)i0]);
    }

    void write(float v) noexcept {
        buf_[(size_t)w_] = v;
        if (++w_ >= (int)buf_.size()) w_ = 0;
    }

    /// Peak of the stored content (runaway diagnostics only — O(n)).
    float contentPeak() const noexcept {
        float p = 0.0f;
        for (float v : buf_) p = std::max(p, std::fabs(v));
        return p;
    }

private:
    std::vector<float> buf_;
    int w_ { 0 };
};

/// One-pole low-pass used inside feedback paths (echo tone, delay
/// damping). Deliberately trivial — stable at any cutoff.
struct OnePoleLp {
    float z { 0.0f };
    float a { 1.0f }; // 1 = bypass
    void setCutoff(double sr, float hz) noexcept {
        const float x = std::exp(-6.28318530718f * hz / (float)sr);
        a = 1.0f - x < 0.0f ? 0.0f : (1.0f - x > 1.0f ? 1.0f : 1.0f - x);
        (void)z;
    }
    float tick(float in) noexcept {
        z += a * (in - z);
        return z;
    }
    void reset() noexcept { z = 0.0f; }
};

/// Cheap phase accumulator LFO (sine).
struct Lfo {
    double phase { 0.0 };
    double inc   { 0.0 };
    void setRateHz(double sr, double hz) noexcept { inc = hz / sr; }
    /// Advance `inc` per sample directly (synced callers compute it
    /// from the division length each block).
    void setInc(double i) noexcept { inc = i; }
    float tick() noexcept {
        phase += inc;
        if (phase >= 1.0) phase -= 1.0;
        return (float)std::sin(6.28318530718 * phase);
    }
    void reset(double p = 0.0) noexcept { phase = p; }
};

} // namespace onda::fx
