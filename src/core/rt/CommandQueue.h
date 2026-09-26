#pragma once

#include <cstdint>
#include <variant>

#include "SpscRingBuffer.h"

namespace onda::rt {

/// Commands sent from UI/control threads to the RT audio thread.
/// POD-only payloads. Applied at the top of the audio callback.
struct SetMasterGain  { float gain; };
struct SetLimiterOn   { bool enabled; };
struct SetCueVol      { float vol; };
struct SetCueMix      { float mix; };          // 0 = cue bus, 1 = master bus
struct SetCrossfader  { float pos; };          // -1..+1
struct SetXfCurve     { uint8_t curve; };      // XfCurve
struct SetPfl         { uint8_t deck; bool on; };
struct SetFader       { uint8_t deck; float gain; };

struct DeckTransport  { uint8_t deck; uint8_t action; }; // 0 pause, 1 play, 2 stop
struct DeckSeek       { uint8_t deck; double sec; };
struct DeckUnload     { uint8_t deck; };
struct SetDeckGain    { uint8_t deck; float gain; };
struct SetTrackGain   { uint8_t deck; float gain; };
struct SetPitch       { uint8_t deck; float pct; };
struct SetPitchRange  { uint8_t deck; float pct; };
struct SetPitchBend   { uint8_t deck; float pct; };      // 0 = release
struct SetKeyLock     { uint8_t deck; bool on; };
struct SetReverse     { uint8_t deck; bool on; };
struct SetQuantize    { uint8_t deck; bool on; };
struct SetLoop        { uint8_t deck; double inSec, outSec; };
struct SetLoopOn      { uint8_t deck; bool on; };
struct SetLoopSize    { uint8_t deck; double sizeSec; };
struct SetEqBand      { uint8_t deck; uint8_t band; float db; };
struct SetFilter      { uint8_t deck; float pos; };

using Command = std::variant<
    SetMasterGain, SetLimiterOn, SetCueVol, SetCueMix,
    SetCrossfader, SetXfCurve, SetPfl, SetFader,
    DeckTransport, DeckSeek, DeckUnload, SetDeckGain, SetTrackGain,
    SetPitch, SetPitchRange, SetPitchBend, SetKeyLock, SetReverse,
    SetQuantize, SetLoop, SetLoopOn, SetLoopSize,
    SetEqBand, SetFilter>;

/// UI thread pushes, audio thread drains at the top of every callback.
using CommandQueue = SpscRingBuffer<Command, 512>;

} // namespace onda::rt
