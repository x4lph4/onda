#pragma once

#include <cstdint>
#include <string>

#include "MidiTypes.h"

namespace onda::ctrl {

/// How an input turns into an action — set per mapping entry.
enum class MappingMode : uint8_t {
    Toggle = 0,   // press → flip target state (buttons)
    Momentary,    // press=1 release=0 (cue, pads, bend)
    Absolute,     // raw 0..127 → scaled target value (knobs/faders)
    Relative,     // encoder delta → target += delta*step
    IncDec,       // encoder step → command ± (jump, track nav)
    Pickup,       // absolute but swallowed until raw crosses current
};

/// Value response curve after normalization.
enum class Curve : uint8_t {
    Linear = 0,
    Inverse,
    Expo,        // x^2 — finer low-end (faders)
    Log,         // sqrt — finer high-end
};

/// How a relative encoder encodes deltas in the 7-bit value field.
enum class EncoderMode : uint8_t {
    Absolute = 0,
    RelBinaryOffset,   // 64 + delta (most common)
    RelTwosComplement, // signed byte delta
    RelSignMagnitude,  // bit6 = sign, low 6 = magnitude
};

/// Jog behaviour — one input, three semantic uses the target chooses.
enum class JogMode : uint8_t {
    JogSeek = 0,    // nudge position (browsing / beatalign)
    JogPitchBend,   // momentary tempo offset while spinning
    JogScratch,     // position follows rotation 1:1-ish
};

/// Physical control class (task §12) — informational tag; the
/// behaviour is defined by mode/encoder/target, this records what
/// the hardware element IS for UI, validation and Learn hints.
enum class ControlKind : uint8_t {
    Unknown = 0,
    Button,       // discrete press/release
    Toggle,       // latching switch
    Momentary,    // spring-return switch
    Encoder,      // rotary (relative or absolute)
    Fader,        // linear absolute control
    Knob,         // rotary absolute control
    Jog,          // jog wheel
    Pad,          // performance pad (velocity capable)
    Switch,       // multi-position selector
};

/// Mapping layers (task §19) — the engine keeps ONE active layer per
/// device; a mapping fires only on its layer. `Default` is always 0;
/// devices with hardware-split shift channels (FLX4) never need a
/// second layer — the firmware already encodes it.
enum class MapLayer : uint8_t {
    Default = 0,
    Shift   = 1,
    Alt     = 2,
    Custom  = 3,
};

/// What a mapping drives. `Param` addresses are mixer::
/// ParameterRegistry ids; `Command` addresses are ui::cmd ids; `Jog`
/// goes to the engine's jog port (deck + JogMode).
struct Target {
    enum class Kind : uint8_t { None = 0, Command, Param, Jog } kind =
        Kind::None;
    uint32_t    cmd     = 0; // Command  → cmd::Id
    uint16_t    paramId = 0; // Param    → ParameterRegistry id
    uint8_t     jogDeck = 0; // Jog      → deck 0..3
    uint8_t     jogMode = 0; //          → JogMode
    std::string cmdName;     // profile may store a name — resolved to
                             // `cmd` when the profile is applied
    std::string control;     // §10 logical id ("decka.play") — resolved
                             // via ControlRegistry at applyProfile,
                             // takes precedence over cmdName
};

/// Feedback output bound to a mapping — LED/pad/motor updates.
struct FeedbackOut {
    MidiAddr addr;
    bool     hasCustom = false;
    uint8_t  onValue  = 127;  // sent when target state == on / active
    uint8_t  offValue = 0;    // sent when off
    bool     followValue = false; // true → echo scaled param (motors)
};

/// One mapping entry — declarative data only (profiles are pure data).
struct Mapping {
    MidiAddr    in;
    Target      target;
    MappingMode mode    = MappingMode::Absolute;
    Curve       curve   = Curve::Linear;
    EncoderMode encoder = EncoderMode::Absolute;
    float       outMin  = 0.0f;   // scaled output range (param units
    float       outMax  = 1.0f;   //   for Param, cmd value for Command)
    float       step    = 1.0f;   // Relative/IncDec step per tick
    bool        hiRes   = false;  // 14-bit CC pair: MSB at in.number,
                                  // LSB at in.number+0x20 (0..16383)
    bool        enabled = true;
    FeedbackOut out;
    int         group = 0;        // 0 = global, else device group id

    // ---- Phase 27 additions ----
    ControlKind kind     = ControlKind::Unknown; // §12 hardware class
    uint8_t     layer    = 0;     // MapLayer — fires only when active
    uint8_t     priority = 0;     // §53 — higher wins on shared input
    /// Long-press (§17): when >0 ms, a Momentary mapping that stays
    /// pressed past the threshold emits `longCmd` INSTEAD of the
    /// short-press release action. 0 = off (default behaviour).
    uint16_t    longMs   = 0;
    uint32_t    longCmd  = 0;     // command id emitted on long-press
    /// Protocol verification (§9): false marks the entry as
    /// UNVERIFIED — works, but flagged in diagnostics until confirmed
    /// against hardware or vendor docs.
    bool        verified = true;
};

/// Per-mapping runtime — pickup state, last raw/emit values.
struct MappingRuntime {
    bool  armed   = true;   // pickup: not yet captured
    bool  pressed = false;  // momentary/toggle edge tracking
    float lastRaw = -1.0f;
    float lastOut = 0.0f;
    float current = 0.0f;   // pickup: last known target value
    uint8_t hiMsb = 0;      // hiRes: cached MSB byte
    uint8_t hiLsb = 0;      // hiRes: cached LSB byte
    bool    hiSeen = false; // hiRes: at least one half arrived

    // ---- long-press state (§17) ----
    double pressAtSec = -1.0;   // <0 = not held
    bool   longFired  = false;  // longCmd already emitted this press
};

} // namespace onda::ctrl
