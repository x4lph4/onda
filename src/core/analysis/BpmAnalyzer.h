#pragma once

#include "AnalysisTypes.h"
#include "OnsetDetector.h"

namespace onda {

/// BPM estimation from the onset envelope.
///
/// Method: autocorrelation of the spectral-flux envelope over the
/// plausible tempo range (55-220 BPM), followed by octave-aware
/// rescoring so half/double-time errors are surfaced as ranked
/// candidates instead of silently picked.
///
/// Stability: the envelope is split into ~16 s windows; each window's
/// dominant lag is compared against the global result. Large spread
/// marks the track as variable-tempo and emits a coarse TempoSegment
/// map for the BeatGridEngine/UI to refine.
///
/// Limitations (documented, not hidden):
///  - Swing/tuplet rhythms can bias the comb towards multiples.
///  - Very short clips (< ~4 s) cannot yield a stable estimate.
///  - Rubato/live music may return low confidence by design.
class BpmAnalyzer {
public:
    static constexpr double kMinBpm = 55.0;
    static constexpr double kMaxBpm = 220.0;

    /// onset: result of OnsetDetector::detect (envelope + hop + rate).
    BpmResult analyze(const OnsetDetector::Result& onset);

private:
    std::vector<float> autocorr_(const float* env, int n,
                                 int lagLo, int lagHi) const;
    double bestLagBpm_(const float* env, int n, double hopSec,
                       int* lagOut, float* scoreOut) const;
};

} // namespace onda
