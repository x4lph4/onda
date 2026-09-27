#pragma once

#include <algorithm>
#include <cmath>

#include "../dsp/Biquad.h"
#include "MixerTypes.h"

namespace onda::mixer {

/// §12 — isolator processor: a true 3-way filter bank, not a shelf EQ.
///
/// Topology (per channel, all LR4 = two cascaded Butterworth biquads):
///   lo  = LP4(x)                 low band   (< kLowFc)
///   mid = LP4_hi(HP4_lo(x))      mid band   (kLowFc..kHighFc)
///   hi  = HP4(x)                 high band  (> kHighFc)
///   out = lo*gL + mid*gM + hi*gH
///
/// A subtractive "residue" mid band was considered and rejected: IIR
/// crossovers carry phase, so x - LP - HP is not band-limited — a
/// killed band leaks back through the residue. The explicit filter
/// bank gives real spectral separation: killing LOW removes the LP4
/// contribution outright (mid path sits ~-46 dB at 80 Hz), killing
/// HIGH removes only the HP4 top end, killing MID leaves lo+hi.
/// Reconstruction at unity gains has a few dB of ripple around the
/// crossover edges — the same character hardware isolators exhibit.
///
/// Kill semantics: a band at/below its detent (kDetentDb) maps to
/// kKillDb (~-72 dB ≈ silence); band gains are LINEAR scalars smoothed
/// per-sample (~1.5 ms), so kills are fast, clickless and stable —
/// the biquad coefficients themselves are static after prepare().
///
/// EqProcessor delegates to this processor when EqMode::Isolator is
/// selected; the shelf cascade stays reserved for Standard mode.
class IsolatorProcessor {
public:
    static constexpr double kKillDb   = -72.0;  // ≈ silence at detent
    static constexpr double kRangeLo  = -72.0;  // applied range floor
    static constexpr double kRangeHi  = 12.0;   // applied range ceiling
    /// Band at/below this target is treated as killed — matches the
    /// registry's -26 dB detent, so a knob at minimum always reaches
    /// silence regardless of the control source.
    static constexpr double kDetentDb = -26.0;

    static constexpr double kLowFc  = 300.0;    // low/mid crossover Hz
    static constexpr double kHighFc = 5000.0;   // mid/high crossover Hz
    static constexpr int    kBands  = 3;

    /// Effective applied dB for a commanded target under isolator rules.
    static double effectiveDb(double tgtDb) noexcept {
        return tgtDb <= kDetentDb ? kKillDb : tgtDb;
    }

    void prepare(double rate) noexcept {
        rate_ = rate;
        configure();
        // ~1.5 ms one-pole on the linear band gains — fast enough for
        // a kill move, slow enough to be zipper-free.
        alpha_ = 1.0f - (float)std::exp(-1.0 / (rate * 0.0015));
        for (int b = 0; b < kBands; ++b) { gL_[b] = gR_[b] = gT_[b] = 1.0f; }
    }

    /// Re-tune a crossover edge (control or RT — coefficient update is
    /// block-rate and clickless because band gains ride on top).
    void setLowFc(double hz) noexcept {
        loFc_ = std::clamp(hz, 60.0, hiFc_ - 400.0);
        configure();
    }
    void setHighFc(double hz) noexcept {
        hiFc_ = std::clamp(hz, loFc_ + 400.0, rate_ * 0.4);
        configure();
    }
    double lowFc()  const noexcept { return loFc_; }
    double highFc() const noexcept { return hiFc_; }

    /// Target LINEAR gain for band b (0..kBands-1: low/mid/high).
    /// Called once per block by EqProcessor::tick with the ramped
    /// value; the per-sample smoother does the rest. RT-safe.
    void setBandGain(int b, float lin) noexcept {
        if (b >= 0 && b < kBands)
            gT_[b] = lin < 0.0f ? 0.0f : (lin > 16.0f ? 16.0f : lin);
    }
    float bandGain(int b) const noexcept {
        return b >= 0 && b < kBands ? gT_[b] : 0.0f;
    }

    /// RT: split, weigh, recombine. Per-band kill reaches ~silence with
    /// real spectral separation — no fixed shelf slope leaking in.
    void process(float* l, float* r, int n) noexcept {
        for (int i = 0; i < n; ++i) {
            const float xl = l[i], xr = r[i];
            const float loL = lpL_[1].process(lpL_[0].process(xl));
            const float loR = lpR_[1].process(lpR_[0].process(xr));
            const float hiL = hpL_[1].process(hpL_[0].process(xl));
            const float hiR = hpR_[1].process(hpR_[0].process(xr));
            const float midL = mlpL_[1].process(mlpL_[0].process(
                                   mhpL_[1].process(mhpL_[0].process(xl))));
            const float midR = mlpR_[1].process(mlpR_[0].process(
                                   mhpR_[1].process(mhpR_[0].process(xr))));
            for (int b = 0; b < kBands; ++b) {
                gL_[b] += (gT_[b] - gL_[b]) * alpha_;
                gR_[b] += (gT_[b] - gR_[b]) * alpha_;
            }
            l[i] = loL * gL_[0] + midL * gL_[1] + hiL * gL_[2];
            r[i] = loR * gR_[0] + midR * gR_[1] + hiR * gR_[2];
        }
    }

private:
    void configure() noexcept {
        for (int s = 0; s < 2; ++s) {
            lpL_[s].setLowpass (rate_, loFc_, 0.707);
            lpR_[s].setLowpass (rate_, loFc_, 0.707);
            mhpL_[s].setHighpass(rate_, loFc_, 0.707);
            mhpR_[s].setHighpass(rate_, loFc_, 0.707);
            mlpL_[s].setLowpass (rate_, hiFc_, 0.707);
            mlpR_[s].setLowpass (rate_, hiFc_, 0.707);
            hpL_[s].setHighpass(rate_, hiFc_, 0.707);
            hpR_[s].setHighpass(rate_, hiFc_, 0.707);
        }
    }

    double rate_ { 48000.0 };
    double loFc_ { kLowFc }, hiFc_ { kHighFc };
    float  alpha_ { 0.001f };
    Biquad lpL_[2],  lpR_[2];    // LP4  @ loFc   (low band)
    Biquad mhpL_[2], mhpR_[2];   // HP4  @ loFc   (mid bottom edge)
    Biquad mlpL_[2], mlpR_[2];   // LP4  @ hiFc   (mid top edge)
    Biquad hpL_[2],  hpR_[2];    // HP4  @ hiFc   (high band)
    float  gL_[kBands] { 1.0f, 1.0f, 1.0f };
    float  gR_[kBands] { 1.0f, 1.0f, 1.0f };
    float  gT_[kBands] { 1.0f, 1.0f, 1.0f };
};

} // namespace onda::mixer
