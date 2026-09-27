#pragma once

#include <cstdint>

#include "../analysis/BeatGrid.h"
#include "QuantizeEngine.h"

namespace onda {

/// §52-53 — cue-jump destination math. Re-resolves the cue's stored
/// musical position (so beatgrid edits keep it correct) and snaps it
/// to the deck's configured quantum when quantize is on. This is the
/// same math CueEngine::trigger applies for hot/memory/loop cues —
/// factored here so cue jumps from any source (pads, keyboard, MIDI
/// learn, future controllers) share ONE implementation and the pure
/// function stays testable without the cue table.
struct CueJumpEngine {

    /// Quantized cue-jump target. `cueBeat` is the stored musical
    /// position (-1 → resolved from cueFrame through the grid).
    static int64_t target(const BeatGrid& g, int64_t cueFrame,
                          double cueBeat, double quantumBeats,
                          bool quantizeOn) noexcept {
        if (!quantizeOn || g.empty()) return cueFrame;
        const double beat = cueBeat >= 0.0 ? cueBeat
                                           : g.frameToBeat(cueFrame);
        const int64_t t =
            QuantizeEngine::snap(g, g.beatToFrame(beat), quantumBeats);
        return t >= 0 ? t : cueFrame;
    }

    /// Frame overload — cue has no stored beat (session cue).
    static int64_t target(const BeatGrid& g, int64_t cueFrame,
                          double quantumBeats, bool quantizeOn) noexcept {
        return target(g, cueFrame, -1.0, quantumBeats, quantizeOn);
    }
};

} // namespace onda
