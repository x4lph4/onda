#pragma once

#include <algorithm>
#include <cmath>

#include "DelayLine.h"
#include "FXProcessor.h"

namespace onda::fx {

/// Multi-stage phaser: N cascaded first-order allpass sections whose
/// corner frequency is swept by an LFO, with regenerative feedback.
///
/// Params:  0 Rate Hz (or synced) | 1 Depth | 2 Feedback
///          3 Stages (2/4/6/8) | 4 Center Hz
class PhaserFX : public FXBase<5> {
public:
    static constexpr int kMaxStages = 8;

    FXKind      kind() const noexcept override { return FXKind::Phaser; }
    const char* name() const noexcept override { return "Phaser"; }
    bool supportsSync() const noexcept override { return true; }

    FXParamInfo paramInfo(int i) const noexcept override {
        static constexpr FXParamInfo k[] = {
            { "Rate",     0.05f,  10.0f,   0.4f,   "Hz", 0.0f },
            { "Depth",    0.0f,   1.0f,    0.65f,  "",   0.0f },
            { "Feedback", 0.0f,   0.85f,   0.30f,  "",   0.0f },
            { "Stages",   2.0f,   8.0f,    6.0f,   "",   2.0f },
            { "Center",   200.0f, 6000.0f, 900.0f, "Hz", 0.0f },
        };
        return k[i % 5];
    }

    void prepare(double sr, int /*maxBlock*/) override {
        rate_ = sr;
        prepareSmooth(sr, 15.0f);
        lfo_.reset();
        reset();
    }

    void reset() noexcept override {
        for (float& s : st_) s = 0.0f;
        fbL_ = fbR_ = 0.0f;
    }

    void process(float* l, float* r, int n) noexcept override {
        if (ctx().synced) {
            const double frames = ctx().divisionFrames(rate_);
            lfo_.setInc(1.0 / (frames > 1.0 ? frames : 1.0));
        } else {
            lfo_.setRateHz(rate_, value(0));
        }

        for (int i = 0; i < n; ++i) {
            const float depth  = tick(1);
            const float fb     = tick(2);
            const int   stages = (int)(target(3) + 0.5f); // switch param
            const float center = tick(4);
            // LFO sweeps the allpass corners across +-3 octaves at depth 1
            const float mod = lfo_.tick();
            const float f = clampF(center * std::pow(2.0f, 3.0f * depth * mod));
            const float t = std::tan(3.14159265358979f * f / (float)rate_);
            const float a = (1.0f - t) / (1.0f + t);

            l[i] = chain(l[i] + fbL_ * fb, a, stages, 0);
            r[i] = chain(r[i] + fbR_ * fb, a, stages, kMaxStages);
            fbL_ = l[i];
            fbR_ = r[i];
        }
    }

private:
    /// First-order allpass cascade. State layout: st_[stage0..7] = L,
    /// st_[8..15] = R. y = a*x + z; z = x - a*y (one register/stage).
    float chain(float in, float a, int stages, int off) noexcept {
        float x = in;
        for (int s = 0; s < stages && s < kMaxStages; ++s) {
            float& z = st_[(size_t)(off + s)];
            const float y = a * x + z;
            z = x - a * y;
            x = y;
        }
        return x;
    }

    float clampF(float f) const noexcept {
        const float ny = (float)rate_ * 0.45f;
        return f < 20.0f ? 20.0f : (f > ny ? ny : f);
    }

    double rate_ { 48000.0 };
    Lfo    lfo_;
    float  st_[2 * kMaxStages] {};
    float  fbL_ { 0 }, fbR_ { 0 };
};

} // namespace onda::fx
