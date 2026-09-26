#pragma once

#include <string>
#include <vector>

#include "LibraryTypes.h"
#include "TrackRepository.h"

namespace onda::library {

/// Played-history: sessions group entries recorded during a set.
///
///   sessions(id, name, started_at, ended_at)
///   history(id, track_id, session_id, deck, started_at, played_sec, counted)
///
/// Play-count rule (documented): `counted` is set when playedSec meets
/// PlayCountRule (default ≥60 s OR ≥50 % of the track, whichever is
/// less). Loading a track NEVER counts — only `recordPlay` with real
/// playback time does. `counted` entries bump play_count/last_played
/// in the same transaction as the history insert.
///
/// Writes go through `recordPlayAsync` on the library worker — the
/// audio/UI threads never wait on SQLite.
class HistoryManager {
public:
    explicit HistoryManager(TrackRepository& repo) : repo_(repo) {}

    void setPlayCountRule(const PlayCountRule& r) { rule_ = r; }
    PlayCountRule playCountRule() const { return rule_; }

    int64_t beginSession(const std::string& name = "");
    bool endSession(int64_t sessionId);
    int64_t currentSession() const { return sessionId_; }

    /// Log a finished playback segment. `playedSec` is real output time.
    /// counted flag decided by the rule; play stats update atomically.
    int64_t recordPlay(int64_t trackId, int deck, int64_t startedAt,
                       double playedSec, int64_t sessionId = -1);

    std::vector<HistoryEntry> entries(int64_t sessionId = -1,
                                    int limit = 500, int offset = 0);
    std::vector<HistoryEntry> forTrack(int64_t trackId, int limit = 100);
    int sessionCount();
    bool renameSession(int64_t id, const std::string& name);
    bool deleteSession(int64_t id);          // entries keep track_id via SET NULL

private:
    TrackRepository& repo_;
    PlayCountRule rule_;
    int64_t sessionId_ { -1 };
};

} // namespace onda::library
