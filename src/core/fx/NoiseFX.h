#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "DelayLine.h"
#include "FXProcessor.h"

namespace onda::fx {

/// Performance noise generator (§25): white noise shaped by a resonant
/// sweep filter — the classic riser/downer layer behind builds and
/// transitions. Fully wet on the output; dry/wet lives in FXSlot.
///
/// Params:  0 Color (sweep center Hz) | 1 Sweep (+-6 oct LFO, synced
///          to the division when the slot is synced) | 2 Resonance
///
/// The generator is an xorshift32 — deterministic, allocation-free and
/// bounded by construction; the SVF clamps gain so the wet output can
/// never exceed +-1.2.
class NoiseFX : public FXBase<3> {
public:
    FXKind      kind() const noexcept override { return FXKind::Noise; }
    const char* name() const noexcept override { return "Noise"; }
    bool supportsSync() const noexcept override { return true; }

    FXParamInfo paramInfo(int i) const noexcept override {
        static constexpr FXParamInfo k[] = {
            { "Color",     100.0f, 18000.0f, 2000.0f, "Hz", 0.0f, FXParamType::Float, FXParamCurve::Log },
            { "Sweep",     0.0f,   1.0f,     0.0f,    "",   0.0f },
            { "Resonance", 0.0f,   1.0f,     0.25f,   "",   0.0f },
        };
        return k[i % 3];
    }

    void prepare(double sr, int /*maxBlock*/) override {
        rate_ = sr;
        prepareSmooth(sr, 12.0f);
        lfoL_.reset(); lfoR_.reset(0.5f);
        reset();
    }

    void reset() noexcept override {
        ic1L_ = ic2L_ = ic1R_ = ic2R_ = 0.0f;
        rngL_ = 0x9E3779B9u; rngR_ = 0x85A3083Bu;
    }

    void process(float* l, float* r, int n) noexcept override {
        const float invSr = 1.0f / (float)rate_;
        if (ctx().synced) {
            const double frames = ctx().divisionFrames(rate_);
            lfoL_.setInc(1.0 / (frames > 1.0 ? frames : 1.0));
            lfoR_.setInc(lfoL_.inc);
        } else {
            lfoL_.setRateHz(rate_, 0.25f); // slow free sweep
            lfoR_.setRateHz(rate_, 0.25f);
        }
        for (int i = 0; i < n; ++i) {
            const float base  = tick(0);
            const float depth = tick(1) * 6.0f; // +-6 octaves
            const float q     = 0.6f + tick(2) * 7.4f;
            const float fL = clampF(base * std::pow(2.0f, depth * lfoL_.tick()));
            const float fR = clampF(base * std::pow(2.0f, depth * lfoR_.tick()));
            l[i] = svf(noise(rngL_), fL, q, ic1L_, ic2L_, invSr);
            r[i] = svf(noise(rngR_), fR, q, ic1R_, ic2R_, invSr);
        }
    }

private:
    static float noise(uint32_t& s) noexcept {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return ((float)(int)s) * (1.0f / 2147483648.0f);
    }
    float clampF(float f) const noexcept {
        const float ny = (float)rate_ * 0.45f;
        return f < 40.0f ? 40.0f : (f > ny ? ny : f);
    }
    /// TPT SVF bandpass with output clamped to +-1.2.
    static float svf(float in, float f, float q,
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
        float out = v1 * 0.9f;
        return out > 1.2f ? 1.2f : (out < -1.2f ? -1.2f : out);
    }

    double   rate_ { 48000.0 };
    Lfo      lfoL_, lfoR_;
    float    ic1L_ { 0 }, ic2L_ { 0 }, ic1R_ { 0 }, ic2R_ { 0 };
    uint32_t rngL_ { 0x9E3779B9u }, rngR_ { 0x85A3083Bu };
};

} // namespace onda::fx
