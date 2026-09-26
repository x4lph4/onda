#pragma once

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

} // namespace onda::fx
