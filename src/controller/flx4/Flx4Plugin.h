#pragma once

#include <cctype>
#include <functional>
#include <string>

#include "../ControllerEngine.h"
#include "../ParamNames.h"
#include "ui/CommandIds.h"
#include "ui/pads/PadTypes.h"
#include "Flx4Ids.h"
#include "Flx4Profile.h"
#include "JogWheelProcessor.h"
#include "PadModes.h"

namespace onda::ctrl::flx4 {

/// DDJ-FLX4 semantic plugin — the parts a static mapping table can
/// not express: pad-mode banks, shift-held state, jog touch/vinyl
/// semantics, browse rotary direction, tempo-range cycling, and
/// mode-selector LED repaint.
///
/// Everything else (transport buttons, EQ, faders, tempo slider, FX)
/// stays in the declarative profile and flows through MappingEngine
/// like any other device — the plugin only *consumes* the messages
/// listed in Flx4Ids with "plugin:" targets.
///
/// attach(): bound once to the engine; onDevice watches for a
/// name-matched DDJ-FLX4, installs + applies the built profile,
/// repaints mode LEDs. preDispatch is shared — handleInput() returns
/// false for every other device so the hook stays cheap.
class Flx4Plugin {
public:
    int  deviceId() const { return devId_; }
    bool bound()    const { return devId_ >= 0; }
    PadMode padMode(int deck) const { return pads_.mode[deck & 1]; }
    const JogWheelProcessor& jog() const { return jog_; }
    int rangeIndex(int deck) const { return rangeIdx_[deck & 1]; }

    /// Wire into an engine. Call once after ports are set.
    void attach(ControllerEngine& eng) {
        eng_ = &eng;
        eng.preDispatch = [this](uint32_t dev, const MidiMsg& m) {
            return handleInput(dev, m);
        };
        eng.onDevice = [this](const DeviceInfo& d, int ev) {
            onDevice(d, ev);
        };
    }

    /// Device events from the engine (DeviceManager::Event as int).
    void onDevice(const DeviceInfo& d, int ev) {
        using Ev = DeviceManager::Event;
        const bool match = nameMatches(d.name);
        if ((ev == (int)Ev::Connected || ev == (int)Ev::Reconnected)
            && match) {
            devId_ = (int)d.id;
            ensureProfile();
            repaintModes();
            if (eng_ && eng_->ports().notify)
                eng_->ports().notify("DDJ-FLX4", "connected", 0);
        } else if (ev == (int)Ev::Disconnected && match &&
                   devId_ == (int)d.id) {
            devId_ = -1;
        }
    }

    /// preDispatch hook — consumes plugin-owned messages only.
    bool handleInput(uint32_t dev, const MidiMsg& m) {
        if (!eng_ || devId_ < 0 || (int)dev != devId_) return false;

        // SHIFT held state (deck channels, note 0x3F)
        if (shift_.ingest(m)) return true;

        // pad-mode selectors (deck channel notes) — set domain mode
        // (single source of truth) + repaint hw selector LEDs
        const int sel = pads_.ingestSelector(m);
        if (sel >= 0) {
            sendCmd(ui::cmd::PadModeSetBase + (uint32_t)m.channel,
                    (float)(int)toDomainMode((PadMode)sel));
            repaintModes();
            return true;
        }

        // performance pads (ch 7..10)
        int deck, bank, pad; bool sh;
        if (PadModeManager::classify(m, deck, sh, pad, bank) == 0)
            return handlePad(deck, sh, pad, bank, m);

        // jog family
        const JogEvent j = jog_.decode(m);
        if (j.kind == JogEvent::Touch) return true;
        if (j.kind == JogEvent::Turn) {
            if (ports_jog())
                eng_->ports().jog(j.deck, (int)j.mode, (double)j.delta);
            return true;
        }

        // browse rotary → next/prev track by direction (rel-64)
        if (m.type == MsgType::ControlChange && m.channel == kMixCh &&
            m.data1 == CC_BROWSE) {
            const int d = (int)m.data2 - 0x40;
            if (d > 0)      sendCmd(ui::cmd::BrowserNextTrack);
            else if (d < 0) sendCmd(ui::cmd::BrowserPrevTrack);
            return true;
        }

        // shift+SYNC → tempo-range cycle {±6,±10,±16,±25}
        if (m.type == MsgType::NoteOn && m.data2 > 0 &&
            m.channel <= 1 && m.data1 == N_TEMPO_RANGE) {
            int& idx = rangeIdx_[m.channel];
            idx = (idx + 1) % kTempoRangeCount;
            setPar(deckParamId(m.channel, kDeckParamPitchRange),
                   kTempoRanges[idx]);
            return true;
        }

        return false;   // everything else → static mappings
    }

    /// Reconnect-safe LED restore.
    void repaintModes() {
        for (int d = 0; d < 2; ++d) {
            for (auto& s : kPadSels) {
                MidiAddr a{ AddrKind::Note, (uint8_t)kDeckCh(d), s.note };
                led(a, s.mode == pads_.mode[d] ? kLedOn : kLedOff);
            }
        }
    }

    /// hw selector mode → domain PadMode (PadTypes.h). Unimplemented
    /// banks map to reserved domain modes so pads are consumed, never
    /// misrouted (a KEYBOARD bank pad must not fire hot cues).
    static ui::pads::PadMode toDomainMode(PadMode m) {
        using D = ui::pads::PadMode;
        switch (m) {
            case PadMode::HotCue:   return D::HotCue;
            case PadMode::Sampler:  return D::Sampler;
            case PadMode::BeatJump: return D::BeatJump;
            case PadMode::BeatLoop: return D::Loop;
            case PadMode::PadFx1:
            case PadMode::PadFx2:   return D::PadFx;
            case PadMode::Keyboard: return D::Keyboard;
            case PadMode::KeyShift: return D::KeyShift;
        }
        return D::HotCue;
    }

    static bool nameMatches(const std::string& osName) {
        // match the OS-reported endpoint name, case-insensitive
        std::string n; n.reserve(osName.size());
        for (char c : osName) n += (char)std::tolower((unsigned char)c);
        return n.find("ddj-flx4") != std::string::npos ||
               n.find("ddj flx4") != std::string::npos ||
               n.find("flx4")      != std::string::npos;
    }

private:
    bool ports_jog() const { return eng_ && eng_->ports().jog != nullptr; }
    void sendCmd(uint32_t id, float v = 1.0f) {
        if (eng_ && eng_->ports().sendCommand)
            eng_->ports().sendCommand(id, v);
    }
    void setPar(uint16_t id, float v) {
        if (eng_ && eng_->ports().setParam)
            eng_->ports().setParam(id, v);
    }
    void led(const MidiAddr& a, uint8_t v) {
        if (!eng_ || devId_ < 0) return;
        auto* st = eng_->device((uint32_t)devId_);
        if (st && st->sendRaw)
            st->sendRaw(makeOut(a, v));
    }

    /// Every pad event → the domain pad layer (ui::pads::PadLayer),
    /// which resolves action by the app's current mode — the plugin
    /// carries zero pad semantics (one semantic source of truth for
    /// all controllers). Velocity normalized 0..1.
    bool handlePad(int deck, bool sh, int pad, int bank,
                   const MidiMsg& m) {
        const bool press =
            (m.type == MsgType::NoteOn && m.data2 > 0);
        (void)bank;   // bank is implied by domain mode (selector sets it)
        sendCmd(ui::cmd::forPad(deck, pad, sh),
                press ? (float)m.data2 / 127.0f : 0.0f);
        return true;
    }

    /// Install + apply the built-in profile if the engine's
    /// auto-match didn't already apply one (runs after onDeviceEvent
    /// auto-match, so only fills the gap on first connect).
    void ensureProfile() {
        if (!eng_ || devId_ < 0) return;
        auto* st = eng_->device((uint32_t)devId_);
        if (!st || !st->profileId.empty()) return;
        std::string err;
        const std::string id =
            eng_->profiles().installProfile(buildProfile(), err);
        if (id.empty()) return;
        std::string aerr;
        eng_->profiles().assignProfile((uint32_t)devId_, id, aerr);
        const ControllerProfile* cur =
            eng_->profiles().profileFor((uint32_t)devId_);
        if (cur) {
            std::string perr;
            if (eng_->applyProfile((uint32_t)devId_, *cur, perr)) {
                st->profileId = id;
                st->capabilities |= cur->capabilities;
            }
        }
    }

    ControllerEngine* eng_ = nullptr;
    int               devId_ = -1;
    ShiftLayer        shift_;
    PadModeManager    pads_;
    JogWheelProcessor jog_;
    int               rangeIdx_[2] = { 0, 0 };
};

} // namespace onda::ctrl::flx4
