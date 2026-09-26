#pragma once

#include <algorithm>
#include <cmath>

#include "../dsp/Biquad.h"
#include "GainSmoother.h"
#include "MixerTypes.h"

namespace onda::mixer {

/// Channel filter, decoupled from ChannelStrip. Two ways to drive it:
///   - Position mode (FilterType::Position): classic single-knob DJ
///     filter, pos -1..+1 (negative = LP sweep 20k->200 Hz, positive =
///     HP sweep 20 Hz->8 kHz, 0 = bypass).
///   - Typed mode (LowPass/HighPass): explicit setCutoffHz for later
///     hardware/UIs that expose the filter differently.
/// The position/cutoff is smoothed per-sample to avoid zipper noise.
class FilterProcessor {
public:
    void prepare(double sampleRate) noexcept {
        rate_ = sampleRate;
        smPos_.prepare(sampleRate, 5.0f);
        smPos_.snap(pos_);
        reset();
    }
    void reset() noexcept { lpL_.reset(); lpR_.reset(); hpL_.reset(); hpR_.reset(); }

    void setType(FilterType t) noexcept { type_ = t; update(); }
    FilterType type() const noexcept { return type_; }

    void setPosition(float p) noexcept {
        smPos_.setTarget(std::clamp(p, -1.0f, 1.0f));
    }
    float position() const noexcept { return smPos_.target(); }

    void setCutoffHz(double hz) noexcept {
        cutoff_ = std::clamp(hz, 20.0, rate_ * 0.45);
        update();
    }
    double cutoffHz() const noexcept { return cutoff_; }

    void process(float* l, float* r, int n) noexcept {
        // advance the smoothed position and refresh coefficients when
        // it moved enough to matter (~0.1 % of travel)
        const float prev = pos_;
        for (int i = 0; i < n; ++i) smPos_.tick();
        pos_ = smPos_.current();
        if (type_ == FilterType::Position && std::fabs(pos_ - prev) > 1e-4f)
            update();

        const int mode = type_ == FilterType::LowPass  ? -1
                       : type_ == FilterType::HighPass ? 1
                       : pos_ < 0.0f ? -1 : (pos_ > 0.0f ? 1 : 0);
        if (mode == 0) return;
        if (mode < 0)
            for (int i = 0; i < n; ++i) { l[i] = lpL_.process(l[i]); r[i] = lpR_.process(r[i]); }
        else
            for (int i = 0; i < n; ++i) { l[i] = hpL_.process(l[i]); r[i] = hpR_.process(r[i]); }
    }

private:
    void update() noexcept {
        switch (type_) {
            case FilterType::LowPass:
                lpL_.setLowpass(rate_, cutoff_, 0.707);
                lpR_.setLowpass(rate_, cutoff_, 0.707);
                break;
            case FilterType::HighPass:
                hpL_.setHighpass(rate_, cutoff_, 0.707);
                hpR_.setHighpass(rate_, cutoff_, 0.707);
                break;
            case FilterType::Position: {
                const float p = pos_;
                if (p < 0.0f) {
                    const double f = 20000.0 * std::pow(200.0 / 20000.0, (double)-p);
                    lpL_.setLowpass(rate_, f, 0.707);
                    lpR_.setLowpass(rate_, f, 0.707);
                } else if (p > 0.0f) {
                    const double f = 20.0 * std::pow(8000.0 / 20.0, (double)p);
                    hpL_.setHighpass(rate_, f, 0.707);
                    hpR_.setHighpass(rate_, f, 0.707);
                }
                break;
            }
        }
    }

    double rate_  { 48000.0 };
    FilterType type_ { FilterType::Position };
    float  pos_   { 0.0f };
    double cutoff_{ 1000.0 };
    GainSmoother smPos_;
    Biquad lpL_, lpR_, hpL_, hpR_;
};

} // namespace onda::mixer
