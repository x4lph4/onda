#pragma once

#include <array>
#include <cstdint>

#include "../fx/FXTypes.h"
#include "MixerTypes.h"

namespace onda::mixer {

/// Central description of every mixer parameter: ID, default, range,
/// resolution step and smoothing hint. Pure metadata table — no audio
/// state lives here — so UI, MIDI, HID, automation and tests can all
/// enumerate/validate parameters the same way.
///
/// Values themselves travel to the RT thread through the command queue
/// (rt::SetParam); the registry just defines what a legal value is.
struct ParamDef {
    uint16_t    id;
    const char* name;
    float       def;
    float       minV;
    float       maxV;
    float       step;      // UI/hardware resolution hint (0 = continuous)
    float       smoothMs;  // recommended smoothing (informational)
};

class ParameterRegistry {
public:
    static constexpr int kMaxChannels = 8;

    /// Well-known bus/global parameter IDs (ParamId-friendly aliases of
    /// the names required by the mixer spec).
    static constexpr uint16_t CROSSFADER    = (uint16_t)BusParam::CrossfaderPos;
    static constexpr uint16_t XF_CURVE      = (uint16_t)BusParam::XfCurve;
    static constexpr uint16_t MASTER_GAIN   = (uint16_t)BusParam::MasterGain;
    static constexpr uint16_t MASTER_MUTE   = (uint16_t)BusParam::MasterMute;
    static constexpr uint16_t LIMITER_ON    = (uint16_t)BusParam::LimiterOn;
    static constexpr uint16_t LIMITER_THR   = (uint16_t)BusParam::LimiterThreshold;
    static constexpr uint16_t CUE_VOLUME    = (uint16_t)BusParam::CueVolume;
    static constexpr uint16_t CUE_MIX       = (uint16_t)BusParam::CueMix;
    static constexpr uint16_t FX_RETURN_A   = (uint16_t)BusParam::FxReturnA;
    static constexpr uint16_t FX_RETURN_B   = (uint16_t)BusParam::FxReturnB;

    // ---- FX parameter ids (fx:: encoding, 0x1000 space) -------------
    using K = fx::FXSlotKind;
    static constexpr uint16_t fxId(int target, int slot, K kind) {
        return fx::paramId(target, slot, kind);
    }
    static constexpr uint16_t fxType    (int t, int s) { return fxId(t, s, K::Type); }
    static constexpr uint16_t fxEnabled (int t, int s) { return fxId(t, s, K::Enabled); }
    static constexpr uint16_t fxMix     (int t, int s) { return fxId(t, s, K::Mix); }
    static constexpr uint16_t fxInGain  (int t, int s) { return fxId(t, s, K::InGain); }
    static constexpr uint16_t fxOutGain (int t, int s) { return fxId(t, s, K::OutGain); }
    static constexpr uint16_t fxSyncMode(int t, int s) { return fxId(t, s, K::SyncMode); }
    static constexpr uint16_t fxDivision(int t, int s) { return fxId(t, s, K::Division); }
    /// Normalized 0..1 effect parameter (P1..P8 maps to param 0..7).
    static constexpr uint16_t fxParam(int t, int s, int p) {
        return fxId(t, s, (K)((int)K::P1 + (p & 7)));
    }

    // Spec-name aliases (deck A = target 0, slot 0, etc.)
    static constexpr int T_DECK_A = (int)fx::FXTarget::DeckA;
    static constexpr int T_DECK_B = (int)fx::FXTarget::DeckB;
    static constexpr int T_MASTER = (int)fx::FXTarget::Master;
    static constexpr int T_SEND_A = (int)fx::FXTarget::SendA;
    static constexpr int T_SEND_B = (int)fx::FXTarget::SendB;
    static constexpr uint16_t FX_A_TYPE = fx::paramId(T_DECK_A, 0, K::Type);
    static constexpr uint16_t FX_A_MIX  = fx::paramId(T_DECK_A, 0, K::Mix);
    static constexpr uint16_t FX_A_P1   = fx::paramId(T_DECK_A, 0, K::P1);
    static constexpr uint16_t FX_A_P2   = fx::paramId(T_DECK_A, 0, K::P2);
    static constexpr uint16_t FX_A_P3   = fx::paramId(T_DECK_A, 0, K::P3);

    // CHANNEL_x_* convenience builders (spec names)
    static constexpr uint16_t channelGain   (int ch) { return chParam(ch, ChParam::Gain); }
    static constexpr uint16_t channelLow    (int ch) { return chParam(ch, ChParam::Low); }
    static constexpr uint16_t channelMid    (int ch) { return chParam(ch, ChParam::Mid); }
    static constexpr uint16_t channelHigh   (int ch) { return chParam(ch, ChParam::High); }
    static constexpr uint16_t channelMidLow (int ch) { return chParam(ch, ChParam::MidLow); }
    static constexpr uint16_t channelFilter (int ch) { return chParam(ch, ChParam::Filter); }
    static constexpr uint16_t channelFader  (int ch) { return chParam(ch, ChParam::Fader); }
    static constexpr uint16_t channelCue    (int ch) { return chParam(ch, ChParam::Cue); }
    static constexpr uint16_t channelMute   (int ch) { return chParam(ch, ChParam::Mute); }
    static constexpr uint16_t channelSolo   (int ch) { return chParam(ch, ChParam::Solo); }
    static constexpr uint16_t channelXfAssign (int ch) { return chParam(ch, ChParam::XfAssign); }
    static constexpr uint16_t channelFaderCurve(int ch){ return chParam(ch, ChParam::FaderCurve); }
    static constexpr uint16_t channelSendA  (int ch) { return chParam(ch, ChParam::SendA); }
    static constexpr uint16_t channelSendB  (int ch) { return chParam(ch, ChParam::SendB); }

    /// Definition for a parameter id, or nullptr if unknown.
    static const ParamDef* find(uint16_t id) noexcept {
        if (fx::isFxParam(id)) {
            switch (fx::fxKindOf(id)) {
                case K::Type:     return &kFxType;
                case K::Enabled:  return &kFxEnabled;
                case K::Mix:      return &kFxMix;
                case K::InGain:   return &kFxInGain;
                case K::OutGain:  return &kFxOutGain;
                case K::SyncMode: return &kFxSync;
                case K::Division: return &kFxDiv;
                default:          return &kFxPn; // P1..P8 normalized
            }
        }
        if (id < (uint16_t)BusParam::CrossfaderPos) {
            const int ch   = id >> 5;
            const auto kind= (ChParam)(id & 0x1f);
            if (ch >= kMaxChannels) return nullptr;
            switch (kind) {
                case ChParam::Gain:      return &kChGain;
                case ChParam::Low:       return &kChLow;
                case ChParam::Mid:       return &kChMid;
                case ChParam::High:      return &kChHigh;
                case ChParam::MidLow:    return &kChMidLow;
                case ChParam::Filter:    return &kChFilter;
                case ChParam::Fader:     return &kChFader;
                case ChParam::Cue:       return &kChCue;
                case ChParam::Mute:      return &kChMute;
                case ChParam::Solo:      return &kChSolo;
                case ChParam::XfAssign:  return &kChXfAssign;
                case ChParam::FaderCurve:return &kChFaderCurve;
                case ChParam::SendA:     return &kChSendA;
                case ChParam::SendB:     return &kChSendB;
                case ChParam::PhaseInvert:return &kChPhase;
            }
            return nullptr;
        }
        switch ((BusParam)id) {
            case BusParam::CrossfaderPos:    return &kXfPos;
            case BusParam::XfCurve:          return &kXfCurve;
            case BusParam::MasterGain:       return &kMasterGain;
            case BusParam::MasterMute:       return &kMasterMute;
            case BusParam::LimiterOn:        return &kLimiterOn;
            case BusParam::LimiterThreshold: return &kLimiterThr;
            case BusParam::CueVolume:        return &kCueVol;
            case BusParam::CueMix:           return &kCueMix;
            case BusParam::FxReturnA:        return &kFxRetA;
            case BusParam::FxReturnB:        return &kFxRetB;
        }
        return nullptr;
    }

    static bool exists(uint16_t id) noexcept { return find(id) != nullptr; }

    /// Clamp/quantize a value per the parameter definition.
    static float sanitize(uint16_t id, float v) noexcept {
        const ParamDef* d = find(id);
        if (d == nullptr) return v;
        v = v < d->minV ? d->minV : (v > d->maxV ? d->maxV : v);
        if (d->step > 0.0f)
            v = d->minV + d->step * (float)(int)((v - d->minV) / d->step + 0.5f);
        return v < d->minV ? d->minV : (v > d->maxV ? d->maxV : v);
    }

private:
    // channel-param templates (id field = kind, channel encoded by caller)
    static constexpr ParamDef kChGain   { (uint16_t)ChParam::Gain,   "Gain",   1.0f,  0.0f, 2.0f,  0.001f, 12.0f };
    static constexpr ParamDef kChLow    { (uint16_t)ChParam::Low,    "Low",    0.0f, -26.0f, 6.0f, 0.1f,   8.0f };
    static constexpr ParamDef kChMid    { (uint16_t)ChParam::Mid,    "Mid",    0.0f, -26.0f, 6.0f, 0.1f,   8.0f };
    static constexpr ParamDef kChHigh   { (uint16_t)ChParam::High,   "High",   0.0f, -26.0f, 6.0f, 0.1f,   8.0f };
    static constexpr ParamDef kChMidLow { (uint16_t)ChParam::MidLow, "MidLow", 0.0f, -26.0f, 6.0f, 0.1f,   8.0f };
    static constexpr ParamDef kChFilter { (uint16_t)ChParam::Filter, "Filter", 0.0f, -1.0f,  1.0f, 0.001f, 5.0f };
    static constexpr ParamDef kChFader  { (uint16_t)ChParam::Fader,  "Fader",  1.0f,  0.0f,  1.0f, 0.001f, 5.0f };
    static constexpr ParamDef kChCue    { (uint16_t)ChParam::Cue,    "Cue",    0.0f,  0.0f,  1.0f, 1.0f,   0.0f };
    static constexpr ParamDef kChMute   { (uint16_t)ChParam::Mute,   "Mute",   0.0f,  0.0f,  1.0f, 1.0f,   4.0f };
    static constexpr ParamDef kChSolo   { (uint16_t)ChParam::Solo,   "Solo",   0.0f,  0.0f,  1.0f, 1.0f,   4.0f };
    static constexpr ParamDef kChXfAssign { (uint16_t)ChParam::XfAssign, "XfAssign", 0.0f, 0.0f, 2.0f, 1.0f, 0.0f };
    static constexpr ParamDef kChFaderCurve { (uint16_t)ChParam::FaderCurve, "FaderCurve", 0.0f, 0.0f, 2.0f, 1.0f, 0.0f };
    static constexpr ParamDef kChSendA    { (uint16_t)ChParam::SendA,  "SendA",  0.0f, 0.0f, 1.0f, 0.001f, 5.0f };
    static constexpr ParamDef kChSendB    { (uint16_t)ChParam::SendB,  "SendB",  0.0f, 0.0f, 1.0f, 0.001f, 5.0f };
    static constexpr ParamDef kChPhase    { (uint16_t)ChParam::PhaseInvert, "PhaseInvert", 0.0f, 0.0f, 1.0f, 1.0f, 0.0f };

    static constexpr ParamDef kXfPos      { (uint16_t)BusParam::CrossfaderPos, "Crossfader", 0.0f, -1.0f, 1.0f, 0.001f, 5.0f };
    static constexpr ParamDef kXfCurve    { (uint16_t)BusParam::XfCurve,       "XfCurve",    1.0f,  0.0f, 3.0f, 1.0f,   0.0f };
    static constexpr ParamDef kMasterGain { (uint16_t)BusParam::MasterGain,    "MasterGain", 1.0f,  0.0f, 2.0f, 0.001f, 8.0f };
    static constexpr ParamDef kMasterMute { (uint16_t)BusParam::MasterMute,    "MasterMute", 0.0f,  0.0f, 1.0f, 1.0f,   2.0f };
    static constexpr ParamDef kLimiterOn  { (uint16_t)BusParam::LimiterOn,     "LimiterOn",  1.0f,  0.0f, 1.0f, 1.0f,   0.0f };
    static constexpr ParamDef kLimiterThr { (uint16_t)BusParam::LimiterThreshold, "LimiterThreshold", 0.891f, 0.1f, 1.0f, 0.001f, 0.0f };
    static constexpr ParamDef kCueVol     { (uint16_t)BusParam::CueVolume,     "CueVolume",  0.8f,  0.0f, 2.0f, 0.001f, 8.0f };
    static constexpr ParamDef kCueMix     { (uint16_t)BusParam::CueMix,        "CueMix",     0.5f,  0.0f, 1.0f, 0.001f, 8.0f };
    static constexpr ParamDef kFxRetA     { (uint16_t)BusParam::FxReturnA,     "FxReturnA",  1.0f,  0.0f, 2.0f, 0.001f, 8.0f };
    static constexpr ParamDef kFxRetB     { (uint16_t)BusParam::FxReturnB,     "FxReturnB",  1.0f,  0.0f, 2.0f, 0.001f, 8.0f };

    // ---- FX slot params (kind templates; ids encoded by caller) ----
    static constexpr ParamDef kFxType     { 0, "FxType",     0.0f, 0.0f,
        (float)((int)fx::FXKind::kCount - 1),                 1.0f,   0.0f };
    static constexpr ParamDef kFxEnabled  { 1, "FxEnabled",  0.0f, 0.0f, 1.0f,  1.0f,  10.0f };
    static constexpr ParamDef kFxMix      { 2, "FxMix",      0.5f, 0.0f, 1.0f,  0.001f, 10.0f };
    static constexpr ParamDef kFxInGain   { 3, "FxInGain",   1.0f, 0.0f, 2.0f,  0.001f, 8.0f  };
    static constexpr ParamDef kFxOutGain  { 4, "FxOutGain",  1.0f, 0.0f, 2.0f,  0.001f, 8.0f  };
    static constexpr ParamDef kFxSync     { 5, "FxSyncMode", 1.0f, 0.0f, 1.0f,  1.0f,   0.0f  };
    static constexpr ParamDef kFxDiv      { 6, "FxDivision", 3.0f, 0.0f,
        (float)(fx::beatDivCount() - 1),                      1.0f,   0.0f  };
    static constexpr ParamDef kFxPn       { 7, "FxParam",    0.5f, 0.0f, 1.0f,  0.001f, 10.0f };
};

} // namespace onda::mixer
