#pragma once

#include <array>
#include <cstdint>

#include "MixerTypes.h"
#include "ParameterRegistry.h"

namespace onda::mixer {

/// §38 — complete mixer state snapshot: every channel's gain/EQ/
/// filter/fader/cue/xf/mute/solo/sends/phase, every bus parameter,
/// the channel-source routing table and the deck-mode. Versioned POD
/// — presets serialize this; a full snapshot history system can layer
/// on top later without changing the capture format.
///
/// Capture/apply travel through MixerController (control plane): the
/// scene reads the *commanded* values (what the user set) rather than
/// the smoothed RT state — snapshots round-trip exactly what was set.
struct MixerScene {
    /// v2 (Phase 24): + per-channel pan/eqMode/filterType, + boothGain.
    /// Older captures lack those fields — defaults below are the v1
    /// behavior (pan center, Standard EQ, Position filter, booth unity),
    /// so applying a v1 scene reproduces a v1 mixer exactly.
    static constexpr int kVersion = 2;
    static constexpr int kMaxCh   = ParameterRegistry::kMaxChannels;

    struct Channel {
        float gain    { 1.0f };
        float low     { 0.0f }, mid { 0.0f }, high { 0.0f }, midLow { 0.0f };
        float filterPos { 0.0f }, filterRes { 0.707f };
        float pan     { 0.0f };           // v2 — balance -1..+1
        float eqMode  { 0.0f };           // v2 — EqMode enum
        float filterType { 0.0f };        // v2 — FilterType enum
        float fader   { 1.0f };
        float cue     { 0.0f }, mute { 0.0f }, solo { 0.0f };
        float xfAssign{ 2.0f };           // Thru
        float faderCurve { 0.0f };
        float sendA   { 0.0f }, sendB { 0.0f };
        float phase   { 0.0f };
        ChSource source { ChSourceType::Silence, 0 };
    };

    float xfPos    { 0.0f };
    float xfCurve  { 1.0f };              // EqualPower
    float masterGain { 1.0f }, masterMute { 0.0f };
    float limiterOn { 0.0f }, limiterThr { 0.891f };
    float cueVol { 0.8f }, cueMix { 0.5f }, masterCue { 0.0f };
    float fxRetA { 1.0f }, fxRetB { 1.0f };
    float boothGain { 1.0f };             // v2 — booth monitor level
    int   deckMode { 4 };                 // visible deck channels (2/4)
    std::array<Channel, kMaxCh> ch {};
};

/// Channel param kinds captured by a scene, in a stable order.
constexpr ChParam kSceneChParams[] = {
    ChParam::Gain, ChParam::Low, ChParam::Mid, ChParam::High,
    ChParam::MidLow, ChParam::Filter, ChParam::FilterRes,
    ChParam::Pan, ChParam::EqMode, ChParam::FilterType,
    ChParam::Fader, ChParam::Cue, ChParam::Mute, ChParam::Solo,
    ChParam::XfAssign, ChParam::FaderCurve,
    ChParam::SendA, ChParam::SendB, ChParam::PhaseInvert,
};
constexpr int kSceneChParamCount = (int)(sizeof(kSceneChParams) / sizeof(kSceneChParams[0]));

constexpr BusParam kSceneBusParams[] = {
    BusParam::CrossfaderPos, BusParam::XfCurve, BusParam::MasterGain,
    BusParam::MasterMute, BusParam::LimiterOn, BusParam::LimiterThreshold,
    BusParam::CueVolume, BusParam::CueMix, BusParam::MasterCue,
    BusParam::FxReturnA, BusParam::FxReturnB, BusParam::BoothGain,
};
constexpr int kSceneBusParamCount = (int)(sizeof(kSceneBusParams) / sizeof(kSceneBusParams[0]));

/// Read one channel field back from its ChParam kind.
inline float& sceneField(MixerScene::Channel& c, ChParam k) noexcept {
    switch (k) {
        case ChParam::Gain:      return c.gain;
        case ChParam::Low:       return c.low;
        case ChParam::Mid:       return c.mid;
        case ChParam::High:      return c.high;
        case ChParam::MidLow:    return c.midLow;
        case ChParam::Filter:    return c.filterPos;
        case ChParam::FilterRes: return c.filterRes;
        case ChParam::Fader:     return c.fader;
        case ChParam::Cue:       return c.cue;
        case ChParam::Mute:      return c.mute;
        case ChParam::Solo:      return c.solo;
        case ChParam::XfAssign:  return c.xfAssign;
        case ChParam::FaderCurve:return c.faderCurve;
        case ChParam::SendA:     return c.sendA;
        case ChParam::SendB:     return c.sendB;
        case ChParam::PhaseInvert:return c.phase;
        case ChParam::Pan:       return c.pan;
        case ChParam::EqMode:    return c.eqMode;
        case ChParam::FilterType:return c.filterType;
    }
    return c.gain; // unreachable — every kind is mapped
}
inline const float& sceneField(const MixerScene::Channel& c, ChParam k) noexcept {
    return sceneField(const_cast<MixerScene::Channel&>(c), k);
}

inline float& sceneField(MixerScene& s, BusParam k) noexcept {
    switch (k) {
        case BusParam::CrossfaderPos:    return s.xfPos;
        case BusParam::XfCurve:          return s.xfCurve;
        case BusParam::MasterGain:       return s.masterGain;
        case BusParam::MasterMute:       return s.masterMute;
        case BusParam::LimiterOn:        return s.limiterOn;
        case BusParam::LimiterThreshold: return s.limiterThr;
        case BusParam::CueVolume:        return s.cueVol;
        case BusParam::CueMix:           return s.cueMix;
        case BusParam::MasterCue:        return s.masterCue;
        case BusParam::FxReturnA:        return s.fxRetA;
        case BusParam::FxReturnB:        return s.fxRetB;
        case BusParam::BoothGain:        return s.boothGain;
    }
    return s.xfPos; // unreachable
}
inline const float& sceneField(const MixerScene& s, BusParam k) noexcept {
    return sceneField(const_cast<MixerScene&>(s), k);
}

} // namespace onda::mixer
