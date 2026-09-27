#include "HistoryManager.h"

#include <ctime>

namespace onda::library {

static int64_t now() { return (int64_t)std::time(nullptr); }

int64_t HistoryManager::beginSession(const std::string& name) {
    std::lock_guard lk(repo_.db().mutex());
    auto s = repo_.db().prepare(
        "INSERT INTO sessions(name,started_at) VALUES(?,?)");
    s.bind(1, name); s.bind(2, now());
    s.step();
    sessionId_ = repo_.db().lastInsertId();
    return sessionId_;
}

bool HistoryManager::endSession(int64_t sessionId) {
    std::lock_guard lk(repo_.db().mutex());
    auto s = repo_.db().prepare(
        "UPDATE sessions SET ended_at=? WHERE id=?");
    s.bind(1, now()); s.bind(2, sessionId);
    s.step();
    if (sessionId_ == sessionId) sessionId_ = -1;
    return true;
}

int64_t HistoryManager::recordPlay(int64_t trackId, int deck,
                                   int64_t startedAt, double playedSec,
                                   int64_t sessionId) {
    std::lock_guard lk(repo_.db().mutex());
    Track t;
    if (!repo_.getById(trackId, t)) return -1;
    const bool counted = rule_.qualifies(playedSec, t.durationSec);
    const int64_t sid = sessionId >= 0 ? sessionId
                        : (sessionId_ >= 0 ? sessionId_
                                           : beginSession("auto"));
    Database::Tx tx(repo_.db());
    int64_t id;
    {
        auto s = repo_.db().prepare(
            "INSERT INTO history(track_id,session_id,deck,started_at,"
            "played_sec,counted) VALUES(?,?,?,?,?,?)");
        s.bind(1, trackId); s.bind(2, sid); s.bind(3, (int64_t)deck);
        s.bind(4, startedAt); s.bind(5, playedSec);
        s.bind(6, (int64_t)(counted ? 1 : 0));
        s.step();
        id = repo_.db().lastInsertId();
    }
    repo_.bumpPlayStats(trackId, playedSec, startedAt, counted);
    tx.commit();
    return id;
}

static HistoryEntry rowToEntry(Database::Stmt& s) {
    HistoryEntry e;
    e.id = s.colInt(0); e.trackId = s.colInt(1);
    e.sessionId = s.colInt(2); e.deck = (int)s.colInt(3);
    e.startedAt = s.colInt(4); e.playedSec = s.colDouble(5);
    e.counted = (int)s.colInt(6);
    return e;
}

std::vector<HistoryEntry> HistoryManager::entries(int64_t sessionId,
                                                  int limit, int offset) {
    std::lock_guard lk(repo_.db().mutex());
    std::vector<HistoryEntry> out;
    Database::Stmt s = sessionId < 0
        ? repo_.db().prepare(
            "SELECT id,track_id,session_id,deck,started_at,played_sec,"
            "counted FROM history ORDER BY started_at DESC LIMIT ? OFFSET ?")
        : repo_.db().prepare(
            "SELECT id,track_id,session_id,deck,started_at,played_sec,"
            "counted FROM history WHERE session_id=? "
            "ORDER BY started_at LIMIT ? OFFSET ?");
    if (sessionId < 0) {
        s.bind(1, (int64_t)limit); s.bind(2, (int64_t)offset);
    } else {
        s.bind(1, sessionId); s.bind(2, (int64_t)limit);
        s.bind(3, (int64_t)offset);
    }
    while (s.step()) out.push_back(rowToEntry(s));
    return out;
}

std::vector<HistoryEntry> HistoryManager::forTrack(int64_t trackId,
                                                   int limit) {
    std::lock_guard lk(repo_.db().mutex());
    auto s = repo_.db().prepare(
        "SELECT id,track_id,session_id,deck,started_at,played_sec,counted "
        "FROM history WHERE track_id=? ORDER BY started_at DESC LIMIT ?");
    s.bind(1, trackId); s.bind(2, (int64_t)limit);
    std::vector<HistoryEntry> out;
    while (s.step()) out.push_back(rowToEntry(s));
    return out;
}

SessionHistory HistoryManager::session(int64_t sessionId) {
    std::lock_guard lk(repo_.db().mutex());
    SessionHistory h;
    h.id = sessionId;
    {
        auto s = repo_.db().prepare(
            "SELECT name,started_at,ended_at FROM sessions WHERE id=?");
        s.bind(1, sessionId);
        if (s.step()) {
            h.name = s.colText(0);
            h.startedAt = s.colInt(1);
            h.endedAt = s.colInt(2);
        }
    }
    {
        auto s = repo_.db().prepare(
            "SELECT id,track_id,session_id,deck,started_at,played_sec,"
            "counted FROM history WHERE session_id=? "
            "ORDER BY started_at");
        s.bind(1, sessionId);
        while (s.step()) h.entries.push_back(rowToEntry(s));
    }
    return h;
}

int HistoryManager::sessionCount() {
    std::lock_guard lk(repo_.db().mutex());
    auto s = repo_.db().prepare("SELECT COUNT(*) FROM sessions");
    return s.step() ? (int)s.colInt(0) : 0;
}

bool HistoryManager::renameSession(int64_t id, const std::string& name) {
    std::lock_guard lk(repo_.db().mutex());
    auto s = repo_.db().prepare("UPDATE sessions SET name=? WHERE id=?");
    s.bind(1, name); s.bind(2, id);
    s.step();
    return true;
}

bool HistoryManager::deleteSession(int64_t id) {
    std::lock_guard lk(repo_.db().mutex());
    Database::Tx tx(repo_.db());
    {
        auto s = repo_.db().prepare(
            "UPDATE history SET session_id=NULL WHERE session_id=?");
        s.bind(1, id); s.step();
    }
    {
        auto s = repo_.db().prepare("DELETE FROM sessions WHERE id=?");
        s.bind(1, id); s.step();
    }
    tx.commit();
    if (sessionId_ == id) sessionId_ = -1;
    return true;
}

} // namespace onda::library
