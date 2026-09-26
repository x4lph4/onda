#pragma once

#include <array>

#include "../CommandIds.h"
#include "PadTypes.h"

namespace onda::ui::pads {

/// The domain pad-mode table — the SINGLE place that decides what a
/// physical pad does in each mode (task §13/§15). Controllers never
/// hardcode semantics: they emit `pad.press`/`pad.mode` commands and
/// this layer resolves the action. Works identically for the
/// DDJ-FLX4, keyboard, UI clicks and future hardware.
class PadModeManager {
public:
    static constexpr int kDecks = 2;
    static constexpr int kPads  = 8;

    PadMode mode(int deck) const { return mode_[deck & (kDecks-1)]; }
    void setMode(int deck, PadMode m) { mode_[deck & (kDecks-1)] = m; }

    /// press=true → pad down, press=false → release (gate/hold modes).
    /// shift = hardware shift layer active during the press.
    PadAction resolve(int deck, int pad, bool shift, bool press,
                      float velocity = 1.0f) const {
        PadAction a;
        deck &= (kDecks - 1); pad &= 7;
        const PadMode m = mode_[deck];

        switch (m) {
            case PadMode::HotCue:
                // press = trigger; shift+press = delete (documented);
                // releases are consumed silently (cue is press-only)
                if (!press) return a;
                a.kind = PadAction::Command;
                a.cmdId = cmd::forHotCue(deck, pad, shift ? 2 : 0);
                a.label = "CUE " + std::to_string(pad + 1) +
                          (shift ? " DEL" : "");
                return a;

            case PadMode::Sampler:
                if (shift) {                    // shift+pad = stop
                    if (!press) { a.kind = PadAction::None; return a; }
                    a.kind = PadAction::SampleStop;
                    a.slot = pad;
                    a.label = "SMP " + std::to_string(pad + 1) + " STOP";
                    return a;
                }
                a.kind = press ? PadAction::SampleTrig
                               : PadAction::SampleRelease; // pad up→hold release
                // release only matters for HOLD/GATE slots — the
                // sampler decides from the slot's mode; trigger side
                // sends it anyway (cheap, idempotent)
                a.slot = pad;
                a.arg  = velocity;
                a.label = "SMP " + std::to_string(pad + 1);
                return a;

            case PadMode::BeatJump:
                if (!press) { a.kind = PadAction::None; return a; }
                a.kind = PadAction::Command;
                a.cmdId = cmd::forDeck(
                    pad < 4 ? cmd::DeckJumpBack : cmd::DeckJumpFwd, deck);
                a.arg = std::fabs(beatJumpBeats(pad));  // size hint
                a.label = std::string(pad < 4 ? "◀" : "▶") +
                          std::to_string((int)std::fabs(
                              beatJumpBeats(pad))) + "BT";
                return a;

            case PadMode::Loop:
                if (!press) { a.kind = PadAction::None; return a; }
                a.kind = PadAction::Command;
                // pads: 1-2 halve/double, 3 in, 4 out,
                //       5-8 sized loops via loopToggle
                a.cmdId = cmd::forDeck(
                    pad == 0 ? cmd::DeckLoopHalve
                  : pad == 1 ? cmd::DeckLoopDouble
                  : pad == 2 ? cmd::DeckLoopIn
                  : pad == 3 ? cmd::DeckLoopOut
                             : cmd::DeckLoopToggle, deck);
                a.label = pad == 0 ? "LP/2" : pad == 1 ? "LPx2"
                        : pad == 2 ? "L-IN" : pad == 3 ? "L-OUT"
                        : "LOOP";
                return a;

            case PadMode::PadFx:
                if (!press) { a.kind = PadAction::None; return a; }
                // reserved for a future FX-preset surface — consumed
                // but unmapped until the preset bank exists
                a.kind = PadAction::None;
                a.label = "FX " + std::to_string(pad + 1);
                return a;

            default:                              // reserved modes
                a.kind = PadAction::None;
                a.label = padModeName(m);
                return a;
        }
    }

private:
    std::array<PadMode, kDecks> mode_ { PadMode::HotCue,
                                        PadMode::HotCue };
};

} // namespace onda::ui::pads
