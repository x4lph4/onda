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
    static constexpr int kDecks = 4;
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

            case PadMode::Loop: {
                if (!press) { a.kind = PadAction::None; return a; }
                a.kind = PadAction::Command;
                // pads 1-4: halve/double/in/out — pads 5-8: sized
                // loops 0.5/1/2/4 beats (arg = 10 + size-table index,
                // §11 encoding used by deck.loopSized)
                static const int kSz[4] = { 4, 5, 6, 7 }; // .5/1/2/4bt
                a.cmdId = cmd::forDeck(
                    pad == 0 ? cmd::DeckLoopHalve
                  : pad == 1 ? cmd::DeckLoopDouble
                  : pad == 2 ? cmd::DeckLoopIn
                  : pad == 3 ? cmd::DeckLoopOut
                             : cmd::DeckLoopSizeTrig, deck);
                if (pad >= 4) a.arg = (float)(10 + kSz[pad - 4]);
                a.label = pad == 0 ? "LP/2" : pad == 1 ? "LPx2"
                        : pad == 2 ? "L-IN" : pad == 3 ? "L-OUT"
                        : std::string("LP") +
                          std::to_string(kSz[pad - 4] >= 5
                              ? 1 << (kSz[pad - 4] - 5) : 0)
                          + (kSz[pad - 4] == 4 ? ".5" : "");
                return a; }

            case PadMode::Roll:
                // §15-16 gated loop roll: pad down = roll at that
                // size, pad up = release (slip restores the shadow).
                // pads 1-6 → 1/32…1 beat; 7-8 → extended 2/4 beats.
                a.kind = PadAction::Command;
                a.cmdId = cmd::forDeck(cmd::DeckRollGate, deck);
                a.arg = press ? (float)(pad + 1) : 0.0f;
                {   static const char* kN[8] = { "1/32","1/16","1/8",
                                                 "1/4","1/2","1","2","4" };
                    a.label = std::string("RL") + kN[pad]; }
                return a;

            case PadMode::Stems:
                // §21 — pads 1-4 = stem mute toggle (shift = solo);
                // pads 5-8 = momentary stem gate (hold = kill, release
                // = restore). Mapping lives HERE only — engines and
                // hardware never hardcode it.
                a.kind = PadAction::Command;
                if (pad < 4) {
                    if (!press) { a.kind = PadAction::None; return a; }
                    a.cmdId = cmd::forDeck(cmd::DeckStemBase, deck)
                              + pad * 8 + (shift ? 1 : 0);
                    a.label = std::string(pad == 0 ? "VOC" : pad == 1 ? "DRM"
                                    : pad == 2 ? "BAS" : "INS")
                              + (shift ? " SOLO" : " MUTE");
                    return a;
                }
                a.cmdId = cmd::forDeck(cmd::DeckStemBase, deck)
                          + (pad - 4) * 8 + 6;      // gate
                a.arg = press ? 1.0f : 0.0f;
                a.label = std::string(pad == 4 ? "VOC" : pad == 5 ? "DRM"
                                : pad == 6 ? "BAS" : "INS") + " GATE";
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
    std::array<PadMode, kDecks> mode_ { PadMode::HotCue, PadMode::HotCue,
                                        PadMode::HotCue, PadMode::HotCue };
};

} // namespace onda::ui::pads
