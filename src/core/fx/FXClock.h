#pragma once

#include <cmath>

#include "FXTypes.h"

namespace onda::fx {

/// FXClock — the single place that turns a musical division into
/// sample-domain timing (§23). Every synced processor funnels through
/// here via SyncContext, so no effect ever computes BPM itself.
///
/// The `bpm` fed into these functions comes from the owning target's
/// clock source (the deck's BeatGrid effBpm, or the sync master for
/// send/master racks) — i.e. it *is* the MasterClock value, already
/// tempo-map-aware upstream.
struct FXClock {
    /// Clamp a stored division index into a valid BeatDiv.
    static BeatDiv divOf(int division) noexcept {
        return division < 0 ? BeatDiv::D_1_4
             : division >= (int)BeatDiv::kCount ? BeatDiv::D_1_4
             : (BeatDiv)division;
    }
    /// Length of `division` in samples at `bpm`/`sampleRate`.
    static double framesFor(float bpm, int division,
                            double sampleRate) noexcept {
        const float b = bpm > 1.0f ? bpm : 120.0f;
        return (double)beatsOf(divOf(division)) * 60.0 / b * sampleRate;
    }
    /// Same, in milliseconds.
    static double msFor(float bpm, int division) noexcept {
        const float b = bpm > 1.0f ? bpm : 120.0f;
        return (double)beatsOf(divOf(division)) * 60000.0 / b;
    }
    /// Whole beats covered by `division`.
    static float beats(int division) noexcept {
        return beatsOf(divOf(division));
    }
};

/// §33 — the read-only musical clock every synced processor may
/// consult. A lightweight view over the per-block `SyncContext` the
/// slot stamps (bpm from the target's tempo source, phase of the
/// master clock, active division), so effect code never re-derives
/// beat math and never reaches back into deck/mixer internals.
struct BeatClock {
    float  bpm        { 120.0f };
    float  beatPhase  { 0.0f };  // 0..1 inside the current beat
    int    division   { (int)BeatDiv::D_1_4 };
    int    synced     { 0 };
    double sampleRate { 48000.0 };

    static BeatClock from(const SyncContext& c, double rate) noexcept {
        return { c.bpm, c.beatPhase, c.division, c.synced, rate };
    }
    BeatDiv div()        const noexcept { return FXClock::divOf(division); }
    float   divBeats()   const noexcept { return beatsOf(div()); }
    double  divFrames()  const noexcept {
        return FXClock::framesFor(bpm, division, sampleRate);
    }
    double  divMs()      const noexcept {
        return FXClock::msFor(bpm, division);
    }
    /// Beat fraction within the current bar (4/4 assumed — the
    /// subdivision grids are all bar-quantized today).
    float barPhase() const noexcept {
        const float d = divBeats();
        return d >= 4.0f ? beatPhase
             : d > 0.0f  ? std::fmod(beatPhase / d, 1.0f) : beatPhase;
    }
};

} // namespace onda::fx
