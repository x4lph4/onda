#pragma once

#include <algorithm>
#include <cmath>

#include "../dsp/Biquad.h"
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
            numBands_ = 3;
            bands_[0] = { 250.0,  0.707, BandDef::LowShelf,  false };
            bands_[1] = { 1000.0, 0.8,   BandDef::Peak,      false };
            bands_[2] = { 4000.0, 0.707, BandDef::HighShelf, false };
            bands_[3] = { 300.0,  0.707, BandDef::Peak,      true  }; // spare
        } else {
            // Isolator-style voicing placeholder: same topology for now,
            // steeper slopes land with the FX phase work.
            numBands_ = 3;
            bands_[0] = { 200.0,  0.707, BandDef::LowShelf,  false };
            bands_[1] = { 1000.0, 1.0,   BandDef::Peak,      false };
            bands_[2] = { 6000.0, 0.707, BandDef::HighShelf, false };
            bands_[3] = { 300.0,  0.707, BandDef::Peak,      true  };
        }
        applyAll();
    }

    int numBands() const noexcept { return numBands_; }

    void setGainDb(int band, double db) noexcept {
        if (band < 0 || band >= numBands_) return;
        tgtDb_[band] = std::clamp(db, -30.0, 12.0);
    }
    double gainDb(int band) const noexcept {
        return band >= 0 && band < numBands_ ? tgtDb_[band] : 0.0;
    }
    void setFreq(int band, double hz) noexcept {
        if (band < 0 || band >= numBands_) return;
        bands_[band].freq = std::clamp(hz, 20.0, rate_ * 0.45);
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
    void tick() noexcept {
        for (int b = 0; b < numBands_; ++b) {
            const double tgt = tgtDb_[b];
            double& cur = curDb_[b];
            if (cur == tgt) continue;
            double step = tgt - cur;
            step = std::clamp(step, -3.0, 3.0); // <= 3 dB per block
            cur += step;
            applyBand(b);
        }
    }

    void process(float* l, float* r, int n) noexcept {
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
    EqMode  mode_ { EqMode::Standard };
    int     numBands_ { 3 };
    BandDef bands_[kMaxBands] {};
    double  curDb_[kMaxBands] { 0.0, 0.0, 0.0, 0.0 };
    double  tgtDb_[kMaxBands] { 0.0, 0.0, 0.0, 0.0 };
    Biquad  l_[kMaxBands], r_[kMaxBands];
};

} // namespace onda::mixer
