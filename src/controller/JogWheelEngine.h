#pragma once

#include <cstdint>
#include <functional>

#include "MappingTypes.h"

namespace onda::ctrl {

/// Generic jog-wheel engine (tasks §20-26) — device-agnostic layer
/// between a decoded jog event and the application jog port. Device
/// plugins (FLX4) and plain MIDI mappings feed normalized events in;
/// the engine owns per-deck mode policy, touch state and sensitivity,
/// then emits either a deck jog action or a browse step.
///
///   hardware decode → JogWheelEngine::turn(deck, ticks, ctx)
///                       mode policy (VINYL/CDJ/SEARCH/BROWSE)
///                     → ports.deckJog(deck, JogMode, delta)
///                     → ports.browse(dir)        (Browse mode)
///
/// The engine never touches DeckEngine directly and never implements
/// scratch DSP (§23) — ScratchEngine/deck-side code owns that; here
/// we only route mode + scale.
class JogWheelEngine {
public:
    /// Hardware-facing jog modes (§21). Vinyl = touch→scratch,
    /// free-spin→seek; Cdj = always seek (no scratch); Search =
    /// amplified seek; Browse = library navigation (§25: deck
    /// position is never modified while browsing).
    enum class HwMode : uint8_t { Vinyl = 0, Cdj, Search, Browse };

    struct DeckCfg {
        HwMode mode        = HwMode::Vinyl;
        float  sensitivity = 1.0f;   // §26 multiplier on raw ticks
        float  searchBoost = 3.0f;   // extra gain in Search mode
        bool   touchCapable = true;  // §22 — device has touch plates
    };

    struct Ports {
        /// deck jog action (same shape as ControllerEngine::Ports::jog)
        std::function<void(int deck, int mode, double delta)> deckJog;
        /// library navigation step (+1/-1) used by Browse mode
        std::function<void(int dir)> browse;
    };

    void setPorts(Ports p) { ports_ = std::move(p); }

    // ---- configuration ----
    DeckCfg&       cfg(int deck)       { return cfg_[clamp(deck)]; }
    const DeckCfg& cfg(int deck) const { return cfg_[clamp(deck)]; }
    bool touching(int deck) const { return touch_[clamp(deck)]; }

    // ---- input ----
    /// Touch plate event (§22). No-op when the deck's cfg says the
    /// hardware has no touch sensor — harmless to feed anyway.
    void touch(int deck, bool down) {
        DeckCfg& c = cfg_[clamp(deck)];
        if (!c.touchCapable) return;
        touch_[clamp(deck)] = down;
    }

    /// One decoded rotation event. `ticks` = signed hardware delta,
    /// `reqMode` = the JogMode the device decoder suggested,
    /// `isPlatter` = true for the platter/top surface, false for
    /// wheel-edge/search sources (mode policy only rewrites platter
    /// turns — a side-edge nudge must never become a scratch).
    /// Returns true when the turn produced an action.
    bool turn(int deck, double ticks, JogMode reqMode, bool isPlatter) {
        const int d = clamp(deck);
        const DeckCfg& c = cfg_[d];
        const double scaled = ticks * (double)c.sensitivity;

        switch (c.mode) {
            case HwMode::Browse:
                // §25 — the jog navigates the library; the deck
                // position is untouched. One step per accumulated
                // ~4 ticks so fast rotation doesn't flood the queue.
                accum_[d] += scaled;
                if (accum_[d] >= 4.0 || accum_[d] <= -4.0) {
                    if (ports_.browse)
                        ports_.browse(accum_[d] > 0 ? 1 : -1);
                    accum_[d] = 0.0;
                }
                return true;

            case HwMode::Vinyl: {
                const JogMode m =
                    isPlatter && c.touchCapable && touch_[d]
                        ? JogMode::JogScratch : reqMode;
                if (ports_.deckJog)
                    ports_.deckJog(d, (int)m, scaled);
                return true;
            }
            case HwMode::Cdj: {
                // CDJ platters seek-nudge; edge/side events keep the
                // decoder's request (pitch bend etc.).
                const JogMode m = isPlatter ? JogMode::JogSeek : reqMode;
                if (ports_.deckJog)
                    ports_.deckJog(d, (int)m, scaled);
                return true;
            }
            case HwMode::Search: {
                const JogMode m = isPlatter ? JogMode::JogSeek : reqMode;
                if (ports_.deckJog)
                    ports_.deckJog(d, (int)m,
                                   scaled * (double)c.searchBoost);
                return true;
            }
        }
        return false;
    }

private:
    static int clamp(int deck) {
        return deck < 0 ? 0 : deck > 3 ? 3 : deck;
    }

    Ports   ports_;
    DeckCfg cfg_[4];
    bool    touch_[4]  = { false, false, false, false };
    double  accum_[4]  = { 0.0, 0.0, 0.0, 0.0 };
};

} // namespace onda::ctrl
