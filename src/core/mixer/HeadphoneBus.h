#pragma once

#include "GainSmoother.h"
#include "MeteringEngine.h"

namespace onda::mixer {

/// Dedicated headphone/cue bus, fully separate from the master bus.
///   phones = (cueSum * (1 - mix) + masterPost * mix) * cueVol
/// where masterPost is the post-master-gain program. The cue path never
/// writes back into the master sum, so PFL cannot contaminate the mains.
/// Per block it also meters itself so the UI can show cue level.
class HeadphoneBus {
public:
    void prepare(double sampleRate) noexcept {
        smVol_.prepare(sampleRate, 8.0f);
        smVol_.snap(vol_);
        smMix_.prepare(sampleRate, 8.0f);
        smMix_.snap(mix_);
        smMono_.prepare(sampleRate, 8.0f);
        smMono_.snap(0.0f);
        meters_.prepare(sampleRate);
    }

    void setVolume(float v) noexcept { smVol_.setTarget(v); }
    float volume() const noexcept    { return smVol_.target(); }
    void setMix(float m) noexcept    { smMix_.setTarget(m); }
    float mix() const noexcept       { return smMix_.target(); }
    /// §24 — cue mono/stereo. Smoothed so the fold-down never clicks;
    /// mono sums L+R at -6 dB into both legs.
    void setMono(bool on) noexcept { smMono_.setTarget(on ? 1.0f : 0.0f); }
    bool mono() const noexcept     { return smMono_.target() > 0.5f; }
    const ChannelMeters& meters() const noexcept { return mstats_; }

    /// cueL/R = summed pre-fader channel taps, masterL/R = post-gain
    /// master program. Writes `outL/outR`.
    void process(const float* cueL, const float* cueR,
                 const float* masterL, const float* masterR,
                 float* outL, float* outR, int n) noexcept {
        for (int i = 0; i < n; ++i) {
            const float m  = smMix_.tick();
            const float v  = smVol_.tick();
            const float mo = smMono_.tick();
            outL[i] = (cueL[i] * (1.0f - m) + masterL[i] * m) * v;
            outR[i] = (cueR[i] * (1.0f - m) + masterR[i] * m) * v;
            if (mo > 1e-6f) {
                const float mm = (outL[i] + outR[i]) * 0.5f;
                outL[i] += (mm - outL[i]) * mo;
                outR[i] += (mm - outR[i]) * mo;
            }
        }
        meters_.update(outL, outR, n, mstats_);
    }

private:
    float  vol_ { 0.8f };
    float  mix_ { 0.5f };
    GainSmoother smVol_, smMix_, smMono_;
    MeteringEngine meters_;
    ChannelMeters  mstats_;
};

} // namespace onda::mixer
