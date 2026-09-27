#pragma once

#include "GainSmoother.h"
#include "MeteringEngine.h"

namespace onda::mixer {

/// §33 — booth monitor bus. Taps the master program AFTER the channel
/// sum and master insert FX but BEFORE master gain/mute/limiter, so
/// the booth level is fully independent of the house level — the DJ's
/// booth knob can never change what the audience hears, and muting the
/// house doesn't blind the booth.
///
///   program (pre-master-gain)
///     -> booth gain (smoothed) * mute (smoothed)
///     -> meters (peak/RMS/clip)
///     -> mapped physical outputs
///
/// RT-only: called from MixerEngine::render, never blocks or allocs.
class BoothBus {
public:
    void prepare(double sampleRate) noexcept {
        smGain_.prepare(sampleRate, 8.0f);
        smGain_.snap(gain_);
        smMute_.prepare(sampleRate, 4.0f);
        smMute_.snap(1.0f);
        metersEngine_.prepare(sampleRate);
    }

    void setGain(float g) noexcept {
        gain_ = g < 0.0f ? 0.0f : (g > 2.0f ? 2.0f : g);
        smGain_.setTarget(gain_);
    }
    float gain() const noexcept { return gain_; }
    void setMute(bool m) noexcept { smMute_.setTarget(m ? 0.0f : 1.0f); }

    /// RT: copy the program into the booth output pair scaled by the
    /// smoothed booth gain; writes meters. in may equal out (in-place
    /// safe — each sample is read once before being overwritten).
    void process(const float* inL, const float* inR,
                 float* outL, float* outR, int n) noexcept {
        for (int i = 0; i < n; ++i) {
            const float g = smGain_.tick() * smMute_.tick();
            outL[i] = inL[i] * g;
            outR[i] = inR[i] * g;
        }
        metersEngine_.update(outL, outR, n, meters_);
    }

    /// Advance the smoothers without producing output (booth outs
    /// unmapped this block) so meters/positions stay sane.
    void idle(int n) noexcept {
        for (int i = 0; i < n; ++i) { smGain_.tick(); smMute_.tick(); }
    }

    const ChannelMeters& meters() const noexcept { return meters_; }

private:
    float gain_ { 1.0f };
    GainSmoother smGain_, smMute_;
    MeteringEngine metersEngine_;
    ChannelMeters  meters_;
};

} // namespace onda::mixer
