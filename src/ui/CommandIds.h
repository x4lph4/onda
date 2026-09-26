#pragma once

#include <cstdint>

namespace onda::ui::cmd {

/// Stable command ids — the address space every input source (UI,
/// keyboard, MIDI, HID, automation) shares. Numbered by category so
/// decks C/D and future commands slot in without renumbering.
///
/// Encoding: (category << 10) | index — matches how a controller
/// mapping table would address them.
enum Id : uint32_t {
    // ---- transport per deck: (deck << 10) | action, deck 0..3 ----
    DeckPlayToggle   = 0x0000,  // +deck*0x400
    DeckStop         = 0x0001,
    DeckCueTrigger   = 0x0002,  // press = jump+play-from, release = return
    DeckSyncToggle   = 0x0003,
    DeckQuantizeTgl  = 0x0004,
    DeckSlipToggle   = 0x0005,
    DeckKeyLockTgl   = 0x0006,
    DeckReverseTgl   = 0x0007,
    DeckLoopToggle   = 0x0008,
    DeckLoopIn       = 0x0009,
    DeckLoopOut      = 0x000A,
    DeckLoopHalve    = 0x000B,
    DeckLoopDouble   = 0x000C,
    DeckLoopMoveL    = 0x000D,
    DeckLoopMoveR    = 0x000E,
    DeckJumpBack     = 0x000F,  // beat jump -N (N = quantize res)
    DeckJumpFwd      = 0x0010,
    DeckBendDown     = 0x0011,  // momentary while held
    DeckBendUp       = 0x0012,
    DeckLoad         = 0x0013,  // load browser selection to this deck
    DeckUnload       = 0x0014,
    DeckPflToggle    = 0x0015,
    // hot cues: base + slot (0..7)
    DeckHotCueBase   = 0x0020,  // DeckHotCueBase + slot*4 + action
    //   action: 0 trigger, 1 set, 2 delete

    // ---- mixer: 0x2000 + (ch << 6) | kind ----
    MixChannelBase   = 0x2000,  // channel ops take (ch, kind) — routed via
                                // MixerController param ids, not commands
    MixMasterGainUp  = 0x2F00,
    MixMasterGainDn  = 0x2F01,
    MixCueVolUp      = 0x2F02,
    MixCueVolDn      = 0x2F03,
    MixCueMixToggle  = 0x2F04,
    MixLimiterTgl    = 0x2F05,
    MixXfToA         = 0x2F06,  // nudge crossfader to A side
    MixXfToB         = 0x2F07,
    MixXfCenter      = 0x2F08,

    // ---- browser / library: 0x3000 ----
    BrowserFocusSearch = 0x3000,
    BrowserNextTrack   = 0x3001,
    BrowserPrevTrack   = 0x3002,
    BrowserLoadDeckA   = 0x3003,
    BrowserLoadDeckB   = 0x3004,
    BrowserTogglePanel = 0x3005,
    BrowserAddToPlaylist = 0x3006,

    // ---- performance pads: 0x5000 (Phase 11) ----
    PadPressBase   = 0x5000,   // + deck*0x20 + pad(0..7) + (shift? 0x10:0)
    PadModeSetBase = 0x5080,   // + deck; value = PadMode enum

    // ---- sampler: 0x6000 (Phase 11) ----
    SamplerTrigBase = 0x6000,  // + slot (0..31); value = velocity 0..1
    SamplerStopBase = 0x6040,  // + slot
    SamplerStopAll  = 0x6080,
    SamplerQuantize = 0x6081,  // value = QuantizeMode
    SamplerSyncTgl  = 0x6082,
    SamplerBankNext = 0x6083,
    SamplerBankPrev = 0x6084,
    SamplerBankSet  = 0x6085,  // value = bank index

    // ---- recording: 0x7000 (Phase 12) ----
    RecArm    = 0x7000,
    RecStart  = 0x7001,
    RecStop   = 0x7002,
    RecPause  = 0x7003,
    RecResume = 0x7004,
    RecSplit  = 0x7005,
    RecToggle = 0x7006,   // Quick Record: start/stop

    // ---- analysis edits: 0x9000 + deck*0x400 (Phase 14) ----
    AnaSetFirstBeat = 0x9000,  // grid beat 0 = current playhead
    AnaNudgeLeft    = 0x9001,  // shift grid −10 ms
    AnaNudgeRight   = 0x9002,  // shift grid +10 ms
    AnaBpmDouble    = 0x9003,  // grid BPM ×2 (60→120 interpretation)
    AnaBpmHalf      = 0x9004,  // grid BPM ÷2
    AnaSetDownbeat  = 0x9005,  // beat at playhead becomes bar beat 1
    AnaGridReset    = 0x9006,  // drop user override → detected grid
    AnaReanalyze    = 0x9007,  // requeue analysis at high priority

    // ---- app: 0x3F00 ----
    AppToggleSettings  = 0x3F00,
    AppLayoutNext      = 0x3F01,
    AppFullscreen      = 0x3F02,
    AppPanic           = 0x3F03,   // stop all decks + center xf
    AppToggleFx        = 0x3F04,

    Invalid = 0xFFFFFFFF
};

/// Encode a deck command: cmd::forDeck(DeckPlayToggle, deck).
constexpr uint32_t forDeck(Id base, int deck) {
    return (uint32_t)base + ((uint32_t)(deck & 0xF) << 10);
}
constexpr int deckOf(uint32_t id)   { return (int)((id >> 10) & 0xF); }
constexpr uint32_t actionOf(uint32_t id) { return id & 0x3FFu; }
/// Hot-cue: id = DeckHotCueBase + slot*4 + action (0 trig/1 set/2 del)
constexpr uint32_t forHotCue(int deck, int slot, int action) {
    return forDeck(DeckHotCueBase, deck) + (uint32_t)(slot * 4 + action);
}
/// Pad press: id = PadPressBase + deck*0x20 + pad + (shift? 0x10 : 0)
constexpr uint32_t forPad(int deck, int pad, bool shift = false) {
    return PadPressBase + (uint32_t)(deck * 0x20) +
           (uint32_t)(pad & 7) + (shift ? 0x10u : 0u);
}
constexpr int  padDeckOf(uint32_t id)  { return (int)((id - PadPressBase) >> 5); }
constexpr int  padIndexOf(uint32_t id) { return (int)(id & 7); }
constexpr bool padShiftOf(uint32_t id) { return (id & 0x10) != 0; }

} // namespace onda::ui::cmd
