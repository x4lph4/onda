#pragma once

#include <cmath>

#include "../analysis/BeatGrid.h"

namespace onda {

/// Grid snapping for quantized actions. Pure math — never touches the
/// audio thread. Quantize does NOT mean "wait for the beat": it means
/// the *target position* is recomputed onto the musical grid and the
/// seek is executed immediately with a faded, click-free jump.
struct QuantizeEngine {

    /// Snap `frame` to the nearest boundary of `quantumBeats` on `g`.
    static int64_t snap(const BeatGrid& g, int64_t frame,
                        double quantumBeats) noexcept {
        if (g.empty() || quantumBeats <= 0.0) return frame;
        const double b   = g.frameToBeat(frame);
        const double qb  = std::floor(b / quantumBeats + 0.5) * quantumBeats;
        const int64_t f  = g.beatToFrame(qb);
        return f >= 0 ? f : frame;
    }

    /// Next quantum boundary strictly after `frame` (for loop-in style
    /// "quantize forward" semantics).
    static int64_t nextBoundary(const BeatGrid& g, int64_t frame,
                                double quantumBeats) noexcept {
        if (g.empty()) return frame;
        const double b = g.frameToBeat(frame);
        const double q = (std::floor(b / quantumBeats) + 1.0) * quantumBeats;
        return g.beatToFrame(q);
    }

    /// Frame distance of `beats` starting at `fromBeat` (tempo-map safe:
    /// resolves through the grid rather than assuming constant tempo).
    static int64_t beatsAhead(const BeatGrid& g, double fromBeat,
                              double beats) noexcept {
        const int64_t a = g.beatToFrame(fromBeat);
        const int64_t b = g.beatToFrame(fromBeat + beats);
        if (a < 0 || b < 0) return 0;
        return b - a;
    }
};

} // namespace onda
