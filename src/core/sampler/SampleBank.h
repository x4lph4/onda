#pragma once

#include <array>
#include <deque>
#include <string>
#include <vector>

#include "SamplerTypes.h"

namespace onda::sampler {

/// A bank of performance slots (task §2). 8 slots per bank today;
/// the container is sized dynamically so 16/32+ banks work later.
/// Pads always address the ACTIVE bank's slots 0..kPadSlots-1.
struct SampleBank {
    static constexpr int kPadSlots = 8;   // slots the pads can reach

    int64_t     id = -1;
    std::string name = "Bank";
    /// deque: SampleSlot is non-movable (atomics) — deque::resize
    /// never relocates existing elements.
    std::deque<SampleSlot> slots;

    explicit SampleBank(int n = kPadSlots) { resize(n); }
    void resize(int n) {
        const int old = (int)slots.size();
        slots.resize((size_t)n);
        for (int i = old; i < n; ++i) slots[(size_t)i].slot = i;
    }
    int slotCount() const { return (int)slots.size(); }
    SampleSlot&       slot(int i)       { return slots[(size_t)(i & 0x7FF)]; }
    const SampleSlot& slot(int i) const { return slots[(size_t)(i & 0x7FF)]; }
};

} // namespace onda::sampler
