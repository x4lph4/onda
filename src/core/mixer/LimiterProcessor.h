#pragma once

#include "../dsp/PeakLimiter.h"
#include "MeteringEngine.h"

namespace onda::mixer {

/// Master-bus limiter facade. Wraps the zero-lookahead PeakLimiter with
/// configurable threshold/release and an enable flag. A lookahead
/// limiter can replace the internals later without touching MasterBus.
/// IMPORTANT: the limiter is a safety net, not a gain-staging fix —
/// clipping is still detected and reported even when limiting is on.
class LimiterProcessor {
public:
    void prepare(double sampleRate) noexcept {
        lim_.prepare(sampleRate);
        lim_.setThreshold(threshold_);
    }
    void reset() noexcept { lim_.reset(); }

    void setEnabled(bool on) noexcept      { enabled_ = on; if (!on) lim_.reset(); }
    bool enabled() const noexcept          { return enabled_; }
    void setThreshold(float t) noexcept    { threshold_ = t; lim_.setThreshold(t); }
    float threshold() const noexcept       { return threshold_; }
    float gainReduction() const noexcept   { return lim_.lastGainReduction(); }

    void process(float* l, float* r, int n) noexcept {
        if (enabled_) lim_.process(l, r, n);
        else lim_.reset();
    }

private:
    PeakLimiter lim_;
    float threshold_ { 0.891f }; // ~ -1 dBFS
    bool  enabled_   { true };
};

} // namespace onda::mixer
