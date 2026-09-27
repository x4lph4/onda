#pragma once

#include <cstdint>
#include <functional>

#include "ui/CommandIds.h"

namespace onda::ctrl {

/// Generic performance-pad engine (tasks §27-29) — the device-agnostic
/// pad surface: per-deck state for 8 pads plus shift variant, velocity
/// normalization, and one emission path into the domain PadLayer
/// (`ui::cmd::forPad` commands — §30: pad semantics live in
/// ui::pads::PadLayer, never duplicated here).
///
///   device plugin / mapping → PadEngine::pad(deck, idx, shift, vel01)
///                               → state update (pressed/active/color)
///                               → ports.sendCommand(forPad(...))
///
/// `PadState` doubles as the LED data model (§28): `color` is the
/// hardware color/brightness index the device shows; `active` marks
/// pads bound to a live action (hot cue set, loop armed, …) so the
/// feedback layer can render dim-vs-lit without re-deriving state.
class PadEngine {
public:
    static constexpr int kDecks = 4;
    static constexpr int kPads  = 8;

    /// Per-pad surface state (§28).
    struct PadState {
        bool    pressed  = false;   // currently held
        float   velocity = 0.0f;    // last press velocity 0..1
        uint8_t color    = 0;       // device color/brightness index
        bool    active   = false;   // bound to a live function
    };

    struct Ports {
        /// → CommandManager::invoke (same port as ControllerEngine).
        std::function<void(uint32_t cmd, float v)> sendCommand;
    };

    void setPorts(Ports p) { ports_ = std::move(p); }

    // ---- input ----
    /// Normalized pad event: vel01 = 0 → release, >0 → press w/
    /// velocity (§29). Emits the forPad command and tracks state.
    /// Returns false for out-of-range inputs (never a crash).
    bool pad(int deck, int index, bool shift, float vel01) {
        if (deck < 0 || deck >= kDecks || index < 0 || index >= kPads)
            return false;
        PadState& s = state_[deck][index][shift ? 1 : 0];
        s.pressed = vel01 > 0.0f;
        if (s.pressed) s.velocity = vel01;
        if (ports_.sendCommand)
            ports_.sendCommand(
                ui::cmd::forPad(deck, index, shift), vel01);
        return true;
    }

    /// Press+release pair helper for test/devices that emit taps.
    void tap(int deck, int index, bool shift = false,
             float vel01 = 1.0f) {
        pad(deck, index, shift, vel01);
        pad(deck, index, shift, 0.0f);
    }

    // ---- surface state (§28) ----
    const PadState& padState(int deck, int index, bool shift = false) const {
        static const PadState kEmpty{};
        if (deck < 0 || deck >= kDecks || index < 0 || index >= kPads)
            return kEmpty;
        return state_[deck][index][shift ? 1 : 0];
    }

    /// LED-facing model: mark a pad's hardware color + active flag.
    /// The feedback layer renders `color` onto the pad address.
    void setPadColor(int deck, int index, uint8_t color, bool active) {
        if (deck < 0 || deck >= kDecks || index < 0 || index >= kPads)
            return;
        state_[deck][index][0].color  = color;
        state_[deck][index][0].active = active;
        state_[deck][index][1] = state_[deck][index][0];
    }

    /// Any pad currently held on the deck (UI/LED refresh hints).
    bool anyPressed(int deck) const {
        if (deck < 0 || deck >= kDecks) return false;
        for (int i = 0; i < kPads; ++i)
            if (state_[deck][i][0].pressed || state_[deck][i][1].pressed)
                return true;
        return false;
    }

private:
    Ports    ports_;
    PadState state_[kDecks][kPads][2];   // [deck][pad][shift]
};

} // namespace onda::ctrl
