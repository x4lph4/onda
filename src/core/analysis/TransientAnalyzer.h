#pragma once

#include <algorithm>
#include <vector>

#include "AnalysisTypes.h"
#include "OnsetDetector.h"

namespace onda {

/// Transient extraction (§20): converts the onset detector's events
/// into a compact marker list for sampler/slicer/beat-repeat/waveform
/// markers. Strongest-first cap keeps storage small; beat detection
/// already consumed the same envelope upstream.
struct TransientAnalyzer {
    static constexpr size_t kMaxEvents = 4000;   // ~1 per 100 ms @7min
    static constexpr float  kMinStrength = 0.05f;

    std::vector<TransientEvent> analyze(
            const OnsetDetector::Result& onset) const {
        std::vector<TransientEvent> out;
        out.reserve(onset.onsets.size());
        for (const auto& o : onset.onsets)
            if (o.strength >= kMinStrength)
                out.push_back({ o.frame, o.strength });
        if (out.size() > kMaxEvents) {
            // keep the strongest — partial sort, stable enough for
            // marker purposes
            std::sort(out.begin(), out.end(),
                      [](const TransientEvent& a,
                         const TransientEvent& b) {
                          return a.strength > b.strength; });
            out.resize(kMaxEvents);
            std::sort(out.begin(), out.end(),
                      [](const TransientEvent& a,
                         const TransientEvent& b) {
                          return a.frame < b.frame; });
        }
        return out;
    }
};

} // namespace onda
