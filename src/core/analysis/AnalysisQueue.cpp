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
                               int maxRetries) {
    auto j = std::make_shared<Job>();
    j->path = path; j->priority = priority; j->retries = maxRetries;
    j->cancel = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard<std::mutex> lk(m_);
        j->id = nextId_++;
        heap_.push_back(j);
        std::push_heap(heap_.begin(), heap_.end(), higherPriority);
        jobs_[j->id] = j;
        states_[j->id] = { (int)AnalysisState::NotAnalyzed, 0.0f, 0 };
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

std::unordered_map<int64_t, AnalysisQueue::JobInfo> AnalysisQueue::snapshot() const {
    std::lock_guard<std::mutex> lk(m_);
    return states_;
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
                return exit_.load() || (!paused_.load() && !heap_.empty());
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
        auto bundle = engine_.analyzeTrack(job->path, job->cancel, progressCb);

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
