#pragma once

#include <array>
#include <cstdio>
#include <string>

namespace onda {

/// Non-RT performance counters (Phase 20 §50). One per deck, updated
/// from PerformanceEngine on every action — never from the audio
/// thread. `snapshot()` is a trivially-copyable view for UI/tests.
struct PerfDiag {
    int64_t cuesSet        { 0 };
    int64_t cuesDeleted    { 0 };
    int64_t cuesTriggered  { 0 };
    int64_t cueButtonPress { 0 };
    int64_t instantRestart { 0 };
    int64_t loopsEntered   { 0 };
    int64_t loopsExited    { 0 };
    int64_t loopRolls      { 0 };
    int64_t loopsResized   { 0 };
    int64_t beatJumps      { 0 };
    int64_t quantizeSnaps  { 0 };   // actions that snapped a target
    int64_t slipBegins     { 0 };
    int64_t slipEnds       { 0 };
    int64_t reverses       { 0 };
    int64_t actionsPosted  { 0 };   // scheduler input
    int64_t actionsDropped { 0 };   // coalesced/queue-full (§43)
    int64_t rejects        { 0 };   // failed actions (§43)
    int64_t rtCmdsSent     { 0 };
};

class PerformanceDiagnostics {
public:
    static constexpr int kMaxDecks = 4;

    PerfDiag&       deck(int d)       { return d_[(size_t)(d & 3)]; }
    const PerfDiag& deck(int d) const { return d_[(size_t)(d & 3)]; }

    PerfDiag totals() const {
        PerfDiag t;
        for (const auto& d : d_) {
            t.cuesSet += d.cuesSet; t.cuesDeleted += d.cuesDeleted;
            t.cuesTriggered += d.cuesTriggered;
            t.cueButtonPress += d.cueButtonPress;
            t.instantRestart += d.instantRestart;
            t.loopsEntered += d.loopsEntered;
            t.loopsExited += d.loopsExited;
            t.loopRolls += d.loopRolls;
            t.loopsResized += d.loopsResized;
            t.beatJumps += d.beatJumps;
            t.quantizeSnaps += d.quantizeSnaps;
            t.slipBegins += d.slipBegins; t.slipEnds += d.slipEnds;
            t.reverses += d.reverses;
            t.actionsPosted += d.actionsPosted;
            t.actionsDropped += d.actionsDropped;
            t.rejects += d.rejects; t.rtCmdsSent += d.rtCmdsSent;
        }
        return t;
    }

    std::string text(int d) const {
        const auto& p = deck(d);
        char buf[384];
        std::snprintf(buf, sizeof buf,
            "cues +%lld -%lld trig %lld | loops %lld/%lld roll %lld | "
            "jumps %lld snaps %lld | slip %lld/%lld rev %lld | "
            "post %lld drop %lld rej %lld rt %lld",
            (long long)p.cuesSet, (long long)p.cuesDeleted,
            (long long)p.cuesTriggered,
            (long long)p.loopsEntered, (long long)p.loopsExited,
            (long long)p.loopRolls,
            (long long)p.beatJumps, (long long)p.quantizeSnaps,
            (long long)p.slipBegins, (long long)p.slipEnds,
            (long long)p.reverses,
            (long long)p.actionsPosted, (long long)p.actionsDropped,
            (long long)p.rejects, (long long)p.rtCmdsSent);
        return buf;
    }

    void reset() { for (auto& d : d_) d = PerfDiag{}; }

private:
    std::array<PerfDiag, kMaxDecks> d_{};
};

} // namespace onda
