#pragma once

#include <cstdint>

#include "../analysis/BeatGrid.h"
#include "QuantizeEngine.h"

namespace onda {

/// Pure loop math over the BeatGrid. All functions return frame pairs;
/// the facade turns them into SetLoopFrames commands. Works on the
/// musical grid, so loops stay beat-aligned under tempo maps.
struct SmartLoopEngine {

    struct Loop { int64_t in; int64_t out; };

    /// Beat-quantized auto-loop of `beats` starting at `pos`.
    /// in = nearest beat boundary at-or-before pos (snap if quantize,
    /// else raw pos resolved to next beat); out = in + beats via grid.
    static Loop beatLoop(const BeatGrid& g, int64_t pos, double beats,
                         bool quantizeOn) noexcept {
        if (g.empty() || beats <= 0.0) return { pos, pos };
        const double b = g.frameToBeat(pos);
        const double inBeat = quantizeOn ? std::floor(b + 0.5)
                                         : std::ceil(b); // next beat
        const int64_t in  = g.beatToFrame(inBeat);
        const int64_t out = g.beatToFrame(inBeat + beats);
        return out > in ? Loop{in, out} : Loop{pos, pos};
    }

    /// Resize an existing loop to `beats`, keeping `in`.
    static Loop resizeBeats(const BeatGrid& g, Loop l, double beats) noexcept {
        if (g.empty()) return l;
        const double inBeat = g.frameToBeat(l.in);
        const int64_t out = g.beatToFrame(inBeat + beats);
        return out > l.in ? Loop{l.in, out} : l;
    }

    static Loop halve(const BeatGrid& g, Loop l) noexcept {
        const double beats = g.frameToBeat(l.out) - g.frameToBeat(l.in);
        return resizeBeats(g, l, std::max(beats / 2.0, 1.0 / 32.0));
    }

    static Loop double_(const BeatGrid& g, Loop l) noexcept {
        const double beats = g.frameToBeat(l.out) - g.frameToBeat(l.in);
        return resizeBeats(g, l, beats * 2.0);
    }

    /// Shift the whole loop by `beats` (preserves length & phase).
    static Loop move(const BeatGrid& g, Loop l, double beats) noexcept {
        if (g.empty()) return l;
        const double inBeat  = g.frameToBeat(l.in);
        const double outBeat = g.frameToBeat(l.out);
        const int64_t in  = g.beatToFrame(inBeat + beats);
        const int64_t out = g.beatToFrame(outBeat + beats);
        return out > in ? Loop{in, out} : l;
    }

    // ---- Phase 35 §56 — end-of-track safety. maxF <= 0 disables. ----
    /// Clamp `out` into [in+1, maxF]; returns an empty Loop (in==out)
    /// when even `in` lies at/past the end so callers can reject.
    static Loop clampOut(Loop l, int64_t maxF) noexcept {
        if (maxF <= 0) return l;
        if (l.in >= maxF - 1) return { l.in, l.in };
        if (l.out > maxF) l.out = maxF;
        return l;
    }
    /// move() that keeps the WHOLE loop inside [0, maxF) — works on
    /// the BEAT axis so negative/past-end requests clamp before the
    /// frame conversion (beatToFrame(<0) → -1, which would corrupt a
    /// frame-space clamp). Length is preserved; returns the input
    /// loop when no valid placement exists.
    static Loop boundedMove(const BeatGrid& g, Loop l, double beats,
                            int64_t maxF) noexcept {
        if (g.empty() || l.out <= l.in) return l;
        const double len = g.frameToBeat(l.out) - g.frameToBeat(l.in);
        if (len <= 0.0) return l;
        double nb = g.frameToBeat(l.in) + beats;
        if (maxF > 0) {
            const double endB = g.frameToBeat(maxF);
            if (endB > 0.0 && nb + len > endB) nb = endB - len;
        }
        if (nb < 0.0) nb = 0.0;
        const int64_t in  = g.beatToFrame(nb);
        const int64_t out = g.beatToFrame(nb + len);
        return (out > in && (maxF <= 0 || out <= maxF))
            ? Loop{in, out} : l;
    }
};

} // namespace onda
