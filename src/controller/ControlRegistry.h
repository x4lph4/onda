#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "MappingTypes.h"
#include "ParamNames.h"
#include "ui/CommandIds.h"

namespace onda::ctrl {

/// Logical control registry (task §10) — the canonical namespace the
/// rest of the application speaks. Hardware mappings address these
/// logical ids; the registry resolves them into concrete targets
/// (CommandManager ids, ParameterRegistry ids, jog ports).
///
/// Naming: "<scope>.<control>[.<index>]" — all lowercase:
///   decka.play       decka.cue        decka.sync       decka.jog
///   decka.pitch      decka.pad.1      decka.loop.size  decka.keylock
///   ch1.eq.low       ch1.fader        mixer.crossfader mixer.headmix
///   browser.next     browser.load.a   fx.unit.on       sampler.pad.1
///
/// Deck letters are a..d (1..4 in docs speak); channel numbers are
/// 1..kMaxChannels human-facing (0-based internally).
struct LogicalControl {
    std::string id;          // "decka.play"
    std::string label;       // human name for the mapping editor
    ControlKind kind = ControlKind::Unknown;
    Target      target;      // resolved app-side target
    bool        momentary = true; // default MappingMode hint
};

/// Static catalog — generated once, deterministic. `resolve()` returns
/// nullptr for unknown ids so profiles can validate a control name
/// before the engine is asked to apply it.
class ControlRegistry {
public:
    ControlRegistry() { build(); }

    /// Logical id → catalog entry (nullptr when unknown).
    const LogicalControl* find(const std::string& id) const {
        auto it = byId_.find(id);
        return it == byId_.end() ? nullptr : &list_[it->second];
    }

    /// All registered controls (mapping editor, diagnostics).
    const std::vector<LogicalControl>& list() const { return list_; }

    /// Build a Mapping skeleton for a logical control: fills target +
    /// a sensible mode/kind hint, leaving `in` for the hardware side
    /// (used by Learn Mode templates and the editor).
    bool skeleton(const std::string& id, Mapping& out) const {
        const LogicalControl* c = find(id);
        if (!c) return false;
        out.target = c->target;
        out.kind   = c->kind;
        out.mode   = c->momentary ? MappingMode::Momentary
                                : MappingMode::Absolute;
        return true;
    }

private:
    void cmd_(const std::string& id, const std::string& label,
              uint32_t cmd, ControlKind k,
              std::string storeName = "") {
        LogicalControl c;
        c.id = id; c.label = label; c.kind = k; c.momentary = true;
        c.target.kind = Target::Kind::Command;
        c.target.cmd  = cmd;
        c.target.cmdName = std::move(storeName);
        byId_[c.id] = list_.size();
        list_.push_back(std::move(c));
    }
    void par_(const std::string& id, const std::string& label,
              uint16_t param, ControlKind k) {
        LogicalControl c;
        c.id = id; c.label = label; c.kind = k; c.momentary = false;
        c.target.kind = Target::Kind::Param;
        c.target.paramId = param;
        byId_[c.id] = list_.size();
        list_.push_back(std::move(c));
    }
    void jog_(const std::string& id, const std::string& label,
              int deck, JogMode mode) {
        LogicalControl c;
        c.id = id; c.label = label; c.kind = ControlKind::Jog;
        c.momentary = false;
        c.target.kind = Target::Kind::Jog;
        c.target.jogDeck = (uint8_t)deck;
        c.target.jogMode = (uint8_t)mode;
        byId_[c.id] = list_.size();
        list_.push_back(std::move(c));
    }

    void build() {
        using namespace onda::ui;
        // ---- deck controls (4 decks, spec §36) ----
        for (int d = 0; d < 4; ++d) {
            const std::string dn = "deck" + std::string(1, char('a' + d));
            const std::string lb = "Deck " + std::string(1, char('A' + d));
            auto D = [&](const char* tail, const char* what,
                         cmd::Id base, ControlKind k,
                         const char* cmdName) {
                // storeName = the REAL CommandManager name — needed
                // when the resolved id is 0 (deck A PlayToggle is a
                // valid id that collides with "unset").
                cmd_(dn + "." + tail,
                     lb + " " + what,
                     cmd::forDeck(base, d), k,
                     dn + "." + cmdName);
            };
            D("play",      "Play/Pause",  cmd::DeckPlayToggle,
              ControlKind::Button,   "playToggle");
            D("cue",       "Cue",         cmd::DeckCueTrigger,
              ControlKind::Button,   "cueTrigger");
            D("sync",      "Sync",        cmd::DeckSyncToggle,
              ControlKind::Toggle,   "syncToggle");
            D("quantize",  "Quantize",    cmd::DeckQuantizeTgl,
              ControlKind::Toggle,   "quantizeToggle");
            D("keylock",   "Key Lock",    cmd::DeckKeyLockTgl,
              ControlKind::Toggle,   "keyLockToggle");
            D("pfl",       "PFL Cue",     cmd::DeckPflToggle,
              ControlKind::Toggle,   "pflToggle");
            D("loop.in",   "Loop In",     cmd::DeckLoopIn,
              ControlKind::Button,   "loopIn");
            D("loop.out",  "Loop Out",    cmd::DeckLoopOut,
              ControlKind::Button,   "loopOut");
            D("loop.toggle","Loop On/Off",cmd::DeckLoopToggle,
              ControlKind::Toggle,   "loopToggle");
            D("loop.halve","Loop 1/2",    cmd::DeckLoopHalve,
              ControlKind::Button,   "loopHalve");
            D("loop.double","Loop x2",    cmd::DeckLoopDouble,
              ControlKind::Button,   "loopDouble");
            D("jump.fwd",  "Beat Jump +", cmd::DeckJumpFwd,
              ControlKind::Button,   "jumpFwd");
            D("jump.back", "Beat Jump -", cmd::DeckJumpBack,
              ControlKind::Button,   "jumpBack");
            D("stop",      "Stop",        cmd::DeckStop,
              ControlKind::Button,   "stop");
            D("reverse",   "Reverse",     cmd::DeckReverseTgl,
              ControlKind::Momentary,"reverseToggle");
            D("load",      "Load Track",  cmd::DeckLoad,
              ControlKind::Button,   "load");

            par_(dn + ".pitch", lb + " Tempo",
                 deckParamId(d, kDeckParamPitch), ControlKind::Fader);
            par_(dn + ".pitch.range", lb + " Tempo Range",
                 deckParamId(d, kDeckParamPitchRange), ControlKind::Switch);
            par_(dn + ".eq.low",  lb + " EQ Low",
                 mixer::ParameterRegistry::channelLow(d), ControlKind::Knob);
            par_(dn + ".eq.mid",  lb + " EQ Mid",
                 mixer::ParameterRegistry::channelMid(d), ControlKind::Knob);
            par_(dn + ".eq.high", lb + " EQ High",
                 mixer::ParameterRegistry::channelHigh(d), ControlKind::Knob);
            par_(dn + ".gain",    lb + " Trim",
                 mixer::ParameterRegistry::channelGain(d), ControlKind::Knob);
            par_(dn + ".filter",  lb + " Filter",
                 mixer::ParameterRegistry::channelFilter(d), ControlKind::Knob);
            par_(dn + ".fader",   lb + " Channel Fader",
                 mixer::ParameterRegistry::channelFader(d), ControlKind::Fader);

            jog_(dn + ".jog",       lb + " Jog Seek",    d, JogMode::JogSeek);
            jog_(dn + ".jog.bend",  lb + " Jog Nudge",   d, JogMode::JogPitchBend);
            jog_(dn + ".jog.scratch",lb + " Jog Scratch",d, JogMode::JogScratch);

            for (int p = 0; p < 8; ++p) {
                for (int sh = 0; sh < 2; ++sh) {
                    const std::string id =
                        dn + ".pad." + std::to_string(p + 1) +
                        (sh ? ".shift" : "");
                    cmd_(id, lb + " Pad " + std::to_string(p + 1) +
                         (sh ? " (shift)" : ""),
                         cmd::forPad(d, p, sh != 0), ControlKind::Pad,
                         // PadLayer registers "pad.<d>.<idx>[.shift]"
                         "pad." + std::string(1, char('a' + d)) + "." +
                         std::to_string(p) + (sh ? ".shift" : ""));
                }
            }
        }

        // ---- mixer / bus (§35) ----
        par_("mixer.crossfader", "Crossfader",
             mixer::ParameterRegistry::CROSSFADER, ControlKind::Fader);
        par_("mixer.xf.curve", "Crossfader Curve",
             mixer::ParameterRegistry::XF_CURVE, ControlKind::Knob);
        par_("mixer.master.gain", "Master Gain",
             mixer::ParameterRegistry::MASTER_GAIN, ControlKind::Knob);
        par_("mixer.headmix", "Phones Cue/Master",
             mixer::ParameterRegistry::CUE_MIX, ControlKind::Knob);
        par_("mixer.headvol", "Phones Level",
             mixer::ParameterRegistry::CUE_VOLUME, ControlKind::Knob);
        par_("mixer.headmono", "Phones Mono",
             (uint16_t)mixer::BusParam::HeadphoneMono, ControlKind::Toggle);
        par_("mixer.booth", "Booth Level",
             mixer::ParameterRegistry::BOOTH_GAIN, ControlKind::Knob);
        for (int ch = 0; ch < mixer::ParameterRegistry::kMaxChannels; ++ch) {
            par_("ch" + std::to_string(ch + 1) + ".cue",
                 "Channel " + std::to_string(ch + 1) + " PFL",
                 mixer::ParameterRegistry::channelCue(ch), ControlKind::Toggle);
        }

        // ---- browser (§36 browse) ----
        cmd_("browser.next",  "Next Track",      cmd::BrowserNextTrack,
             ControlKind::Encoder, "browser.nextTrack");
        cmd_("browser.prev",  "Previous Track",  cmd::BrowserPrevTrack,
             ControlKind::Encoder, "browser.prevTrack");
        cmd_("browser.load.a","Load Deck A",     cmd::BrowserLoadDeckA,
             ControlKind::Button, "browser.loadDeckA");
        cmd_("browser.load.b","Load Deck B",     cmd::BrowserLoadDeckB,
             ControlKind::Button, "browser.loadDeckB");
        cmd_("browser.focus", "Focus Browser",   cmd::BrowserTogglePanel,
             ControlKind::Button, "browser.togglePanel");

        // ---- app-level ----
        cmd_("app.panic", "Panic (stop all)", cmd::AppPanic,
             ControlKind::Switch, "app.panic");
        cmd_("record.toggle", "Quick Record", cmd::RecToggle,
             ControlKind::Toggle, "rec.toggle");
    }

    std::vector<LogicalControl> list_;
    std::unordered_map<std::string, size_t> byId_;
};

/// Process-wide registry (Meyers singleton — trivial, header-only).
inline const ControlRegistry& controlRegistry() {
    static const ControlRegistry r;
    return r;
}

} // namespace onda::ctrl
