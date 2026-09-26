#pragma once

#include <cmath>
#include <string>
#include <vector>

#include "AnalysisTypes.h"
#include "BeatGrid.h"
#include "OnsetDetector.h"

namespace onda {

/// Beat-grid plausibility checks (§15). Returns a verdict with
/// human-readable reasons; `needsReview=true` marks the analysis so
/// the UI can flag it instead of silently trusting a bad grid.
struct GridVerdict {
    bool needsReview { false };
    float score { 1.0f };                 // 0..1 grid-vs-onsets match
    std::vector<std::string> reasons;
};

struct BeatGridValidator {
    /// grid   — candidate grid (device-rate frames)
    /// bpm    — analyzer result (confidence/stability)
    /// onsets — detected onsets used to build the grid
    GridVerdict check(const BeatGrid& g, const BpmResult& bpm,
                      const OnsetDetector::Result& onsets,
                      int64_t numFrames) const {
        GridVerdict v;
        if (g.empty()) {
            v.needsReview = true;
            v.reasons.push_back("empty grid — no tempo information");
            return v;
        }
        const double b = g.segments.front().bpm;
        if (b < 55.0 || b > 220.0) {
            v.needsReview = true;
            v.reasons.push_back("implausible tempo " +
                                std::to_string((int)b) + " BPM");
        }
        if (bpm.confidence < 0.35f) {
            v.needsReview = true;
            v.reasons.push_back("low BPM confidence " +
                                std::to_string((int)(bpm.confidence*100)) +
                                "%");
        }
        if (bpm.stability < 0.4f && bpm.variable) {
            v.reasons.push_back("variable tempo detected — constant "
                                "grid may drift");
        }

        // ---- onset↔grid agreement: fraction of onsets landing near
        // a grid beat (±18% of a beat) --------------------------------
        if (!onsets.onsets.empty() && !g.segments.empty()) {
            int near = 0, total = 0;
            for (const auto& o : onsets.onsets) {
                const double bt = g.frameToBeat(o.frame);
                if (bt < 0.0) continue;
                ++total;
                const double frac = bt - std::floor(bt);
                if (frac < 0.18 || frac > 0.82) ++near;
            }
            if (total >= 16) {
                v.score = (float)near / total;
                if (v.score < 0.55f) {
                    v.needsReview = true;
                    v.reasons.push_back(
                        "only " + std::to_string((int)(v.score*100)) +
                        "% of onsets align with the grid");
                }
            }
        }

        // ---- spacing sanity: first-beat anchor vs first onset ------
        if (!onsets.onsets.empty() && !g.segments.empty()) {
            const int64_t firstOnset = onsets.onsets.front().frame;
            const double fpb = g.framesPerBeat(g.segments.front());
            const double off =
                std::fabs((double)(g.firstBeatFrame - firstOnset));
            if (fpb > 0.0 && off > fpb) {
                v.reasons.push_back("grid start is more than a beat "
                                    "from the first onset");
            }
        }

        // ---- coverage: grid should span most of the track ----------
        if (numFrames > 0 && !g.segments.empty()) {
            const double totalBeats = g.frameToBeat(numFrames);
            if (totalBeats < 4.0) {
                v.needsReview = true;
                v.reasons.push_back("grid covers under 4 beats");
            }
        }
        return v;
    }
};

} // namespace onda
