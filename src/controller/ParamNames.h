#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/fx/FXTypes.h"
#include "core/mixer/MixerTypes.h"
#include "core/mixer/ParameterRegistry.h"

namespace onda::ctrl {

/// Stable name ↔ ParameterRegistry id mapping used by profiles,
/// Learn Mode and diagnostics. Names are generated deterministically
/// (no hand-written table to drift out of sync):
///
///   Channels:   CH_<n>_<KIND>      e.g. CH_0_GAIN, CH_1_FADER
///   Bus/global: CROSSFADER, MASTER_GAIN, CUE_VOLUME, …
///   FX slots:   FX_<T>_<S>_<KIND>  e.g. FX_DECK_A_0_MIX, FX_MASTER_1_P3
///
/// Unknown names → 0xFFFF (kInvalidParam).

inline constexpr uint16_t kInvalidParam = 0xFFFF;

/// Deck-engine parameter space (0x8000+): continuous deck controls
/// that live outside the mixer registry — tempo pitch and pitch
/// range. Routed by the host port to rt::SetPitch / SetPitchRange.
///   id = kDeckParamBase + deck*2 + kind (0 pitch, 1 pitchRange)
///   Names: DECK_<n>_PITCH, DECK_<n>_PITCH_RANGE
inline constexpr uint16_t kDeckParamBase = 0x8000;
inline constexpr int      kDeckParamPitch      = 0;
inline constexpr int      kDeckParamPitchRange = 1;

inline constexpr uint16_t deckParamId(int deck, int kind) {
    return (uint16_t)(kDeckParamBase + deck * 2 + kind);
}
inline constexpr bool isDeckParam(uint16_t id) {
    return id >= kDeckParamBase && id < kDeckParamBase + 0x40;
}
inline constexpr int deckParamDeck(uint16_t id) {
    return (id - kDeckParamBase) / 2;
}
inline constexpr int deckParamKind(uint16_t id) {
    return (id - kDeckParamBase) & 1;
}

/// Sample-slot parameters (Phase 11) — ids in the 0x8100 space so
/// mixer channels (0x0000+), FX (0x1000+) and deck params (0x8000+)
/// stay disjoint. Routed by the host port to SamplerEngine.
///   id = kSampleParamBase + slot*8 + kind
///   Names: SMP_<n>_VOL / PAN / GAIN / PITCH / MODE / FILTER
inline constexpr uint16_t kSampleParamBase = 0x8100;
inline constexpr int      kSampleParamStride = 8;
inline constexpr int      kSampleParamMaxSlots = 32;
enum SampleParamKind : int {
    kSmpVol = 0, kSmpPan, kSmpGain, kSmpPitch,
    kSmpMode, kSmpFilter, kSmpQuant, kSmpSync,
};
inline constexpr uint16_t sampleParamId(int slot, int kind) {
    return (uint16_t)(kSampleParamBase +
                      slot * kSampleParamStride + kind);
}
inline constexpr bool isSampleParam(uint16_t id) {
    return id >= kSampleParamBase &&
           id < kSampleParamBase +
                kSampleParamMaxSlots * kSampleParamStride;
}
inline constexpr int sampleParamSlot(uint16_t id) {
    return (id - kSampleParamBase) / kSampleParamStride;
}
inline constexpr int sampleParamKind(uint16_t id) {
    return (id - kSampleParamBase) % kSampleParamStride;
}

namespace detail {

inline const char* chKindName(mixer::ChParam k) {
    switch (k) {
        case mixer::ChParam::Gain:        return "GAIN";
        case mixer::ChParam::Low:         return "LOW";
        case mixer::ChParam::Mid:         return "MID";
        case mixer::ChParam::High:        return "HIGH";
        case mixer::ChParam::MidLow:      return "MIDLOW";
        case mixer::ChParam::Filter:      return "FILTER";
        case mixer::ChParam::Fader:       return "FADER";
        case mixer::ChParam::Cue:         return "CUE";
        case mixer::ChParam::Mute:        return "MUTE";
        case mixer::ChParam::Solo:        return "SOLO";
        case mixer::ChParam::XfAssign:    return "XFASSIGN";
        case mixer::ChParam::FaderCurve:  return "FADERCURVE";
        case mixer::ChParam::SendA:       return "SENDA";
        case mixer::ChParam::SendB:       return "SENDB";
        case mixer::ChParam::PhaseInvert: return "PHASEINVERT";
    }
    return "?";
}

inline bool chKindFromName(const std::string& s, mixer::ChParam& k) {
    static const std::pair<const char*, mixer::ChParam> tab[] = {
        {"GAIN", mixer::ChParam::Gain}, {"LOW", mixer::ChParam::Low},
        {"MID", mixer::ChParam::Mid}, {"HIGH", mixer::ChParam::High},
        {"MIDLOW", mixer::ChParam::MidLow},
        {"FILTER", mixer::ChParam::Filter},
        {"FADER", mixer::ChParam::Fader}, {"CUE", mixer::ChParam::Cue},
        {"MUTE", mixer::ChParam::Mute}, {"SOLO", mixer::ChParam::Solo},
        {"XFASSIGN", mixer::ChParam::XfAssign},
        {"FADERCURVE", mixer::ChParam::FaderCurve},
        {"SENDA", mixer::ChParam::SendA}, {"SENDB", mixer::ChParam::SendB},
        {"PHASEINVERT", mixer::ChParam::PhaseInvert},
    };
    for (auto& [n, v] : tab)
        if (s == n) { k = v; return true; }
    return false;
}

inline const char* fxTargetName(int t) {
    switch ((fx::FXTarget)t) {
        case fx::FXTarget::DeckA:  return "DECK_A";
        case fx::FXTarget::DeckB:  return "DECK_B";
        case fx::FXTarget::DeckC:  return "DECK_C";
        case fx::FXTarget::DeckD:  return "DECK_D";
        case fx::FXTarget::SendA:  return "SEND_A";
        case fx::FXTarget::SendB:  return "SEND_B";
        case fx::FXTarget::Master: return "MASTER";
        case fx::FXTarget::Spare:  return "SPARE";
        case fx::FXTarget::kCount: break;
    }
    return "?";
}

inline int fxTargetFromName(const std::string& s) {
    static const char* names[] = {"DECK_A","DECK_B","DECK_C","DECK_D",
                                  "SEND_A","SEND_B","MASTER","SPARE"};
    for (int i = 0; i < 8; ++i)
        if (s == names[i]) return i;
    return -1;
}

inline const char* fxKindName(fx::FXSlotKind k) {
    switch (k) {
        case fx::FXSlotKind::Type:     return "TYPE";
        case fx::FXSlotKind::Enabled:  return "ENABLED";
        case fx::FXSlotKind::Mix:      return "MIX";
        case fx::FXSlotKind::InGain:   return "INGAIN";
        case fx::FXSlotKind::OutGain:  return "OUTGAIN";
        case fx::FXSlotKind::SyncMode: return "SYNCMODE";
        case fx::FXSlotKind::Division: return "DIVISION";
        case fx::FXSlotKind::P1: return "P1"; case fx::FXSlotKind::P2: return "P2";
        case fx::FXSlotKind::P3: return "P3"; case fx::FXSlotKind::P4: return "P4";
        case fx::FXSlotKind::P5: return "P5"; case fx::FXSlotKind::P6: return "P6";
        case fx::FXSlotKind::P7: return "P7"; case fx::FXSlotKind::P8: return "P8";
        case fx::FXSlotKind::kCount: break;
    }
    return "?";
}

inline bool fxKindFromName(const std::string& s, fx::FXSlotKind& k) {
    static const std::pair<const char*, fx::FXSlotKind> tab[] = {
        {"TYPE", fx::FXSlotKind::Type},
        {"ENABLED", fx::FXSlotKind::Enabled},
        {"MIX", fx::FXSlotKind::Mix},
        {"INGAIN", fx::FXSlotKind::InGain},
        {"OUTGAIN", fx::FXSlotKind::OutGain},
        {"SYNCMODE", fx::FXSlotKind::SyncMode},
        {"DIVISION", fx::FXSlotKind::Division},
        {"P1", fx::FXSlotKind::P1}, {"P2", fx::FXSlotKind::P2},
        {"P3", fx::FXSlotKind::P3}, {"P4", fx::FXSlotKind::P4},
        {"P5", fx::FXSlotKind::P5}, {"P6", fx::FXSlotKind::P6},
        {"P7", fx::FXSlotKind::P7}, {"P8", fx::FXSlotKind::P8},
    };
    for (auto& [n, v] : tab)
        if (s == n) { k = v; return true; }
    return false;
}

/// split "A_B_C" — keeps '_' semantics (targets contain '_').
inline std::vector<std::string> split(const std::string& s, char d) {
    std::vector<std::string> out;
    size_t a = 0;
    for (;;) {
        const size_t b = s.find(d, a);
        if (b == std::string::npos) { out.push_back(s.substr(a)); break; }
        out.push_back(s.substr(a, b - a));
        a = b + 1;
    }
    return out;
}

} // namespace detail

/// name → ParameterRegistry id (kInvalidParam when unknown).
inline uint16_t paramIdFromName(const std::string& name) {
    // DECK_<n>_PITCH[_RANGE] — split keeps "PITCH" and "RANGE" parts
    if (name.rfind("DECK_", 0) == 0) {
        auto p = detail::split(name, '_');
        if (p.size() == 3 && p[0] == "DECK" && p[2] == "PITCH") {
            const int d = std::atoi(p[1].c_str());
            if (d >= 0 && d < 32)
                return deckParamId(d, kDeckParamPitch);
        }
        if (p.size() == 4 && p[0] == "DECK" && p[2] == "PITCH" &&
            p[3] == "RANGE") {
            const int d = std::atoi(p[1].c_str());
            if (d >= 0 && d < 32)
                return deckParamId(d, kDeckParamPitchRange);
        }
        return kInvalidParam;
    }
    auto parts = detail::split(name, '_');
    if (parts.size() == 3 && parts[0] == "CH") {
        const int ch = std::atoi(parts[1].c_str());
        mixer::ChParam k;
        if (ch >= 0 && ch < mixer::ParameterRegistry::kMaxChannels &&
            detail::chKindFromName(parts[2], k))
            return mixer::chParam(ch, k);
        return kInvalidParam;
    }
    if (name.rfind("FX_", 0) == 0) {
        // FX_<TARGET>_<SLOT>_<KIND> — target names contain '_', so
        // match the target prefix first, then split the tail
        static const char* targets[] = {
            "DECK_A","DECK_B","DECK_C","DECK_D",
            "SEND_A","SEND_B","MASTER","SPARE" };
        for (int t = 0; t < 8; ++t) {
            const std::string pre =
                std::string("FX_") + targets[t] + "_";
            if (name.rfind(pre, 0) != 0) continue;
            const std::string tail = name.substr(pre.size());
            const size_t u = tail.find('_');
            if (u == std::string::npos) continue;
            const int s = std::atoi(tail.substr(0, u).c_str());
            fx::FXSlotKind k;
            if (s >= 0 && s < 16 &&
                detail::fxKindFromName(tail.substr(u + 1), k))
                return fx::paramId(t, s, k);
        }
        return kInvalidParam;
    }
    static const std::pair<const char*, uint16_t> bus[] = {
        {"CROSSFADER",         mixer::ParameterRegistry::CROSSFADER},
        {"XF_CURVE",           mixer::ParameterRegistry::XF_CURVE},
        {"MASTER_GAIN",        mixer::ParameterRegistry::MASTER_GAIN},
        {"MASTER_MUTE",        mixer::ParameterRegistry::MASTER_MUTE},
        {"LIMITER_ON",         mixer::ParameterRegistry::LIMITER_ON},
        {"LIMITER_THRESHOLD",  mixer::ParameterRegistry::LIMITER_THR},
        {"CUE_VOLUME",         mixer::ParameterRegistry::CUE_VOLUME},
        {"CUE_MIX",            mixer::ParameterRegistry::CUE_MIX},
        {"FX_RETURN_A",        mixer::ParameterRegistry::FX_RETURN_A},
        {"FX_RETURN_B",        mixer::ParameterRegistry::FX_RETURN_B},
    };
    for (auto& [n, id] : bus)
        if (name == n) return id;
    return kInvalidParam;
}

/// id → canonical name ("" for unknown ids).
inline std::string paramNameFromId(uint16_t id) {
    if (isDeckParam(id)) {
        const char* k = deckParamKind(id) == kDeckParamPitchRange
                        ? "_PITCH_RANGE" : "_PITCH";
        return "DECK_" + std::to_string(deckParamDeck(id)) + k;
    }
    if (fx::isFxParam(id)) {
        return "FX_" + std::string(detail::fxTargetName(fx::fxTarget(id)))
             + "_" + std::to_string(fx::fxSlot(id))
             + "_" + detail::fxKindName(fx::fxKindOf(id));
    }
    if (id < (uint16_t)mixer::BusParam::CrossfaderPos) {
        const int ch = id >> 5;
        const auto k = (mixer::ChParam)(id & 0x1F);
        if (ch < mixer::ParameterRegistry::kMaxChannels &&
            (int)k < (int)mixer::ChParam::PhaseInvert + 1)
            return "CH_" + std::to_string(ch) + "_" +
                   detail::chKindName(k);
        return "";
    }
    switch ((mixer::BusParam)id) {
        case mixer::BusParam::CrossfaderPos:    return "CROSSFADER";
        case mixer::BusParam::XfCurve:          return "XF_CURVE";
        case mixer::BusParam::MasterGain:       return "MASTER_GAIN";
        case mixer::BusParam::MasterMute:       return "MASTER_MUTE";
        case mixer::BusParam::LimiterOn:        return "LIMITER_ON";
        case mixer::BusParam::LimiterThreshold: return "LIMITER_THRESHOLD";
        case mixer::BusParam::CueVolume:        return "CUE_VOLUME";
        case mixer::BusParam::CueMix:           return "CUE_MIX";
        case mixer::BusParam::FxReturnA:        return "FX_RETURN_A";
        case mixer::BusParam::FxReturnB:        return "FX_RETURN_B";
    }
    return "";
}

} // namespace onda::ctrl
