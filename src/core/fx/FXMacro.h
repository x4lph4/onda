#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "FXTypes.h"

namespace onda::fx {

/// FX macro (§28): one control that drives several slot parameters at
/// once. Each mapping scales the macro's normalized value into its own
/// [min,max] range with an optional response curve, so a "BUILD" knob
/// can raise filter cutoff + echo feedback + reverb mix together —
/// while every destination keeps its own safe bounds.
///
/// Pure control-side logic: evaluate() produces a list of parameter
/// writes; the caller pushes them through the normal rt::SetParam /
/// FXSlot::setParam path. Macros never reach into a processor.
class FXMacro {
public:
    static constexpr int kMaxMappings = 8;

    enum class Curve : int { Linear = 0, Square, Sqrt, kCount };

    /// Mapping.target sentinel: resolve to whatever rack the caller
    /// passes as `focusTarget` in evaluate() — lets one macro bank
    /// drive whichever deck/unit is focused.
    static constexpr int kFocusTarget = -2;

    struct Mapping {
        int   target  { -1 };   // FXTarget or kFocusTarget
        int   slot    { 0 };    // chain position
        int   param   { 0 };    // FXSlotKind value (P1..P8 for FX params)
        float min     { 0.0f }; // normalized range at macro = 0
        float max     { 1.0f }; // normalized range at macro = 1
        int   curve   { 0 };    // Curve
    };

    struct Write {
        uint16_t paramId;
        float    value;         // normalized
    };

    const char* name { "Macro" };

    void setValue(float v) noexcept { value_ = v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
    float value() const noexcept { return value_; }

    int  mappingCount() const noexcept { return count_; }
    const Mapping& mapping(int i) const noexcept {
        return maps_[(size_t)(i < 0 ? 0 : i % kMaxMappings)];
    }

    /// Add or replace a mapping for (target,slot,param). Returns false
    /// when the bank is full and the mapping is new.
    bool addMapping(Mapping m) noexcept {
        for (int i = 0; i < count_; ++i) {
            auto& e = maps_[(size_t)i];
            if (e.target == m.target && e.slot == m.slot
                && e.param == m.param) { e = m; return true; }
        }
        if (count_ >= kMaxMappings) return false;
        maps_[(size_t)count_++] = m;
        return true;
    }
    bool removeMapping(int target, int slot, int param) noexcept {
        for (int i = 0; i < count_; ++i) {
            const auto& e = maps_[(size_t)i];
            if (e.target == target && e.slot == slot && e.param == param) {
                maps_[(size_t)i] = maps_[(size_t)--count_];
                return true;
            }
        }
        return false;
    }
    void clear() noexcept { count_ = 0; }

    /// Resolve the macro into concrete param writes. Mappings with
    /// target == kFocusTarget bind to `focusTarget`. Returns the
    /// number of writes stored in `out` (capacity `cap`).
    int evaluate(Write* out, int cap, int focusTarget = -1) const noexcept {
        int n = 0;
        const float v = value_;
        for (int i = 0; i < count_ && n < cap; ++i) {
            const auto& m = maps_[(size_t)i];
            const int t = m.target == kFocusTarget ? focusTarget
                                                   : m.target;
            if (t < 0) continue;
            float shaped = v;
            switch ((Curve)m.curve) {
                case Curve::Square: shaped = v * v; break;
                case Curve::Sqrt:   shaped = std::sqrt(v); break;
                default: break;
            }
            float val = m.min + (m.max - m.min) * shaped;
            val = val < 0.f ? 0.f : (val > 1.f ? 1.f : val);
            out[n++] = { paramId(t, m.slot, (FXSlotKind)m.param), val };
        }
        return n;
    }

private:
    std::array<Mapping, kMaxMappings> maps_ {};
    int   count_ { 0 };
    float value_ { 0.0f };
};

} // namespace onda::fx
