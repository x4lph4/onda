#pragma once

#include <cstdint>

namespace onda {

/// Phase 35 §46 — performance parameter registry. The mixer-side
/// ParameterRegistry is channel/bus scoped; performance parameters
/// are per-deck and get their own descriptive table — same idea
/// (pure metadata, no audio state): UI, MIDI learn and tests all
/// enumerate/validate against one source of truth.
///
/// Id layout: id = deck * kPerDeck + idx (deck 0-3, 16 slots per
/// deck reserved). Ranges are in the parameter's native unit
/// documented below — never normalized differently per caller.

enum class PerfParam : uint8_t {
    LoopSize = 0,    // selected auto-loop size, beats (1/32 … 32)
    LoopActive,      // 0/1 — RT loop flag
    LoopQuantize,    // 0/1 — quantize loop boundaries on create
    BeatJumpSize,    // selected jump size, beats (1 … 64)
    SlipActive,      // 0/1 — user slip mode (momentary refs extra)
    QuantizeMode,    // Quantize enum index (0 … 10)
    BeatRollSize,    // selected roll size, beats (1/32 … 1)
    Count
};

struct PerfParamDef {
    PerfParam   idx;
    const char* name;      // stable key for mappings/persistence
    float       def;
    float       minV;
    float       maxV;
    float       step;      // UI/hardware resolution (0 = continuous)
    bool        quantized; // step-quantized control (0/1 toggles)
};

inline constexpr PerfParamDef kPerfParams[] = {
    { PerfParam::LoopSize,     "loop.size",      1.0f, 0.03125f, 32.0f, 0.0f, true  },
    { PerfParam::LoopActive,   "loop.active",    0.0f, 0.0f,     1.0f,  1.0f, true  },
    { PerfParam::LoopQuantize, "loop.quantize",  1.0f, 0.0f,     1.0f,  1.0f, true  },
    { PerfParam::BeatJumpSize, "beatJump.size",  4.0f, 1.0f,     64.0f, 1.0f, true  },
    { PerfParam::SlipActive,   "slip.active",    0.0f, 0.0f,     1.0f,  1.0f, true  },
    { PerfParam::QuantizeMode, "quantize.mode",  0.0f, 0.0f,     10.0f, 1.0f, true  },
    { PerfParam::BeatRollSize, "beatRoll.size",  0.25f,0.03125f, 1.0f,  0.0f, true  },
};

inline constexpr int kPerfParamCount =
    (int)(sizeof(kPerfParams) / sizeof(kPerfParams[0]));
inline constexpr int kPerfDeckStride = 16;   // reserved ids per deck

/// Global param id for `param` on `deck` (clamped to 4 decks).
inline constexpr uint16_t perfParamId(int deck, PerfParam p) {
    const int d = deck < 0 ? 0 : (deck > 3 ? 3 : deck);
    return (uint16_t)(d * kPerfDeckStride + (int)p);
}

/// Definition lookup by enum — always in sync with the table.
inline constexpr const PerfParamDef& perfParamDef(PerfParam p) {
    return kPerfParams[(int)p];
}

/// Clamp/step a value per the definition — the same sanitize rule
/// for UI, MIDI learn and tests.
inline float sanitizePerfParam(PerfParam p, float v) {
    const PerfParamDef& d = perfParamDef(p);
    if (d.step > 0.0f) {
        v = (float)(int)(v / d.step + (v >= 0.0f ? 0.5f : -0.5f))
            * d.step;
    }
    return v < d.minV ? d.minV : (v > d.maxV ? d.maxV : v);
}

} // namespace onda
