#pragma once

#include "AnalysisOverride.h"
#include "BeatGrid.h"

namespace onda {

/// Sync-facing view of a track's timing data (§32): everything the
/// sync engine needs in one immutable value — grid + tempo map live
/// inside `grid`; confidence gates how much sync should trust it.
struct SyncReference {
    double  bpm { 0.0 };          // effective (override-aware) BPM
    float   confidence { 0.0f };  // BPM detection confidence
    bool    variable { false };   // tempo map has >1 segment
    bool    reliable { false };   // safe to sync on
    BeatGrid grid;                // effective grid (override applied)
    double  firstBeatSec { 0.0 }; // convenience: grid anchor in sec

    static SyncReference from(const AnalysisResult& r,
                              const BeatGrid& detectedGrid,
                              const AnalysisOverride& ovr = {}) {
        SyncReference s;
        s.bpm        = ovr.effectiveBpm(r.bpm.bpm);
        s.confidence = r.bpm.confidence;
        s.variable   = r.bpm.variable;
        s.grid       = ovr.effectiveGrid(detectedGrid);
        s.grid.setRate(r.sampleRate);
        s.firstBeatSec = r.sampleRate > 0.0
            ? s.grid.firstBeatFrame / r.sampleRate : 0.0;
        s.reliable = !s.grid.empty() && s.bpm > 0.0
                  && !r.needsReview && s.confidence >= 0.35f;
        return s;
    }
};

} // namespace onda
