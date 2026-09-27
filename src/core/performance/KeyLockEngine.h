#pragma once

#include <cstdint>

namespace onda {

/// Non-RT key-lock state (Phase 28 §29-31). The actual pitch-preserving
/// DSP lives in DeckEngine through the ITimeStretcher seam — this
/// engine only decides WHEN the RT side toggles and which algorithm
/// family it should prefer, keeping the algorithm decoupled from the
/// deck (§30): a future stretcher backend plugs in by implementing
/// ITimeStretcher; nothing here changes.
struct KeyLockEngine {
    /// Algorithm family preference (§30). Only Wsola has an RT
    /// implementation today; the value is persisted so the choice
    /// survives sessions and a future backend picks it up.
    enum class Algo : int { Auto = 0, Wsola = 1 };

    bool on   { false };
    Algo algo { Algo::Auto };

    /// Returns true on a real state edge (callers emit SetKeyLock).
    bool set(bool want) noexcept {
        if (on == want) return false;
        on = want;
        return true;
    }
    void clear() noexcept { on = false; }
};

} // namespace onda
