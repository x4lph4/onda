#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <vector>

#include "../audio/AudioFormat.h"
#include "../fx/FXEngine.h"
#include "../model/MasterStats.h"
#include "../rt/AudioStats.h"
#include "ChannelStrip.h"
#include "CrossfaderEngine.h"
#include "CueManager.h"
#include "HeadphoneBus.h"
#include "MasterBus.h"
#include "MixerDiagnostics.h"
#include "MixerTypes.h"
#include "ParameterRegistry.h"
#include "RoutingEngine.h"

namespace onda {

class DeckEngine;
namespace sampler { class SamplerRt; }

/// Central mix engine. Per block:
///
///   sources render into strip inputs  (decks today; sampler/external/
///   FX-return sources later — they just write strip.inL/inR)
///     per channel:  insert FX rack -> trim -> EQ -> filter
///                   -> [PFL cue tap] -> fader/mute/solo
///                   -> [post tap -> send buses -> send FX -> return]
///                   -> xf assign -> master sum
///     master insert FX rack
///     master bus:  gain/mute -> limiter -> meters -> out[0..1]
///     phones bus:  (cueSum * (1-mix) + masterPost * mix) * cueVol -> out[2..3]
///
/// Cue behavior: the PFL tap is pre-fader but post insert-FX — the DJ
/// hears exactly what the channel produces; send/return FX are post-
/// fader and intentionally NOT in the cue path.
///
/// All parameter setters are RT-thread only — they are invoked from the
/// engine's command dispatch, never cross-thread. Use MixerController /
/// ParameterRegistry + rt::SetParam from any control thread.
class MixerEngine {
public:
    static constexpr int kMaxChannels = mixer::ParameterRegistry::kMaxChannels;
    static constexpr int kMaxDecks    = kMaxChannels; // legacy alias
    /// Channel strip the sampler engine writes into (Phase 11): gets
    /// gain/EQ/filter/fader/cue/send for free through chParam(4,*).
    static constexpr int kSamplerChannel = 4;

    MixerEngine();
    void prepare(double sampleRate) noexcept; // allocate + init (non-RT)

    // ---- channel access ----
    mixer::ChannelStrip& channel(int i) noexcept { return strips_[i & (kMaxChannels - 1)]; }
    const mixer::ChannelStrip& channel(int i) const noexcept { return strips_[i & (kMaxChannels - 1)]; }
    int numActiveChannels() const noexcept { return active_; }
    void setActiveChannels(int n) noexcept; // non-RT config (<= kMaxChannels)

    // ---- channel source routing (§3/§34) -----------------------------
    /// What feeds channel `ch`. Non-RT: atomically published, takes
    /// effect on the next audio block. Remapping never locks the
    /// callback — a source index that is out of range simply renders
    /// silence until the config catches up.
    void setChannelSource(int ch, mixer::ChSource src) noexcept;
    mixer::ChSource channelSource(int ch) const noexcept {
        return mixer::unpackSource(
            chSource_[ch & (kMaxChannels - 1)].load(std::memory_order_relaxed));
    }
    /// Convenience: deck d → channel ch (any deck on any channel, §34).
    void mapDeckToChannel(int deck, int ch) noexcept {
        setChannelSource(ch, mixer::ChSource::deck(deck));
    }
    /// §32/§33 — deck count mode: decks 0..n-1 map to channels 0..n-1,
    /// channels n..3 go silent (skipped in the render loop — near-zero
    /// cost, not deleted). The sampler channel (4) always stays live.
    void setDeckMode(int n) noexcept;
    int deckMode() const noexcept { return deckMode_.load(std::memory_order_relaxed); }
    // -------------------------------------------------------------------

    // ---- bus access ----
    mixer::MasterBus&      master() noexcept      { return master_; }
    mixer::HeadphoneBus&   phones() noexcept      { return phones_; }
    mixer::CrossfaderEngine& xf() noexcept        { return xf_; }
    mixer::CueManager&     cues() noexcept        { return cueMgr_; }
    const mixer::RoutingEngine& routing() const noexcept { return routing_; }

    // ---- FX engine (insert racks, send/return buses, master rack) ----
    fx::FXEngine&       fx() noexcept       { return fx_; }
    const fx::FXEngine& fx() const noexcept { return fx_; }
    /// Which deck feeds tempo to send/master racks (sync master).
    void setFxSyncMaster(int deck) noexcept { fx_.setSyncMaster(deck); }
    /// Non-RT housekeeping: frees FX processors retired by the RT
    /// thread after effect swaps. Call periodically from a control
    /// thread (MixerController's loader does it per swap).
    void fxHousekeeping() noexcept { fx_.collectTrash(); }

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
    /// `sampler` (nullable) renders into channel kSamplerChannel —
    /// needs setActiveChannels(kSamplerChannel+1) to be audible.
    void render(float* const* out, int numOut, int numSamples,
                DeckEngine* decks, int numDecks,
                rt::AudioStats& stats,
                sampler::SamplerRt* sampler = nullptr) noexcept;

    const MasterStats& masterStats() const noexcept { return mstats_; }

    /// §42 — latency seam: report the insert-rack latency each channel
    /// presents (0 today — every processor is latency-free; the accessor
    /// is the contract for future processors + PDC).
    int latencySamples(int ch) const noexcept {
        return fx_.targetLatency(ch < 0 ? 0 : (ch > 3 ? 3 : ch));
    }
    int masterLatencySamples() const noexcept {
        return fx_.targetLatency((int)fx::FXTarget::Master);
    }

    /// §40 — fill a diagnostics snapshot from RT state + atomic meters.
    /// Control-thread only (reads everything once; no locks needed —
    /// every field is atomic or RT-published).
    void snapshot(mixer::MixerBusDiag& bus,
                  mixer::MixerChannelDiag* chans, int nCh) const noexcept;
    std::string diagnosticReport() const noexcept;

    /// Deck index -> crossfader side: 0,2 = A; 1,3 = B.
    static bool deckOnSideA(int deck) noexcept { return (deck % 2) == 0; }

    /// Post-master program bus — valid after render() inside the same
    /// callback (limiter/comp already applied). Read-only taps only.
    const float* masterL() const { return mixL_.data(); }
    const float* masterR() const { return mixR_.data(); }

    /// Phase 13: physical output mapping (non-RT set, atomic publish).
    /// master→[0,1], cue→[2,3]; -1 disables a pair. Defaults {0,1,2,3}.
    void setOutputMap(int mL, int mR, int cL, int cR) noexcept {
        outMap_[0].store(mL, std::memory_order_relaxed);
        outMap_[1].store(mR, std::memory_order_relaxed);
        outMap_[2].store(cL, std::memory_order_relaxed);
        outMap_[3].store(cR, std::memory_order_relaxed);
    }
    int outMap(int i) const noexcept {
        return outMap_[i & 3].load(std::memory_order_relaxed);
    }

private:
    void rebuildRouting() noexcept;

    double rate_ { 48000.0 };
    int    active_ { 4 };

    mixer::ChannelStrip    strips_[kMaxChannels];
    mixer::CrossfaderEngine xf_;
    mixer::MasterBus        master_;
    mixer::HeadphoneBus     phones_;
    mixer::CueManager       cueMgr_;
    mixer::RoutingEngine    routing_;
    fx::FXEngine            fx_;

    std::vector<float> mixL_, mixR_, cueL_, cueR_; // bus scratch (preallocated)
    std::array<std::atomic<int>,4> outMap_ { 0, 1, 2, 3 }; // mL,mR,cL,cR
    /// Channel source table (packed ChSource). Written on the control
    /// thread, read every block — atomics only, never a lock.
    std::array<std::atomic<uint32_t>, kMaxChannels> chSource_ {};
    std::atomic<int> deckMode_ { 4 };

    MasterStats mstats_; // legacy published struct (filled from buses)
};

} // namespace onda
