#pragma once

#include <cstdint>
#include <vector>

#include "../ControllerEngine.h"
#include "Flx4Ids.h"

namespace onda::ctrl::flx4 {

/// Virtual DDJ-FLX4 (task §21) — simulates the real wire protocol:
/// deck channels, 14-bit MSB+LSB pairs, pad banks on ch 7-10, mixer
/// on ch 6, FX on ch 4/5. Captures every outbound message (LED
/// echoes, position dumps) the same way VirtualMidiController does.
struct VirtualFlx4 {
    explicit VirtualFlx4(ControllerEngine& e) : eng(&e) {}

    static DeviceInfo info() {
        return { kId, "DDJ-FLX4", kManufacturer, Transport::Virtual,
                 CAP_JOG_WHEEL | CAP_TOUCH | CAP_PADS | CAP_FADERS |
                 CAP_EQ | CAP_FILTER | CAP_CROSSFADER |
                 CAP_FX_CONTROLS | CAP_LED_FEEDBACK,
                 true, true };
    }
    static constexpr uint32_t kId = 0xF140;   // arbitrary test id

    uint32_t attach(double nowSec = 0.0) {
        return (uint32_t)eng->attachDevice(info(),
            [this](const MidiMsg& m) { outbox.push_back(m); }, nowSec);
    }
    void detach(double nowSec) { eng->detachDevice(kId, nowSec); }

    // ---- physical control simulation ----
    void send(const MidiMsg& m) { eng->processInput(kId, m); }
    void note(int ch, int n, int vel, int64_t us = 0) {
        send({ vel > 0 ? MsgType::NoteOn : MsgType::NoteOff,
               (uint8_t)ch, (uint8_t)n, (uint8_t)vel, us });
    }
    void press(int ch, int n, int64_t us = 0) {
        note(ch, n, 0x7F, us); note(ch, n, 0, us + 5000);
    }
    void hold(int ch, int n, int64_t us = 0)  { note(ch, n, 0x7F, us); }
    void release(int ch, int n, int64_t us = 0){ note(ch, n, 0, us); }
    void cc(int ch, int c, int v, int64_t us = 0) {
        send({ MsgType::ControlChange, (uint8_t)ch, (uint8_t)c,
               (uint8_t)v, us });
    }
    /// 14-bit continuous control: MSB first, then LSB (per OFF list)
    void cc14(int ch, int c, int v14, int64_t us = 0) {
        cc(ch, c, (v14 >> 7) & 0x7F, us);
        cc(ch, c + 0x20, v14 & 0x7F, us + 1000);
    }

    // ---- deck helpers (deck = 0/1) ----
    void pressPlay(int d, int64_t us = 0)  { press(kDeckCh(d), N_PLAY, us); }
    void pressCue(int d, int64_t us = 0)   { press(kDeckCh(d), N_CUE, us); }
    void pressSync(int d, int64_t us = 0)  { press(kDeckCh(d), N_SYNC, us); }
    void pressPfl(int d, int64_t us = 0)   { press(kDeckCh(d), N_PFL, us); }
    void shiftDown(int d, int64_t us = 0)  { hold(kDeckCh(d), N_SHIFT, us); }
    void shiftUp(int d, int64_t us = 0)    { release(kDeckCh(d), N_SHIFT, us); }
    void padMode(int d, PadSel sel, int64_t us = 0) {
        press(kDeckCh(d), sel.note, us); }
    void padMode(int d, PadMode m, int64_t us = 0) {
        press(kDeckCh(d), padSelNote(m), us); }
    void pad(int d, int bank, int pad, bool sh = false, int64_t us = 0) {
        press(sh ? kPadChShift(d) : kPadCh(d), bank + pad, us); }
    void jogTouch(int d, bool down, int64_t us = 0) {
        note(kDeckCh(d), N_JOG_TOUCH, down ? 0x7F : 0, us); }
    void jogTurn(int d, int ccNum, int ticks, int64_t us = 0) {
        cc(kDeckCh(d), ccNum, 0x40 + ticks, us); }
    void jogPlatter(int d, int ticks, int64_t us = 0) {
        jogTurn(d, CC_JOG_VON, ticks, us); }
    void jogSide(int d, int ticks, int64_t us = 0) {
        jogTurn(d, CC_JOG_SIDE, ticks, us); }
    void jogSearch(int d, int ticks, int64_t us = 0) {
        jogTurn(d, CC_JOG_SEARCH, ticks, us); }
    void tempo(int d, int v14, int64_t us = 0) {
        cc14(kDeckCh(d), CC_TEMPO, v14, us); }
    void trim(int d, int v14, int64_t us = 0) {
        cc14(kDeckCh(d), CC_TRIM, v14, us); }
    void eqHi(int d, int v14, int64_t us = 0) {
        cc14(kDeckCh(d), CC_EQ_HI, v14, us); }
    void chFader(int d, int v14, int64_t us = 0) {
        cc14(kDeckCh(d), CC_FADER, v14, us); }

    // ---- mixer helpers ----
    void crossfader(int v14, int64_t us = 0) { cc14(kMixCh, CC_XF, v14, us); }
    void cfx(int deck, int v14, int64_t us = 0) {
        cc14(kMixCh, deck ? CC_CFX_B : CC_CFX_A, v14, us); }
    void browse(int ticks, int64_t us = 0) {
        cc(kMixCh, CC_BROWSE, 0x40 + ticks, us); }
    void loadDeckA(int64_t us = 0) { press(kMixCh, N_LOAD_A, us); }
    void loadDeckB(int64_t us = 0) { press(kMixCh, N_LOAD_B, us); }
    void fxOn(int unit, int64_t us = 0) {
        press(unit ? kFxChB : kFxChA, N_FX_ON, us); }
    void fxDepth(int v14, int64_t us = 0) { cc14(kFxChA, CC_FX_DEPTH, v14, us); }

    // ---- output capture ----
    std::vector<MidiMsg> outbox;
    int outValueFor(const MidiAddr& a) const {
        for (auto it = outbox.rbegin(); it != outbox.rend(); ++it)
            if (addrOf(*it) == a) return it->data2;
        return -1;
    }
    void clearOutbox() { outbox.clear(); }

    ControllerEngine* eng;
};

} // namespace onda::ctrl::flx4
