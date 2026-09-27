#pragma once

#include <array>
#include <cstdint>

namespace onda {

/// Performance action model (Phase 28 §42-44). Every deck-side
/// performance gesture is one typed action with an explicit state —
/// never a bare boolean — so simultaneous inputs resolve
/// deterministically instead of corrupting each other.
enum class PerfAction : int {
    HotCue = 0, Loop, LoopRoll, BeatJump,
    Scratch, Reverse, Slip, Count
};

/// §44 — action lifecycle. Instantaneous actions (hot cue, beat jump)
/// go ARMED → ACTIVE → COMPLETED within one call; held actions (roll,
/// scratch, momentary reverse) pass RELEASING before COMPLETED and
/// then return to IDLE.
enum class ActionState : int {
    Idle = 0, Armed, Active, Releasing, Completed
};

/// One tracked action on a deck (§42 — carries its timestamp).
struct PerformanceAction {
    PerfAction  type       { PerfAction::HotCue };
    ActionState state      { ActionState::Idle };
    int         slot       { -1 };    // cue slot / action arg
    uint32_t    timestampMs{ 0 };     // injected clock — never a timer
    double      value      { 0.0 };   // beats / position depending on type
};

/// §43 — deterministic priority + compatibility rules when actions
/// arrive simultaneously. The resolver is pure: given the deck's
/// action table and an incoming action it returns a decision; the
/// facade performs the side effects.
struct PerformanceActionResolver {
    enum class Decision : int { Allow = 0, Deny, Resize };

    /// Lower number = higher priority (documented policy; the number
    /// itself is only used for ordering, never timing).
    static int priority(PerfAction t) noexcept {
        switch (t) {
            case PerfAction::Scratch:   return 0;  // platter always wins
            case PerfAction::LoopRoll:  return 1;  // held temporary loop
            case PerfAction::Reverse:   return 2;  // momentary layer
            case PerfAction::HotCue:    return 3;
            case PerfAction::BeatJump:  return 4;
            case PerfAction::Loop:      return 5;
            case PerfAction::Slip:      return 6;
            default:                    return 7;
        }
    }

    /// Compatibility of `incoming` against the running action table.
    /// Rules (documented — do not silently weaken):
    ///  - LoopRoll while a roll runs        → Resize (live size change)
    ///  - Scratch while a roll is held     → Deny (corrupts the roll
    ///                                       window mid-flight)
    ///  - LoopRoll while scratching        → Deny
    ///  - everything else                  → Allow (slip refs compose)
    template <size_t N>
    static Decision resolve(
        const std::array<PerformanceAction, N>& table,
        PerfAction incoming) noexcept {
        const bool rolling =
            table[(size_t)PerfAction::LoopRoll].state
                == ActionState::Active;
        const bool scratching =
            table[(size_t)PerfAction::Scratch].state
                == ActionState::Active;
        if (incoming == PerfAction::LoopRoll)
            return rolling ? Decision::Resize
                 : scratching ? Decision::Deny : Decision::Allow;
        if (incoming == PerfAction::Scratch && rolling)
            return Decision::Deny;
        return Decision::Allow;
    }
};

} // namespace onda
