#pragma once

#include <array>
#include <vector>

#include "StemTypes.h"

namespace onda::stems {

/// ------------------------------------------------------------------
/// §17-19 — stem routing model.
///
/// Stems always render INSIDE their deck (they share the deck's
/// timeline/pos_), so they reach the mixer through the deck's existing
/// ChannelStrip — trim, EQ, filter, fader, crossfader, PFL all apply
/// to the stem mix for free, and no second mixer exists (§19).
///
/// `StemFXRouter` describes per-stem FX intent (§18): a stem may carry
/// an FX slot BEFORE joining the channel mix. Today the plan resolves
/// to the deck's own FX chain; a future per-stem FX bus consumes the
/// same plan unchanged. Legality is checked here (stem present, stems
/// mode on, slot valid) so UI/MIDI never build invalid routings.
/// ------------------------------------------------------------------
struct StemRoute {
    int stem   { -1 };
    int channel{ -1 };          // mixer channel the deck feeds (== deck)
    int fxSlot { -1 };          // -1 = direct to channel (§17 default)
    bool valid { false };
};

struct StemRoutingPlan {
    std::array<StemRoute, kStemMax> routes {};
    uint32_t presentMask { 0 };
    bool stemsActive { false };        // stems mode on + set loaded
    int  channel     { -1 };
};

class StemRouter {
public:
    /// Resolve the plan for a deck. `stemsModeOn`/`presentMask` come
    /// from the deck's stem state; `fxSlotWanted` per stem (or -1).
    StemRoutingPlan plan(int deck, bool stemsModeOn, uint32_t presentMask,
                         const int* fxSlotWanted = nullptr) const {
        StemRoutingPlan p;
        p.presentMask = presentMask;
        p.channel = deck;
        p.stemsActive = stemsModeOn && presentMask != 0;
        for (int i = 0; i < kStemMax; ++i) {
            StemRoute r;
            r.stem = i;
            r.channel = deck;
            const bool present = (presentMask & (1u << i)) != 0;
            const int want = fxSlotWanted ? fxSlotWanted[i] : -1;
            // an FX insert is legal only on a present stem while stems
            // mode is active, and slots are 0..3 (FX chain size)
            r.fxSlot = (p.stemsActive && present && want >= 0 && want < 4)
                           ? want : -1;
            r.valid = present;
            p.routes[(size_t)i] = r;
        }
        return p;
    }
};

/// Per-deck FX intent for stems — the configuration UI/controllers
/// edit; StemRouter turns it into a legal plan.
class StemFXRouter {
public:
    /// Assign stem → fx slot (0..3), or -1 to clear. Returns false
    /// for illegal requests (bad stem/slot) — never half-applies.
    bool assign(int stem, int slot) {
        if (stem < 0 || stem >= kStemMax) return false;
        if (slot < -1 || slot >= 4)       return false;
        fx_[(size_t)stem] = slot;
        return true;
    }
    int slotFor(int stem) const {
        if (stem < 0 || stem >= kStemMax) return -1;
        return fx_[(size_t)stem];
    }
    const int* wanted() const { return fx_.data(); }
    void clear() { fx_.fill(-1); }

private:
    std::array<int, kStemMax> fx_ { -1, -1, -1, -1, -1, -1, -1, -1, -1 };
};

} // namespace onda::stems
