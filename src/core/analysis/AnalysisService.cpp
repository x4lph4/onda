#include "AnalysisService.h"

namespace onda {

AnalysisService::AnalysisService(DecoderFactory factory, Database& db)
    : engine_(std::move(factory)), store_(db), queue_(engine_) {
    queue_.addListener([this](const AnalysisQueue::Notification& n) {
        onJobEvent(n);
    });
}

int64_t AnalysisService::analyzeTrack(const std::string& path, int priority) {
    return queue_.enqueue(path, priority);
}

std::shared_ptr<AnalysisBundle> AnalysisService::analyzeTrackSync(
    const std::string& path) {
    auto b = engine_.analyzeTrack(path, nullptr, nullptr);
    if (b) {
        // the decoder is the only metadata source for now
        store_.save(path, *b, {}, {});
        std::lock_guard<std::mutex> lk(cacheM_);
        cache_[path] = b;
    }
    return b;
}

AnalysisState AnalysisService::stateFor(const std::string& path) {
    return store_.stateFor(path);
}

void AnalysisService::onJobEvent(const AnalysisQueue::Notification& n) {
    if (n.event == AnalysisQueue::Event::Completed && n.bundle) {
        store_.save(n.path, *n.bundle, {}, {});
        std::lock_guard<std::mutex> lk(cacheM_);
        cache_[n.path] = n.bundle;
    }
}

std::shared_ptr<AnalysisBundle> AnalysisService::getAnalysis(
    const std::string& path) {
    {
        std::lock_guard<std::mutex> lk(cacheM_);
        if (auto it = cache_.find(path); it != cache_.end())
            return it->second;
    }
    auto b = store_.load(path);
    if (b) {
        std::lock_guard<std::mutex> lk(cacheM_);
        cache_[path] = b;
    }
    return b;
}

BeatGrid AnalysisService::getBeatGrid(const std::string& path) {
    if (auto b = getAnalysis(path)) return b->grid;
    return {};
}

const AnalysisBundle* AnalysisService::getWaveformPtr(const std::string& path) {
    // returns a view into the cached bundle (stable while cached)
    static thread_local std::shared_ptr<AnalysisBundle> hold;
    hold = getAnalysis(path);
    return hold.get();
}

KeyResult AnalysisService::getKey(const std::string& path) {
    if (auto b = getAnalysis(path)) return b->result.key;
    return {};
}

int64_t AnalysisService::trackIdFor(const std::string& path) {
    return store_.trackIdFor(path);
}

double AnalysisService::getBpm(const std::string& path) {
    const auto id = store_.trackIdFor(path);
    if (auto b = getAnalysis(path))
        return store_.effectiveBpm(id, b->result.bpm.bpm);
    return store_.effectiveBpm(id, 0.0);
}

void AnalysisService::setUserBpm(const std::string& path, double bpm) {
    store_.setUserBpm(store_.trackIdFor(path), bpm);
}

void AnalysisService::setUserKey(const std::string& path, int keyIndex) {
    store_.setUserKey(store_.trackIdFor(path), keyIndex);
}

void AnalysisService::saveUserGrid(const std::string& path, const BeatGrid& g) {
    const auto id = store_.trackIdFor(path);
    store_.saveGrid(id, g, true);
    store_.markGridManual(id);
    std::lock_guard<std::mutex> lk(cacheM_);
    if (auto it = cache_.find(path); it != cache_.end())
        it->second->grid = g;
}

} // namespace onda
