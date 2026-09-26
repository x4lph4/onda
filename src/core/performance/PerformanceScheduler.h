#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <vector>

#include "../rt/CommandQueue.h"

namespace onda {

/// §29-31 — deterministic ordering layer between the performance
/// facade and the rt command sink. Every performance action posts its
/// rt commands HERE; flush() emits them in documented priority order.
/// UI/MIDI/keyboard event arrival order can therefore never decide
/// what the audio thread does when several commands belong to one
/// musical action.
///
/// Priority (lower emits earlier — the policy, DO NOT reorder):
///   0 SlipEnd    — release the shadow before repositioning resumes
///   1 SlipBegin  — anchor the shadow before the action that needs it
///   2 LoopExit   — SetLoopOn(false) before seeking/arming a new loop
///   3 Seek       — reposition playhead (frame- or second-accurate)
///   4 LoopArm    — SetLoopFrames / SetLoop / SetLoopSize / SetLoopOn(true)
///   5 Transport  — play / pause / stop
///   6 State      — everything else (reverse, quantize, sync, pitch…)
///
/// Coalescing (per deck, per class): a later command supersedes an
/// unflushed earlier one of the same coalesce class — e.g. two seeks
/// inside one action only ever emit the final target. This keeps the
/// audio thread free of stale positioning commands.
class PerformanceScheduler {
public:
    /// Coalesce classes — same class + same deck ⇒ newest wins.
    enum class Cls : uint8_t {
        None, SlipEnd, SlipBegin, LoopExit, Seek, LoopArm, Transport
    };
    struct Pending {
        rt::Command cmd;
        uint8_t  deck { 0 };
        uint8_t  prio { 6 };
        Cls      cls  { Cls::None };
        uint32_t seq  { 0 };
    };
    static constexpr size_t kMaxPending = 64;

    void setSink(std::function<void(const rt::Command&)> s) {
        sink_ = std::move(s);
    }

    /// Enqueue one command for `deck`. Returns false when the bounded
    /// queue is full — the caller reports PerfError::SchedulerFull
    /// (§43) and keeps the previous valid state.
    bool post(int deck, const rt::Command& cmd) {
        const uint8_t prio = priorityOf(cmd);
        const Cls cls = clsOf(cmd, prio);
        for (auto& p : pend_)
            if (p.deck == deck && p.cls == cls && cls != Cls::None) {
                p.cmd = cmd;              // superseded — newest wins
                ++coalesced_;
                return true;
            }
        if (pend_.size() >= kMaxPending) { ++dropped_; return false; }
        pend_.push_back(Pending{ cmd, (uint8_t)(deck & 0xF), prio, cls,
                                 seq_++ });
        return true;
    }

    /// Emit all pending commands in (priority, submission) order.
    /// Returns the number emitted.
    int flush() {
        std::stable_sort(pend_.begin(), pend_.end(),
            [](const Pending& a, const Pending& b) {
                if (a.prio != b.prio) return a.prio < b.prio;
                return a.seq < b.seq;
            });
        const int n = (int)pend_.size();
        for (auto& p : pend_)
            if (sink_) sink_(p.cmd);
        pend_.clear();
        return n;
    }

    size_t pending()   const { return pend_.size(); }
    int64_t coalesced() const { return coalesced_; }
    int64_t dropped()   const { return dropped_; }
    void resetStats() { coalesced_ = dropped_ = 0; }

    /// Documented priority table — also used by tests to pin policy.
    static uint8_t priorityOf(const rt::Command& c) {
        return std::visit([](const auto& x) -> uint8_t {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, rt::SlipEnd>)        return 0;
            if constexpr (std::is_same_v<T, rt::SlipBegin>)      return 1;
            if constexpr (std::is_same_v<T, rt::SetLoopOn>)
                return x.on ? 4 : 2;
            if constexpr (std::is_same_v<T, rt::DeckSeek> ||
                          std::is_same_v<T, rt::DeckSeekFrame>)  return 3;
            // re-anchor rides with seeks but never coalesces with
            // them — same priority, seq ordering (seek → re-anchor).
            if constexpr (std::is_same_v<T, rt::SlipReanchor>)   return 3;
            if constexpr (std::is_same_v<T, rt::SetLoopFrames> ||
                          std::is_same_v<T, rt::SetLoop> ||
                          std::is_same_v<T, rt::SetLoopSize>)    return 4;
            if constexpr (std::is_same_v<T, rt::DeckTransport>)  return 5;
            return 6;
        }, c);
    }

    static Cls clsOf(const rt::Command& c, uint8_t prio) {
        // SlipReanchor shares prio 3 but must never coalesce with a
        // seek — both commands are needed, in seq order.
        if (std::holds_alternative<rt::SlipReanchor>(c))
            return Cls::None;
        switch (prio) {
            case 0: return Cls::SlipEnd;
            case 1: return Cls::SlipBegin;
            case 2: return Cls::LoopExit;
            case 3: return Cls::Seek;
            case 4: return Cls::LoopArm;
            case 5: return Cls::Transport;
            default: return Cls::None;
        }
    }

private:
    std::function<void(const rt::Command&)> sink_;
    std::vector<Pending> pend_;
    uint32_t seq_      { 0 };
    int64_t  coalesced_{ 0 };
    int64_t  dropped_  { 0 };
};

} // namespace onda
