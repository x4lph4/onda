#pragma once

#include <cstdint>

#include "SamplerTypes.h"

namespace onda::sampler {

/// One live playback instance. POD — lives in a fixed pool inside the
/// RT engine, zero allocation during playback.
struct SampleVoice {
    const SampleData* data   = nullptr;
    int     slot      = -1;
    double  pos       = 0.0;    // fractional read position (frames)
    double  step      = 1.0;    // pitch * speed * rate correction
    float   gainL     = 0.0f;   // post gain+pan, smoothed toward target
    float   gainR     = 0.0f;
    float   targetGL  = 0.0f;
    float   targetGR  = 0.0f;
    PlaybackMode mode = PlaybackMode::OneShot;
    bool    active    = false;
    bool    releasing = false;  // fade-out in progress
    bool    held      = false;  // HOLD: pad currently down
    bool    loop      = false;  // wraps at sample end
    float   vel       = 1.0f;   // trigger velocity 0..1
    int64_t delay     = 0;      // quantize: frames until start
    int64_t releaseAfter = -1;  // §23 quantized stop countdown
    uint32_t age      = 0;      // render ticks since start (steal: oldest)
    float   env       = 0.0f;   // anti-click envelope 0..1
    float   envInc    = kFadeIn;   // §28 per-slot attack rate
    float   envDec    = kFadeOut;  // §28 per-slot release rate
    uint32_t seq      = 0;      // trigger sequence (steal: same-sample)

    // §59-61 — non-destructive playback window + loop region
    // (decoded frames; loopIn/loopOut < 0 fall back to the window).
    double  startF    = 0.0;
    double  endF      = -1.0;   // <0 → sample end
    double  loopIn    = -1.0;
    double  loopOut   = -1.0;

    // §32 — per-slot tilt filter (one-pole; <0 LP sweep, >0 HP tilt)
    float   fltA      = 0.0f;   // pole coefficient 0..1 (0 = bypass)
    int     fltMode   = 0;      // -1 LP, +1 HP, 0 off
    float   fltYl     = 0.0f;
    float   fltYr     = 0.0f;

    static constexpr float kFadeIn  = 0.0002f;  // ~10 samples @48k env step
    static constexpr float kFadeOut = 0.0004f;  // ~21ms release fade

    /// current audible gain (for Quietest steal policy)
    float level() const { return (gainL + gainR) * 0.5f * env; }
};

} // namespace onda::sampler
