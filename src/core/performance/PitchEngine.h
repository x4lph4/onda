#pragma once

#include <cstdint>

namespace onda {

/// Non-RT pitch/tempo range state (Phase 28 §34-37). The fader itself
/// is a plain rt::SetPitch — this engine owns the RANGE table and the
/// bookkeeping for temporary bends so every input path (fader, jog
/// nudge, ±bend buttons) agrees on what "range" means.
struct PitchEngine {
    /// Selectable fader ranges in % (§35): ±6 / ±10 / ±16 / WIDE(50).
    static constexpr int kRangeCount = 4;
    static constexpr float kRanges[kRangeCount] = { 6.f, 10.f, 16.f, 50.f };

    int   rangeIdx { 2 };       // default ±16 (spec default)
    float rangePct { 16.0f };
    bool  bending  { false };   // a bend/jog nudge is currently applied

    float range() const noexcept { return rangePct; }

    /// Set the range by percent — snapped to the table when it lands
    /// on a preset, raw value otherwise (custom ranges allowed).
    void setRange(float pct) noexcept {
        rangePct = pct;
        rangeIdx = -1;
        for (int i = 0; i < kRangeCount; ++i)
            if (kRanges[i] == pct) { rangeIdx = i; break; }
    }
    /// Cycle the preset table (hardware tempo-range button semantic).
    float cycle() noexcept {
        rangeIdx = rangeIdx < 0 ? 0 : (rangeIdx + 1) % kRangeCount;
        rangePct = kRanges[rangeIdx];
        return rangePct;
    }
    float stepRange(int dir) noexcept {
        const int i = rangeIdx < 0 ? (dir > 0 ? 0 : kRangeCount - 1)
                                 : (rangeIdx + dir + kRangeCount)
                                       % kRangeCount;
        rangeIdx = i; rangePct = kRanges[i];
        return rangePct;
    }
    void setBending(bool on) noexcept { bending = on; }
    void clear() noexcept { bending = false; }
};

} // namespace onda
