#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../analysis/AnalysisService.h"
#include "../analysis/AnalysisTypes.h"

namespace onda::smart {

/// Phase 23 §41-43 — named analysis priorities + import policy on
/// top of the existing AnalysisService/AnalysisQueue. The queue
/// already guarantees: below-normal workers, cooperative cancel,
/// and a CPU gate (`setPlaybackActive`) that yields to playback —
/// this class only gives the policy names and the §42 import modes.
class SmartAnalysisQueue {
public:
    /// §41 — request classes, ordered by urgency. Mapped onto the
    /// proven Low/Normal/High/Realtime priorities.
    enum class Class : int {
        LibraryBackground = 0,    // bulk library pass
        RecentlyAdded     = 1,    // newly imported files
        UserRequested     = 2,    // explicit analyze request
        PreloadedTrack    = 2,    // track staged on a deck
        CurrentDeck       = 3,    // playing now — front of queue
    };

    /// §42 — when does imported content get analyzed?
    enum class ImportPolicy : int {
        Manual   = 0,    // user triggers analysis explicitly
        OnImport = 1,    // enqueue at import time (default)
        WhenIdle = 2,    // only when nothing is playing (CPU gate)
    };

    explicit SmartAnalysisQueue(AnalysisService& svc) : svc_(svc) {}

    static int priorityOf(Class c) {
        switch (c) {
            case Class::CurrentDeck:
                return (int)AnalysisPriority::Realtime;
            case Class::UserRequested:
            case Class::PreloadedTrack:
                return (int)AnalysisPriority::High;
            case Class::RecentlyAdded:
                return (int)AnalysisPriority::Normal;
            default:
                return (int)AnalysisPriority::Low;
        }
    }

    int64_t request(const std::string& path, Class c) {
        return svc_.analyzeTrack(path, priorityOf(c));
    }
    int request(const std::vector<std::string>& paths, Class c) {
        int n = 0;
        for (const auto& p : paths)
            if (request(p, c) >= 0) ++n;
        return n;
    }

    /// §42 — should an import auto-enqueue under this policy?
    /// WhenIdle still enqueues (at Low) — the CPU gate does the
    /// yielding work so playback is never disturbed (§41).
    static bool shouldAutoAnalyze(ImportPolicy p) {
        return p != ImportPolicy::Manual;
    }
    static Class classForImport(ImportPolicy p) {
        return p == ImportPolicy::WhenIdle
            ? Class::LibraryBackground : Class::RecentlyAdded;
    }

    /// §41 — playback gate passthrough; AudioEngine owns the flag.
    void setPlaybackActive(bool on) { svc_.setPlaybackActive(on); }

private:
    AnalysisService& svc_;
};

} // namespace onda::smart
