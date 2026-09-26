#pragma once

#include <cstdint>

#include "../MidiTypes.h"
#include "../MappingTypes.h"
#include "Flx4Ids.h"

namespace onda::ctrl::flx4 {

/// Tracks the held state of the deck SHIFT buttons (note 0x3F on the
/// deck's own channel, press/release). The firmware puts shifted
/// *pads* on a second channel (ch+1), so the layer mostly arrives
/// pre-decoded — but buttons like SYNC share one note with different
/// semantics under SHIFT, and the state must still be tracked for
/// plugin features (range cycle, LED mode).
struct ShiftLayer {
    bool held[2] = { false, false };

    /// note/vel on deck channel ch → updates held state; returns true
    /// when the message was a shift-button event.
    bool ingest(const MidiMsg& m) {
        if (m.type != MsgType::NoteOn && m.type != MsgType::NoteOff)
            return false;
        if (m.data1 != N_SHIFT || m.channel > 1) return false;
        held[m.channel] =
            (m.type == MsgType::NoteOn && m.data2 > 0);
        return true;
    }
    bool isHeld(int deck) const {
        return deck >= 0 && deck < 2 && held[deck];
    }
};

/// Pad banks the DDJ-FLX4 firmware exposes. The 8 pad-mode selector
/// notes live on the deck channel; the pads themselves emit one note
/// per bank position (bank base + pad index) on the pad channel
/// (shift layer = pad channel + 1).
enum class PadMode : uint8_t {
    HotCue = 0, PadFx1, BeatJump, Sampler,
    Keyboard, PadFx2, BeatLoop, KeyShift,
};

struct PadSel { uint8_t note; PadMode mode; };
inline constexpr PadSel kPadSels[] = {
    { N_PM_HOTCUE,   PadMode::HotCue   },
    { N_PM_PADFX1,   PadMode::PadFx1   },
    { N_PM_BEATJUMP, PadMode::BeatJump },
    { N_PM_SAMPLER,  PadMode::Sampler  },
    { N_PM_KEYBOARD, PadMode::Keyboard },
    { N_PM_PADFX2,   PadMode::PadFx2   },
    { N_PM_BEATLOOP, PadMode::BeatLoop },
    { N_PM_KEYSHIFT, PadMode::KeyShift },
};
inline bool padSelFor(uint8_t note, PadMode& out) {
    for (auto& s : kPadSels)
        if (s.note == note) { out = s.mode; return true; }
    return false;
}
inline uint8_t padSelNote(PadMode m) {
    for (auto& s : kPadSels) if (s.mode == m) return s.note;
    return N_PM_HOTCUE;
}
inline const char* padModeName(PadMode m) {
    switch (m) {
        case PadMode::HotCue:   return "HOT_CUE";
        case PadMode::PadFx1:   return "PAD_FX";
        case PadMode::BeatJump: return "BEAT_JUMP";
        case PadMode::Sampler:  return "SAMPLER";
        case PadMode::Keyboard: return "KEYBOARD";
        case PadMode::PadFx2:   return "PAD_FX2";
        case PadMode::BeatLoop: return "BEAT_LOOP";
        case PadMode::KeyShift: return "KEY_SHIFT";
    }
    return "?";
}
inline uint8_t padBankBase(PadMode m) {
    switch (m) {
        case PadMode::HotCue:   return kPadBankHotCue;
        case PadMode::BeatJump: return kPadBankBeatJump;
        case PadMode::Sampler:  return kPadBankSampler;
        case PadMode::Keyboard: return kPadBankKeyboard;
        case PadMode::PadFx1:   return kPadBankKeyboard; // pads share
        case PadMode::PadFx2:   return kPadBankKeyboard; //  0x40 bank
        case PadMode::BeatLoop: return kPadBankBeatLoop;
        case PadMode::KeyShift: return kPadBankKeyShift;
    }
    return 0;
}

/// Per-deck pad-mode state. Selector notes arrive on the deck
/// channel; a press switches `mode` and the plugin repaints pad/mode
/// LEDs. Pad presses are classified by (channel,note): bank base
/// identifies which logical pad (low nibble = pad index).
struct PadModeManager {
    PadMode mode[2] = { PadMode::HotCue, PadMode::HotCue };

    /// Selector note on deck channel → returns new mode or -1.
    int ingestSelector(const MidiMsg& m) {
        if (m.type != MsgType::NoteOn || m.data2 == 0 ||
            m.channel > 1) return -1;
        PadMode nm;
        if (!padSelFor(m.data1, nm)) return -1;
        mode[m.channel] = nm;
        return (int)nm;
    }

    /// Pad note on ch 7..10 → (deck, shiftLayer, padIndex, bank) or -1.
    static int classify(const MidiMsg& m, int& deck, bool& shift,
                        int& pad, int& bank) {
        if (m.type != MsgType::NoteOn && m.type != MsgType::NoteOff)
            return -1;
        if (m.channel < 7 || m.channel > 10) return -1;
        deck  = (m.channel - 7) / 2;
        shift = ((m.channel - 7) & 1) == 1;
        bank  = m.data1 & 0x70;
        pad   = m.data1 & 0x0F;
        if (pad > 7) return -1;
        return 0;
    }
};

} // namespace onda::ctrl::flx4
