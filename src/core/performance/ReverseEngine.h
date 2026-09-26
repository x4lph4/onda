#pragma once

namespace onda {

/// Per-deck playback-direction state (Phase 20 §27-28). Tracks the
/// latched reverse toggle AND the momentary hold layer — a momentary
/// press while latched ON temporarily goes FORWARD (hardware semantic),
/// release restores the latch. `resolve()` is the single place that
/// computes the effective direction — the facade sends SetReverse only
/// on edges.
class ReverseEngine {
public:
    /// Latched state (button toggle). Returns true on effective edge.
    bool setLatched(bool on) noexcept {
        latched_ = on;
        return edge();
    }
    bool toggle() noexcept { return setLatched(!latched_); }

    /// Momentary hold (pad/hotkey down-up). Returns true on edge.
    bool setMomentary(bool held) noexcept {
        momentary_ = held;
        return edge();
    }

    /// Effective direction: XOR of latch and momentary layers.
    bool effective() const noexcept { return latched_ != momentary_; }
    bool latched()   const noexcept { return latched_; }
    bool momentary() const noexcept { return momentary_; }
    void clear() noexcept { latched_ = momentary_ = false; }

private:
    bool edge() const noexcept {
        // callers compare effective() before/after — nothing to cache;
        // keep a shadow so `edge()` is honest
        const bool now = effective();
        const bool changed = now != last_;
        last_ = now;
        return changed;
    }
    bool latched_   { false };
    bool momentary_ { false };
    mutable bool last_ { false };
};

} // namespace onda
