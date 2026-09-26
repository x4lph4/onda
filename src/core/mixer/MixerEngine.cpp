#include "MixerEngine.h"

#include <algorithm>
#include <cstring>

#include "../engine/DeckEngine.h"

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
}

void MixerEngine::prepare(double sampleRate) noexcept {
    rate_ = sampleRate;
    for (auto& s : strips_) s.prepare(sampleRate);
    xf_.prepare(sampleRate);
    master_.prepare(sampleRate);
    phones_.prepare(sampleRate);
    rebuildRouting();
}

void MixerEngine::setActiveChannels(int n) noexcept {
    active_ = std::clamp(n, 1, kMaxChannels);
    rebuildRouting();
}

void MixerEngine::rebuildRouting() noexcept {
    using NT = mixer::RoutingEngine::NodeType;
    using TP = mixer::RoutingEngine::Tap;
    routing_.clear();
    const int xf     = routing_.addNode(NT::Crossfader, -1, 0);
    const int master = routing_.addNode(NT::MasterBus,  -1, 0);
    const int cue    = routing_.addNode(NT::CueBus,     -1, 0);
    const int out    = routing_.addNode(NT::Output,      0, 0);
    const int outCue = routing_.addNode(NT::Output,      1, 0);
    for (int i = 0; i < active_; ++i) {
        const int src   = routing_.addNode(NT::Source,       i, 0);
        const int strip = routing_.addNode(NT::ChannelStrip, i, 0);
        routing_.addEdge(src,   strip, TP::PostStrip);
        routing_.addEdge(strip, xf,    TP::PostFader);
        routing_.addEdge(strip, cue,   TP::PreFader);
    }
    routing_.addEdge(xf,     master, TP::PostFader);
    routing_.addEdge(master, out,    TP::PostFader);
    routing_.addEdge(cue,    outCue, TP::PreFader);
}

void MixerEngine::setParam(uint16_t id, float v) noexcept {
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
    }
}

void MixerEngine::render(float* const* out, int numOut, int n,
                         DeckEngine* decks, int numDecks,
                         rt::AudioStats& stats) noexcept {
    n = std::min(n, (int)AudioFormat::kMaxBlockFrames);
    const int chans = std::min({ active_, numDecks, kMaxChannels });
    if (chans > active_) {} // unreachable, kept for clarity

    // 1) each source renders into its strip's input
    for (int d = 0; d < chans; ++d)
        decks[d].renderPcm(strips_[d].inL(), strips_[d].inR(), n);

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

    // 3) master bus in place -> outputs 0-1
    master_.process(mixL_.data(), mixR_.data(), n);
    float* L = numOut > 0 ? out[0] : nullptr;
    float* R = numOut > 1 ? out[1] : nullptr;
    if (L != nullptr) std::memcpy(L, mixL_.data(), (size_t)n * sizeof(float));
    if (R != nullptr) std::memcpy(R, mixR_.data(), (size_t)n * sizeof(float));

    // legacy clip accounting (MasterStats + AudioStats still published)
    const uint64_t clips = master_.meters().clips.load(std::memory_order_relaxed);
    const uint64_t prev  = mstats_.clipCount.load(std::memory_order_relaxed);
    if (clips > prev)
        stats.clipCount.fetch_add(clips - prev, std::memory_order_relaxed);
    mstats_.clipCount.store(clips, std::memory_order_relaxed);

    // 4) phones bus -> outputs 2-3
    const bool cue = numOut >= 4 && out[2] != nullptr && out[3] != nullptr;
    if (cue)
        phones_.process(cueL_.data(), cueR_.data(),
                        mixL_.data(), mixR_.data(),
                        out[2], out[3], n);
    for (int ch = 4; ch < numOut; ++ch)
        if (out[ch] != nullptr)
            std::memset(out[ch], 0, (size_t)n * sizeof(float));

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

} // namespace onda
