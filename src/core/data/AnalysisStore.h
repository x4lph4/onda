#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "../analysis/AnalysisTypes.h"
#include "../analysis/BeatGrid.h"
#include "../analysis/MusicAnalysisEngine.h"
#include "../analysis/WaveformData.h"
#include "Database.h"

namespace onda {

/// Persistent analysis cache over SQLite.
///
/// Track identity = path (unique). Cache validity = fingerprint
/// (size ^ mtime ^ content hash of head/tail) + kAnalysisVersion.
/// Detected values live in `analysis`; user corrections live in
/// `user_overrides` and NEVER overwrite detections.
class AnalysisStore {
public:
    explicit AnalysisStore(Database& db) : db_(db) {}

    /// Content fingerprint for cache invalidation (file size + mtime +
    /// FNV-1a over head and tail blocks). 0 when file unreadable.
    static uint64_t fingerprint(const std::string& path);

    int64_t trackIdFor(const std::string& path);   // creates row if needed
    std::optional<std::string> pathOf(int64_t trackId);

    /// Effective state for a track, honouring version/fingerprint drift.
    AnalysisState stateFor(const std::string& path);

    void save(const std::string& path, const AnalysisBundle& b,
              const std::string& title, const std::string& artist);
    std::shared_ptr<AnalysisBundle> load(const std::string& path);

    /// Phase 22 — rich key detail for one track (alternatives,
    /// segments, algo identity); loaded on demand, not in `load()`.
    std::optional<harmonic::KeyAnalysisResult> loadKeyDetail(
        int64_t trackId);

    /// Manual overrides — detected data is left untouched.
    void setUserBpm(int64_t trackId, double bpm);        // -1 clears
    void setUserKey(int64_t trackId, int keyIndex);      // -1 clears
    void markGridManual(int64_t trackId);
    void saveGrid(int64_t trackId, const BeatGrid& g, bool manual);
    /// Phase 14: user grid lives in user_overrides.grid_json — the
    /// detected `beatgrids` row is never overwritten by edits.
    void saveUserGrid(int64_t trackId, const BeatGrid& g);
    void clearUserGrid(int64_t trackId);

    /// Effective BPM for playback/UI: user override if present.
    double effectiveBpm(int64_t trackId, double detected) const;
    std::optional<double> userBpm(int64_t trackId) const;

    /// Re-mark rows whose fingerprint/version no longer matches.
    int invalidateStale();

private:
    std::string serializeGrid(const BeatGrid& g) const;
    BeatGrid deserializeGrid(const std::string& s, double rate) const;
    Database& db_;
};

} // namespace onda
