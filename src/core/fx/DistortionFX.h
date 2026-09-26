#pragma once

#include <algorithm>
#include <cmath>

#include "DelayLine.h"
#include "FXProcessor.h"

namespace onda::fx {

/// Waveshaping distortion — tanh soft-clip blended into a harder
/// foldback shape, with a post tone filter and output gain. The
/// shaped signal is always bounded (±1-ish) so `Output` is the only
/// way to push level up — no uncontrolled digital clipping.
///
/// Params:  0 Drive | 1 Tone Hz | 2 Curve (0 soft .. 1 hard)
///          3 Output | 4 Asym (bias into the shaper)
class DistortionFX : public FXBase<5> {
public:
    FXKind      kind() const noexcept override { return FXKind::Distortion; }
    const char* name() const noexcept override { return "Distortion"; }
    bool supportsSync() const noexcept override { return false; }

    FXParamInfo paramInfo(int i) const noexcept override {
        static constexpr FXParamInfo k[] = {
            { "Drive",  1.0f,    48.0f,   6.0f,    "x",  0.0f },
            { "Tone",   200.0f,  18000.0f,6000.0f, "Hz", 0.0f },
            { "Curve",  0.0f,    1.0f,    0.25f,   "",   0.0f },
            { "Output", 0.0f,    2.0f,    1.0f,    "",   0.0f },
            { "Asym",  -0.5f,    0.5f,    0.0f,    "",   0.0f },
        };
        return k[i % 5];
    }

    void prepare(double sr, int /*maxBlock*/) override {
        rate_ = sr;
        prepareSmooth(sr, 6.0f);
        lpL_.reset(); lpR_.reset();
    }

    void reset() noexcept override { lpL_.reset(); lpR_.reset(); }

    void process(float* l, float* r, int n) noexcept override {
        for (int i = 0; i < n; ++i) {
            const float drv = tick(0);
            const float cv  = tick(2);
            const float out = tick(3);
            const float asy = tick(4);
            const float tn  = tick(1);
            lpL_.setCutoff(rate_, tn);
            lpR_.setCutoff(rate_, tn);
            l[i] = lpL_.tick(shape(l[i], drv, cv, asy)) * out;
            r[i] = lpR_.tick(shape(r[i], drv, cv, asy)) * out;
        }
    }

private:
    /// Bounded shaper: soft = tanh, hard = folded sine (still ≤1).
    static float shape(float in, float drive, float curve,
                       float asym) noexcept {
        const float x = in * drive + asym;
        const float soft = std::tanh(x);
        float hard = std::sin(1.5707963f * x);
        hard = hard > 1.0f ? 1.0f : (hard < -1.0f ? -1.0f : hard);
        return soft + (hard - soft) * curve;
    }

    double    rate_ { 48000.0 };
    OnePoleLp lpL_, lpR_;
};

} // namespace onda::fx
