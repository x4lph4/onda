#pragma once

#include <cmath>
#include <vector>

#include "AnalysisTypes.h"
#include "BeatGrid.h"
#include "OnsetDetector.h"

namespace onda {

/// §29 — picks which beat index inside the bar carries the downbeat.
/// Method: every detected onset votes for the beat-of-bar position it
/// lands on, weighted by onset strength; the strongest position is
/// the bar start. This is a *phase* detector — it assumes the beat
/// grid itself is already correct (BeatGridEngine runs it after the
/// anchor/tempo pass).
///
/// `beatsPerBar` is a parameter, never hard-coded to 4 — unusual
/// meters keep working when the grid's time signature changes.
class DownbeatDetector {
public:
    struct Result {
        int   offset { 0 };       // beat index that is bar-beat 1
        float confidence { 0.0f };// winner share of total vote 0..1
    };

    static Result phase(const OnsetDetector::Result& onsets,
                        const BeatGrid& grid, int beatsPerBar) {
        Result r;
        const int bpb = beatsPerBar < 1 ? 1 : beatsPerBar;
        if (onsets.onsets.empty() || grid.empty()) return r;

        std::vector<double> score((size_t)bpb, 0.0);
        double total = 0.0;
        for (const auto& o : onsets.onsets) {
            const double b = grid.frameToBeat(o.frame);
            if (b < 0.0) continue;
            int idx = (int)llround(b) % bpb;
            if (idx < 0) idx += bpb;
            score[(size_t)idx] += o.strength;
            total += o.strength;
        }
        int best = 0;
        for (int i = 1; i < bpb; ++i)
            if (score[(size_t)i] > score[(size_t)best]) best = i;
        r.offset = best;
        r.confidence = total > 0.0
            ? (float)(score[(size_t)best] / total) : 0.0f;
        return r;
    }
};

} // namespace onda
