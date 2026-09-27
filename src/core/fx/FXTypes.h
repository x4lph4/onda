#pragma once

#include <cstdint>

namespace onda::fx {

/// Hardware-agnostic FX parameter addressing.
///
/// FX params live in their own id space, above the mixer ranges:
///   id = 0x1000 | (target << 8) | (slot << 4) | kind
///   target: 0..3 = deck insert racks, 4/5 = send A/B return racks,
///           6 = master rack, 7 = sampler insert (Spare),
///           8..15 reserved (external/record/stem buses)
///   slot:   0..15 — 0..3 = rack chain slots; on deck targets 4..7
///           address the per-stem FX slots inside the deck
///           (stem s = slot 4+s; stems::StemType order Vocals/Drums/
///           Bass/Instruments)
///   kind:   FXSlotKind below (0..15)
///
/// Values themselves travel through rt::SetParam like every other
/// parameter; only an effect *type* change needs the non-RT swap path
/// (FXSlot::loadEffect) because it allocates.
enum class FXSlotKind : uint8_t {
    Type = 0,    // FXKind value — informational via setParam; real swap via loadEffect
    Enabled,     // 0/1 (smoothed bypass crossfade)
    Mix,         // 0..1 dry/wet
    InGain,      // 0..2 into the effect
    OutGain,     // 0..2 out of the effect
    SyncMode,    // tempo source: 0 = MANUAL (free), 1 = DECK (target tempo),
                 // 2 = MASTER (sync-master deck tempo)
    Division,    // BeatDiv value (used by synced effects)
    P1, P2, P3, P4, P5, P6, P7, P8, // normalized 0..1 effect params
    kCount
};

enum class FXKind : int {
    None = 0,
    Delay,
    Echo,
    Reverb,
    Filter,
    Flanger,
    Phaser,
    Gate,
    // Phase 16 additions — appended so persisted kind values stay valid
    Chorus,
    Distortion,
    BitCrusher,
    Compressor,
    BeatRepeat,
    BeatRoll,
    // Phase 25 additions — appended so persisted kind values stay valid
    Noise,
    kCount
};

enum class FXTarget : int {
    DeckA = 0, DeckB, DeckC, DeckD,
    SendA, SendB,
    Master,
    Spare,
    kCount
};

/// Beat-synced divisions expressed in beats (4 beats = 1 bar).
/// Triplet values (§12) are appended — persisted division indices
/// remain stable.
enum class BeatDiv : int {
    D_1_32 = 0,  // 1/8 beat
    D_1_16,      // 1/4 beat
    D_1_8,       // 1/2 beat
    D_1_4,       // 1 beat
    D_1_2,       // 2 beats
    D_1_1,       // 4 beats (1 bar)
    D_2Bar,      // 8 beats
    D_4Bar,      // 16 beats
    D_1_16T,     // 1/6 beat  (16th-note triplet)
    D_1_8T,      // 1/3 beat  (8th-note triplet)
    D_1_4T,      // 2/3 beat  (quarter-note triplet)
    kCount
};

constexpr float beatsOf(BeatDiv d) noexcept {
    switch (d) {
        case BeatDiv::D_1_32: return 0.125f;
        case BeatDiv::D_1_16: return 0.25f;
        case BeatDiv::D_1_8:  return 0.5f;
        case BeatDiv::D_1_4:  return 1.0f;
        case BeatDiv::D_1_2:  return 2.0f;
        case BeatDiv::D_1_1:  return 4.0f;
        case BeatDiv::D_2Bar: return 8.0f;
        case BeatDiv::D_4Bar: return 16.0f;
        case BeatDiv::D_1_16T: return 1.0f / 6.0f;
        case BeatDiv::D_1_8T:  return 1.0f / 3.0f;
        case BeatDiv::D_1_4T:  return 2.0f / 3.0f;
        case BeatDiv::kCount:  break;
    }
    return 1.0f;
}

/// Valid division index count for UI/mapping (excludes sentinel).
constexpr int beatDivCount() noexcept { return (int)BeatDiv::kCount; }
/// Short label for UI/mapping.
inline const char* beatDivName(int d) noexcept {
    static const char* k[] = {
        "1/32", "1/16", "1/8", "1/4", "1/2", "1", "2", "4",
        "1/16T", "1/8T", "1/4T"
    };
    return d >= 0 && d < (int)(sizeof(k) / sizeof(k[0])) ? k[d] : "?";
}

/// Musical context handed to every processor before each block.
/// `bpm` comes from the target's source (deck grid, master deck).
struct SyncContext {
    float   bpm       { 120.0f };
    float   beatPhase { 0.0f };  // 0..1 within the current beat (master clock)
    int     synced    { 0 };     // slot SyncMode
    int     division  { (int)BeatDiv::D_1_4 };

    /// Effective beat length of the selected division, in samples.
    /// Mirrors FXClock::framesFor exactly (FXClock can't be included
    /// here — it depends on this header).
    double divisionFrames(double sampleRate) const noexcept {
        const float b = bpm > 1.0f ? bpm : 120.0f;
        const int d = division < 0 ? 0
                    : division >= (int)BeatDiv::kCount ? (int)BeatDiv::D_1_4
                    : division;
        return (double)beatsOf((BeatDiv)d) * 60.0 / b * sampleRate;
    }
    /// Same, in milliseconds.
    double divisionMs() const noexcept {
        const float b = bpm > 1.0f ? bpm : 120.0f;
        const int d = division < 0 ? 0
                    : division >= (int)BeatDiv::kCount ? (int)BeatDiv::D_1_4
                    : division;
        return (double)beatsOf((BeatDiv)d) * 60000.0 / b;
    }
};

/// id encode/decode helpers -------------------------------------------------
constexpr uint16_t kFxParamBase = 0x1000;

constexpr uint16_t paramId(int target, int slot, FXSlotKind k) noexcept {
    return (uint16_t)(kFxParamBase | ((target & 0xF) << 8)
                      | ((slot & 0xF) << 4) | ((uint8_t)k & 0xF));
}
constexpr bool isFxParam(uint16_t id) noexcept {
    return id >= kFxParamBase && id < (kFxParamBase + 0x1000);
}
constexpr int fxTarget(uint16_t id) noexcept { return (id >> 8) & 0xF; }
constexpr int fxSlot  (uint16_t id) noexcept { return (id >> 4) & 0xF; }
constexpr FXSlotKind fxKindOf(uint16_t id) noexcept {
    return (FXSlotKind)(id & 0xF);
}

/// RT -> control events (POD; pushed from the audio thread, polled by UI).
/// §55 — CPU/quality tier for effects that can trade internal work
/// for resources (e.g. ReverbFX comb/allpass count). A control-side
/// configuration, not a realtime param — no slot-kind id is burned.
enum class FXQuality : uint8_t {
    Low = 0,
    Medium,
    High,
};

enum class FXEventType : uint8_t {
    Enabled = 0, Disabled, ParamChanged, TypeChanged, TypeRequested,
    SyncChanged, DivisionChanged, SlotChanged, Fault
};

struct FXEvent {
    uint8_t type;    // FXEventType
    uint8_t target;
    uint8_t slot;
    uint8_t aux;     // e.g. parameter index for ParamChanged
    float   v;
};

/// Serializable per-effect state: kind + native parameter values.
/// No pointers, no object references — safe to persist.
static constexpr int kMaxFxParams = 8;
struct FXState {
    int   kind { 0 };
    float params[kMaxFxParams] {};
};

} // namespace onda::fx
