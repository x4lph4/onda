#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include "FXSlot.h"

namespace onda::fx {

/// A fixed-length serial chain of FXSlots:
///   in -> slot0 -> slot1 -> slot2 -> slot3 -> out
///
/// kSlotsPerRack bounds chain length deliberately: every slot costs
/// CPU on the audio thread, and the UI/controller surface maps cleanly
/// onto a small number of slots. Racks themselves are bound to targets
/// by FXEngine (deck inserts, send returns, master).
///
/// Chain order is an indirection: `order_` packs four 2-bit physical
/// slot indices, one per chain position. moveSlot() rewrites the table
/// atomically — the RT thread picks the new order up at the next block
/// with no allocation, no lock, and no processor swaps.
class FXRack {
public:
    static constexpr int kSlotsPerRack = 4;

    void prepare(double sr, rt::SpscRingBuffer<FXEvent, 256>* events,
                 int target) noexcept {
        events_ = events; target_ = target;
        for (int s = 0; s < kSlotsPerRack; ++s) {
            slots_[(size_t)s].prepare(sr);
            slots_[(size_t)s].bindEvents(events, target, s);
        }
        order_.store(kIdentity, std::memory_order_relaxed);
    }

    /// Chain-position addressing: slot(0) is the FIRST position in the
    /// processing order (not necessarily physical slot 0).
    FXSlot&       slot(int i) noexcept {
        return slots_[(size_t)physAt(i)];
    }
    const FXSlot& slot(int i) const noexcept {
        return slots_[(size_t)physAt(i)];
    }

    /// Move the effect at chain position `from` to position `to`.
    /// Non-RT (control thread) — takes effect at the next audio block.
    void moveSlot(int from, int to) noexcept {
        from &= 3; to &= 3;
        if (from == to) return;
        const uint32_t o = order_.load(std::memory_order_acquire);
        int idx[kSlotsPerRack];
        for (int p = 0; p < kSlotsPerRack; ++p)
            idx[p] = (int)((o >> (p * 2)) & 0x3u);
        const int phys = idx[from];
        if (from < to)
            for (int p = from; p < to; ++p) idx[p] = idx[p + 1];
        else
            for (int p = from; p > to; --p) idx[p] = idx[p - 1];
        idx[to] = phys;
        uint32_t packed = 0;
        for (int p = 0; p < kSlotsPerRack; ++p)
            packed |= (uint32_t)(idx[p] & 3) << (p * 2);
        order_.store(packed, std::memory_order_release);
        // Refresh event position ids so RT events keep reporting the
        // chain position UI/mappings address (benign int rewrite).
        for (int p = 0; p < kSlotsPerRack; ++p)
            slots_[(size_t)idx[p]].bindEvents(events_, target_, p);
    }

    /// Physical slot index currently at chain position p.
    int physAt(int p) const noexcept {
        const uint32_t o = order_.load(std::memory_order_acquire);
        return (int)((o >> ((p & 3) * 2)) & 0x3u);
    }
    /// Current chain order packed (for serialization/diagnostics).
    uint32_t orderPacked() const noexcept {
        return order_.load(std::memory_order_acquire);
    }

    /// RT. Process the whole chain in place; ctx applies to every slot
    /// (each stamps its own sync mode/division onto it). `masterCtx`
    /// is forwarded to slots whose SyncMode selects MASTER tempo (§22).
    void process(float* l, float* r, int n, const SyncContext& ctx,
                 const SyncContext* masterCtx = nullptr) noexcept {
        const uint32_t o = order_.load(std::memory_order_relaxed);
        for (int p = 0; p < kSlotsPerRack; ++p) {
            FXSlot& s = slots_[(size_t)((o >> (p * 2)) & 0x3u)];
            if (s.busy() || s.enabled() || s.hasPending())
                s.process(l, r, n, ctx, masterCtx);
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
    // position 0..3 -> phys slot; identity = 0b11'10'01'00
    static constexpr uint32_t kIdentity = 0xE4u;
    std::array<FXSlot, kSlotsPerRack> slots_;
    std::atomic<uint32_t> order_ { kIdentity };
    rt::SpscRingBuffer<FXEvent, 256>* events_ { nullptr };
    int target_ { -1 };
};

} // namespace onda::fx
