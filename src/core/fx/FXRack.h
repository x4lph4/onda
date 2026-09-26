#pragma once

#include <array>

#include "FXSlot.h"

namespace onda::fx {

/// A fixed-length serial chain of FXSlots:
///   in -> slot0 -> slot1 -> slot2 -> slot3 -> out
///
/// kSlotsPerRack bounds chain length deliberately: every slot costs
/// CPU on the audio thread, and the UI/controller surface maps cleanly
/// onto a small number of slots. Racks themselves are bound to targets
/// by FXEngine (deck inserts, send returns, master).
class FXRack {
public:
    static constexpr int kSlotsPerRack = 4;

    void prepare(double sr, rt::SpscRingBuffer<FXEvent, 256>* events,
                 int target) noexcept {
        for (int s = 0; s < kSlotsPerRack; ++s) {
            slots_[(size_t)s].prepare(sr);
            slots_[(size_t)s].bindEvents(events, target, s);
        }
    }

    FXSlot&       slot(int i) noexcept       { return slots_[(size_t)(i & 3)]; }
    const FXSlot& slot(int i) const noexcept { return slots_[(size_t)(i & 3)]; }

    /// RT. Process the whole chain in place; ctx applies to every slot
    /// (each stamps its own sync mode/division onto it).
    void process(float* l, float* r, int n, const SyncContext& ctx) noexcept {
        for (auto& s : slots_) {
            if (s.busy() || s.enabled() || s.hasPending())
                s.process(l, r, n, ctx);
        }
    }

    int latencySamples() const noexcept {
        int total = 0;
        for (const auto& s : slots_) total += s.latencySamples();
        return total;
    }

    /// Any slot actively processing, enabled, or holding a pending
    /// adoption (skips idle chains cheaply).
    bool busy() const noexcept {
        for (const auto& s : slots_)
            if (s.busy() || s.enabled() || s.hasPending()) return true;
        return false;
    }

    void collectTrash() noexcept {
        for (auto& s : slots_) s.collectTrash();
    }

private:
    std::array<FXSlot, kSlotsPerRack> slots_;
};

} // namespace onda::fx
