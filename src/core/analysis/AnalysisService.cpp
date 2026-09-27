#include "AnalysisService.h"

#include <chrono>
#include <unordered_map>

#include "TapBpmTracker.h"

namespace onda {

namespace {
/// §56 — per-path tap trackers for the TapBpm edit action.
std::mutex g_tapM;
std::unordered_map<std::string, TapBpmTracker> g_taps;
}

AnalysisService::AnalysisService(DecoderFactory factory, Database& db,
                                 int numWorkers)
    : engine_(std::move(factory)), store_(db), queue_(engine_, numWorkers) {
    queue_.addListener([this](const AnalysisQueue::Notification& n) {
        onJobEvent(n);
    });
}

int64_t AnalysisService::analyzeTrack(const std::string& path,
                                      int priority, uint32_t mask) {
    return queue_.enqueue(path, priority, 1, mask);
}

int64_t AnalysisService::analyzePartial(const std::string& path,
                                        uint32_t mask, int priority,
                                        int64_t trackId) {
    return queue_.enqueue(path, priority, 1, mask, trackId);
}

int64_t AnalysisService::analyzeForDeck(const std::string& path,
                                        int64_t trackId) {
    // §52 — essentials first at deck-load priority…
    const int64_t job = queue_.enqueue(
        path, (int)AnalysisPriority::Realtime, 1,
        AnalysisMask::Essential, trackId);
    // …§53 — the rest continues in the background and merges
    // component-wise on save (§8 — nothing is re-analyzed).
    queue_.enqueue(path, (int)AnalysisPriority::Low, 1,
                   AnalysisMask::All & ~AnalysisMask::Essential,
                   trackId);
    return job;
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
        queue_.markSaving(n.jobId);
        // §8 — fold prior analyzed components into this result so a
        // partial run completes the record instead of replacing it.
        if (auto prior = store_.load(n.path))
            mergePrior(*n.bundle, *prior);
        store_.save(n.path, *n.bundle, {}, {});
        std::lock_guard<std::mutex> lk(cacheM_);
        cache_[n.path] = n.bundle;
    } else if (n.event == AnalysisQueue::Event::Failed) {
        // §60 — persist the failure (state + technical error) so a
        // rescan reports Failed instead of silently re-queueing
        // forever. The track itself is never deleted.
        queue_.markSaving(n.jobId);
        store_.saveFailed(n.path, n.error);
    }
}

void AnalysisService::mergePrior(AnalysisBundle& fresh,
                                 const AnalysisBundle& prior) const {
    auto& r = fresh.result;
    // Which components the fresh run actually produced — captured
    // BEFORE statuses merge, otherwise the status copy below would
    // make skipped components look "done" and block the data merge.
    bool ran[kNumComponents];
    for (int c = 0; c < kNumComponents; ++c)
        ran[c] = r.components[c].state == (int)AnalysisState::Analyzed;
    auto keep = [&](AnalysisComponent c) {
        return prior.result.components[(int)c].state ==
               (int)AnalysisState::Analyzed;
    };
    for (int c = 0; c < kNumComponents; ++c)
        if (!ran[c] && keep((AnalysisComponent)c))
            r.components[c] = prior.result.components[c];

    if (!ran[(int)AnalysisComponent::Bpm] &&
        keep(AnalysisComponent::Bpm))
        r.bpm = prior.result.bpm;
    if (!ran[(int)AnalysisComponent::Key] &&
        keep(AnalysisComponent::Key)) {
        r.key = prior.result.key; fresh.keyDetail = prior.keyDetail;
    }
    if (!ran[(int)AnalysisComponent::Waveform] &&
        keep(AnalysisComponent::Waveform) &&
        fresh.waveform.empty())
        fresh.waveform = prior.waveform;
    if (!ran[(int)AnalysisComponent::BeatGrid] &&
        keep(AnalysisComponent::BeatGrid) &&
        fresh.grid.empty())
        fresh.grid = prior.grid;
    if (!ran[(int)AnalysisComponent::Loudness] &&
        keep(AnalysisComponent::Loudness))
        r.loudness = prior.result.loudness;
    if (!ran[(int)AnalysisComponent::Spectral] &&
        keep(AnalysisComponent::Spectral)) {
        r.spectral = prior.result.spectral;
        if (fresh.waveform.bands.empty())
            fresh.waveform.bands = prior.waveform.bands;
    }
    if (!ran[(int)AnalysisComponent::Energy] &&
        keep(AnalysisComponent::Energy))
        r.energy = prior.result.energy;
    if (!ran[(int)AnalysisComponent::Transients] &&
        keep(AnalysisComponent::Transients))
        r.transients = prior.result.transients;
    if (fresh.overrides.empty())
        fresh.overrides = prior.overrides;
    // track-level measurements that don't belong to one component
    if (r.firstSoundFrame < 0 && prior.result.firstSoundFrame >= 0) {
        r.firstSoundFrame = prior.result.firstSoundFrame;
        r.lastSoundFrame  = prior.result.lastSoundFrame;
        r.silences        = prior.result.silences;
    }
    if (r.needsReview == false && prior.result.needsReview) {
        r.needsReview   = prior.result.needsReview;
        r.reviewReasons = prior.result.reviewReasons;
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
    if (action == AnalysisEdit::TapBpm) {
        // §56 — feed the tap clock; ≥2 taps produce a BPM override.
        const int64_t now = std::chrono::duration_cast<
            std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();
        double est = 0.0;
        {
            std::lock_guard lk(g_tapM);
            est = g_taps[path].tap(now);
        }
        if (est > 0.0) setUserBpm(path, est);
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
