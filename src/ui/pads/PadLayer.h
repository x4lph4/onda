#pragma once

#include <functional>
#include <string>

#include "../CommandManager.h"
#include "PadModeManager.h"
#include "PadTypes.h"

namespace onda::ui::pads {

/// Domain pad layer (task §14/§15/§30). Registers `pad.<d>.<n>[.shift]`
/// and `pad.<d>.mode` commands into the CommandManager; every pad
/// event — DDJ-FLX4, keyboard, UI, any controller — resolves through
/// PadModeManager into a deck command or a sampler action. One
/// semantic source of truth; hardware stays semantics-free.
///
///   pad event → resolve(deck,pad,shift,press,vel)
///     → Command      → commands.invoke(cmdId, src, arg)
///     → SampleTrig   → ports.sampleTrig(slot, vel)
///     → SampleStop   → ports.sampleStop(slot)
class PadLayer {
public:
    struct Ports {
        /// resolve to a CommandManager invoke (recursive-safe — pad
        /// commands dispatch inner commands through the same manager)
        std::function<void(int slot, float vel)> sampleTrig;
        std::function<void(int slot)>            sampleRelease;
        std::function<void(int slot)>            sampleStop;
    };

    void setPorts(Ports p) { ports_ = std::move(p); }
    PadModeManager&       modes()       { return mgr_; }
    const PadModeManager& modes() const { return mgr_; }

    /// Register pad.press + pad.mode commands for both decks.
    /// (also binds the internal dispatch to this CommandManager)
    void registerCommands(CommandManager& cm) {
        cm_ = &cm;
        for (int d = 0; d < PadModeManager::kDecks; ++d) {
            const std::string dn = std::string(1, char('a' + d));
            for (int pad = 0; pad < PadModeManager::kPads; ++pad) {
                for (int sh = 0; sh < 2; ++sh) {
                    Command c;
                    c.id = cmd::forPad(d, pad, sh != 0);
                    c.name = "pad." + dn + "." + std::to_string(pad) +
                             (sh ? ".shift" : "");
                    c.category = "Pads";
                    c.run = [this, d, pad, sh](float v, CommandSource src) {
                        dispatch(cm_, d, pad, sh != 0,
                                 v > 0.5f, v > 0.0f ? v : 1.0f, src);
                    };
                    cm.add(std::move(c));
                }
            }
            Command mc;
            mc.id = cmd::PadModeSetBase + (uint32_t)d;
            mc.name = "pad." + dn + ".mode";
            mc.category = "Pads";
            mc.run = [this, d](float v, CommandSource) {
                mgr_.setMode(d, (PadMode)(int)v);
            };
            mc.state = [this, d]() {
                return (int)mgr_.mode(d);
            };
            cm.add(std::move(mc));
        }
    }

    /// For controller profiles that want a readable label per pad.
    std::string label(int deck, int pad, bool shift = false) const {
        return mgr_.resolve(deck, pad, shift, true).label;
    }

    /// For pad state in UI / LED color hints.
    PadMode mode(int deck) const { return mgr_.mode(deck); }

private:
    void dispatch(CommandManager* cm, int deck, int pad, bool shift,
                  bool press, float vel, CommandSource src) {
        if (!cm) return;
        PadAction a = mgr_.resolve(deck, pad, shift, press, vel);
        switch (a.kind) {
            case PadAction::Command:
                if (a.cmdId) cm->invoke(a.cmdId, src, a.arg);
                break;
            case PadAction::SampleTrig:
                if (ports_.sampleTrig) ports_.sampleTrig(a.slot, a.arg);
                break;
            case PadAction::SampleRelease:
                if (ports_.sampleRelease) ports_.sampleRelease(a.slot);
                break;
            case PadAction::SampleStop:
                if (ports_.sampleStop) ports_.sampleStop(a.slot);
                break;
            default: break;
        }
    }

    CommandManager* cm_ = nullptr;

    Ports          ports_;
    PadModeManager mgr_;
};

} // namespace onda::ui::pads
