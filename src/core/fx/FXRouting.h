#pragma once

#include "FXTypes.h"

namespace onda::fx {

/// Where an FX rack sits in the mixer signal flow (§9). The routing is
/// a *descriptor* — the actual wiring lives in MixerEngine; this keeps
/// routes declarative instead of hardcoded inside effects.
enum class FXRoute : int {
    DeckInsert = 0,  // deck source -> insert rack -> channel strip
    SendReturn,      // post-fader send bus -> rack -> return into master
    MasterInsert,    // channel sum -> master rack -> master bus/limiter
    SamplerInsert,   // future: sampler bus -> rack -> mixer
    kCount
};

/// One row of the routing table: which role a target plays and which
/// deck/node it attaches to (-1 when not deck-bound).
struct FXRouting {
    FXTarget target;
    FXRoute  route;
    int      node;        // deck index / send index / -1 for master
    bool     postFader;   // send taps are post-fader
    bool     affectsCue;  // cue bus isolation (never true today)
};

/// Canonical routing table — Phase 16. Source of truth for
/// diagnostics, preset save/load and future re-routing; the mixer
/// topology is built to match it.
inline FXRouting routingFor(FXTarget t) noexcept {
    switch (t) {
        case FXTarget::DeckA:  return { t, FXRoute::DeckInsert, 0, false, false };
        case FXTarget::DeckB:  return { t, FXRoute::DeckInsert, 1, false, false };
        case FXTarget::DeckC:  return { t, FXRoute::DeckInsert, 2, false, false };
        case FXTarget::DeckD:  return { t, FXRoute::DeckInsert, 3, false, false };
        case FXTarget::SendA:  return { t, FXRoute::SendReturn, 0, true,  false };
        case FXTarget::SendB:  return { t, FXRoute::SendReturn, 1, true,  false };
        case FXTarget::Master: return { t, FXRoute::MasterInsert, -1, false, false };
        case FXTarget::Spare:  return { t, FXRoute::SamplerInsert, 0, false, false };
        default:               return { FXTarget::Master, FXRoute::MasterInsert, -1, false, false };
    }
}

inline const char* fxRouteName(FXRoute r) noexcept {
    switch (r) {
        case FXRoute::DeckInsert:   return "DeckInsert";
        case FXRoute::SendReturn:   return "SendReturn";
        case FXRoute::MasterInsert: return "MasterInsert";
        case FXRoute::SamplerInsert:return "SamplerInsert";
        default:                    return "?";
    }
}

inline const char* fxTargetName(FXTarget t) noexcept {
    switch (t) {
        case FXTarget::DeckA:  return "DeckA";
        case FXTarget::DeckB:  return "DeckB";
        case FXTarget::DeckC:  return "DeckC";
        case FXTarget::DeckD:  return "DeckD";
        case FXTarget::SendA:  return "SendA";
        case FXTarget::SendB:  return "SendB";
        case FXTarget::Master: return "Master";
        case FXTarget::Spare:  return "Spare";
        default:               return "?";
    }
}

} // namespace onda::fx
