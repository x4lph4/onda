#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "AnalysisQueue.h"
#include "BeatGridEngine.h"
#include "MusicAnalysisEngine.h"
#include "SyncReference.h"
#include "../data/AnalysisStore.h"

namespace onda {

/// App-facing analysis API.
///
///   service.analyzeTrack(path)      → background job (queue)
///   service.analyzeTrackSync(path)  → blocking (tests/CLI import)
///   service.getAnalysis/getBeatGrid/getWaveform/getKey/getBpm
///   service.stateFor(path)          → cache-aware state
///
/// Events (AnalysisQueue::Notification) fire on worker threads —
/// UI should marshal to its own thread (JUCE AsyncUpdater pattern).
///
/// Results are memoised in memory; manual grid edits go through
/// saveUserGrid so detection data survives correction.
class AnalysisService {
public:
    /// `numWorkers` — analysis worker threads (≤0 → auto from CPU).
    AnalysisService(DecoderFactory factory, Database& db, int numWorkers = -1);

    // ---- jobs ----
    int64_t analyzeTrack(const std::string& path, int priority = 0);
    std::shared_ptr<AnalysisBundle> analyzeTrackSync(const std::string& path);
    void cancel(int64_t jobId)          { queue_.cancel(jobId); }
    void retry(int64_t jobId)           { queue_.retry(jobId); }
    void pauseQueue()                   { queue_.pause(); }
    void resumeQueue()                  { queue_.resume(); }
    AnalysisQueue& queue()              { return queue_; }
    int  workerCount() const            { return queue_.workerCount(); }

    /// CPU gate (§29): while true, only ≥HIGH priority jobs run so
    /// background analysis never competes with playback/recording.
    void setPlaybackActive(bool active) {
        queue_.setMinActivePriority(active ? (int)AnalysisPriority::High : 0);
    }

    void addListener(AnalysisQueue::Listener l) { queue_.addListener(std::move(l)); }

    // ---- reads ----
    AnalysisState stateFor(const std::string& path);
    std::shared_ptr<AnalysisBundle> getAnalysis(const std::string& path);
    BeatGrid      getBeatGrid(const std::string& path);
    const AnalysisBundle* getWaveformPtr(const std::string& path);
    KeyResult     getKey(const std::string& path);
    double        getBpm(const std::string& path);   // effective (override-aware)
    int64_t       trackIdFor(const std::string& path); // persistent id

    // ---- manual overrides (never destroy detected data) ----
    void setUserBpm(const std::string& path, double bpm);   // -1 clears
    void setUserKey(const std::string& path, int keyIndex); // -1 clears
    void saveUserGrid(const std::string& path, const BeatGrid& g);
    int  invalidateStale() { return store_.invalidateStale(); }

    /// Unified edit surface (AnalysisEdit actions). Returns the new
    /// effective grid so callers can republish it to decks; an invalid
    /// action yields an empty grid.
    BeatGrid applyEdit(const std::string& path, AnalysisEdit action,
                       double value = 0.0);

    // ---- component-level status & review (§26–27) ----
    int      componentState(const std::string& path, AnalysisComponent c);
    bool     needsReview(const std::string& path);
    bool     hasOverride(const std::string& path);

    /// Sync-ready view for the performance engine (§32).
    std::optional<SyncReference> syncReference(const std::string& path);

    AnalysisStore& store() { return store_; }

private:
    void onJobEvent(const AnalysisQueue::Notification& n);

    MusicAnalysisEngine engine_;
    AnalysisStore store_;
    AnalysisQueue queue_;
    std::mutex cacheM_;
    std::unordered_map<std::string, std::shared_ptr<AnalysisBundle>> cache_;
};

} // namespace onda
