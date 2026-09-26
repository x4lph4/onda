#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "FXTypes.h"

namespace onda::fx {

/// Automation seam (§25): a lane of (beat position -> normalized
/// value) breakpoints for one FX parameter. Beat-synchronized by
/// construction — positions are musical beats from the MasterClock
/// domain, not wall-clock time.
///
/// This is a data model + evaluator only, on purpose: there is no
/// recorder, scheduler or UI here yet. A control-side owner samples
/// lanes each poll against the current beat position and pushes the
/// resulting values through the normal SetParam path, so automation
/// is indistinguishable from a hardware or UI gesture downstream.
/// NOT realtime-safe (std::vector) — never touched by the audio
/// thread.
struct FXAutomationPoint {
    float beat;   // position in beats (session timeline or loop-local)
    float value;  // normalized 0..1
};

class FXAutomationLane {
public:
    uint16_t paramId { 0 };

    void addPoint(FXAutomationPoint p) {
        auto it = std::lower_bound(points_.begin(), points_.end(), p.beat,
            [](const FXAutomationPoint& a, float b) { return a.beat < b; });
        if (it != points_.end() && it->beat == p.beat) *it = p;
        else points_.insert(it, p);
    }
    void clear() { points_.clear(); }
    int  pointCount() const noexcept { return (int)points_.size(); }
    const std::vector<FXAutomationPoint>& points() const noexcept {
        return points_;
    }

    /// Linear-interpolated value at `beat`. Holds the edge values
    /// outside the recorded range; 0.5 for an empty lane.
    float sampleAt(float beat) const noexcept {
        if (points_.empty()) return 0.5f;
        if (beat <= points_.front().beat) return points_.front().value;
        if (beat >= points_.back().beat)  return points_.back().value;
        const auto hi = std::lower_bound(points_.begin(), points_.end(),
            beat, [](const FXAutomationPoint& a, float b) {
                return a.beat < b; });
        const auto lo = hi - 1;
        const float span = hi->beat - lo->beat;
        const float t = span > 1e-6f ? (beat - lo->beat) / span : 0.0f;
        return lo->value + t * (hi->value - lo->value);
    }

private:
    std::vector<FXAutomationPoint> points_;
};

/// A bank of lanes keyed by FX param id. Control-side container;
/// caller iterates lanes() each poll and pushes sampleAt(beat)
/// through SetParam.
class FXAutomation {
public:
    static constexpr int kMaxLanes = 32;

    FXAutomationLane& laneFor(uint16_t param) {
        for (auto& l : lanes_)
            if (l.paramId == param) return l;
        lanes_.emplace_back();
        lanes_.back().paramId = param;
        return lanes_.back();
    }
    void removeLane(uint16_t param) {
        lanes_.erase(std::remove_if(lanes_.begin(), lanes_.end(),
            [param](const FXAutomationLane& l) {
                return l.paramId == param; }), lanes_.end());
    }
    void clear() { lanes_.clear(); }
    const std::vector<FXAutomationLane>& lanes() const noexcept {
        return lanes_;
    }

    /// Sample every lane at `beat`; writes into `out` pairs of
    /// (paramId, value). Returns write count.
    int evaluate(float beat, uint16_t* ids, float* vals,
                 int cap) const noexcept {
        int n = 0;
        for (const auto& l : lanes_) {
            if (n >= cap) break;
            if (l.pointCount() == 0) continue;
            ids[n]  = l.paramId;
            vals[n] = l.sampleAt(beat);
            ++n;
        }
        return n;
    }

private:
    std::vector<FXAutomationLane> lanes_;
};

} // namespace onda::fx
