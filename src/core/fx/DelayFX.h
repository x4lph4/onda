#pragma once

#include <algorithm>
#include <cmath>

#include "DelayLine.h"
#include "FXProcessor.h"

namespace onda::fx {

/// Stereo delay with feedback filtering and ping-pong mode.
/// Fully wet on the output; dry/wet lives in FXSlot.
///
/// Params:  0 Time ms | 1 Feedback | 2 Filter Hz (LP in loop)
///          3 PingPong (0/1)
/// Synced mode derives the time from the slot's SyncContext division
/// instead of the Time parameter.
class DelayFX : public FXBase<4> {
public:
    static constexpr float kMaxDelayMs = 2500.0f;

    FXKind      kind() const noexcept override { return FXKind::Delay; }
    const char* name() const noexcept override { return "Delay"; }
    bool supportsSync() const noexcept override { return true; }
    bool hasTail()      const noexcept override { return true; }

    FXParamInfo paramInfo(int i) const noexcept override {
        static constexpr FXParamInfo k[] = {
            { "Time",     5.0f,    kMaxDelayMs, 350.0f,   "ms",  0.0f },
            { "Feedback", 0.0f,    0.98f,       0.35f,    "",    0.0f },
            { "Filter",   300.0f,  18000.0f,    12000.0f, "Hz",  0.0f },
            { "PingPong", 0.0f,    1.0f,        0.0f,     "",    1.0f },
        };
        return k[i & 3];
    }

    void prepare(double sr, int /*maxBlock*/) override {
        rate_ = sr;
        prepareSmooth(sr, 15.0f);
        lineL_.prepare((int)(kMaxDelayMs * 0.001 * sr) + 4);
        lineR_.prepare((int)(kMaxDelayMs * 0.001 * sr) + 4);
        lpL_.reset(); lpR_.reset();
        lpL_.setCutoff(sr, 12000.0f);
        lpR_.setCutoff(sr, 12000.0f);
        prevD_ = 0.0f;
        reset();
    }

    void reset() noexcept override {
        lineL_.reset(); lineR_.reset();
        lpL_.reset(); lpR_.reset();
    }

    void process(float* l, float* r, int n) noexcept override {
        // Effective delay time (free or synced), smoothed across block.
        const double targetFrames =
            ctx().synced ? ctx().divisionFrames(rate_)
                         : (double)target(0) * 0.001 * rate_;
        float d = prevD_ > 0.0f ? prevD_ : (float)targetFrames;
        const float dInc = ((float)targetFrames - d) / (float)n;

        const bool ping = target(3) > 0.5f; // switch param — no smoothing
        for (int i = 0; i < n; ++i) {
            const float fb = tick(1);
            // filter param: gentle per-sample tracking is overkill —
            // update the one-pole every sample but it's a cheap set.
            lpL_.setCutoff(rate_, value(2));
            lpR_.setCutoff(rate_, value(2));
            d += dInc;

            const float tapL = lineL_.read(d);
            const float tapR = lineR_.read(d);
            if (ping) {
                const float inM = 0.5f * (l[i] + r[i]);
                lineL_.write(inM + lpL_.tick(tapR) * fb);
                lineR_.write(lpR_.tick(tapL) * fb);
            } else {
                lineL_.write(l[i] + lpL_.tick(tapL) * fb);
                lineR_.write(r[i] + lpR_.tick(tapR) * fb);
            }
            // fully wet: echo replaces the signal (slot mixes dry back)
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
