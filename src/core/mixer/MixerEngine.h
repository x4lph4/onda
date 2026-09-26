#pragma once

#include <cstddef>
#include <vector>

#include "../audio/AudioFormat.h"
#include "../model/MasterStats.h"
#include "../rt/AudioStats.h"
#include "ChannelStrip.h"
#include "CrossfaderEngine.h"
#include "HeadphoneBus.h"
#include "MasterBus.h"
#include "MixerTypes.h"
#include "ParameterRegistry.h"
#include "RoutingEngine.h"

namespace onda {

class DeckEngine;

/// Central mix engine. Per block:
///
///   sources render into strip inputs  (decks today; sampler/external/
///   FX-return sources later — they just write strip.inL/inR)
///     for each channel:  trim -> EQ -> filter -> [PFL cue tap]
///                        -> fader/mute/solo -> xf assign -> master sum
///     master bus:  gain/mute -> limiter -> meters -> out[0..1]
///     phones bus:  (cueSum * (1-mix) + masterPost * mix) * cueVol -> out[2..3]
///
/// All parameter setters are RT-thread only — they are invoked from the
/// engine's command dispatch, never cross-thread. Use MixerController /
/// ParameterRegistry + rt::SetParam from any control thread.
class MixerEngine {
public:
    static constexpr int kMaxChannels = mixer::ParameterRegistry::kMaxChannels;
    static constexpr int kMaxDecks    = kMaxChannels; // legacy alias

    MixerEngine();
    void prepare(double sampleRate) noexcept; // allocate + init (non-RT)

    // ---- channel access ----
    mixer::ChannelStrip& channel(int i) noexcept { return strips_[i & (kMaxChannels - 1)]; }
    const mixer::ChannelStrip& channel(int i) const noexcept { return strips_[i & (kMaxChannels - 1)]; }
    int numActiveChannels() const noexcept { return active_; }
    void setActiveChannels(int n) noexcept; // non-RT config (<= kMaxChannels)

    // ---- bus access ----
    mixer::MasterBus&      master() noexcept      { return master_; }
    mixer::HeadphoneBus&   phones() noexcept      { return phones_; }
    mixer::CrossfaderEngine& xf() noexcept        { return xf_; }
    const mixer::RoutingEngine& routing() const noexcept { return routing_; }

    /// RT: generic parameter setter (ids from ParameterRegistry).
    void setParam(uint16_t id, float v) noexcept;

    // ---- legacy RT setters (kept for command compatibility) ----
    void setMasterGain(float g) noexcept     { master_.setGain(g); }
    void setLimiterEnabled(bool on) noexcept { master_.limiter().setEnabled(on); }
    void setFader(int deck, float v) noexcept      { channel(deck).setFader(v); }
    void setCrossfader(float x) noexcept           { xf_.setPosition(x); }
    void setXfCurve(int c) noexcept                { xf_.setCurve((XfCurve)c); }
    void setPfl(int deck, bool on) noexcept        { channel(deck).setCue(on); }
    void setCueVol(float v) noexcept               { phones_.setVolume(v); }
    void setCueMix(float m) noexcept               { phones_.setMix(m); }
    // ------------------------------------------------------------

    /// RT. outs[0..1]=master, outs[2..3]=headphones (if numOut>=4).
    void render(float* const* out, int numOut, int numSamples,
                DeckEngine* decks, int numDecks,
                rt::AudioStats& stats) noexcept;

    const MasterStats& masterStats() const noexcept { return mstats_; }

    /// Deck index -> crossfader side: 0,2 = A; 1,3 = B.
    static bool deckOnSideA(int deck) noexcept { return (deck % 2) == 0; }

private:
    void rebuildRouting() noexcept;

    double rate_ { 48000.0 };
    int    active_ { 4 };

    mixer::ChannelStrip    strips_[kMaxChannels];
    mixer::CrossfaderEngine xf_;
    mixer::MasterBus        master_;
    mixer::HeadphoneBus     phones_;
    mixer::RoutingEngine    routing_;

    std::vector<float> mixL_, mixR_, cueL_, cueR_; // bus scratch (preallocated)

    MasterStats mstats_; // legacy published struct (filled from buses)
};

} // namespace onda
