#pragma once

#include <cmath>

#include "../analysis/BeatGrid.h"

namespace onda::sync {

/// Beat/phase/bar extraction (§10). Reads the deck's BeatGrid — which
/// already carries the tempo map — so phase tracking follows variable
/// tempo automatically (§11-12).
struct BeatPhaseTracker {
    struct Phase {
        double beat  { -1.0 };  // fractional beat position
        int    bar   { 0 };     // 1-based bar
        double phase { 0.0 };   // 0..1 inside the current beat
        double bpm   { 0.0 };   // grid BPM at this position
    };

    static Phase track(const BeatGrid& g, int64_t frame) noexcept {
        Phase p;
        if (g.empty()) return p;
        p.beat  = g.frameToBeat(frame);
        if (p.beat < 0.0) return p;
        p.phase = p.beat - std::floor(p.beat);
        p.bar   = g.barOf(p.beat);
        p.bpm   = g.bpmAt(p.beat);
        return p;
    }

    /// Phase the deck will show `ahead` frames from now — mirrors the
    /// RT predictor used so corrections are render-order independent.
    static Phase predict(const BeatGrid& g, int64_t frame,
                         double rateMul, int64_t ahead) noexcept {
        return track(g, frame + (int64_t)(rateMul * ahead));
    }
};

} // namespace onda::sync
