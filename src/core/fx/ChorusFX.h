#pragma once

#include <algorithm>
#include <cmath>

#include "DelayLine.h"
#include "FXProcessor.h"

namespace onda::fx {

/// Stereo chorus — two modulated short delays with a quarter-cycle
/// phase offset between L and R for width.
///
/// Params:  0 Rate Hz (free) — synced: one LFO cycle per division
///          1 Depth ms | 2 Delay ms | 3 Width 0..1 | 4 Tone Hz
class ChorusFX : public FXBase<5> {
public:
    static constexpr float kMaxDelayMs = 60.0f;

    FXKind      kind() const noexcept override { return FXKind::Chorus; }
    const char* name() const noexcept override { return "Chorus"; }
    bool supportsSync() const noexcept override { return true; }

    FXParamInfo paramInfo(int i) const noexcept override {
        static constexpr FXParamInfo k[] = {
            { "Rate",  0.02f, 8.0f,    0.6f,    "Hz", 0.0f },
            { "Depth", 0.0f,  12.0f,   2.5f,    "ms", 0.0f },
            { "Delay", 1.0f,  40.0f,   14.0f,   "ms", 0.0f },
            { "Width", 0.0f,  1.0f,    0.8f,    "",   0.0f },
            { "Tone",  400.0f,18000.0f,9000.0f, "Hz", 0.0f },
        };
        return k[i % 5];
    }

    void prepare(double sr, int /*maxBlock*/) override {
        rate_ = sr;
        prepareSmooth(sr, 10.0f);
        lineL_.prepare((int)(kMaxDelayMs * 0.001 * sr) + 4);
        lineR_.prepare((int)(kMaxDelayMs * 0.001 * sr) + 4);
        lpL_.reset(); lpR_.reset();
        lfoL_.reset(0.0); lfoR_.reset(0.25);
    }

    void reset() noexcept override {
        lineL_.reset(); lineR_.reset();
        lpL_.reset();   lpR_.reset();
        lfoL_.reset(0.0); lfoR_.reset(0.25);
    }

    void process(float* l, float* r, int n) noexcept override {
        // synced → one LFO cycle per division; free → Rate param
        double inc;
        if (ctx().synced) {
            const double frames = ctx().divisionFrames(rate_);
            inc = 1.0 / (frames > 1.0 ? frames : 1.0);
        } else {
            inc = value(0) / rate_;
        }
        lfoL_.setInc(inc);
        lfoR_.setInc(inc);

        const float invSr = 1.0f / (float)rate_;
        for (int i = 0; i < n; ++i) {
            const float depth = tick(1) * 0.001f * (float)rate_; // frames
            const float base  = tick(2) * 0.001f * (float)rate_;
            const float width = tick(3);
            const float tone  = tick(4);
            lpL_.setCutoff(rate_, tone);
            lpR_.setCutoff(rate_, tone);

            const float wL = lfoL_.tick();
            const float wR = lfoR_.tick();
            // width pans the LFO mix between correlated (0) and
            // quarter-cycle-offset (1) movement
            const float dL = base + depth * (0.5f + 0.5f * wL);
            const float dR = base + depth * (0.5f + 0.5f *
                (wR * width + wL * (1.0f - width)));
            (void)invSr;
            const float yl = lpL_.tick(lineL_.read(dL));
            const float yr = lpR_.tick(lineR_.read(dR));
            lineL_.write(l[i]);
            lineR_.write(r[i]);
            l[i] = yl;
            r[i] = yr;
        }
    }

private:
    double    rate_ { 48000.0 };
    DelayLine lineL_, lineR_;
    OnePoleLp lpL_, lpR_;
    Lfo       lfoL_, lfoR_;
};

} // namespace onda::fx
