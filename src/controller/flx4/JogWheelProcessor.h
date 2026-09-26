#pragma once

#include <cstdint>

#include "../MidiTypes.h"
#include "../MappingTypes.h"
#include "Flx4Ids.h"
#include "ScratchEngine.h"

namespace onda::ctrl::flx4 {

/// DDJ-FLX4 jog-wheel decoder (task §6/§11): classifies the four jog
/// CCs and the touch notes into semantic events, tracks touch state
/// and drives the per-deck ScratchEngine.
///
/// Wire encoding (OFF): all jog CCs are relative-64 — value 0x40 is
/// rest, 0x41.. up (CW), 0x3F.. down (CCW). Touch is NoteOn/NoteOff
/// 0x36 (+shift variant 0x67) on the deck channel.
///
///   CC 0x21  wheel side        → pitch bend (CDJ edge nudge)
///   CC 0x22  platter, vinyl on → scratch while touching, else nudge
///   CC 0x23  platter, vinyl off→ seek nudge
///   CC 0x29  shift + wheel     → fast search
struct JogEvent {
    enum Kind : uint8_t { None, Touch, Turn } kind = None;
    int  deck     = -1;
    bool touching = false;   // Touch events
    int  delta    = 0;       // Turn events (signed ticks)
    JogMode mode  = JogMode::JogSeek;
};

struct JogWheelProcessor {
    ScratchEngine scratch[2];

    /// Returns a filled JogEvent for jog-family messages, else
    /// kind==None. Touch state is updated for Touch events before
    /// callers inspect `scratch[d].touching`.
    JogEvent decode(const MidiMsg& m) {
        JogEvent e;
        if (m.type == MsgType::NoteOn || m.type == MsgType::NoteOff) {
            if (m.channel > 1) return e;
            if (m.data1 != N_JOG_TOUCH && m.data1 != N_JOG_TOUCH_SH)
                return e;
            e.deck = m.channel;
            e.touching = (m.type == MsgType::NoteOn && m.data2 > 0);
            if (e.touching) scratch[e.deck].touchStart();
            else            scratch[e.deck].touchEnd();
            e.kind = JogEvent::Touch;
            return e;
        }
        if (m.type != MsgType::ControlChange || m.channel > 1)
            return e;
        e.deck  = m.channel;
        e.delta = (int)m.data2 - 0x40;     // rel-64
        if (e.delta == 0) { e.deck = -1; return e; }
        switch (m.data1) {
            case CC_JOG_SIDE:   e.mode = JogMode::JogPitchBend; break;
            case CC_JOG_VON:
                e.mode = scratch[e.deck].touching
                         ? JogMode::JogScratch : JogMode::JogSeek;
                break;
            case CC_JOG_VOFF:   e.mode = JogMode::JogSeek;      break;
            case CC_JOG_SEARCH: e.mode = JogMode::JogSeek;
                                e.delta *= 3;                 break;
            default: return JogEvent{};
        }
        e.kind = JogEvent::Turn;
        return e;
    }

    /// Route a turn through the scratch model → position delta
    /// (seconds). Used when the caller prefers deck-seek semantics
    /// over the engine's jog port.
    double applyTurn(int deck, int delta) {
        if (deck < 0 || deck > 1) return 0.0;
        return scratch[deck].apply(delta);
    }
};

} // namespace onda::ctrl::flx4
