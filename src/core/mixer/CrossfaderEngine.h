#pragma once

#include <algorithm>
#include <functional>

#include "../dsp/CrossfaderCurves.h"
#include "GainSmoother.h"
#include "MixerTypes.h"

namespace onda::mixer {

/// Crossfader position + curve + per-channel assignment gain.
/// Position is smoothed (~5 ms) so quick cuts stay clean but knob
/// movements don't zip. `gainFor(assign)` resolves each channel's
/// effective crossfader gain; Thru channels ignore the fader entirely.
class CrossfaderEngine {
public:
    void prepare(double sampleRate) noexcept {
        smPos_.prepare(sampleRate, 5.0f);
        smPos_.snap(pos_);
    }
    void setPosition(float x) noexcept {
        smPos_.setTarget(std::clamp(x, -1.0f, 1.0f));
    }
    float position() const noexcept { return smPos_.target(); }
    void setCurve(XfCurve c) noexcept { curve_ = c; }
    XfCurve curve() const noexcept { return curve_; }

    /// Optional user-defined curve: f(x in -1..1, sideA) -> gain 0..1.
    /// Set from non-RT threads only (std::function, no RT allocation
    /// when merely *called*).
    void setCustomCurve(std::function<float(float, bool)> f) noexcept {
        custom_ = std::move(f);
        if (curve_ == XfCurve::Custom && !custom_)
            curve_ = XfCurve::EqualPower;
    }

    /// Advance smoothing across a render block (per-sample smoother).
    float tickPos(int n) noexcept {
        for (int i = 0; i < n; ++i) pos_ = smPos_.tick();
        return pos_;
    }

    float gainFor(XfAssign a) const noexcept {
        switch (a) {
            case XfAssign::Thru: return 1.0f;
            case XfAssign::A:    return sideGain(true);
            case XfAssign::B:    return sideGain(false);
        }
        return 1.0f;
    }

private:
    float sideGain(bool a) const noexcept {
        if (curve_ == XfCurve::Custom && custom_)
            return std::clamp(custom_(pos_, a), 0.0f, 1.0f);
        return xfGain(curve_ == XfCurve::Custom ? XfCurve::EqualPower : curve_,
                      pos_, a);
    }

    XfCurve curve_ { XfCurve::EqualPower };
    float   pos_   { 0.0f };
    GainSmoother smPos_;
    std::function<float(float, bool)> custom_;
};

} // namespace onda::mixer
