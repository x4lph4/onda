#pragma once

#include <cstdint>
#include <string>

namespace onda::ctrl {

/// Library-agnostic MIDI model — the rest of the app never sees a
/// juce::MidiMessage or platform handle. Timestamps are microseconds,
/// assigned by the input adapter (hardware/driver time when exposed).
enum class MsgType : uint8_t {
    Unknown = 0,
    NoteOn, NoteOff,
    ControlChange,
    ProgramChange,
    PitchBend,
    ChannelPressure,          // aftertouch
    // realtime clock family (no channel)
    ClockTick, ClockStart, ClockStop, ClockContinue,
};

struct MidiMsg {
    MsgType type    = MsgType::Unknown;
    uint8_t channel = 0;      // 0..15
    uint8_t data1   = 0;      // note / cc / program / lsb
    uint8_t data2   = 0;      // velocity / value / msb
    int64_t timeUs  = 0;      // µs — input time, for latency metrics

    bool isRealtime() const {
        return type >= MsgType::ClockTick;
    }
};

/// Address of one physical control — how mappings and feedback find
/// each other. NoteOn/NoteOff share the "Note" kind (a pad is one
/// control; the message family differs by velocity).
enum class AddrKind : uint8_t {
    Note = 0, CC, PitchBend, ChannelPressure, Program,
};

struct MidiAddr {
    AddrKind kind    = AddrKind::Note;
    uint8_t  channel = 0;
    uint8_t  number  = 0;     // note# / cc# / program# (0 for pb/aftertouch)

    bool operator==(const MidiAddr& o) const {
        return kind == o.kind && channel == o.channel &&
               number == o.number;
    }
    uint32_t pack() const {   // dense key for maps
        return ((uint32_t)kind << 20) |
               ((uint32_t)(channel & 0xF) << 8) | number;
    }
    static MidiAddr unpack(uint32_t v) {
        return { (AddrKind)(v >> 20),
                 (uint8_t)((v >> 8) & 0xF), (uint8_t)(v & 0xFF) };
    }
};

/// Message → address it belongs to (NoteOff folds into Note).
inline MidiAddr addrOf(const MidiMsg& m) {
    switch (m.type) {
        case MsgType::NoteOn:
        case MsgType::NoteOff:      return { AddrKind::Note,
                                             m.channel, m.data1 };
        case MsgType::ControlChange:return { AddrKind::CC,
                                             m.channel, m.data1 };
        case MsgType::PitchBend:    return { AddrKind::PitchBend,
                                             m.channel, 0 };
        case MsgType::ChannelPressure: return { AddrKind::ChannelPressure,
                                                m.channel, 0 };
        case MsgType::ProgramChange:return { AddrKind::Program,
                                             m.channel, m.data1 };
        default:                    return { AddrKind::Note, 0, 0 };
    }
}

/// Raw 14-bit-ish normalized value of a message (0..1).
/// Pitch bend uses both data bytes; everything else is 7-bit.
inline float msgValue01(const MidiMsg& m) {
    if (m.type == MsgType::PitchBend)
        return ((int)m.data2 * 128 + (int)m.data1) / 16383.0f;
    return m.data2 / 127.0f;
}

/// Build a MsgType for an output address (Note/CC only for LEDs).
inline MidiMsg makeOut(const MidiAddr& a, uint8_t value) {
    MidiMsg m;
    m.channel = a.channel;
    m.data1 = a.number;
    m.data2 = value;
    m.type = a.kind == AddrKind::CC ? MsgType::ControlChange
                                    : MsgType::NoteOn;
    return m;
}

/// Transport classes — what physical link carries the messages.
enum class Transport : uint8_t {
    MidiUsb = 0, MidiDin, MidiBle, Hid, Virtual,
};

/// Device capabilities bitfield — the UI reads this to adapt.
enum Cap : uint32_t {
    CAP_JOG_WHEEL   = 1u << 0,
    CAP_TOUCH       = 1u << 1,
    CAP_PADS        = 1u << 2,
    CAP_FADERS      = 1u << 3,
    CAP_EQ          = 1u << 4,
    CAP_FILTER      = 1u << 5,
    CAP_CROSSFADER  = 1u << 6,
    CAP_FX_CONTROLS = 1u << 7,
    CAP_LED_FEEDBACK= 1u << 8,
    CAP_DISPLAY     = 1u << 9,
    CAP_MIDI_CLOCK  = 1u << 10,
    CAP_HID         = 1u << 11,
};

/// A controller as seen by DeviceManager (platform-agnostic).
struct DeviceInfo {
    uint32_t    id = 0;
    std::string name;             // "DDJ-FLX4" (OS-reported)
    std::string manufacturer;
    Transport   transport = Transport::MidiUsb;
    uint32_t    capabilities = 0;
    bool        hasInput  = true;
    bool        hasOutput = false;
};

} // namespace onda::ctrl
