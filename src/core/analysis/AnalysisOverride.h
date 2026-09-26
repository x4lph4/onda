#pragma once

#include <optional>

#include "AnalysisTypes.h"
#include "BeatGrid.h"

namespace onda {

/// Unified user-correction model (§16, §25, §41): every field the
/// user can override, kept strictly separate from detected data.
/// `nullopt`/empty = "use detected". Applying overrides NEVER mutates
/// the stored detection — readers merge at query time.
struct AnalysisOverride {
    std::optional<double> bpm;            // effective BPM override
    std::optional<int>    keyIndex;       // 0..23 standard index
    std::optional<BeatGrid> grid;         // full edited grid
    bool empty() const {
        return !bpm && !keyIndex && !grid;
    }

    /// Effective BPM given the detection.
    double effectiveBpm(double detected) const {
        return bpm.value_or(detected);
    }
    int effectiveKey(int detected) const {
        return keyIndex.value_or(detected);
    }
    /// Effective grid: override when present, else detected.
    BeatGrid effectiveGrid(const BeatGrid& detected) const {
        return grid.value_or(detected);
    }
};

} // namespace onda
