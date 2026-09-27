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

    // ---- waveform/deck view actions: +deck*0x400 (Phase 19) ----
    WaveZoomIn       = 0x0080,  // zoom in at playhead
    WaveZoomOut      = 0x0081,  // zoom out at playhead
    WaveZoomFit      = 0x0082,  // fit whole track to the strip
    WaveAutoScroll   = 0x0083,  // cycle Off → Center → Smart
    WaveGridEditTgl  = 0x0084,  // grid-edit mode on/off
    DeckTimeMode     = 0x0085,  // cycle ELAPSED → REMAINING → TOTAL

    // ---- Phase 20 performance surface: +deck*0x400 ----
    DeckCueSet       = 0x0086,  // set temporary cue point at playhead
    DeckInstantRestart = 0x0087,// restart from cue point (sync untouched)
    DeckLoopSizeUp   = 0x0088,  // step auto-loop size up (1/32…32 bt)
    DeckLoopSizeDn   = 0x0089,  // step auto-loop size down
    DeckLoopSizeTrig = 0x008A,  // fire auto-loop at the selected size
    DeckRollGate     = 0x008B,  // press=loop roll@arg idx, release=end
    DeckJumpSizeUp   = 0x008C,  // step beat-jump size up (1…64 bt)
    DeckJumpSizeDn   = 0x008D,  // step beat-jump size down
    DeckRevHold      = 0x008E,  // momentary reverse (press/release)
    DeckPitchReset   = 0x008F,  // tempo reset → 0% (Phase 28 §36)

    // ---- Phase 28: transport/cue extensions: +deck*0x400 ----
    DeckBrakeTgl     = 0x0094,  // vinyl brake toggle (spin-down/up)
    DeckCueMode      = 0x0095,  // cycle cue behaviour Hold/Start/LastCue

    // ---- stems surface: +deck*0x400 (Phase 21) ----
    DeckStemModeTgl  = 0x0090,  // toggle original ↔ stems render
    DeckStemAnalyze  = 0x0091,  // queue separation for the loaded track
    DeckStemCancel   = 0x0092,  // cancel that deck's separation job
    DeckStemUnmute   = 0x0093,  // clear all mutes+solos on the deck
    // per-stem actions: base + stem(0..3)*8 + kind
    //   0 mute tgl, 1 solo tgl, 2 gain up, 3 gain down,
    //   4 pan left, 5 pan right, 6 gate (arg 1=down,0=up), 7 gain set
    DeckStemBase     = 0x00C0,
    DeckMemCueBase   = 0x00A0,  // + slot*4 + action (0 goto,1 set,2 del)

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
    // ---- Phase 17: cue/deck-mode/scenes/presets/diagnostics ----
    MixMasterCueTgl  = 0x2F09,  // master cue on/off (post-master → phones)
    MixDeckModeTgl   = 0x2F0A,  // 2-deck ↔ 4-deck (sources, not backend)
    MixDiagCopy      = 0x2F0B,  // copy mixer diagnostics text
    MixSceneSave     = 0x2F0C,  // value = scene slot 0..7
    MixSceneRecall   = 0x2F0D,  // value = scene slot 0..7
    MixPresetNext    = 0x2F0E,
    MixPresetPrev    = 0x2F0F,
    // ---- Phase 32: crossfader orientation ----
    MixXfReverseTgl  = 0x2F10,  // hamster/reverse A↔B (§26)

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

    // ---- sync control: 0x9100 (Phase 15) ----
    SyncMasterCycle = 0x9100,  // app-level: AUTO → A → B → C → D → AUTO
    SyncLockTgl     = 0x9101,  // +deck*0x400 — sync survives track swap
    SyncQuantRes    = 0x9102,  // +deck*0x400 — cycle quantize resolution
    SyncBpmMode     = 0x9103,  // +deck*0x400 — tempo-only sync toggle
    SyncDiagCopy    = 0x9104,  // +deck*0x400 — copy sync diagnostics

    // ---- FX surface: 0xA000 (Phase 16) — all act on the focused
    //      (target, slot) held by FXSurface ----
    FxFocusTarget  = 0xA000,  // value = FXTarget
    FxFocusSlot    = 0xA001,  // value = chain position 0..3
    FxEnable       = 0xA002,  // toggle
    FxTypeNext     = 0xA003,
    FxTypePrev     = 0xA004,
    FxTypeSet      = 0xA005,  // value = FXKind
    FxMix          = 0xA006,  // value = 0..1
    FxSyncToggle   = 0xA007,
    FxDivNext      = 0xA008,
    FxDivPrev      = 0xA009,
    FxDivSet       = 0xA00A,  // value = BeatDiv
    FxParamSetBase = 0xA010,  // + param 0..7, value = normalized
    FxParamUpBase  = 0xA018,  // + param 0..7 — nudge +0.02
    FxParamDnBase  = 0xA020,  // + param 0..7 — nudge -0.02
    FxPresetNext   = 0xA028,
    FxPresetPrev   = 0xA029,
    FxPresetApply  = 0xA02A,  // apply current selection
    FxMacroBase    = 0xA030,  // + macro 0..7, value = 0..1
    FxSlotMoveL    = 0xA038,  // move focus slot left in chain
    FxSlotMoveR    = 0xA039,
    FxDiagCopy     = 0xA03A,  // copy FX diagnostics text

    // ---- app: 0x3F00 ----
    AppToggleSettings  = 0x3F00,
    AppLayoutNext      = 0x3F01,
    AppFullscreen      = 0x3F02,
    AppPanic           = 0x3F03,   // stop all decks + center xf
    AppToggleFx        = 0x3F04,
    AppToggleHelp      = 0x3F05,   // shortcut cheat-sheet overlay
    // ---- Phase 20: performance undo ----
    PerfUndo           = 0x3F10,   // undo last cue/loop/grid edit
    PerfRedo           = 0x3F11,

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
/// Memory cue: id = DeckMemCueBase + slot*4 + action (0 goto/1 set/2 del)
constexpr uint32_t forMemCue(int deck, int slot, int action) {
    return forDeck(DeckMemCueBase, deck) + (uint32_t)(slot * 4 + action);
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
