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
    uint32_t age      = 0;      // render ticks since start (steal: oldest)
    float   env       = 0.0f;   // anti-click envelope 0..1
    uint32_t seq      = 0;      // trigger sequence (steal: same-sample)

    static constexpr float kFadeIn  = 0.0002f;  // ~10 samples @48k env step
    static constexpr float kFadeOut = 0.0004f;  // ~21ms release fade

    /// current audible gain (for Quietest steal policy)
    float level() const { return (gainL + gainR) * 0.5f * env; }
};

} // namespace onda::sampler
