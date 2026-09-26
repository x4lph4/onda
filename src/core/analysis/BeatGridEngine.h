#pragma once

#include <cstdint>

#include "BeatGrid.h"
#include "OnsetDetector.h"
#include "AnalysisTypes.h"

namespace onda {

/// Builds a BeatGrid from BPM analysis + onsets, and exposes the manual
/// editing API (grid shift / BPM adjust / tempo segments / reset).
///
/// The grid itself is plain data (serialisable); every edit rederives
/// beat positions deterministically with f64 math — no accumulated
/// drift even on very long tracks.
class BeatGridEngine {
public:
    /// Auto-build: first beat = first strong onset near the start,
    /// tempo = constant BPM or the detected tempo map.
    /// Onsets vote for the downbeat phase (which of the beatsPerBar
    /// positions carries the most energy).
    BeatGrid build(double rate, const BpmResult& bpm,
                   const OnsetDetector::Result& onsets,
                   int64_t numFrames) const;

    // ---- manual edit operations (UI hook layer) ----
    static void setFirstBeat(BeatGrid& g, int64_t frame);
    static void moveBeatGrid(BeatGrid& g, int64_t deltaFrames);
    static void nudgeBeatGrid(BeatGrid& g, double deltaMs);
    static void adjustBpm(BeatGrid& g, double newBpm);
    static void setBeatsPerBar(BeatGrid& g, int bpb);
    static void setDownbeat(BeatGrid& g, int beatIndex);
    static void setTempoSegment(BeatGrid& g, size_t index, double bpm);
    static bool addTempoChange(BeatGrid& g, int64_t frame, double bpm);
    static bool removeTempoChange(BeatGrid& g, size_t index);
    static void resetTo(BeatGrid& g, const BeatGrid& source);
};

} // namespace onda
