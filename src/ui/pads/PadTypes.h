#pragma once

#include <cstdint>
#include <string>

namespace onda::ui::pads {

/// Performance-pad modes (task §13). Stems/Roll/Slicer reserved —
/// enum slots kept so saved state stays stable.
enum class PadMode : int {
    HotCue   = 0,
    Sampler  = 1,
    BeatJump = 2,
    PadFx    = 3,
    Loop     = 4,
    // ---- reserved (architecture-ready, unimplemented) ----
    Stems    = 5,
    Roll     = 6,
    Slicer   = 7,
    Keyboard = 8,
    KeyShift = 9,
    Count
};

inline const char* padModeName(PadMode m) {
    switch (m) {
        case PadMode::HotCue:   return "HOT CUE";
        case PadMode::Sampler:  return "SAMPLER";
        case PadMode::BeatJump: return "BEAT JUMP";
        case PadMode::PadFx:    return "PAD FX";
        case PadMode::Loop:     return "LOOP";
        case PadMode::Stems:    return "STEMS";
        case PadMode::Roll:     return "ROLL";
        case PadMode::Slicer:   return "SLICER";
        case PadMode::Keyboard: return "KEYBOARD";
        case PadMode::KeyShift: return "KEY SHIFT";
        default:                return "?";
    }
}
inline bool padModeImplemented(PadMode m) {
    return (int)m >= 0 && (int)m <= (int)PadMode::Loop;
}

/// A resolved pad action — produced by PadModeManager, consumed by
/// CommandManager (commands) or the sampler port (triggers).
struct PadAction {
    enum Kind : int { None = 0, Command, SampleTrig, SampleRelease,
                      SampleStop };
    Kind     kind = None;
    uint32_t cmdId = 0;      // when kind == Command
    int      slot  = -1;     // when kind == Sample*
    float    arg   = 1.0f;
    std::string label;       // for pad display/LED hint
};

/// UI/hardware-facing pad descriptor (task §14). Velocity/pressure
/// are OPTIONAL — controllers that don't emit them just use 1.0.
struct PerformancePad {
    int      id    = -1;
    int      deck  = 0;
    PadMode  mode  = PadMode::HotCue;
    int      state = 0;        // mode-specific (playing/empty…)
    uint32_t colorArgb = 0;
    std::string label;
    uint32_t command = 0;      // resolved command id (for editors)
    float    param    = 0.0f;
    bool     hasVelocity = false;
    bool     hasPressure = false;
};

/// Beat-jump sizes per pad (task §17): pads 1-4 jump backward,
/// pads 5-8 forward — sizes 1/2/4/8 beats (upper bits of `arg` for
/// future 1/32..16 range extension).
inline float beatJumpBeats(int pad) {
    static const float kSizes[4] = { 1.0f, 2.0f, 4.0f, 8.0f };
    const int i = pad & 3;
    return pad < 4 ? -kSizes[i] : kSizes[i];
}

} // namespace onda::ui::pads
