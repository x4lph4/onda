#pragma once

#include <algorithm>
#include <cmath>

#include "SyncTypes.h"

namespace onda::sync {

/// Phase-error correction policy (§14-16). Mirrors the RT corrector in
/// DeckEngine so the same thresholds drive diagnostics and tests.
///
///   Deadband   — <8 frames (~0.17 ms @48k): silence, no jitter.
///   Chase      — spread across the block, capped at ~5% of block.
///   Reposition — >35% of a beat: one faded snap instead of a long,
///                audible chase. A reposition is always beat-aligned
///                (the correction is exactly the phase error), which
///                keeps the musical grid intact.
struct SyncCorrection {
    static constexpr double kDeadbandFrames = 8.0;
    static constexpr double kRepositionFrac = 0.35;
    static constexpr double kChaseCapFrac   = 0.05;

    static CorrectionKind classify(double errFrames,
                                   double framesPerBeat) noexcept {
        const double a = std::fabs(errFrames);
        if (a < kDeadbandFrames) return CorrectionKind::Deadband;
        if (a > framesPerBeat * kRepositionFrac)
            return CorrectionKind::Reposition;
        return CorrectionKind::Chase;
    }

    /// Per-block rate correction for the Chase band (fraction of the
    /// block to add/subtract). Bounded so corrections stay inaudible.
    static double chaseRate(double errFrames, int blockFrames,
                          double rateMul) noexcept {
        if (blockFrames <= 0) return 0.0;
        const double cap = blockFrames * kChaseCapFrac * std::fabs(rateMul);
        return std::clamp(errFrames, -cap, cap) / (double)blockFrames;
    }

    /// Wrap a phase difference into [-0.5, +0.5] beats — the shortest
    /// direction to catch the master phase.
    static double wrapBeats(double errBeats) noexcept {
        return errBeats - std::floor(errBeats + 0.5);
    }
};

} // namespace onda::sync
