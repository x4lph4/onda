#pragma once

#include <algorithm>
#include <cmath>

#include "DelayLine.h"
#include "FXProcessor.h"

namespace onda::fx {

/// Classic flanger: short modulated delay (0.5..10 ms) mixed into the
/// feedback loop, dual LFO with a stereo phase offset.
///
/// Params:  0 Rate Hz (or synced) | 1 Depth | 2 Feedback | 3 StereoPhase
class FlangerFX : public FXBase<4> {
public:
    FXKind      kind() const noexcept override { return FXKind::Flanger; }
    const char* name() const noexcept override { return "Flanger"; }
    bool supportsSync() const noexcept override { return true; }

    FXParamInfo paramInfo(int i) const noexcept override {
        static constexpr FXParamInfo k[] = {
            { "Rate",        0.05f, 5.0f, 0.30f, "Hz", 0.0f, FXParamType::Float, FXParamCurve::Log },
            { "Depth",       0.0f,  1.0f, 0.70f, "",   0.0f },
            { "Feedback",    0.0f,  0.9f, 0.40f, "",   0.0f },
            { "StereoPhase", 0.0f,  1.0f, 0.25f, "",   0.0f },
        };
        return k[i & 3];
    }

    void prepare(double sr, int /*maxBlock*/) override {
        rate_ = sr;
        prepareSmooth(sr, 15.0f);
        const int maxD = (int)(0.012 * sr) + 4; // 12 ms headroom
        lineL_.prepare(maxD);
        lineR_.prepare(maxD);
        lfoL_.reset(); lfoR_.reset();
        reset();
    }

    void reset() noexcept override {
        lineL_.reset(); lineR_.reset();
        lfoL_.reset(); lfoR_.reset();
    }

    void process(float* l, float* r, int n) noexcept override {
        if (ctx().synced) {
            const double frames = ctx().divisionFrames(rate_);
            lfoL_.setInc(1.0 / (frames > 1.0 ? frames : 1.0));
            lfoR_.setInc(lfoL_.inc);
        } else {
            lfoL_.setRateHz(rate_, value(0));
            lfoR_.setRateHz(rate_, value(0));
        }
        const float base = 0.5f;                       // ms
        const float span = 9.5f;                       // ms
        for (int i = 0; i < n; ++i) {
            const float depth = tick(1);
            const float fb    = tick(2);
            const float phOff = tick(3);
            const float sL = lfoL_.tick();
            lfoR_.tick(); // advance phase — offset-applied value below
            // apply stereo offset to the right LFO
            const float sRo = (float)std::sin(6.28318530718 *
                                  (lfoR_.phase - phOff));
            const float dL = (base + span * depth * (0.5f + 0.5f * sL))
                             * 0.001f * (float)rate_;
            const float dR = (base + span * depth * (0.5f + 0.5f * sRo))
                             * 0.001f * (float)rate_;
            const float tL = lineL_.read(dL);
            const float tR = lineR_.read(dR);
            lineL_.write(l[i] + tL * fb);
            lineR_.write(r[i] + tR * fb);
            l[i] = tL;
            r[i] = tR;
        }
    }

private:
    double rate_ { 48000.0 };
    DelayLine lineL_, lineR_;
    Lfo       lfoL_, lfoR_;
};

} // namespace onda::fx
