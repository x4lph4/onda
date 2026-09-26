#include "AnalysisService.h"

namespace onda {

AnalysisService::AnalysisService(DecoderFactory factory, Database& db,
                                 int numWorkers)
    : engine_(std::move(factory)), store_(db), queue_(engine_, numWorkers) {
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
    // Effective grid: user override when present, else detected.
    if (auto b = getAnalysis(path)) return b->effectiveGrid();
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
    store_.saveUserGrid(id, g);   // user_overrides row — detected kept
    std::lock_guard<std::mutex> lk(cacheM_);
    if (auto it = cache_.find(path); it != cache_.end())
        it->second->overrides.grid = g;
}

BeatGrid AnalysisService::applyEdit(const std::string& path,
                                    AnalysisEdit action, double value) {
    if (action == AnalysisEdit::Reanalyze) {
        analyzeTrack(path, (int)AnalysisPriority::High);
        return {};
    }
    if (action == AnalysisEdit::SetKey) {
        setUserKey(path, (int)value);
        std::lock_guard<std::mutex> lk(cacheM_);
        if (auto it = cache_.find(path); it != cache_.end())
            it->second->overrides.keyIndex = (int)value >= 0
                ? std::optional<int>{(int)value} : std::nullopt;
        return {};
    }
    if (action == AnalysisEdit::SetBpmOverride) {
        setUserBpm(path, value);
        std::lock_guard<std::mutex> lk(cacheM_);
        if (auto it = cache_.find(path); it != cache_.end())
            it->second->overrides.bpm = value > 0.0
                ? std::optional<double>{value} : std::nullopt;
        return {};
    }
    if (action == AnalysisEdit::ResetGrid) {
        store_.clearUserGrid(store_.trackIdFor(path));
        std::lock_guard<std::mutex> lk(cacheM_);
        if (auto it = cache_.find(path); it != cache_.end()) {
            it->second->overrides.grid.reset();
            return it->second->grid;
        }
        return getBeatGrid(path);
    }

    auto b = getAnalysis(path);
    if (!b) return {};
    BeatGrid g = b->effectiveGrid();
    switch (action) {
        case AnalysisEdit::SetFirstBeat:
            BeatGridEngine::setFirstBeat(g, (int64_t)value); break;
        case AnalysisEdit::NudgeGrid:
            BeatGridEngine::nudgeBeatGrid(g, value); break;
        case AnalysisEdit::ChangeBpm:
            if (value <= 0.0) return {};
            BeatGridEngine::adjustBpm(g, value); break;
        case AnalysisEdit::DoubleTempo:
            BeatGridEngine::adjustBpm(g, g.bpmAt(0) * 2.0); break;
        case AnalysisEdit::HalfTempo:
            BeatGridEngine::adjustBpm(g, g.bpmAt(0) * 0.5); break;
        case AnalysisEdit::SetDownbeat:
            BeatGridEngine::setDownbeat(g, (int)value); break;
        case AnalysisEdit::SetBeatsPerBar:
            if ((int)value <= 0) return {};
            BeatGridEngine::setBeatsPerBar(g, (int)value); break;
        default: return {};
    }
    saveUserGrid(path, g);
    return g;
}

int AnalysisService::componentState(const std::string& path,
                                    AnalysisComponent c) {
    if (auto b = getAnalysis(path))
        return b->result.components[(int)c].state;
    return (int)AnalysisState::NotAnalyzed;
}

bool AnalysisService::needsReview(const std::string& path) {
    if (auto b = getAnalysis(path)) return b->result.needsReview;
    return false;
}

bool AnalysisService::hasOverride(const std::string& path) {
    if (auto b = getAnalysis(path)) return !b->overrides.empty();
    return false;
}

std::optional<SyncReference> AnalysisService::syncReference(
    const std::string& path) {
    auto b = getAnalysis(path);
    if (!b) return std::nullopt;
    return SyncReference::from(b->result, b->grid, b->overrides);
}

} // namespace onda
