#pragma once

#include <algorithm>
#include <cmath>

#include "DelayLine.h"
#include "FXProcessor.h"

namespace onda::fx {

/// Tempo-synced amplitude gate — the foundation the stutter / beat-
/// repeat family builds on. One gate cycle per selected division:
/// the envelope is open for `duty` of the cycle and closed for the
/// rest, with a shaped ramp to stay clickless.
///
/// Params:  0 Depth (floor gain = 1-depth) | 1 Shape ms (ramp)
///          2 Duty (open fraction) | 3 StereoAlt (0/1, L/R offset)
class GateFX : public FXBase<4> {
public:
    FXKind      kind() const noexcept override { return FXKind::Gate; }
    const char* name() const noexcept override { return "Gate"; }
    bool supportsSync() const noexcept override { return true; }

    FXParamInfo paramInfo(int i) const noexcept override {
        static constexpr FXParamInfo k[] = {
            { "Depth",     0.0f, 1.0f, 1.0f,  "",   0.0f },
            { "Shape",     0.5f, 50.0f, 4.0f, "ms", 0.0f },
            { "Duty",      0.1f, 0.9f, 0.5f,  "",   0.0f },
            { "StereoAlt", 0.0f, 1.0f, 0.0f,  "",   1.0f },
        };
        return k[i & 3];
    }

    void prepare(double sr, int /*maxBlock*/) override {
        rate_ = sr;
        prepareSmooth(sr, 5.0f);
        phL_ = phR_ = 0.0;
    }

    void reset() noexcept override { phL_ = 0.0; phR_ = 0.0; }

    void process(float* l, float* r, int n) noexcept override {
        const double frames = ctx().divisionFrames(rate_);
        const double inc = 1.0 / (frames > 1.0 ? frames : 1.0);
        const bool alt = value(3) > 0.5f;

        for (int i = 0; i < n; ++i) {
            const float depth = tick(0);
            const float shape = tick(1) * 0.001f; // seconds
            const float duty  = tick(2);
            phL_ += inc;
            if (phL_ >= 1.0) phL_ -= 1.0;
            const double phR = alt ? (phL_ < 0.5 ? phL_ + 0.5 : phL_ - 0.5)
                                   : phL_;
            // ramp length expressed as a fraction of the cycle
            const double rampCyc = std::min(0.5 * duty,
                                    (double)shape * rate_ / frames);
            const float floor = 1.0f - depth;
            l[i] *= env(phL_, duty, rampCyc) * depth + floor;
            r[i] *= env(phR,  duty, rampCyc) * depth + floor;
        }
    }

private:
    /// Trapezoid gate envelope: 1 inside the open region, ramping up
    /// over `rampCyc` at the open edge and down at the close edge.
    static float env(double pos, float duty, double rampCyc) noexcept {
        if (rampCyc < 1e-6) rampCyc = 1e-6;
        const double dFall = pos - duty;
        if (dFall >= 0.0) return 0.0f;
        const double up   = pos / rampCyc;      // fade-in after open
        const double down = -dFall / rampCyc;   // fade-out before close
        const float g = (float)std::min(std::min(up, down), 1.0);
        return g < 0.0f ? 0.0f : g;
    }

    double rate_ { 48000.0 };
    double phL_ { 0 }, phR_ { 0 };
};

} // namespace onda::fx
