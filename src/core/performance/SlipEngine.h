#pragma once

namespace onda {

/// Per-deck slip bookkeeping (Phase 20 §17-18). Slip is REFERENCE-
/// COUNTED: the user-mode toggle and each temporary action (loop roll,
/// momentary reverse, future scratch/cue performance) each hold one
/// handle — the underlying shadow keeps running until the LAST one
/// releases. This fixes the ordering hazard where releasing a roll
/// while user-slip is on would prematurely end the shadow.
///
/// Pure state — callers turn transitions into SlipBegin/SlipEnd rt
/// commands when `edge()` reports a real change.
class SlipEngine {
public:
    enum Source : int { UserMode = 0, Roll = 1, Momentary = 2, Count };

    /// Request/release slip from `src`. Returns true when the ACTIVE
    /// state changed (edge) — only then must an rt command be sent.
    bool set(Source src, bool on) noexcept {
        const bool was = active();
        if (on) refs_ |= (1 << (int)src);
        else    refs_ &= ~(1 << (int)src);
        return active() != was;
    }
    bool active()  const noexcept { return refs_ != 0; }
    bool userOn()  const noexcept { return (refs_ & (1 << (int)UserMode)) != 0; }
    bool rolling() const noexcept { return (refs_ & (1 << (int)Roll)) != 0; }
    int  refs()    const noexcept { return refs_; }
    void clear()   noexcept { refs_ = 0; }

private:
    int refs_ { 0 };
};

} // namespace onda
