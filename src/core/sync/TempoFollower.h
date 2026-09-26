#pragma once

#include <algorithm>

namespace onda::sync {

/// Target-tempo resolution (§13): the multiplier a follower deck must
/// apply to reach the master's effective BPM, clamped to the deck's
/// own pitch range. Pure math shared by tests/diagnostics; the RT
/// corrector applies the same rule per block with slew limiting.
struct TempoFollower {
    struct Result {
        double mul     { 1.0 };   // rate multiplier for the follower
        bool   limited { false }; // required tempo outside pitch range
    };

    /// ownBpm      — the follower track's grid BPM at its position
    /// masterBpm   — the master's effective BPM
    /// pitchRange  — deck pitch range in ±percent (e.g. 8.0 → ±8%)
    static Result resolve(double ownBpm, double masterBpm,
                          float pitchRange) noexcept {
        Result r;
        if (ownBpm <= 0.0 || masterBpm <= 0.0) return r;
        const double want = masterBpm / ownBpm;
        const double lo = 1.0 - (double)pitchRange / 100.0;
        const double hi = 1.0 + (double)pitchRange / 100.0;
        r.limited = want < lo || want > hi;   // SYNC RANGE LIMIT (§25)
        r.mul     = std::clamp(want, lo, hi);
        return r;
    }
};

} // namespace onda::sync
