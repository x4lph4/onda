#pragma once

#include "GainSmoother.h"

namespace onda::mixer {

/// §17-20 — master-cue tap and cue state coordination.
///
/// Per-channel PFL lives on the strips (they tap cueL/cueR pre-fader);
/// HeadphoneBus owns vol/mix. What was missing is MASTER cue: the DJ
/// must be able to audition the program bus on headphones without
/// turning the cue-mix knob. CueManager adds a smoothed 0/1 tap that
/// injects the post-master signal into the cue sum:
///
///   cueBus = sum(PFL strips) + masterCueGain * masterPost
///   phones = (cueBus * (1 - mix) + masterPost * mix) * cueVol
///
/// With master cue ON and mix = 0 the phones carry the full program —
/// an independent monitor path that never feeds back into the mains.
class CueManager {
public:
    void prepare(double sampleRate) noexcept {
        smGain_.prepare(sampleRate, 6.0f);
        smGain_.snap(0.0f);
    }

    /// 0/1 (registry quantizes). Smoothed so toggling never clicks.
    void setMasterCue(bool on) noexcept { smGain_.setTarget(on ? 1.0f : 0.0f); }
    bool masterCue() const noexcept { return smGain_.target() > 0.5f; }

    /// RT: blend `master` into the cue sum. Runs after MasterBus so the
    /// cue path hears post-gain/post-limiter program (what the audience
    /// hears), same as the dedicated cue master on club mixers.
    void process(float* cueL, float* cueR,
                 const float* masterL, const float* masterR,
                 int n) noexcept {
        for (int i = 0; i < n; ++i) {
            const float g = smGain_.tick();
            if (g > 1e-6f) { cueL[i] += masterL[i] * g; cueR[i] += masterR[i] * g; }
        }
    }

private:
    GainSmoother smGain_;
};

} // namespace onda::mixer
