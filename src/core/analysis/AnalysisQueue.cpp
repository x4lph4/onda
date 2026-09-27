#include "AnalysisQueue.h"

#include <algorithm>

#include "MusicAnalysisEngine.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace onda {

AnalysisQueue::AnalysisQueue(MusicAnalysisEngine& engine, int numWorkers)
    : engine_(engine) {
    const int hw = (int)std::thread::hardware_concurrency();
    numWorkers_ = numWorkers > 0 ? numWorkers
                  : std::max(1, std::min(4, (hw > 2 ? hw / 2 : 1)));
    for (int i = 0; i < numWorkers_; ++i)
        workers_.emplace_back([this] { workerMain(); });
}

AnalysisQueue::~AnalysisQueue() {
    exit_.store(true);
    cv_.notify_all();
    for (auto& t : workers_) if (t.joinable()) t.join();
}

int64_t AnalysisQueue::enqueue(const std::string& path, int priority,
                               int maxRetries, uint32_t mask,
                               int64_t trackId) {
    auto j = std::make_shared<Job>();
    j->path = path; j->priority = priority; j->retries = maxRetries;
    j->mask = mask; j->trackId = trackId;
    j->cancel = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard<std::mutex> lk(m_);
        j->id = nextId_++;
        heap_.push_back(j);
        std::push_heap(heap_.begin(), heap_.end(), higherPriority);
        jobs_[j->id] = j;
        states_[j->id] = { (int)AnalysisState::NotAnalyzed, 0.0f, 0 };
        events_[j->id] = -1;                // no event fired yet
    }
    cv_.notify_one();
    return j->id;
}

void AnalysisQueue::cancel(int64_t jobId) {
    std::lock_guard<std::mutex> lk(m_);
    if (auto it = jobs_.find(jobId); it != jobs_.end())
        it->second->cancel->store(true);
}

void AnalysisQueue::retry(int64_t jobId) {
    std::shared_ptr<Job> j;
    {
        std::lock_guard<std::mutex> lk(m_);
        auto it = jobs_.find(jobId);
        if (it == jobs_.end()) return;
        j = it->second;
        j->cancel->store(false);
        heap_.push_back(j);
        std::push_heap(heap_.begin(), heap_.end(), higherPriority);
        states_[jobId] = { (int)AnalysisState::NotAnalyzed, 0.0f,
                           states_[jobId].attempts };
    }
    cv_.notify_one();
}

void AnalysisQueue::pause()  { paused_.store(true); }
void AnalysisQueue::resume() { paused_.store(false); cv_.notify_all(); }

void AnalysisQueue::setMinActivePriority(int p) {
    minActivePriority_.store(p);
    cv_.notify_all();   // reopen the gate
}

std::unordered_map<int64_t, AnalysisQueue::JobInfo> AnalysisQueue::snapshot() const {
    std::lock_guard<std::mutex> lk(m_);
    return states_;
}

void AnalysisQueue::markSaving(int64_t jobId) {
    std::lock_guard<std::mutex> lk(m_);
    events_[jobId] = (int)Event::Saving;
}

/// Map the internal event/progress ledger onto the external §4
/// job lifecycle.
static int toJobState(int lastEvent, float progress) {
    switch ((AnalysisQueue::Event)lastEvent) {
        case AnalysisQueue::Event::Completed: return (int)JobState::Completed;
        case AnalysisQueue::Event::Failed:    return (int)JobState::Failed;
        case AnalysisQueue::Event::Cancelled: return (int)JobState::Cancelled;
        case AnalysisQueue::Event::Retried:   return (int)JobState::Queued;
        case AnalysisQueue::Event::Saving:    return (int)JobState::Caching;
        case AnalysisQueue::Event::Started:
        case AnalysisQueue::Event::Progress:
            return progress < 0.15f ? (int)JobState::Decoding
                                    : (int)JobState::Analyzing;
        default: return (int)JobState::Queued;
    }
}

std::vector<AnalysisJob> AnalysisQueue::snapshotJobs() const {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<AnalysisJob> out;
    out.reserve(jobs_.size());
    for (const auto& [id, j] : jobs_) {
        AnalysisJob d;
        d.jobId = id; d.trackId = j->trackId; d.path = j->path;
        d.priority = j->priority; d.requestedAnalyses = j->mask;
        const auto st = states_.find(id);
        if (st != states_.end()) {
            d.progress = st->second.progress;
            d.attempts = st->second.attempts;
        }
        const auto ev = events_.find(id);
        d.state = toJobState(ev != events_.end() ? ev->second : -1,
                             d.progress);
        const auto er = errors_.find(id);
        if (er != errors_.end()) d.error = er->second;
        out.push_back(std::move(d));
    }
    return out;
}

AnalysisJob AnalysisQueue::jobInfo(int64_t jobId) const {
    const auto all = snapshotJobs();
    for (const auto& j : all)
        if (j.jobId == jobId) return j;
    return {};
}

void AnalysisQueue::addListener(Listener l) {
    std::lock_guard<std::mutex> lk(m_);
    listeners_.push_back(std::move(l));
}

void AnalysisQueue::notify(const Notification& n) {
    std::vector<Listener> ls;
    {
        std::lock_guard<std::mutex> lk(m_);
        ls = listeners_;
        events_[n.jobId] = (int)n.event;
        if (n.event == Event::Failed || n.event == Event::Retried)
            errors_[n.jobId] = n.error;
        if (n.event != Event::Progress) {
            states_[n.jobId].state = n.event == Event::Completed
                ? (int)AnalysisState::Analyzed
                : n.event == Event::Cancelled ? (int)AnalysisState::NotAnalyzed
                : (int)AnalysisState::Failed;
        }
        states_[n.jobId].progress = n.progress;
    }
    for (auto& l : ls) l(n);
}

void AnalysisQueue::workerMain() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    while (!exit_.load()) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [&] {
                return exit_.load()
                    || (!paused_.load() && !heap_.empty()
                        && heap_.front()->priority
                               >= minActivePriority_.load());
            });
            if (exit_.load()) return;
            if (paused_.load() || heap_.empty()) continue;
            std::pop_heap(heap_.begin(), heap_.end(), higherPriority);
            job = heap_.back();
            heap_.pop_back();
            states_[job->id].state = (int)AnalysisState::Analyzing;
            states_[job->id].attempts++;
        }

        notify({ job->id, Event::Started, 0.0f, job->path, {}, nullptr });

        auto progressCb = [this, job](float p) {
            notify({ job->id, Event::Progress, p, job->path, {}, nullptr });
        };
        auto bundle = engine_.analyzeTrack(job->path, job->cancel,
                                           progressCb, job->mask);

        if (job->cancel->load()) {
            notify({ job->id, Event::Cancelled, 0.0f, job->path, {}, nullptr });
        } else if (bundle == nullptr || bundle->result.state != AnalysisState::Analyzed) {
            std::string err = bundle ? bundle->result.error : "decode failed";
            bool requeued = false;
            {
                std::lock_guard<std::mutex> lk(m_);
                if (states_[job->id].attempts <= job->retries) {
                    heap_.push_back(job);
                    std::push_heap(heap_.begin(), heap_.end(), higherPriority);
                    requeued = true;
                }
            }
            if (requeued) {
                notify({ job->id, Event::Retried, 0.0f, job->path, err, nullptr });
                cv_.notify_one();
            } else {
                notify({ job->id, Event::Failed, 0.0f, job->path, err, nullptr });
            }
        } else {
            notify({ job->id, Event::Completed, 1.0f, job->path, {}, bundle });
        }
    }
}

} // namespace onda
