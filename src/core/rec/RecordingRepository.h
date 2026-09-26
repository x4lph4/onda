#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "core/data/Database.h"
#include "core/rec/RecordingSession.h"   // unixNow
#include "core/rec/RecordingTypes.h"

namespace onda::rec {

/// SQLite persistence for recording sessions/files/events/setlist/
/// recovery — same Database instance as the library (onda.db).
/// Tables are created idempotently by init(); all access is locked
/// through Database::mutex. Called from app/worker threads only.
class RecordingRepository {
public:
    explicit RecordingRepository(Database& db) : db_(db) {}

    bool init() {
        std::lock_guard lk(db_.mutex());
        return db_.exec(
            "CREATE TABLE IF NOT EXISTS recording_sessions("
            " id INTEGER PRIMARY KEY,"
            " name TEXT, uuid TEXT, start_ts INTEGER, end_ts INTEGER,"
            " duration_sec REAL, source INTEGER, sample_rate REAL,"
            " bit_depth INTEGER, channels INTEGER, file_path TEXT,"
            " format TEXT, file_bytes INTEGER, frames INTEGER,"
            " peak REAL, clips INTEGER, state INTEGER);")
        && db_.exec(
            "CREATE TABLE IF NOT EXISTS recording_files("
            " id INTEGER PRIMARY KEY, session_id INTEGER, part INTEGER,"
            " path TEXT, bytes INTEGER, frames INTEGER);")
        && db_.exec(
            "CREATE TABLE IF NOT EXISTS recording_events("
            " id INTEGER PRIMARY KEY, session_id INTEGER, ts INTEGER,"
            " kind TEXT, detail TEXT);")
        && db_.exec(
            "CREATE TABLE IF NOT EXISTS session_tracks("
            " id INTEGER PRIMARY KEY, session_id INTEGER, ts INTEGER,"
            " deck INTEGER, path TEXT, title TEXT, artist TEXT,"
            " bpm REAL, key TEXT, played_sec REAL);")
        && db_.exec(
            "CREATE TABLE IF NOT EXISTS recording_recovery("
            " id INTEGER PRIMARY KEY, session_id INTEGER, ts INTEGER,"
            " action TEXT, detail TEXT);");
    }

    // ---- sessions ----
    int64_t beginSession(const SessionInfo& s, const std::string& uuid) {
        std::lock_guard lk(db_.mutex());
        auto q = db_.prepare(
            "INSERT INTO recording_sessions("
            " name,uuid,start_ts,end_ts,duration_sec,source,sample_rate,"
            " bit_depth,channels,file_path,format,file_bytes,frames,"
            " peak,clips,state) VALUES(?,?,?,0,0,?,?,?,?,?,?,0,0,0,0,?)");
        q.bind(1, s.name);       q.bind(2, uuid);
        q.bind(3, s.startTs);    q.bind(4, (int64_t)s.source);
        q.bind(5, s.sampleRate); q.bind(6, (int64_t)s.bitDepth);
        q.bind(7, (int64_t)s.channels); q.bind(8, s.filePath);
        q.bind(9, s.format);     q.bind(10, (int64_t)s.state);
        q.step();
        return db_.lastInsertId();
    }
    void endSession(int64_t id, const SessionInfo& s) {
        std::lock_guard lk(db_.mutex());
        auto q = db_.prepare(
            "UPDATE recording_sessions SET end_ts=?,duration_sec=?,"
            " file_path=?,file_bytes=?,frames=?,peak=?,clips=?,state=?"
            " WHERE id=?");
        q.bind(1, s.endTs);   q.bind(2, s.durationSec);
        q.bind(3, s.filePath);q.bind(4, s.fileBytes);
        q.bind(5, s.frames);  q.bind(6, (double)s.peakLevel);
        q.bind(7, (int64_t)s.clipCount); q.bind(8, (int64_t)s.state);
        q.bind(9, id); q.step();
    }
    void setState(int64_t id, State st) {
        std::lock_guard lk(db_.mutex());
        auto q = db_.prepare(
            "UPDATE recording_sessions SET state=? WHERE id=?");
        q.bind(1, (int64_t)st); q.bind(2, id); q.step();
    }
    void rename(int64_t id, const std::string& name) {
        std::lock_guard lk(db_.mutex());
        auto q = db_.prepare(
            "UPDATE recording_sessions SET name=? WHERE id=?");
        q.bind(1, name); q.bind(2, id); q.step();
    }
    void remove(int64_t id) {
        std::lock_guard lk(db_.mutex());
        for (const char* t : {"recording_sessions","recording_files",
                              "recording_events","session_tracks",
                              "recording_recovery"}) {
            auto q = db_.prepare(
                (std::string("DELETE FROM ") + t +
                 (std::string(t) == "recording_sessions"
                  ? " WHERE id=?" : " WHERE session_id=?")).c_str());
            q.bind(1, id); q.step();
        }
    }
    std::vector<SessionInfo> sessions(int limit = 100) {
        std::lock_guard lk(db_.mutex());
        std::vector<SessionInfo> out;
        auto q = db_.prepare(
            "SELECT id,name,start_ts,end_ts,duration_sec,source,"
            " sample_rate,bit_depth,channels,file_path,format,"
            " file_bytes,frames,peak,clips,state FROM recording_sessions"
            " ORDER BY start_ts DESC LIMIT ?");
        q.bind(1, (int64_t)limit);
        while (q.step()) {
            SessionInfo s;
            s.id = q.colInt(0);       s.name = q.colText(1);
            s.startTs = q.colInt(2);  s.endTs = q.colInt(3);
            s.durationSec = q.colDouble(4);
            s.source = (Source)q.colInt(5);
            s.sampleRate = q.colDouble(6);
            s.bitDepth = (BitDepth)q.colInt(7);
            s.channels = (int)q.colInt(8);
            s.filePath = q.colText(9); s.format = q.colText(10);
            s.fileBytes = q.colInt(11); s.frames = q.colInt(12);
            s.peakLevel = (float)q.colDouble(13);
            s.clipCount = (uint32_t)q.colInt(14);
            s.state = (State)q.colInt(15);
            out.push_back(std::move(s));
        }
        return out;
    }
    /// Sessions left non-terminal by a crash (state=Recording etc.).
    std::vector<SessionInfo> interrupted() {
        std::lock_guard lk(db_.mutex());
        std::vector<SessionInfo> out;
        auto q = db_.prepare(
            "SELECT id,file_path,state FROM recording_sessions"
            " WHERE state IN (1,2,3,4,5)");
        while (q.step()) {
            SessionInfo s;
            s.id = q.colInt(0); s.filePath = q.colText(1);
            s.state = (State)q.colInt(2);
            out.push_back(std::move(s));
        }
        return out;
    }

    // ---- files / events / tracks / recovery log ----
    void addFile(int64_t sessionId, const SessionInfo& part) {
        std::lock_guard lk(db_.mutex());
        auto q = db_.prepare(
            "INSERT INTO recording_files(session_id,part,path,bytes,"
            " frames) VALUES(?,?,?,?,?)");
        q.bind(1, sessionId); q.bind(2, (int64_t)part.part);
        q.bind(3, part.filePath); q.bind(4, part.fileBytes);
        q.bind(5, part.frames); q.step();
    }
    void event(int64_t sessionId, int64_t ts, const char* kind,
               const std::string& detail = {}) {
        std::lock_guard lk(db_.mutex());
        auto q = db_.prepare(
            "INSERT INTO recording_events(session_id,ts,kind,detail)"
            " VALUES(?,?,?,?)");
        q.bind(1, sessionId); q.bind(2, ts);
        q.bind(3, kind); q.bind(4, detail); q.step();
    }
    int64_t addTrack(int64_t sessionId, const TrackPlay& t) {
        std::lock_guard lk(db_.mutex());
        auto q = db_.prepare(
            "INSERT INTO session_tracks(session_id,ts,deck,path,title,"
            " artist,bpm,key,played_sec) VALUES(?,?,?,?,?,?,?,?,?)");
        q.bind(1, sessionId); q.bind(2, t.ts);
        q.bind(3, (int64_t)t.deck); q.bind(4, t.path);
        q.bind(5, t.title); q.bind(6, t.artist);
        q.bind(7, t.bpm); q.bind(8, t.key); q.bind(9, t.playedSec);
        q.step();
        return db_.lastInsertId();
    }
    void updateTrackPlayed(int64_t rowId, double playedSec) {
        std::lock_guard lk(db_.mutex());
        auto q = db_.prepare(
            "UPDATE session_tracks SET played_sec=? WHERE id=?");
        q.bind(1, playedSec); q.bind(2, rowId); q.step();
    }
    std::vector<TrackPlay> tracks(int64_t sessionId) {
        std::lock_guard lk(db_.mutex());
        std::vector<TrackPlay> out;
        auto q = db_.prepare(
            "SELECT ts,deck,path,title,artist,bpm,key,played_sec"
            " FROM session_tracks WHERE session_id=? ORDER BY ts");
        q.bind(1, sessionId);
        while (q.step()) {
            TrackPlay t;
            t.ts = q.colInt(0);    t.deck = (int)q.colInt(1);
            t.path = q.colText(2); t.title = q.colText(3);
            t.artist = q.colText(4); t.bpm = q.colDouble(5);
            t.key = q.colText(6);  t.playedSec = q.colDouble(7);
            out.push_back(std::move(t));
        }
        return out;
    }
    void recoveryLog(int64_t sessionId, const char* action,
                     const std::string& detail) {
        std::lock_guard lk(db_.mutex());
        auto q = db_.prepare(
            "INSERT INTO recording_recovery(session_id,ts,action,"
            " detail) VALUES(?,?,?,?)");
        q.bind(1, sessionId); q.bind(2, unixNow());
        q.bind(3, action); q.bind(4, detail); q.step();
    }

private:
    Database& db_;
};

} // namespace onda::rec
