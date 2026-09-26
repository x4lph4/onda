#pragma once

#include "Biquad.h"

namespace onda {

/// DJ 3-band EQ: low shelf / mid peak / high shelf.
/// Band gains in dB (typically -26..+6). Coefficients are recomputed only
/// when a band value actually changes — cheap enough to run on RT, no
/// transcendentals in the steady state.
class ThreeBandEq {
public:
    static constexpr int kBands = 3;

    void prepare(double sampleRate) noexcept {
        rate_ = sampleRate;
        for (auto& b : bandDb_) b = 0.0;
        applyCoefficients();
        reset();
    }

    void reset() noexcept {
        for (int i = 0; i < kBands; ++i) { l_[i].reset(); r_[i].reset(); }
    }

    /// band: 0=low, 1=mid, 2=high. Value in dB.
    void setGainDb(int band, double db) noexcept {
        if (band < 0 || band >= kBands || db == bandDb_[band]) return;
        bandDb_[band] = db;
        applyBand(band);
    }

    void process(float* l, float* r, int n) noexcept {
        for (int i = 0; i < n; ++i) {
            float xl = l[i], xr = r[i];
            for (int b = 0; b < kBands; ++b) {
                xl = l_[b].process(xl);
                xr = r_[b].process(xr);
            }
            l[i] = xl; r[i] = xr;
        }
    }

private:
    void applyCoefficients() noexcept {
        for (int b = 0; b < kBands; ++b) applyBand(b);
    }

    void applyBand(int b) noexcept {
        const double db = bandDb_[b];
        if (b == 0) { l_[0].setLowShelf(rate_, 250.0, db);  r_[0].setLowShelf(rate_, 250.0, db); }
        if (b == 1) { l_[1].setPeaking(rate_, 1000.0, db, 0.8); r_[1].setPeaking(rate_, 1000.0, db, 0.8); }
        if (b == 2) { l_[2].setHighShelf(rate_, 4000.0, db); r_[2].setHighShelf(rate_, 4000.0, db); }
    }

    double rate_  { 48000.0 };
    double bandDb_[kBands] { 0.0, 0.0, 0.0 };
    Biquad l_[kBands], r_[kBands];
};

} // namespace onda
