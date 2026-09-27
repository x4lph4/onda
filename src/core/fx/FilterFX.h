#pragma once

#include <algorithm>
#include <cmath>

#include "DelayLine.h"
#include "FXProcessor.h"

namespace onda::fx {

/// Resonant multi-mode filter (TPT state-variable filter, Zavalishin
/// topology — stable at any cutoff/resonance, no blowups) with an
/// optional LFO on the cutoff.
///
/// Params:  0 Cutoff Hz | 1 Resonance | 2 Mode (0 LP / 1 BP / 2 HP)
///          3 LfoDepth (0..1 -> +-4 oct) | 4 LfoRate Hz (or synced div)
class FilterFX : public FXBase<5> {
public:
    FXKind      kind() const noexcept override { return FXKind::Filter; }
    const char* name() const noexcept override { return "Filter"; }
    bool supportsSync() const noexcept override { return true; }

    FXParamInfo paramInfo(int i) const noexcept override {
        static constexpr FXParamInfo k[] = {
            { "Cutoff",    20.0f, 20000.0f, 1200.0f, "Hz", 0.0f, FXParamType::Float, FXParamCurve::Log },
            { "Resonance", 0.0f,  1.0f,     0.30f,   "",   0.0f },
            { "Mode",      0.0f,  2.0f,     0.0f,    "",   1.0f },
            { "LfoDepth",  0.0f,  1.0f,     0.0f,    "",   0.0f },
            { "LfoRate",   0.05f, 10.0f,    0.5f,    "Hz", 0.0f, FXParamType::Float, FXParamCurve::Log },
        };
        return k[i % 5];
    }

    void prepare(double sr, int /*maxBlock*/) override {
        rate_ = sr;
        prepareSmooth(sr, 8.0f);
        lfoL_.reset(); lfoR_.reset(0.25);
        reset();
    }

    void reset() noexcept override { ic1L_ = ic2L_ = ic1R_ = ic2R_ = 0.0f; }

    void process(float* l, float* r, int n) noexcept override {
        const float invSr = 1.0f / (float)rate_;
        const int mode = (int)(target(2) + 0.5f); // switch — no smoothing
        // Synced LFO: one LFO cycle per selected division.
        if (ctx().synced) {
            const double frames = ctx().divisionFrames(rate_);
            lfoL_.setInc(1.0 / (frames > 1.0 ? frames : 1.0));
            lfoR_.setInc(lfoL_.inc);
        } else {
            lfoL_.setRateHz(rate_, value(4));
            lfoR_.setRateHz(rate_, value(4));
        }

        for (int i = 0; i < n; ++i) {
            const float depth = tick(3) * 4.0f; // +-4 octaves
            const float base  = tick(0);
            const float q     = 0.5f + tick(1) * 11.5f; // Q 0.5..12
            const float fL = clampF(base * std::pow(2.0f, depth * lfoL_.tick()));
            const float fR = clampF(base * std::pow(2.0f, depth * lfoR_.tick()));
            l[i] = svf(l[i], fL, q, mode, ic1L_, ic2L_, invSr);
            r[i] = svf(r[i], fR, q, mode, ic1R_, ic2R_, invSr);
        }
    }

private:
    float clampF(float f) const noexcept {
        const float ny = (float)rate_ * 0.45f;
        return f < 20.0f ? 20.0f : (f > ny ? ny : f);
    }

    /// One TPT SVF section. mode: 0 LP, 1 BP, 2 HP.
    static float svf(float in, float f, float q, int mode,
                     float& ic1, float& ic2, float invSr) noexcept {
        const float g  = std::tan(3.14159265358979f * f * invSr);
        const float k  = 1.0f / q;
        const float a1 = 1.0f / (1.0f + g * (g + k));
        const float a2 = g * a1;
        const float a3 = g * a2;
        const float v3 = in - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        switch (mode) {
            case 0:  return v2;                 // LP
            case 1:  return v1;                 // BP
            default: return in - k * v1 - v2;   // HP
        }
    }

    double rate_ { 48000.0 };
    Lfo    lfoL_, lfoR_;
    float  ic1L_ { 0 }, ic2L_ { 0 }, ic1R_ { 0 }, ic2R_ { 0 };
};

} // namespace onda::fx
