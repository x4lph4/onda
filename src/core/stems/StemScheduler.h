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

#include "StemTypes.h"

namespace onda::stems {

/// ------------------------------------------------------------------
/// §9/§42/§45 — background separation scheduler.
///
/// Same discipline as AnalysisQueue: workers run below RT/UI priority,
/// cancellation is cooperative (jobs poll their token), a CPU gate
/// (`setMinActivePriority`) sheds low-priority work while playback is
/// active. The scheduler is payload-agnostic — StemsEngine installs a
/// runner that performs decode → analyze → separate → cache.
///
/// Notifications fire ON WORKER THREADS — listeners must marshal to
/// their own thread (JUCE AsyncUpdater on the UI side).
/// ------------------------------------------------------------------
class StemScheduler {
public:
    enum class Event : int {
        Started, Progress, Phase, Completed, Failed, Cancelled
    };
    struct Notification {
        int64_t     jobId { 0 };
        Event       event { Event::Started };
        StemJobPhase phase { StemJobPhase::Queued };
        float       progress { 0.0f };
        int64_t     trackId { 0 };
        uint64_t    contentHash { 0 };
        std::string error;
    };
    using Listener = std::function<void(const Notification&)>;
    /// The work itself. `cancel` is the job's token; `report` feeds
    /// phase/progress notifications back through the scheduler.
    using Runner = std::function<StemError(
        const StemJob& job, StemCancel& cancel,
        const std::function<void(StemJobPhase, float)>& report)>;

    explicit StemScheduler(int numWorkers = 1) {
        numWorkers_ = numWorkers > 0 ? numWorkers : 1;
        for (int i = 0; i < numWorkers_; ++i)
            workers_.emplace_back([this] { workerMain(); });
    }
    ~StemScheduler() {
        exit_.store(true);
        cv_.notify_all();
        for (auto& t : workers_) if (t.joinable()) t.join();
    }

    void setRunner(Runner r) { std::lock_guard<std::mutex> lk(m_); runner_ = std::move(r); }

    int64_t enqueue(const StemJob& spec) {
        std::lock_guard<std::mutex> lk(m_);
        auto j = std::make_shared<Job>();
        j->info = spec;
        j->info.id = nextId_++;
        j->cancel = std::make_shared<StemCancel>(false);
        heap_.push_back(j);
        std::push_heap(heap_.begin(), heap_.end(), higherPriority);
        jobs_[j->info.id] = j;
        states_[j->info.id] = { (int)StemJobPhase::Queued, 0.0f };
        cv_.notify_one();
        return j->info.id;
    }

    /// Cooperative cancel (§42): queued → removed; running → token set,
    /// the job unwinds through its cancel checks and the worker
    /// notifies Cancelled. Temporary/cache cleanup is the runner's
    /// job (it owns the artifacts).
    void cancel(int64_t jobId) {
        Notification n;
        bool send = false;
        {
            std::lock_guard<std::mutex> lk(m_);
            auto it = jobs_.find(jobId);
            if (it == jobs_.end()) return;
            if (it->second->running) {
                it->second->cancel->store(true);
            } else {
                eraseFromHeap(jobId);
                states_[jobId] = { (int)StemJobPhase::Cancelled, 0.0f };
                n = makeNote(*it->second);
                n.event = Event::Cancelled;
                n.phase = StemJobPhase::Cancelled;
                jobs_.erase(it);
                send = true;
            }
        }
        if (send) notify(n);
    }

    /// CPU gate (§45): while set, only jobs with priority >= p run —
    /// background separation never competes with playback.
    void setMinActivePriority(int p) { minActivePriority_.store(p); cv_.notify_all(); }
    int  minActivePriority() const { return minActivePriority_.load(); }
    int  workerCount() const { return numWorkers_; }

    struct JobState { int phase; float progress; };
    std::unordered_map<int64_t, JobState> snapshot() const {
        std::lock_guard<std::mutex> lk(m_);
        return states_;
    }
    JobState stateOf(int64_t jobId) const {
        std::lock_guard<std::mutex> lk(m_);
        auto it = states_.find(jobId);
        return it != states_.end() ? it->second
                                   : JobState{ (int)StemJobPhase::Failed, 0.0f };
    }

    void addListener(Listener l) {
        std::lock_guard<std::mutex> lk(m_);
        listeners_.push_back(std::move(l));
    }

private:
    struct Job {
        StemJob info;
        std::shared_ptr<StemCancel> cancel;
        bool running { false };
    };
    static bool higherPriority(const std::shared_ptr<Job>& a,
                               const std::shared_ptr<Job>& b) {
        return a->info.priority < b->info.priority;
    }

    void workerMain() {
        for (;;) {
            std::shared_ptr<Job> job;
            Runner runner;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [&] {
                    if (exit_.load()) return true;
                    return hasRunnable();
                });
                if (exit_.load()) return;
                job = topRunnable();
                if (!job) continue;   // gate raced a cancel — rescan
                job->running = true;
                runner = runner_;
                states_[job->info.id] = { (int)StemJobPhase::Analyzing, 0.0f };
            }
            {
                auto n = makeNote(*job);
                n.event = Event::Started;
                n.phase = StemJobPhase::Analyzing;
                notify(n);
            }
            if (!runner) { notifyDone(*job, StemError::ModelLoadFailed, "no runner"); continue; }

            auto report = [this, &job](StemJobPhase ph, float p) {
                {
                    std::lock_guard<std::mutex> lk(m_);
                    states_[job->info.id] = { (int)ph, p };
                }
                auto n = makeNote(*job);
                n.event = Event::Progress;
                n.phase = ph; n.progress = p;
                notify(n);
            };

            const StemError err = runner(job->info, *job->cancel, report);
            {
                std::lock_guard<std::mutex> lk(m_);
                jobs_.erase(job->info.id);
            }
            notifyDone(*job, err, {});
        }
    }

    void notifyDone(Job& job, StemError err, const std::string& msg) {
        auto n = makeNote(job);
        const bool cancelled = job.cancel->load() || err == StemError::Cancelled;
        n.phase = cancelled ? StemJobPhase::Cancelled
                  : err == StemError::Ok ? StemJobPhase::Ready
                                         : StemJobPhase::Failed;
        n.event = cancelled ? Event::Cancelled
                  : err == StemError::Ok ? Event::Completed
                                         : Event::Failed;
        n.progress = n.event == Event::Completed ? 1.0f : 0.0f;
        n.error = msg.empty() && err != StemError::Ok
                      ? ("error " + std::to_string((int)err)) : msg;
        {
            std::lock_guard<std::mutex> lk(m_);
            states_[job.info.id] = { (int)n.phase, n.progress };
        }
        notify(n);
    }

    bool hasRunnable() const {
        for (auto& j : heap_)
            if (j->info.priority >= minActivePriority_.load())
                return true;
        return false;
    }
    std::shared_ptr<Job> topRunnable() {
        // honour the CPU gate: skip jobs below minActivePriority —
        // scan for the highest-priority runnable one (heap order is
        // by priority, but a gated top doesn't block lower-but-
        // still-above-threshold entries further down).
        std::shared_ptr<Job> best;
        for (auto& j : heap_)
            if (j->info.priority >= minActivePriority_.load() &&
                (!best || higherPriority(j, best)))
                best = j;
        if (best) {
            // rebuild the heap without `best`
            for (size_t i = 0; i < heap_.size(); ++i)
                if (heap_[i] == best) {
                    heap_.erase(heap_.begin() + (ptrdiff_t)i);
                    std::make_heap(heap_.begin(), heap_.end(),
                                   higherPriority);
                    break;
                }
        }
        return best;
    }
    void eraseFromHeap(int64_t id) {
        for (size_t i = 0; i < heap_.size(); ++i)
            if (heap_[i]->info.id == id) {
                heap_.erase(heap_.begin() + (ptrdiff_t)i);
                std::make_heap(heap_.begin(), heap_.end(), higherPriority);
                return;
            }
    }
    Notification makeNote(const Job& j) const {
        Notification n;
        n.jobId = j.info.id; n.phase = j.info.phase;
        n.progress = j.info.progress;
        n.trackId = j.info.trackId; n.contentHash = j.info.contentHash;
        return n;
    }
    void notify(const Notification& n) {
        std::vector<Listener> ls;
        { std::lock_guard<std::mutex> lk(m_); ls = listeners_; }
        for (auto& l : ls) l(n);
    }

    int numWorkers_ { 1 };
    std::vector<std::thread> workers_;
    std::atomic<bool> exit_{false};
    std::atomic<int>  minActivePriority_{0};
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::vector<std::shared_ptr<Job>> heap_;
    std::unordered_map<int64_t, std::shared_ptr<Job>> jobs_;
    std::unordered_map<int64_t, JobState> states_;
    std::vector<Listener> listeners_;
    Runner runner_;
    int64_t nextId_ { 1 };
};

} // namespace onda::stems
