#pragma once

#include "../model/MasterStats.h"
#include "GainSmoother.h"
#include "LimiterProcessor.h"
#include "MeteringEngine.h"

namespace onda::mixer {

/// Stereo master bus. Per block:
///   smoothed master gain (+ mute) -> limiter (if enabled) -> meters ->
///   caller writes to device outputs.
/// Metering and clip detection run *after* limiting, so a hot mix shows
/// up in `clips`/`clip` even when the limiter is catching the peaks.
class MasterBus {
public:
    void prepare(double sampleRate) noexcept {
        rate_ = sampleRate;
        smGain_.prepare(sampleRate, 8.0f);
        smGain_.snap(gain_);
        smMute_.prepare(sampleRate, 2.0f);
        smMute_.snap(1.0f);
        limiter_.prepare(sampleRate);
        meters_.prepare(sampleRate);
    }

    void setGain(float g) noexcept    { smGain_.setTarget(g); }
    float gain() const noexcept       { return smGain_.target(); }
    void setMute(bool m) noexcept     { smMute_.setTarget(m ? 0.0f : 1.0f); }
    LimiterProcessor& limiter() noexcept { return limiter_; }
    const LimiterProcessor& limiter() const noexcept { return limiter_; }
    const ChannelMeters& meters() const noexcept { return mstats_; }

    /// Process the summed program in place and publish meters.
    void process(float* l, float* r, int n) noexcept {
        for (int i = 0; i < n; ++i) {
            const float g = smGain_.tick() * smMute_.tick();
            l[i] *= g; r[i] *= g;
        }
        limiter_.process(l, r, n);
        meters_.update(l, r, n, mstats_);
        mstats_.limiterGain.store(limiter_.gainReduction(),
                                  std::memory_order_relaxed);
    }

private:
    double rate_ { 48000.0 };
    float  gain_ { 1.0f };
    GainSmoother smGain_, smMute_;
    LimiterProcessor limiter_;
    MeteringEngine meters_;
    ChannelMeters  mstats_;
};

} // namespace onda::mixer
