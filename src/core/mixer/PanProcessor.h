#pragma once

#include <algorithm>

#include "GainSmoother.h"

namespace onda::mixer {

/// §22 — stereo channel pan. This is a *balance* law, not a
/// mono-to-stereo constant-power law: the channel carries a stereo
/// program, so center must stay at unity gain (pan = 0 must not
/// change the mix). Gains: gL = 1 - max(0,p), gR = 1 + min(0,p);
/// full left = {1,0}, full right = {0,1} — predictable, and the
/// attenuated side reaches silence instead of a -3 dB shelf.
///
/// The position is smoothed per-sample (~6 ms) and resolved once per
/// block, so sweeps don't zip and coefficient refresh stays cheap.
class PanProcessor {
public:
    void prepare(double sampleRate, float smoothMs = 6.0f) noexcept {
        smPos_.prepare(sampleRate, smoothMs);
        smPos_.snap(pos_);
    }

    void setPosition(float p) noexcept {
        smPos_.setTarget(std::clamp(p, -1.0f, 1.0f));
    }
    float position() const noexcept { return smPos_.target(); }

    /// RT: advance the smoother across a block and resolve the stereo
    /// gains applied to the post-fader signal.
    void tick(int n, float& gL, float& gR) noexcept {
        for (int i = 0; i < n; ++i) pos_ = smPos_.tick();
        gL = pos_ <= 0.0f ? 1.0f : 1.0f - pos_;
        gR = pos_ >= 0.0f ? 1.0f : 1.0f + pos_;
    }

private:
    float pos_ { 0.0f };
    GainSmoother smPos_;
};

} // namespace onda::mixer
