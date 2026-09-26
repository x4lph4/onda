#pragma once

#include <array>
#include <cstdint>

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

    /// Definition for a parameter id, or nullptr if unknown.
    static const ParamDef* find(uint16_t id) noexcept {
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

    static constexpr ParamDef kXfPos      { (uint16_t)BusParam::CrossfaderPos, "Crossfader", 0.0f, -1.0f, 1.0f, 0.001f, 5.0f };
    static constexpr ParamDef kXfCurve    { (uint16_t)BusParam::XfCurve,       "XfCurve",    1.0f,  0.0f, 3.0f, 1.0f,   0.0f };
    static constexpr ParamDef kMasterGain { (uint16_t)BusParam::MasterGain,    "MasterGain", 1.0f,  0.0f, 2.0f, 0.001f, 8.0f };
    static constexpr ParamDef kMasterMute { (uint16_t)BusParam::MasterMute,    "MasterMute", 0.0f,  0.0f, 1.0f, 1.0f,   2.0f };
    static constexpr ParamDef kLimiterOn  { (uint16_t)BusParam::LimiterOn,     "LimiterOn",  1.0f,  0.0f, 1.0f, 1.0f,   0.0f };
    static constexpr ParamDef kLimiterThr { (uint16_t)BusParam::LimiterThreshold, "LimiterThreshold", 0.891f, 0.1f, 1.0f, 0.001f, 0.0f };
    static constexpr ParamDef kCueVol     { (uint16_t)BusParam::CueVolume,     "CueVolume",  0.8f,  0.0f, 2.0f, 0.001f, 8.0f };
    static constexpr ParamDef kCueMix     { (uint16_t)BusParam::CueMix,        "CueMix",     0.5f,  0.0f, 1.0f, 0.001f, 8.0f };
};

} // namespace onda::mixer
