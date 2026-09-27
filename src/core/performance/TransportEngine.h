#pragma once

#include <cstdint>

namespace onda {

/// Transport verbs — the single transport vocabulary every caller
/// (UI, keyboard, MIDI, HID, automation) shares (Phase 28 §3). There
/// is exactly ONE transport implementation: PerformanceEngine::
/// transport() dispatches these through the scheduler, so no input
/// path may emit DeckTransport commands directly.
///
///   Play/Pause/Toggle/Stop — timeline control
///   CuePress/CueRelease    — the device-agnostic cue button (hold
///                          preview / return semantics live in
///                          PerformanceEngine::cueButton, honouring
///                          the deck's configured CueBehavior)
///   Restart                — instant restart from the cue point,
///                          sync/master untouched
///   Brake/BrakeOn/BrakeOff — vinyl spin-down → pause; release spins
///                          back up and resumes (RT ramp in
///                          DeckEngine, state toggled here)
enum class Transport : int {
    Play = 0, Pause, Toggle, Stop,
    CuePress, CueRelease, Restart,
    Brake, BrakeOn, BrakeOff
};

/// Non-RT per-deck brake bookkeeping (kept in DeckCtx; the ramp itself
/// is RT-side in DeckEngine so it never depends on UI timers).
struct TransportEngine {
    bool   brakeOn  { false };
    /// Brake duration in ms — the RT side receives seconds; tests and
    /// diagnostics read this value back through deckState().
    double brakeSec { 0.55 };

    /// Edge logic: returns the on-state to send (or -1 = no change).
    int apply(Transport t) noexcept {
        switch (t) {
            case Transport::Brake:
                brakeOn = !brakeOn;
                return brakeOn ? 1 : 0;
            case Transport::BrakeOn:
                if (brakeOn) return -1;
                brakeOn = true;
                return 1;
            case Transport::BrakeOff:
                if (!brakeOn) return -1;
                brakeOn = false;
                return 0;
            default: return -1;
        }
    }
    void clear() noexcept { brakeOn = false; }
};

} // namespace onda
