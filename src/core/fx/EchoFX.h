#pragma once

#include <algorithm>
#include <cmath>

#include "DelayLine.h"
#include "FXProcessor.h"

namespace onda::fx {

/// DJ echo: beat-division delay with a tone filter in the feedback
/// loop, ping-pong mode and a bounded freeze.
///
/// Params:  0 Feedback | 1 Tone (LP cutoff in loop) | 2 PingPong (0/1)
///          3 Freeze (0/1)
///
/// Freeze holds feedback at 1.0 and stops feeding input into the line,
/// so the captured tail sustains. Safety: the feedback path is clamped
/// to +-2 and the loop is always damped below unity while not frozen,
/// so the level can never run away.
class EchoFX : public FXBase<4> {
public:
    static constexpr float kMaxDelayMs = 2500.0f;
    static constexpr float kLoopClamp  = 2.0f;

    FXKind      kind() const noexcept override { return FXKind::Echo; }
    const char* name() const noexcept override { return "Echo"; }
    bool supportsSync() const noexcept override { return true; }
    bool hasTail()      const noexcept override { return true; }

    FXParamInfo paramInfo(int i) const noexcept override {
        static constexpr FXParamInfo k[] = {
            { "Feedback", 0.0f,   1.0f,      0.5f,     "",   0.0f },
            { "Tone",     200.0f, 18000.0f,  8000.0f,  "Hz", 0.0f, FXParamType::Float, FXParamCurve::Log },
            { "PingPong", 0.0f,   1.0f,      1.0f,     "",   1.0f },
            { "Freeze",   0.0f,   1.0f,      0.0f,     "",   1.0f },
        };
        return k[i & 3];
    }

    void prepare(double sr, int /*maxBlock*/) override {
        rate_ = sr;
        prepareSmooth(sr, 15.0f);
        lineL_.prepare((int)(kMaxDelayMs * 0.001 * sr) + 4);
        lineR_.prepare((int)(kMaxDelayMs * 0.001 * sr) + 4);
        lpL_.reset(); lpR_.reset();
        prevD_ = 0.0f;
        reset();
    }

    void reset() noexcept override {
        lineL_.reset(); lineR_.reset();
        lpL_.reset(); lpR_.reset();
    }

    void process(float* l, float* r, int n) noexcept override {
        // Echo is always division-driven (free mode just means the slot
        // isn't tracking a grid — the division still applies at 120 BPM
        // fallback or the last known tempo).
        const double targetFrames = ctx().divisionFrames(rate_);
        float d = prevD_ > 0.0f ? prevD_ : (float)targetFrames;
        const float dInc = ((float)targetFrames - d) / (float)n;

        const bool ping  = target(2) > 0.5f;   // switch params: no ramp
        const bool freeze= target(3) > 0.5f;
        for (int i = 0; i < n; ++i) {
            float fb = tick(0);
            fb = freeze ? 1.0f : std::min(fb, 0.98f);
            lpL_.setCutoff(rate_, value(1));
            lpR_.setCutoff(rate_, value(1));
            d += dInc;

            const float tapL = lineL_.read(d);
            const float tapR = lineR_.read(d);
            const float feed = freeze ? 0.0f : 1.0f;
            auto clamp = [](float v) {
                return v > kLoopClamp ? kLoopClamp
                     : v < -kLoopClamp ? -kLoopClamp : v;
            };
            if (ping) {
                const float inM = 0.5f * (l[i] + r[i]) * feed;
                lineL_.write(clamp(inM + lpL_.tick(tapR) * fb));
                lineR_.write(clamp(lpR_.tick(tapL) * fb));
            } else {
                lineL_.write(clamp(l[i] * feed + lpL_.tick(tapL) * fb));
                lineR_.write(clamp(r[i] * feed + lpR_.tick(tapR) * fb));
            }
            l[i] = tapL;
            r[i] = tapR;
        }
        prevD_ = (float)targetFrames;
    }

private:
    double rate_ { 48000.0 };
    DelayLine lineL_, lineR_;
    OnePoleLp lpL_, lpR_;
    float     prevD_ { 0.0f };
};

} // namespace onda::fx
