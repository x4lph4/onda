#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "AnalysisTypes.h"

namespace onda {

class MusicAnalysisEngine;
struct AnalysisBundle;

/// Background analysis scheduler.
///
/// Priority model (documented):
///   RT audio thread  >  UI thread  >  these workers.
/// Workers run at OS "below normal" priority and cooperatively yield;
/// they never hold locks used by RT/UI. Cancelling is cooperative:
/// the pipeline checks a token between stages and inside long loops.
class AnalysisQueue {
public:
    enum class Event : int {
        Started, Progress, Completed, Failed, Cancelled, Retried,
        /// §4 — job entered the persistence stage (store write is
        /// part of the job's lifecycle, not a separate state).
        Saving
    };
    struct Notification {
        int64_t     jobId { 0 };
        Event       event { Event::Started };
        float       progress { 0.0f };
        std::string path;
        std::string error;
        std::shared_ptr<AnalysisBundle> bundle; // Completed only
    };
    using Listener = std::function<void(const Notification&)>;

    explicit AnalysisQueue(MusicAnalysisEngine& engine, int numWorkers = -1);
    ~AnalysisQueue();

    int64_t enqueue(const std::string& path, int priority = 0,
                    int maxRetries = 1,
                    uint32_t mask = AnalysisMask::All,
                    int64_t trackId = -1);
    void cancel(int64_t jobId);
    void retry(int64_t jobId);
    void pause();
    void resume();
    bool paused() const { return paused_.load(); }

    /// CPU gate (§29): while set, only jobs with
    /// `priority >= minActivePriority` run — background work yields to
    /// playback/recording. Lower-priority jobs stay queued; they resume
    /// automatically when the gate reopens (0 = gate off).
    void setMinActivePriority(int p);
    int  minActivePriority() const { return minActivePriority_.load(); }
    int  workerCount() const { return numWorkers_; }

    /// Job states for UI panels: id -> {state, progress}.
    struct JobInfo { int state; float progress; int attempts; };
    std::unordered_map<int64_t, JobInfo> snapshot() const;

    /// §3-4 — public job descriptors: state mapped to the external
    /// JobState lifecycle (Queued/Decoding/Analyzing/Caching/
    /// Completed/Failed/Cancelled), one copy — callers poll or listen.
    std::vector<AnalysisJob> snapshotJobs() const;
    AnalysisJob jobInfo(int64_t jobId) const;

    void addListener(Listener l);
    /// §4 — the persisting stage of a job's lifecycle: the service
    /// marks Caching while it writes the result to the store.
    void markSaving(int64_t jobId);

private:
    struct Job {
        int64_t id; std::string path; int priority; int retries;
        uint32_t mask;
        int64_t trackId;
        std::shared_ptr<std::atomic<bool>> cancel;
    };
    static bool higherPriority(const std::shared_ptr<Job>& a,
                               const std::shared_ptr<Job>& b) {
        return a->priority < b->priority; // std::priority_queue max-first
    }

    void workerMain();
    void notify(const Notification& n);

    MusicAnalysisEngine& engine_;
    int numWorkers_;
    std::vector<std::thread> workers_;
    std::atomic<bool> exit_{false}, paused_{false};
    std::atomic<int>  minActivePriority_{0};

    mutable std::mutex m_;
    std::condition_variable cv_;
    std::vector<std::shared_ptr<Job>> heap_;   // max-heap by priority
    std::unordered_map<int64_t, JobInfo> states_;
    std::unordered_map<int64_t, std::shared_ptr<Job>> jobs_;
    std::unordered_map<int64_t, std::string> errors_;
    std::unordered_map<int64_t, int> events_;   // last Event per job
    std::vector<Listener> listeners_;
    int64_t nextId_ { 1 };
};

} // namespace onda
