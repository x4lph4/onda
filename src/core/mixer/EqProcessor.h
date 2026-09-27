#pragma once

#include <algorithm>
#include <cmath>

#include "../dsp/Biquad.h"
#include "IsolatorProcessor.h"
#include "MixerTypes.h"

namespace onda::mixer {

/// Multi-band channel EQ, decoupled from ChannelStrip. Up to kMaxBands
/// cascaded biquads (stereo each); the default configuration is the
/// standard 3-band DJ EQ (low shelf / mid peak / high shelf). Band gains
/// ramp toward their target in per-block steps so knob sweeps don't zip.
/// Per-band frequency/Q/bypass are configurable; alternate voicings
/// (isolator-style steeper slopes etc.) can be added by changing band
/// shapes without touching ChannelStrip.
class EqProcessor {
public:
    static constexpr int kMaxBands = 4;

    struct BandDef {
        double freq { 250.0 };
        double q    { 0.707 };
        enum Shape : uint8_t { LowShelf, Peak, HighShelf } shape { Peak };
        bool  bypass { false };
    };

    void prepare(double sampleRate) noexcept {
        rate_ = sampleRate;
        iso_.prepare(sampleRate);
        setMode(EqMode::Standard);
        for (int b = 0; b < kMaxBands; ++b) { curDb_[b] = 0.0; tgtDb_[b] = 0.0; }
        applyAll();
        reset();
    }

    void reset() noexcept {
        for (int b = 0; b < kMaxBands; ++b) { l_[b].reset(); r_[b].reset(); }
    }

    void setMode(EqMode m) noexcept {
        mode_ = m;
        if (m == EqMode::Standard) {
            loDb_ = -30.0; hiDb_ = 12.0;
            numBands_ = 3;
            bands_[0] = { 250.0,  0.707, BandDef::LowShelf,  false };
            bands_[1] = { 1000.0, 0.8,   BandDef::Peak,      false };
            bands_[2] = { 4000.0, 0.707, BandDef::HighShelf, false };
            bands_[3] = { 300.0,  0.707, BandDef::Peak,      true  }; // spare
        } else {
            // §12 — isolator mode delegates to IsolatorProcessor's
            // crossover split; the shelf bands stay configured (metadata
            // only) and the applied range deepens so a band at its
            // detent reaches ~silence, not just -26 dB.
            numBands_ = 3;
            loDb_ = IsolatorProcessor::kRangeLo;
            hiDb_ = IsolatorProcessor::kRangeHi;
            bands_[0] = { IsolatorProcessor::kLowFc,  0.707, BandDef::LowShelf,  false };
            bands_[1] = { 1000.0,                     1.0,   BandDef::Peak,      false };
            bands_[2] = { IsolatorProcessor::kHighFc, 0.707, BandDef::HighShelf, false };
            bands_[3] = { 300.0,                      0.707, BandDef::Peak,      true  };
        }
        applyAll();
    }

    EqMode mode() const noexcept { return mode_; }

    int numBands() const noexcept { return numBands_; }

    /// §9 — EQ range is configurable (isolator presets go deeper than
    /// the standard -26/+6 registry range, e.g. full-kill bands).
    void setRangeDb(double lo, double hi) noexcept {
        loDb_ = std::min(lo, hi);
        hiDb_ = std::max(lo, hi);
        for (int b = 0; b < numBands_; ++b)
            tgtDb_[b] = std::clamp(tgtDb_[b], loDb_, hiDb_);
    }
    double rangeLo() const noexcept { return loDb_; }
    double rangeHi() const noexcept { return hiDb_; }

    void setGainDb(int band, double db) noexcept {
        if (band < 0 || band >= numBands_) return;
        tgtDb_[band] = std::clamp(db, loDb_, hiDb_);
    }
    double gainDb(int band) const noexcept {
        return band >= 0 && band < numBands_ ? tgtDb_[band] : 0.0;
    }
    void setFreq(int band, double hz) noexcept {
        if (band < 0 || band >= numBands_) return;
        bands_[band].freq = std::clamp(hz, 20.0, rate_ * 0.45);
        if (mode_ == EqMode::Isolator) {
            // crossover retune: band 0 owns the low edge, band 2 the
            // high edge; mid is the residue and has no fc of its own.
            if (band == 0)      iso_.setLowFc(bands_[band].freq);
            else if (band == 2) iso_.setHighFc(bands_[band].freq);
            return;
        }
        applyBand(band);
    }
    void setQ(int band, double q) noexcept {
        if (band < 0 || band >= numBands_) return;
        bands_[band].q = std::clamp(q, 0.1, 8.0);
        applyBand(band);
    }
    void setBypass(int band, bool b) noexcept {
        if (band >= 0 && band < numBands_) bands_[band].bypass = b;
    }

    /// Advance band gains toward their targets and refresh changed
    /// coefficients. Call once per render block before process().
    /// Isolator mode routes the target through IsolatorProcessor's kill
    /// semantics (detent → ~silence) and uses a faster ramp so a kill
    /// lands in ~80 ms instead of crawling to it; its coefficients are
    /// static — only the linear band gains move.
    void tick() noexcept {
        if (mode_ == EqMode::Isolator) {
            // ramp cur toward the kill-aware target, then convert to a
            // linear band gain; IsolatorProcessor smooths per-sample.
            for (int b = 0; b < numBands_; ++b) {
                const double tgt = bands_[b].bypass
                                 ? 0.0
                                 : IsolatorProcessor::effectiveDb(tgtDb_[b]);
                double& cur = curDb_[b];
                if (cur != tgt) {
                    cur += std::clamp(tgt - cur, -6.0, 6.0);
                }
                iso_.setBandGain(b, (float)std::pow(10.0, cur / 20.0));
            }
            return;
        }
        for (int b = 0; b < numBands_; ++b) {
            double& cur = curDb_[b];
            if (cur == tgtDb_[b]) continue;
            cur += std::clamp(tgtDb_[b] - cur, -3.0, 3.0);
            applyBand(b);
        }
    }

    void process(float* l, float* r, int n) noexcept {
        if (mode_ == EqMode::Isolator) {
            iso_.process(l, r, n);
            return;
        }
        for (int i = 0; i < n; ++i) {
            float xl = l[i], xr = r[i];
            for (int b = 0; b < numBands_; ++b) {
                if (bands_[b].bypass) continue;
                xl = l_[b].process(xl);
                xr = r_[b].process(xr);
            }
            l[i] = xl; r[i] = xr;
        }
    }

private:
    void applyAll() noexcept { for (int b = 0; b < kMaxBands; ++b) applyBand(b); }

    void applyBand(int b) noexcept {
        const auto& d = bands_[b];
        switch (d.shape) {
            case BandDef::LowShelf:
                l_[b].setLowShelf(rate_, d.freq, curDb_[b]);
                r_[b].setLowShelf(rate_, d.freq, curDb_[b]);
                break;
            case BandDef::Peak:
                l_[b].setPeaking(rate_, d.freq, curDb_[b], d.q);
                r_[b].setPeaking(rate_, d.freq, curDb_[b], d.q);
                break;
            case BandDef::HighShelf:
                l_[b].setHighShelf(rate_, d.freq, curDb_[b]);
                r_[b].setHighShelf(rate_, d.freq, curDb_[b]);
                break;
        }
    }

    double  rate_ { 48000.0 };
    double  loDb_ { -30.0 }, hiDb_ { 12.0 };
    EqMode  mode_ { EqMode::Standard };
    int     numBands_ { 3 };
    BandDef bands_[kMaxBands] {};
    double  curDb_[kMaxBands] { 0.0, 0.0, 0.0, 0.0 };
    double  tgtDb_[kMaxBands] { 0.0, 0.0, 0.0, 0.0 };
    Biquad  l_[kMaxBands], r_[kMaxBands];
    IsolatorProcessor iso_;   // §12 — active when mode_ == Isolator
};

} // namespace onda::mixer
