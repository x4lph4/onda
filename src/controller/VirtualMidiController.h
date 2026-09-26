#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ControllerEngine.h"

namespace onda::ctrl {

/// Virtual MIDI controller for tests — simulates buttons, knobs,
/// faders, encoders, jogs and captures every message the engine sends
/// back (LEDs, motors, clock).
///
/// Usage:
///   ControllerEngine eng(tmpDir);
///   VirtualMidiController vc(eng);
///   vc.attach();                       // attach + auto-profile
///   vc.pressButton(36);                // NoteOn/NoteOff pair
///   vc.turnKnob(16, 64);               // CC absolute
///   REQUIRE(vc.outbox().size() == 1);  // LED feedback captured
class VirtualMidiController {
public:
    VirtualMidiController(ControllerEngine& eng,
                          std::string name = "Virtual Ctrl 1")
        : eng_(&eng), info_{ 0xF00D, std::move(name), "OndaTests",
                             Transport::Virtual,
                             CAP_PADS | CAP_FADERS | CAP_EQ |
                             CAP_JOG_WHEEL | CAP_TOUCH |
                             CAP_LED_FEEDBACK | CAP_MIDI_CLOCK,
                             true, true } {}

    /// Attach to the engine (sendRaw captures into outbox_).
    uint32_t attach(double nowSec = 0.0) {
        return (uint32_t)eng_->attachDevice(info_,
            [this](const MidiMsg& m) { outbox_.push_back(m); },
            nowSec);
    }
    void detach(double nowSec) { eng_->detachDevice(info_.id, nowSec); }
    DeviceInfo& info() { return info_; }

    // ---- physical control simulation ----
    void note(int num, uint8_t vel, int64_t us = 0) {
        send({ MsgType::NoteOn, 0, (uint8_t)num, vel, us }); }
    void pressButton(int num, int64_t us = 0) {
        note(num, 127, us); send({ MsgType::NoteOff, 0,
                                   (uint8_t)num, 0, us + 5000 }); }
    void holdButton(int num, int64_t us = 0) { note(num, 127, us); }
    void releaseButton(int num, int64_t us = 0) {
        send({ MsgType::NoteOff, 0, (uint8_t)num, 0, us }); }
    void turnKnob(int cc, int v, int64_t us = 0) {
        send({ MsgType::ControlChange, 0, (uint8_t)cc,
               (uint8_t)v, us }); }
    void moveFader(int cc, int v, int64_t us = 0) { turnKnob(cc, v, us); }
    void spinEncoder(int cc, int ticks, int64_t us = 0) {
        for (int i = 0; i < (ticks > 0 ? ticks : -ticks); ++i)
            send({ MsgType::ControlChange, 0, (uint8_t)cc,
                   (uint8_t)(64 + (ticks > 0 ? 1 : -1)),
                   us + i * 2000 });
    }
    void spinEncoderRaw(int cc, int rawByte, int64_t us = 0) {
        send({ MsgType::ControlChange, 0, (uint8_t)cc,
               (uint8_t)rawByte, us }); }
    void spinJog(int cc, int ticks, int64_t us = 0) {
        spinEncoder(cc, ticks, us); }
    void pitchBend(int v14, int64_t us = 0) {
        send({ MsgType::PitchBend, 0, (uint8_t)(v14 & 0x7F),
               (uint8_t)(v14 >> 7), us }); }
    void clockTick(int64_t us) {
        send({ MsgType::ClockTick, 0, 0, 0, us }); }
    void clockStart(int64_t us = 0) {
        send({ MsgType::ClockStart, 0, 0, 0, us }); }
    void clockStop(int64_t us = 0) {
        send({ MsgType::ClockStop, 0, 0, 0, us }); }

    // ---- engine output capture ----
    std::vector<MidiMsg>&       outbox()       { return outbox_; }
    const MidiMsg*              lastOut()      const {
        return outbox_.empty() ? nullptr : &outbox_.back(); }
    void clearOutbox()          { outbox_.clear(); }
    int outValueFor(const MidiAddr& a) const {
        for (auto it = outbox_.rbegin(); it != outbox_.rend(); ++it)
            if (addrOf(*it) == a) return it->data2;
        return -1;
    }

private:
    void send(const MidiMsg& m) { eng_->processInput(info_.id, m); }

    ControllerEngine* eng_;
    DeviceInfo        info_;
    std::vector<MidiMsg> outbox_;
};

} // namespace onda::ctrl
