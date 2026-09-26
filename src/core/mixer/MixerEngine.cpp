#include "MixerEngine.h"

#include <algorithm>
#include <cstring>

#include "../engine/DeckEngine.h"
#include "../sampler/SamplerRt.h"

namespace onda {

MixerEngine::MixerEngine()
    : mixL_(AudioFormat::kMaxBlockFrames, 0.0f),
      mixR_(AudioFormat::kMaxBlockFrames, 0.0f),
      cueL_(AudioFormat::kMaxBlockFrames, 0.0f),
      cueR_(AudioFormat::kMaxBlockFrames, 0.0f) {
    // default crossfader assignments: ch0/2 -> A, ch1/3 -> B, rest thru
    for (int i = 0; i < kMaxChannels; ++i)
        strips_[i].setXfAssign(i < 4 ? (deckOnSideA(i) ? mixer::XfAssign::A
                                                      : mixer::XfAssign::B)
                                     : mixer::XfAssign::Thru);
    // default source table: deck i -> ch i (0..3), sampler -> ch 4,
    // channels 5..7 silent until assigned (external-in/FX-return seam)
    for (int i = 0; i < kMaxChannels; ++i) {
        const mixer::ChSource s =
            i < 4 ? mixer::ChSource::deck(i)
          : i == kSamplerChannel ? mixer::ChSource::sampler()
                                 : mixer::ChSource::silence();
        chSource_[(size_t)i].store(mixer::packSource(s),
                                   std::memory_order_relaxed);
    }
}

void MixerEngine::prepare(double sampleRate) noexcept {
    rate_ = sampleRate;
    for (auto& s : strips_) s.prepare(sampleRate);
    xf_.prepare(sampleRate);
    master_.prepare(sampleRate);
    phones_.prepare(sampleRate);
    cueMgr_.prepare(sampleRate);
    fx_.prepare(sampleRate);
    rebuildRouting();
}

void MixerEngine::setActiveChannels(int n) noexcept {
    active_ = std::clamp(n, 1, kMaxChannels);
    rebuildRouting();
}

void MixerEngine::setChannelSource(int ch, mixer::ChSource src) noexcept {
    if (ch < 0 || ch >= kMaxChannels) return;
    chSource_[(size_t)ch].store(mixer::packSource(src),
                                std::memory_order_relaxed);
    rebuildRouting();
}

void MixerEngine::setDeckMode(int n) noexcept {
    n = std::clamp(n, 1, 4);
    deckMode_.store(n, std::memory_order_relaxed);
    for (int i = 0; i < 4; ++i) {
        const mixer::ChSource s = i < n ? mixer::ChSource::deck(i)
                                        : mixer::ChSource::silence();
        chSource_[(size_t)i].store(mixer::packSource(s),
                                   std::memory_order_relaxed);
    }
    rebuildRouting();
}

void MixerEngine::rebuildRouting() noexcept {
    using NT = mixer::RoutingEngine::NodeType;
    using TP = mixer::RoutingEngine::Tap;
    routing_.clear();
    const int xf     = routing_.addNode(NT::Crossfader, -1, 0);
    const int mfx    = routing_.addNode(NT::FxReturn,    6, 0); // master insert rack
    const int master = routing_.addNode(NT::MasterBus,  -1, 0);
    const int cue    = routing_.addNode(NT::CueBus,     -1, 0);
    const int out    = routing_.addNode(NT::Output,      0, 0);
    const int outCue = routing_.addNode(NT::Output,      1, 0);
    const int send[2] = { routing_.addNode(NT::FxSend,   0, 0),
                          routing_.addNode(NT::FxSend,   1, 0) };
    const int ret[2]  = { routing_.addNode(NT::FxReturn, 0, 0),
                          routing_.addNode(NT::FxReturn, 1, 0) };
    for (int i = 0; i < active_; ++i) {
        const auto srcTag = [](mixer::ChSourceType t) {
            switch (t) {
                case mixer::ChSourceType::Deck:     return (uint32_t)'deck';
                case mixer::ChSourceType::Sampler:  return (uint32_t)'smpl';
                case mixer::ChSourceType::External: return (uint32_t)'ext ';
                default:                            return (uint32_t)'sil ';
            }
        };
        const auto src    = channelSource(i);
        const int srcN    = routing_.addNode(NT::Source,       i,
                                             srcTag(src.type));
        const int ins     = routing_.addNode(NT::FxReturn,  10 + i, 0); // insert rack
        const int strip   = routing_.addNode(NT::ChannelStrip, i, 0);
        routing_.addEdge(srcN,  ins,    TP::PostStrip);
        routing_.addEdge(ins,   strip,  TP::PostStrip);
        routing_.addEdge(strip, xf,     TP::PostFader);
        routing_.addEdge(strip, cue,    TP::PreFader);
        routing_.addEdge(strip, send[0],TP::PostFader);
        routing_.addEdge(strip, send[1],TP::PostFader);
    }
    routing_.addEdge(send[0], ret[0], TP::PostStrip);
    routing_.addEdge(send[1], ret[1], TP::PostStrip);
    routing_.addEdge(ret[0],  master, TP::PostFader);
    routing_.addEdge(ret[1],  master, TP::PostFader);
    routing_.addEdge(xf,     mfx,    TP::PostFader);
    routing_.addEdge(mfx,    master, TP::PostFader);
    routing_.addEdge(master, out,    TP::PostFader);
    routing_.addEdge(cue,    outCue, TP::PreFader);
}

void MixerEngine::setParam(uint16_t id, float v) noexcept {
    if (fx::isFxParam(id)) {          // 0x1000 FX id space
        fx_.setParam(id, v);
        return;
    }
    if (id < (uint16_t)mixer::BusParam::CrossfaderPos) {
        const int ch = id >> 5;
        auto& s = strips_[ch & (kMaxChannels - 1)];
        switch ((mixer::ChParam)(id & 0x1f)) {
            case mixer::ChParam::Gain:   s.setInputGain(v); break;
            case mixer::ChParam::Low:    s.eq().setGainDb(0, v); break;
            case mixer::ChParam::Mid:    s.eq().setGainDb(1, v); break;
            case mixer::ChParam::High:   s.eq().setGainDb(2, v); break;
            case mixer::ChParam::MidLow: s.eq().setGainDb(3, v); break;
            case mixer::ChParam::Filter: s.filter().setPosition(v); break;
            case mixer::ChParam::FilterRes: s.filter().setResonance(v); break;
            case mixer::ChParam::Fader:  s.setFader(v); break;
            case mixer::ChParam::Cue:    s.setCue(v > 0.5f); break;
            case mixer::ChParam::Mute:   s.setMute(v > 0.5f); break;
            case mixer::ChParam::Solo:   s.setSolo(v > 0.5f); break;
            case mixer::ChParam::XfAssign:
                s.setXfAssign(v < 0.5f ? mixer::XfAssign::A
                            : v < 1.5f ? mixer::XfAssign::B
                                       : mixer::XfAssign::Thru);
                break;
            case mixer::ChParam::FaderCurve:
                s.setFaderCurve((mixer::FaderCurve)(int)v); break;
            case mixer::ChParam::SendA: s.setSendLevel(0, v); break;
            case mixer::ChParam::SendB: s.setSendLevel(1, v); break;
            case mixer::ChParam::PhaseInvert: s.setPhaseInvert(v > 0.5f); break;
        }
        return;
    }
    switch ((mixer::BusParam)id) {
        case mixer::BusParam::CrossfaderPos:    xf_.setPosition(v); break;
        case mixer::BusParam::XfCurve:          xf_.setCurve((XfCurve)(int)v); break;
        case mixer::BusParam::MasterGain:       master_.setGain(v); break;
        case mixer::BusParam::MasterMute:       master_.setMute(v > 0.5f); break;
        case mixer::BusParam::LimiterOn:        master_.limiter().setEnabled(v > 0.5f); break;
        case mixer::BusParam::LimiterThreshold: master_.limiter().setThreshold(v); break;
        case mixer::BusParam::CueVolume:        phones_.setVolume(v); break;
        case mixer::BusParam::CueMix:           phones_.setMix(v); break;
        case mixer::BusParam::FxReturnA:        fx_.setSendReturn(0, v); break;
        case mixer::BusParam::FxReturnB:        fx_.setSendReturn(1, v); break;
        case mixer::BusParam::MasterCue:        cueMgr_.setMasterCue(v > 0.5f); break;
    }
}

void MixerEngine::render(float* const* out, int numOut, int n,
                         DeckEngine* decks, int numDecks,
                         rt::AudioStats& stats,
                         sampler::SamplerRt* sampler) noexcept {
    n = std::min(n, (int)AudioFormat::kMaxBlockFrames);
    const int chans = std::min(active_, kMaxChannels);

    // 1) each channel's source descriptor decides what renders into
    //    its strip: any deck on any channel (§34 remap), the sampler,
    //    an external-in seam (silence until the device layer feeds it),
    //    or Silence (strip skipped — post taps zeroed, near-zero cost).
    for (int d = 0; d < chans; ++d) {
        const auto src = mixer::unpackSource(
            chSource_[(size_t)d].load(std::memory_order_relaxed));
        bool live = true;
        switch (src.type) {
            case mixer::ChSourceType::Deck:
                if (src.index >= 0 && src.index < numDecks)
                    decks[src.index].renderPcm(strips_[d].inL(),
                                               strips_[d].inR(), n),
                    fx_.setTempo(d, (float)decks[src.index].effBpm());
                else live = false;
                break;
            case mixer::ChSourceType::Sampler:
                if (sampler != nullptr) {
                    // SamplerRt sums into the buffers — start from zero
                    std::memset(strips_[d].inL(), 0, (size_t)n * sizeof(float));
                    std::memset(strips_[d].inR(), 0, (size_t)n * sizeof(float));
                    sampler->renderPcm(strips_[d].inL(), strips_[d].inR(), n);
                    fx_.setTempo(d, (float)sampler->clockBpm());
                } else live = false;
                break;
            case mixer::ChSourceType::External:
            case mixer::ChSourceType::Silence:
                live = false;
                break;
        }
        if (!live) {
            // Feed the strip silence: EQ/filter/smoothers still run on
            // zeros (meters decay, post taps go quiet) — the only work
            // skipped is the insert FX rack, which hibernates anyway.
            std::memset(strips_[d].inL(), 0, (size_t)n * sizeof(float));
            std::memset(strips_[d].inR(), 0, (size_t)n * sizeof(float));
            continue;
        }
        fx_.processDeckInsert(d, strips_[d].inL(), strips_[d].inR(), n);
    }

    // 2) advance xf smoothing across the block, then process strips
    xf_.tickPos(n);
    bool anySolo = false;
    for (int d = 0; d < chans; ++d)
        if (strips_[d].soloed()) anySolo = true;

    std::memset(mixL_.data(), 0, (size_t)n * sizeof(float));
    std::memset(mixR_.data(), 0, (size_t)n * sizeof(float));
    std::memset(cueL_.data(), 0, (size_t)n * sizeof(float));
    std::memset(cueR_.data(), 0, (size_t)n * sizeof(float));

    for (int d = 0; d < chans; ++d)
        strips_[d].process(n, mixL_.data(), mixR_.data(),
                           cueL_.data(), cueR_.data(), xf_, anySolo);

    // 2b) FX send/return buses: channel post-fader taps -> send racks
    //     -> return levels -> master sum
    for (int b = 0; b < fx::FXEngine::kSendBuses; ++b)
        fx_.sendReset(b, n);
    for (int d = 0; d < chans; ++d) {
        for (int b = 0; b < fx::FXEngine::kSendBuses; ++b) {
            const float g = strips_[d].sendGain(b, n);
            if (g > 1e-4f)
                fx_.sendAccum(b, strips_[d].postL(), strips_[d].postR(),
                              n, g);
        }
    }
    for (int b = 0; b < fx::FXEngine::kSendBuses; ++b)
        fx_.processSendBus(b, mixL_.data(), mixR_.data(), n);

    // 2c) master insert FX (pre master-gain/limiter — limiter stays last)
    fx_.processMaster(mixL_.data(), mixR_.data(), n);

    // 3) master bus in place -> outputs 0-1
    master_.process(mixL_.data(), mixR_.data(), n);

    // physical output map (Phase 13 AudioRouting — atomic published)
    const int mL = outMap_[0].load(std::memory_order_relaxed);
    const int mR = outMap_[1].load(std::memory_order_relaxed);
    const int cL = outMap_[2].load(std::memory_order_relaxed);
    const int cR = outMap_[3].load(std::memory_order_relaxed);

    // clear every channel first: unmapped outs must carry silence
    for (int ch = 0; ch < numOut; ++ch)
        if (out[ch] != nullptr)
            std::memset(out[ch], 0, (size_t)n * sizeof(float));

    float* L = (mL >= 0 && mL < numOut) ? out[mL] : nullptr;
    float* R = (mR >= 0 && mR < numOut) ? out[mR] : nullptr;
    if (L != nullptr) std::memcpy(L, mixL_.data(), (size_t)n * sizeof(float));
    if (R != nullptr) std::memcpy(R, mixR_.data(), (size_t)n * sizeof(float));

    // legacy clip accounting (MasterStats + AudioStats still published)
    const uint64_t clips = master_.meters().clips.load(std::memory_order_relaxed);
    const uint64_t prev  = mstats_.clipCount.load(std::memory_order_relaxed);
    if (clips > prev)
        stats.clipCount.fetch_add(clips - prev, std::memory_order_relaxed);
    mstats_.clipCount.store(clips, std::memory_order_relaxed);

    // 3b) master cue: post-master program -> cue sum (§17). Runs after
    //     the master bus so phones hear what the audience hears.
    cueMgr_.process(cueL_.data(), cueR_.data(),
                    mixL_.data(), mixR_.data(), n);

    // 4) phones bus -> mapped cue outputs
    const bool cue = cL >= 0 && cR >= 0 && cL < numOut && cR < numOut
                     && out[cL] != nullptr && out[cR] != nullptr;
    if (cue)
        phones_.process(cueL_.data(), cueR_.data(),
                        mixL_.data(), mixR_.data(),
                        out[cL], out[cR], n);

    // 5) publish legacy MasterStats from bus meters
    const auto& mm = master_.meters();
    mstats_.peakL.store(mm.peakL.load(std::memory_order_relaxed),
                        std::memory_order_relaxed);
    mstats_.peakR.store(mm.peakR.load(std::memory_order_relaxed),
                        std::memory_order_relaxed);
    mstats_.rmsL.store(mm.rmsL.load(std::memory_order_relaxed),
                       std::memory_order_relaxed);
    mstats_.rmsR.store(mm.rmsR.load(std::memory_order_relaxed),
                       std::memory_order_relaxed);
    mstats_.limiterGain.store(master_.limiter().gainReduction(),
                              std::memory_order_relaxed);
    mstats_.cueAvailable.store(cue, std::memory_order_relaxed);
}

void MixerEngine::snapshot(mixer::MixerBusDiag& bus,
                           mixer::MixerChannelDiag* chans,
                           int nCh) const noexcept {
    bus = mixer::MixerBusDiag{};
    bus.xfPos = xf_.position();
    bus.xfCurve = (int)xf_.curve();
    bus.masterGain = master_.gain();
    bus.limiterOn = master_.limiter().enabled();
    bus.limiterGain = master_.limiter().gainReduction();
    bus.cueVol = phones_.volume();
    bus.cueMix = phones_.mix();
    bus.masterCue = cueMgr_.masterCue();
    bus.deckMode = deckMode();
    bus.activeChannels = active_;
    bus.masterLatencySamples = masterLatencySamples();
    const auto& mm = master_.meters();
    bus.masterPeakL = mm.peakL.load(std::memory_order_relaxed);
    bus.masterPeakR = mm.peakR.load(std::memory_order_relaxed);
    bus.masterRmsL  = mm.rmsL.load(std::memory_order_relaxed);
    bus.masterRmsR  = mm.rmsR.load(std::memory_order_relaxed);
    bus.masterClips = mm.clips.load(std::memory_order_relaxed);
    const auto& cm = phones_.meters();
    bus.cuePeakL = cm.peakL.load(std::memory_order_relaxed);
    bus.cuePeakR = cm.peakR.load(std::memory_order_relaxed);
    for (int i = 0; i < std::min(nCh, kMaxChannels); ++i) {
        const auto& s = strips_[i];
        auto& d = chans[i];
        d = mixer::MixerChannelDiag{};
        d.active = i < active_;
        const auto src = channelSource(i);
        d.sourceType = (int)src.type; d.sourceIndex = src.index;
        d.gain = s.inputGain();
        d.lowDb = (float)s.eq().gainDb(0);
        d.midDb = (float)s.eq().gainDb(1);
        d.highDb = (float)s.eq().gainDb(2);
        d.filterPos = s.filter().position();
        d.filterRes = (float)s.filter().resonance();
        d.fader = s.fader();
        d.sendA = s.sendLevel(0); d.sendB = s.sendLevel(1);
        d.xfAssign = (int)s.xfAssign();
        d.cue = s.cueOn(); d.mute = s.muted();
        d.solo = s.soloed(); d.phase = s.phaseInverted();
        const auto& m = s.meters();
        d.peakL = m.peakL.load(std::memory_order_relaxed);
        d.peakR = m.peakR.load(std::memory_order_relaxed);
        d.rmsL  = m.rmsL.load(std::memory_order_relaxed);
        d.rmsR  = m.rmsR.load(std::memory_order_relaxed);
        d.holdL = m.holdL.load(std::memory_order_relaxed);
        d.holdR = m.holdR.load(std::memory_order_relaxed);
        d.clips = m.clips.load(std::memory_order_relaxed);
        d.latencySamples = latencySamples(i);
        if (d.solo) ++bus.soloCount;
        if (d.cue) ++bus.cueCount;
    }
}

std::string MixerEngine::diagnosticReport() const noexcept {
    mixer::MixerBusDiag bus;
    mixer::MixerChannelDiag chans[kMaxChannels];
    snapshot(bus, chans, kMaxChannels);
    return mixer::mixerReport(bus, chans, kMaxChannels);
}

} // namespace onda
