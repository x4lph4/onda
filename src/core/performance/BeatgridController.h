#pragma once

#include <cmath>
#include <cstdint>
#include <memory>

#include "../analysis/BeatGrid.h"
#include "QuantizeEngine.h"

namespace onda {

/// Per-deck beatgrid controller (Phase 28 §41): the single place the
/// performance layer turns frames into musical positions. Owns the
/// shared_ptr (lifetime for the RT handoff slot lives in DeckCtx) and
/// exposes beat-aware helpers so every action — hot cues, loops, beat
/// jump, quantize — goes through one musical-position authority.
///
/// Fallback contract (§41): when no grid is attached every helper
/// degrades deterministically — beats/frames return raw values and
/// callers fall back to sample/time positioning instead of failing.
struct BeatgridController {
    std::shared_ptr<const BeatGrid> grid;
    int64_t trackId   { -1 };
    float   confidence{ 0.7f };   // analysis SyncReference confidence

    bool   has()        const noexcept { return grid && !grid->empty(); }
    const BeatGrid* raw() const noexcept {
        return has() ? grid.get() : nullptr; }

    double frameToBeat(int64_t frame) const noexcept {
        return has() ? grid->frameToBeat(frame) : -1.0;
    }
    int64_t beatToFrame(double beat) const noexcept {
        return has() ? grid->beatToFrame(beat) : -1;
    }
    int beatsPerBar() const noexcept {
        return has() ? grid->beatsPerBar : 4;
    }
    double bpmAt(double beat) const noexcept {
        return has() ? grid->bpmAt(beat) : 0.0;
    }

    /// Snap a frame to the nearest `quantumBeats` boundary — returns
    /// the input unchanged without a grid (fallback contract).
    int64_t snap(int64_t frame, double quantumBeats) const noexcept {
        return has() ? QuantizeEngine::snap(*grid, frame, quantumBeats)
                     : frame;
    }
    int64_t nextBoundary(int64_t frame, double quantumBeats) const noexcept {
        return has() ? QuantizeEngine::nextBoundary(*grid, frame,
                                                    quantumBeats)
                     : frame;
    }
    int64_t beatsAhead(double fromBeat, double beats) const noexcept {
        return has() ? QuantizeEngine::beatsAhead(*grid, fromBeat, beats)
                     : 0;
    }
};

} // namespace onda
