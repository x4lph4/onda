#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

#include "../audio/AudioFormat.h"
#include "../dsp/PeakLimiter.h"
#include "../model/MasterStats.h"
#include "../rt/AudioStats.h"

namespace onda {

class DeckEngine;

/// Central mix bus. Per block:
///   decks render post-EQ/filter audio -> per-deck buffers
///   main bus = sum(buf * channelFader * crossfaderGain)
///   cue bus  = sum(buf) for decks with PFL enabled (pre-fader)
///   master = main * masterGain -> optional limiter -> meters -> out[0..1]
///   phones = (cue*(1-mix) + master*mix) * cueVol        -> out[2..3]
///
/// All parameter setters are plain values applied on the RT thread via
/// the engine's command dispatch — never call them cross-thread.
class MixerEngine {
public:
    static constexpr int kMaxDecks = 4;

    MixerEngine();
    void prepare(double sampleRate) noexcept; // allocate + init (non-RT)

    // ---- RT-applied parameters ----
    void setMasterGain(float g) noexcept       { master_ = g; }
    void setLimiterEnabled(bool on) noexcept   { limiterOn_ = on; }
    void setFader(int deck, float v) noexcept;
    void setCrossfader(float x) noexcept;
    void setXfCurve(int c) noexcept            { curve_ = c; }
    void setPfl(int deck, bool on) noexcept;
    void setCueVol(float v) noexcept           { cueVol_ = v; }
    void setCueMix(float m) noexcept           { cueMix_ = m; }
    // --------------------------------

    /// RT. outs[0..1]=master, outs[2..3]=headphones (if numOut>=4).
    void render(float* const* out, int numOut, int numSamples,
                DeckEngine* decks, int numDecks,
                rt::AudioStats& stats) noexcept;

    const MasterStats& masterStats() const noexcept { return mstats_; }

    /// Deck index -> crossfader side: 0,2 = A; 1,3 = B.
    static bool deckOnSideA(int deck) noexcept { return (deck % 2) == 0; }

private:
    double rate_ { 48000.0 };

    // per-deck post-chain buffers + bus buffers (preallocated)
    std::vector<float> bufL_[kMaxDecks], bufR_[kMaxDecks];
    std::vector<float> mixL_, mixR_, cueL_, cueR_;

    // params (RT-written)
    float fader_[kMaxDecks] { 1.0f, 1.0f, 1.0f, 1.0f };
    float xf_      { 0.0f };
    int   curve_   { 1 };      // XfCurve::EqualPower
    bool  pfl_[kMaxDecks] { false, false, false, false };
    float cueVol_  { 0.8f };
    float cueMix_  { 0.5f };
    float master_  { 1.0f };
    bool  limiterOn_ { true };
    float smMaster_ { 1.0f };

    PeakLimiter limiter_;
    MasterStats mstats_;
};

} // namespace onda
