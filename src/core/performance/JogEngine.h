#pragma once

#include <algorithm>
#include <cstdint>

namespace onda {

/// Deck-domain jog modes (Phase 28 §39). Mirrors ctrl::JogMode by
/// design — the controller layer passes its mode straight through
/// (numeric values intentionally identical) while the performance
/// layer stays controller-free.
enum class DeckJogMode : int {
    Seek      = 0,   // CDJ-style position nudge
    PitchBend = 1,   // temporary tempo nudge, released back to target
    Scratch   = 2,   // platter-touch: timeline wiggle at scratch rate
    Browse    = 3    // library navigation — NEVER moves the playhead
};

/// What one jog tick wants — the facade translates it into rt
/// commands. POD: no positions live here, the facade resolves the
/// current position when the decision says SeekSec.
struct JogDecision {
    enum class Kind : int { None = 0, SeekSec, BendPct, Browse } kind;
    double deltaSec { 0.0 };
    float  bendPct  { 0.0f };
};

/// Per-deck jog engine (Phase 28 §38-40): turns raw jog deltas into
/// one of four musical decisions. Sensitivity is per-deck configurable;
/// scratch seeks are handled like any seek — under slip they are
/// automatically TEMPORARY (the shadow keeps advancing and endSlip
/// returns to it), which is the real scratch-slip semantic: wiggle the
/// audio, release, the track is where it would have been.
struct JogEngine {
    DeckJogMode mode { DeckJogMode::Seek };
    double seekScale    { 0.04 };  // seconds per tick — search/jog
    double scratchScale { 0.10 };  // platter-touch wiggle rate
    double bendGain     { 0.25 };  // pct bend per tick
    double bendMax      { 8.0 };   // clamp — matches the old wiring
    bool   scratching   { false }; // action bookkeeping (§42-44)

    JogDecision tick(DeckJogMode m, double deltaTicks) noexcept {
        JogDecision d;
        switch (m) {
            case DeckJogMode::Browse:
                d.kind = JogDecision::Kind::Browse;
                break;
            case DeckJogMode::PitchBend:
                d.kind = JogDecision::Kind::BendPct;
                d.bendPct = (float)std::clamp(deltaTicks * bendGain,
                                              -bendMax, bendMax);
                break;
            case DeckJogMode::Scratch:
                d.kind = JogDecision::Kind::SeekSec;
                d.deltaSec = deltaTicks * scratchScale;
                scratching = true;
                break;
            case DeckJogMode::Seek:
            default:
                d.kind = JogDecision::Kind::SeekSec;
                d.deltaSec = deltaTicks * seekScale;
                break;
        }
        // Leaving scratch mode ends the scratch action (§44 state).
        if (m != DeckJogMode::Scratch) scratching = false;
        return d;
    }
    void clear() noexcept { scratching = false; }
};

} // namespace onda
