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

    /// §31 — meter as a TimeSignature. `beatsPerBar` is the
    /// numerator; the denominator stays 4 until a meter model needs
    /// otherwise (bars are still counted in quarter-note beats).
    TimeSignature timeSignature() const noexcept {
        return { beatsPerBar, 4 };
    }

    /// BPM in effect at a (fractional) beat position.
    double bpmAt(double beat) const noexcept {
        if (segments.empty()) return 0.0;
        const TempoSegment* s = &segments.front();
        for (const auto& seg : segments) {
            if (beat >= (double)seg.startBeat) s = &seg; else break;
        }
        return s->bpm;
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

    /// Copy rescaled to a different frame rate. Analysis runs at the
    /// reference rate while decks position themselves in device-rate
    /// frames — publish `grid.rescaled(deviceRate)` to a deck.
    BeatGrid rescaled(double newRate) const {
        BeatGrid g;
        g.beatsPerBar    = beatsPerBar;
        g.downbeatOffset = downbeatOffset;
        if (newRate <= 0.0 || rate_ <= 0.0 || newRate == rate_) {
            g.firstBeatFrame = firstBeatFrame;
            g.segments = segments;
            g.rate_ = newRate > 0.0 ? newRate : rate_;
            return g;
        }
        const double k = newRate / rate_;
        g.firstBeatFrame = (int64_t)llround(firstBeatFrame * k);
        g.segments.reserve(segments.size());
        for (auto s : segments) {
            s.startFrame = (int64_t)llround(s.startFrame * k);
            g.segments.push_back(s);
        }
        g.rate_ = newRate;
        return g;
    }

private:
    double rate_ { 48000.0 };
};

} // namespace onda
