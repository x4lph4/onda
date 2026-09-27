#pragma once

#include <array>
#include <cmath>

#include "StemTypes.h"

namespace onda::stems {

/// ------------------------------------------------------------------
/// §12-16 — per-deck stem mixing state. Pure model: resolves the
/// audible contribution of every stem from gain/mute/solo/pan.
///
/// Explicit solo semantics (§14): if ANY stem is soloed, non-solo
/// stems are fully silenced (industry-standard behaviour). Mute wins
/// over solo on the same stem — a muted stem is never audible.
///
/// Pan (§16): constant-power on the stem's stereo image —
///   gL = cos((pan+1)*π/4), gR = sin((pan+1)*π/4)   (pan ∈ -1..+1)
///
/// The resolved values feed DeckEngine's RT smoothing (smStemGain_),
/// so a mute toggle is a click-free gain ramp — never a hard gate.
/// ------------------------------------------------------------------
struct StemOut { float gL { 1.0f }, gR { 1.0f }; };

class StemMixer {
public:
    struct Param { float gain { 1.0f }, pan { 0.0f };
                   bool muted { false }, solo { false }; };

    void setGain(int stem, float g) {
        p(stem).gain = clamp(g, 0.0f, 2.0f); }
    void setPan(int stem, float v) {
        p(stem).pan = clamp(v, -1.0f, 1.0f); }
    void setMute(int stem, bool m)  { p(stem).muted = m; }
    void setSolo(int stem, bool s)  { p(stem).solo  = s; }
    void clearSolos()               { for (auto& x : par_) x.solo = false; }
    void clearMutes()               { for (auto& x : par_) x.muted = false; }

    const Param& param(int stem) const {
        static const Param kEmpty;
        return (stem >= 0 && stem < kStemMax) ? par_[(size_t)stem]
                                              : kEmpty;
    }
    bool anySolo() const {
        for (const auto& x : par_) if (x.solo) return true;
        return false;
    }

    /// Resolved stereo gain for one stem — mute/solo already applied.
    /// `present` mask lets callers skip unavailable stems (§26).
    StemOut out(int stem, bool present = true) const {
        const Param& x = param(stem);
        if (!present || x.muted || (anySolo() && !x.solo))
            return { 0.0f, 0.0f };
        const double t = ((double)x.pan + 1.0) * 0.7853981633974483;
        return { (float)(x.gain * std::cos(t)),
                 (float)(x.gain * std::sin(t)) };
    }

    /// All present stems' resolved mask — bit per audible stem.
    uint32_t audibleMask(uint32_t presentMask) const {
        uint32_t m = 0;
        for (int i = 0; i < kStemMax; ++i)
            if ((presentMask & (1u << i))) {
                const StemOut o = out(i, true);
                if (o.gL != 0.0f || o.gR != 0.0f) m |= (1u << i);
            }
        return m;
    }

    void reset() { par_ = {}; }

private:
    Param& p(int stem) {
        static Param sink;   // out-of-range writes land here, safely
        return (stem >= 0 && stem < kStemMax) ? par_[(size_t)stem]
                                              : sink;
    }
    static float clamp(float v, float lo, float hi) {
        return v < lo ? lo : v > hi ? hi : v;
    }
    std::array<Param, kStemMax> par_ {};
};

} // namespace onda::stems
