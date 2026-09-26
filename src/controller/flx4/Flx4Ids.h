#pragma once

#include <cstdint>

namespace onda::ctrl::flx4 {

/// DDJ-FLX4 MIDI address constants — every value below is taken from
/// one or more of these public, non-proprietary sources:
///
///   [OFF] Pioneer/AlphaTheta, "DDJ-FLX4 List of MIDI messages",
///         DDJ-FLX4_MIDI_message_List_E1.pdf (published for
///         MIDI-compatible software authors).
///   [MIX] Mixxx project, Pioneer-DDJ-FLX4.midi.xml + script
///         (GPL, community mapping exercised on firmware 1.02).
///   [SLP] slipmate docs/midi-ddj-flx4.md — cross-references OFF+MIX
///         with measurements taken on a physical unit.
///
/// Nothing here comes from reverse-engineering proprietary software.
///
/// Deck channel: deck 0 uses MIDI ch 0 (0x90/0xB0), deck 1 ch 1.
/// Mixer/browser/master: ch 6 (0x96/0xB6). Beat-FX unit: ch 4/5
/// (0x94/0x95, 0xB4). Pad banks: deck0 ch 7 plain / ch 8 shift;
/// deck1 ch 9 plain / ch 10 shift.
inline constexpr int kDeckCh(int deck) { return deck; }
inline constexpr int kPadCh(int deck)    { return 7 + deck * 2; }
inline constexpr int kPadChShift(int deck){ return 8 + deck * 2; }
inline constexpr int kMixCh   = 6;
inline constexpr int kFxChA   = 4;
inline constexpr int kFxChB   = 5;

// ---- deck notes (ch = deck) -------------------------------------
inline constexpr uint8_t N_PLAY        = 0x0B; // [OFF][MIX]
inline constexpr uint8_t N_PLAY_SHIFT  = 0x0E; // reverse roll [OFF][MIX]
inline constexpr uint8_t N_CUE         = 0x0C; // [OFF][MIX]
inline constexpr uint8_t N_CUE_SHIFT   = 0x48; // start/stop [OFF][MIX]
inline constexpr uint8_t N_SHIFT       = 0x3F; // [OFF][MIX]
inline constexpr uint8_t N_JOG_TOUCH   = 0x36; // platter top  [OFF][MIX]
inline constexpr uint8_t N_JOG_TOUCH_SH= 0x67; // shift+touch  [MIX]
inline constexpr uint8_t N_LOOP_IN     = 0x10; // [MIX][SLP-hw]
inline constexpr uint8_t N_LOOP_OUT    = 0x11; // [MIX][SLP-hw]
inline constexpr uint8_t N_LOOPADJ_IN  = 0x4C; // [MIX]
inline constexpr uint8_t N_LOOPADJ_OUT = 0x4E; // [MIX]
inline constexpr uint8_t N_4BEAT_EXIT  = 0x4D; // set/exit     [MIX][SLP-hw]
inline constexpr uint8_t N_RELOOP_STOP = 0x50; // [MIX]
inline constexpr uint8_t N_CALL_L      = 0x51; // halve loop   [MIX][SLP-hw]
inline constexpr uint8_t N_CALL_R      = 0x53; // double loop  [MIX][SLP-hw]
inline constexpr uint8_t N_PFL         = 0x54; // channel cue  [MIX]
inline constexpr uint8_t N_SYNC        = 0x58; // press        [OFF][MIX]
inline constexpr uint8_t N_SYNC_LONG   = 0x5C; // long press   [MIX]
inline constexpr uint8_t N_TEMPO_RANGE = 0x60; // shift+sync   [MIX]
inline constexpr uint8_t N_JUMP_FWD    = 0x3D; // 32-beat jump [MIX]
inline constexpr uint8_t N_JUMP_BACK   = 0x3E; // 32-beat jump [MIX]
inline constexpr uint8_t N_QUANTIZE    = 0x68; // shift+cue-ch [MIX]

// ---- pad-mode selector notes (ch = deck) -------------------------
inline constexpr uint8_t N_PM_HOTCUE    = 0x1B; // [OFF][MIX][SLP-hw]
inline constexpr uint8_t N_PM_PADFX1    = 0x1E; // [OFF][MIX][SLP-hw]
inline constexpr uint8_t N_PM_BEATJUMP  = 0x20; // [OFF][MIX][SLP-hw]
inline constexpr uint8_t N_PM_SAMPLER   = 0x22; // [OFF][MIX][SLP-hw]
inline constexpr uint8_t N_PM_KEYBOARD  = 0x69; // shift+hotcue  [MIX][SLP-hw]
inline constexpr uint8_t N_PM_PADFX2    = 0x6B; // shift+padfx1  [MIX][SLP-hw]
inline constexpr uint8_t N_PM_BEATLOOP  = 0x6D; // shift+beatjmp [MIX][SLP-hw]
inline constexpr uint8_t N_PM_KEYSHIFT  = 0x6F; // shift+sampler [MIX][SLP-hw]

// ---- pad note banks (ch = 7+deck*2 / 8+deck*2) -------------------
inline constexpr uint8_t kPadBankHotCue   = 0x00; // 0x00-0x07 [OFF][MIX]
inline constexpr uint8_t kPadBankBeatJump = 0x20; // 0x20-0x27 [MIX]
inline constexpr uint8_t kPadBankSampler  = 0x30; // 0x30-0x37 [MIX]
inline constexpr uint8_t kPadBankKeyboard = 0x40; // 0x40-0x47 [MIX][SLP-hw]
inline constexpr uint8_t kPadBankBeatLoop = 0x60; // 0x60-0x67 [MIX]
inline constexpr uint8_t kPadBankKeyShift = 0x70; // 0x70-0x77 [MIX]
inline constexpr uint8_t kLedPadDim       = 0x20; // dim level [SLP-hw]
inline constexpr uint8_t kLedOn           = 0x7F;
inline constexpr uint8_t kLedOff          = 0x00;

// ---- deck CCs (ch = deck); continuous controls are 14-bit
// pairs: MSB at CC n, LSB at CC n+0x20 ---------------------------
inline constexpr uint8_t CC_TEMPO      = 0x00; // [OFF][MIX][SLP-hw]
inline constexpr uint8_t CC_VU         = 0x02; // meter LED out [MIX]
inline constexpr uint8_t CC_TRIM       = 0x04; // [OFF][MIX]
inline constexpr uint8_t CC_EQ_HI      = 0x07; // [OFF][MIX][SLP]
inline constexpr uint8_t CC_EQ_MID     = 0x0B; // [OFF][MIX][SLP]
inline constexpr uint8_t CC_EQ_LOW     = 0x0F; // [OFF][MIX][SLP]
inline constexpr uint8_t CC_FADER      = 0x13; // [OFF][MIX]
inline constexpr uint8_t CC_JOG_SIDE   = 0x21; // wheel edge   [OFF][MIX]
inline constexpr uint8_t CC_JOG_VON    = 0x22; // platter, vinyl on  [OFF][MIX]
inline constexpr uint8_t CC_JOG_VOFF   = 0x23; // platter, vinyl off [OFF][MIX]
inline constexpr uint8_t CC_JOG_SEARCH = 0x29; // shift+jog    [OFF][SLP-hw]

// ---- mixer/browser CCs+notes (ch 6) ------------------------------
inline constexpr uint8_t CC_XF        = 0x1F; // crossfader 14-bit [OFF][MIX]
inline constexpr uint8_t CC_HEADMIX   = 0x0C; // phones mix 14-bit [OFF][MIX]
inline constexpr uint8_t CC_CFX_A     = 0x17; // smart cfx deck A  [SLP][MIX]
inline constexpr uint8_t CC_CFX_B     = 0x18; // smart cfx deck B  [SLP][MIX]
inline constexpr uint8_t CC_BROWSE    = 0x40; // rotary, rel     [OFF][MIX]
inline constexpr uint8_t CC_BROWSE_Z  = 0x64; // shift+rotary    [MIX]
inline constexpr uint8_t N_BROWSE_FWD = 0x41; // focus forward   [MIX]
inline constexpr uint8_t N_BROWSE_BCK = 0x42; // focus back      [MIX]
inline constexpr uint8_t N_LOAD_A     = 0x46; // load to deck A  [OFF][MIX]
inline constexpr uint8_t N_LOAD_B     = 0x47; // load to deck B  [OFF][MIX]
inline constexpr uint8_t N_FSTART_PLAY= 0x66; // fader-start play (unmapped)
inline constexpr uint8_t N_FSTART_CUE = 0x52; // fader-start cue  (unmapped)

// ---- beat-FX unit (ch 4 = left select, ch 5 = right) -------------
inline constexpr uint8_t N_FX_CHSEL_A = 0x10; // ch4 [OFF][MIX]
inline constexpr uint8_t N_FX_CHSEL_B = 0x11; // ch5 [OFF][MIX]
inline constexpr uint8_t N_FX_SELECT  = 0x63; // ch4 [MIX]
inline constexpr uint8_t N_FX_SEL_SH  = 0x64; // ch4 shift [MIX]
inline constexpr uint8_t N_FX_BEAT_L  = 0x4A; // ch4 [MIX]
inline constexpr uint8_t N_FX_BEAT_R  = 0x4B; // ch4 [MIX]
inline constexpr uint8_t CC_FX_DEPTH  = 0x02; // ch4, 14-bit [OFF][MIX]
inline constexpr uint8_t N_FX_ON      = 0x47; // ch4/ch5 [OFF][MIX]
inline constexpr uint8_t N_FX_ON_SH   = 0x43; // ch4/ch5 shift [MIX]

// ---- SysEx (community-sourced, Wireshark capture; position dump /
// keep-alive). Optional — the profile works without it. ------------
inline constexpr uint8_t kSysexPositionDump[] = {
    0xF0, 0x00, 0x40, 0x05, 0x00, 0x00,
    0x04, 0x05, 0x00, 0x50, 0x02, 0xF7 };
inline constexpr int kSysexPositionDumpLen = 12;

// ---- identity ----------------------------------------------------
inline constexpr const char* kNameMatch   = "DDJ-FLX4";   // OS MIDI name
inline constexpr const char* kAudioName   = "DDJ-FLX4";   // WASAPI/Cor eAudio
inline constexpr const char* kManufacturer= "Pioneer DJ";
inline constexpr const char* kModel       = "DDJ-FLX4";
// USB VID/PID: UNKNOWN — not published in the sources above; the
// engine therefore matches on the OS-reported device name.

// Tempo-range cycle for shift+SYNC, per the hardware diagram / Mixxx
// "cycleTempoRange" (+/-6, +/-10, +/-16, +/-25 percent).
inline constexpr float kTempoRanges[] = { 6.0f, 10.0f, 16.0f, 25.0f };
inline constexpr int   kTempoRangeCount = 4;

} // namespace onda::ctrl::flx4
