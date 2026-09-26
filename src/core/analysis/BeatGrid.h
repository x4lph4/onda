#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "AnalysisTypes.h"

namespace onda {

/// Tempo-aware beatgrid: piecewise-constant tempo segments, each
/// mapping beat index <-> device-rate frame with f64 math (no drift).
///
/// Constant tempo = one segment starting at firstBeatFrame.
/// `downbeatOffset` rotates the bar phase (0 = grid beat 0 is bar down).
struct BeatGrid {
    int64_t firstBeatFrame { 0 };   // frame of grid beat 0
    int     beatsPerBar    { 4 };
    int     downbeatOffset { 0 };   // which beat index is bar 1 beat 1
    std::vector<TempoSegment> segments; // >=1 always once built

    bool   empty() const noexcept { return segments.empty(); }
    double rate() const noexcept  { return rate_; }
    void   setRate(double r)      { rate_ = r; }

    /// Musical position (fractional beats from grid start) for a frame.
    double frameToBeat(int64_t frame) const noexcept {
        if (segments.empty()) return -1.0;
        // locate last segment starting <= frame
        const TempoSegment* s = &segments.front();
        for (const auto& seg : segments) {
            if (frame >= seg.startFrame) s = &seg; else break;
        }
        return (double)s->startBeat +
               (frame - s->startFrame) / framesPerBeat(*s);
    }

    /// Frame position of a (possibly fractional) beat.
    int64_t beatToFrame(double beat) const noexcept {
        if (segments.empty() || beat < 0.0) return -1;
        const TempoSegment* s = &segments.front();
        for (const auto& seg : segments) {
            if (beat >= (double)seg.startBeat) s = &seg; else break;
        }
        return s->startFrame +
               (int64_t)llround((beat - (double)s->startBeat) * framesPerBeat(*s));
    }

    bool isDownbeat(double beat) const noexcept {
        const int b = (int)llround(beat) - downbeatOffset;
        return (b % beatsPerBar + beatsPerBar) % beatsPerBar == 0;
    }
    int barOf(double beat) const noexcept {
        const int b = (int)llround(beat) - downbeatOffset;
        return (int)std::floor((double)b / beatsPerBar) + 1; // 1-based
    }

    /// Nearest beat frame to `frame` (for quantize).
    int64_t nearestBeatFrame(int64_t frame) const noexcept {
        return beatToFrame(frameToBeat(frame) + 0.5 > frameToBeat(frame)
                             ? std::ceil(frameToBeat(frame))
                             : std::floor(frameToBeat(frame)));
    }
    double framesPerBeat(const TempoSegment& s) const noexcept {
        return s.bpm > 0.0 ? rate_ * 60.0 / s.bpm : 1e30;
    }

private:
    double rate_ { 48000.0 };
};

} // namespace onda
