#pragma once

#include <array>
#include <cstdint>

#include "../../core/performance/PerformanceTypes.h"
#include "PadTypes.h"

namespace onda::ui::pads {

/// §36 — per-pad visual state model. Pure function of (pad mode,
/// cue set, loop state, selected sizes): the SAME states drive the UI
/// pad row and hardware LEDs (FLX4 pads take velocity levels via
/// flx4LedValue below — RGB color is a UI-only extra).
enum class PadLightState : int {
    Off = 0,     // nothing there
    Dim,         // mapped but empty / unavailable
    Set,         // content present (cue set / size available)
    Selected,    // the currently-selected size / active slot
    Playing,     // actively performing (playing cue / engaged roll)
};

struct PadLight {
    PadLightState state { PadLightState::Off };
    uint32_t      argb  { 0 };       // cue color when known (UI)
};

/// Inputs the caller assembles from the deck — plain data so tests
/// and the app shell can both drive it.
struct PadDeckState {
    PadMode mode { PadMode::HotCue };
    std::array<const Cue*, 8> hot {};      // nullptr = empty slot
    std::array<const Cue*, 8> mem {};
    bool   playing     { false };
    bool   loopOn      { false };
    bool   rolling     { false };
    int    activeCueSlot { -1 };           // last triggered hot cue
    int    loopSizeIdx   { 5 };
    int    jumpSizeIdx   { 2 };
    int    pendingLoopIn { -1 };           // in-point armed, awaiting out
};

class PadFeedbackEngine {
public:
    /// Resolve one pad's light. Deterministic — same inputs, same
    /// light, regardless of which input source performed the action.
    PadLight light(const PadDeckState& s, int pad) const {
        PadLight out;
        switch (s.mode) {
            case PadMode::HotCue: {
                const Cue* c = s.hot[(size_t)pad];
                if (!c)                 { out.state = PadLightState::Dim; break; }
                out.argb = c->argb;
                if (!c->enabled)        { out.state = PadLightState::Dim; break; }
                if (pad == s.activeCueSlot && s.playing)
                    out.state = PadLightState::Playing;
                else
                    out.state = PadLightState::Set;
                break;
            }
            case PadMode::Loop:
                // pads 0-3 = ops (dim, armed in-point lit), 4-7 = sizes
                if (pad < 4) {
                    out.state = (pad == 2 && s.pendingLoopIn >= 0)
                        ? PadLightState::Selected : PadLightState::Dim;
                } else {
                    static const int kIdx[4] = { 4, 5, 6, 7 };
                    out.state = (kIdx[pad - 4] == s.loopSizeIdx && s.loopOn)
                        ? PadLightState::Playing
                      : (kIdx[pad - 4] == s.loopSizeIdx)
                        ? PadLightState::Selected : PadLightState::Set;
                }
                break;
            case PadMode::Roll:
                out.state = s.rolling ? PadLightState::Playing
                                      : PadLightState::Set;
                break;
            case PadMode::BeatJump:
                // pads 0-3 back, 4-7 fwd; magnitudes 1/2/4/8 → idx 0-3
                out.state = ((pad & 3) == s.jumpSizeIdx)
                    ? PadLightState::Selected : PadLightState::Set;
                break;
            default:
                out.state = PadLightState::Off;   // sampler/other own it
                break;
        }
        return out;
    }

    /// All 8 pads for a deck (LED bulk update / UI row).
    std::array<PadLight, 8> lights(const PadDeckState& s) const {
        std::array<PadLight, 8> out;
        for (int i = 0; i < 8; ++i) out[(size_t)i] = light(s, i);
        return out;
    }

    /// §36 — FLX4 LED velocity for a state (single-intensity pads:
    /// dim/full levels only — documented hardware limit, not our model).
    static uint8_t flx4LedValue(PadLightState st) {
        switch (st) {
            case PadLightState::Off:      return 0x00;
            case PadLightState::Dim:      return 0x10;
            case PadLightState::Set:      return 0x20;
            case PadLightState::Selected: return 0x50;
            case PadLightState::Playing:  return 0x7F;
            default:                      return 0x00;
        }
    }
};

} // namespace onda::ui::pads
