#pragma once

#include <cstdint>

namespace onda::mixer {

/// Hardware-agnostic parameter addressing for the mixer.
///
/// Channel params: id = (channel << 5) | kind   (8 channels × 32 kinds)
/// Bus params:     id >= 0x400 (well-known list below)
///
/// These IDs are what UI, MIDI, HID, automation and tests use — the
/// concrete widgets are never referenced from the audio engine.
enum class ChParam : uint8_t {
    Gain = 0,   // input trim 0..2 (unity at 1)
    Low,        // EQ band dB
    Mid,
    High,
    MidLow,     // reserved 4th band
    Filter,     // -1..+1 position
    Fader,      // 0..1
    Cue,        // 0/1 PFL
    Mute,       // 0/1
    Solo,       // 0/1
    XfAssign,   // XfAssign enum value
    FaderCurve, // FaderCurve enum value
    SendA,      // post-fader send level into FX bus 0 (0..1)
    SendB,      // post-fader send level into FX bus 1 (0..1)
    PhaseInvert,// reserved: channel polarity flip (0/1)
};

constexpr uint16_t chParam(int ch, ChParam k) noexcept {
    return (uint16_t)((ch << 5) | (uint8_t)k);
}

enum class BusParam : uint16_t {
    CrossfaderPos    = 0x400,
    XfCurve,
    MasterGain,
    MasterMute,
    LimiterOn,
    LimiterThreshold,
    CueVolume,
    CueMix,
    FxReturnA = 0x408, // FX send bus 0 return level (0..2)
    FxReturnB,         // FX send bus 1 return level (0..2)
};

enum class XfAssign : int { A = 0, B = 1, Thru = 2 };

enum class FaderCurve : int { Linear = 0, AudioTaper = 1 /* ~x^2 */, Custom = 2 };

enum class FilterType : int { Position = 0, LowPass = 1, HighPass = 2 };

enum class EqMode : int { Standard = 0, Isolator = 1 }; // more modes later

} // namespace onda::mixer
