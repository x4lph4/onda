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
    PhaseInvert,// channel polarity flip (0/1)
    FilterRes,  // channel filter resonance (Q 0.2..8, default 0.707)
    Pan,        // channel pan -1..+1, constant power, center 0 (§22)
    EqMode,     // EqMode enum value (§9)
    FilterType, // FilterType enum value (§14)
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
    MasterCue,         // 0/1 — taps post-master program into the cue bus
    BoothGain = 0x40B, // booth monitor gain 0..2 — independent of master (§33)
    HeadphoneMono,     // 0/1 — fold the headphone bus to mono (§24)
};

enum class XfAssign : int { A = 0, B = 1, Thru = 2 };

enum class FaderCurve : int { Linear = 0, AudioTaper = 1 /* ~x^2 */, Custom = 2 };

enum class FilterType : int { Position = 0, LowPass = 1, HighPass = 2 };

enum class EqMode : int { Standard = 0, Isolator = 1 }; // more modes later

// ---------------------------------------------------------------- sources
/// What feeds a channel strip. Sources are never hardcoded into the
/// strip itself — the MixerEngine resolves the descriptor each block
/// (§3/§34: decks, sampler, external in, silence; remappable).
enum class ChSourceType : uint8_t { Deck = 0, Sampler, External, Silence };

struct ChSource {
    ChSourceType type  { ChSourceType::Silence };
    int          index { 0 };                 // deck/external index
    static constexpr ChSource deck(int i)     { return { ChSourceType::Deck,     i }; }
    static constexpr ChSource sampler()       { return { ChSourceType::Sampler,  0 }; }
    static constexpr ChSource external(int i) { return { ChSourceType::External, i }; }
    static constexpr ChSource silence()       { return { ChSourceType::Silence,  0 }; }
};

/// Pack a source descriptor for atomic publication (control → RT).
constexpr uint32_t packSource(ChSource s) noexcept {
    return (uint32_t)((uint8_t)s.type << 24) | (uint32_t)(s.index & 0xFFFFFF);
}
constexpr ChSource unpackSource(uint32_t p) noexcept {
    return { (ChSourceType)((p >> 24) & 0xFF), (int)(p & 0xFFFFFF) };
}

} // namespace onda::mixer
