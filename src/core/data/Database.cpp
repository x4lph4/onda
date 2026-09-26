#include "Database.h"

#include <ctime>

#include <sqlite3.h>

namespace onda {

static const char* kMigrations[] = {
    // v1 — library + analysis schema
    R"SQL(
CREATE TABLE IF NOT EXISTS schema_migrations(
    version INTEGER PRIMARY KEY,
    applied_at INTEGER NOT NULL);

CREATE TABLE IF NOT EXISTS tracks(
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    path TEXT NOT NULL UNIQUE,
    fingerprint INTEGER NOT NULL DEFAULT 0,
    title TEXT, artist TEXT,
    duration_sec REAL NOT NULL DEFAULT 0,
    sample_rate REAL NOT NULL DEFAULT 0,
    analysis_state INTEGER NOT NULL DEFAULT 0,
    analysis_version INTEGER NOT NULL DEFAULT 0,
    error TEXT,
    updated_at INTEGER NOT NULL DEFAULT 0);

CREATE TABLE IF NOT EXISTS analysis(
    track_id INTEGER PRIMARY KEY REFERENCES tracks(id) ON DELETE CASCADE,
    bpm REAL NOT NULL DEFAULT 0,
    bpm_confidence REAL NOT NULL DEFAULT 0,
    bpm_stability REAL NOT NULL DEFAULT 0,
    bpm_variable INTEGER NOT NULL DEFAULT 0,
    bpm_candidates TEXT,            -- JSON [[bpm,score],...]
    key_index INTEGER NOT NULL DEFAULT -1,
    key_confidence REAL NOT NULL DEFAULT 0,
    first_sound_frame INTEGER NOT NULL DEFAULT 0,
    last_sound_frame INTEGER NOT NULL DEFAULT 0,
    silences TEXT,                  -- JSON [[start,end],...]
    first_onset_frame INTEGER NOT NULL DEFAULT -1);

CREATE TABLE IF NOT EXISTS beatgrids(
    track_id INTEGER PRIMARY KEY REFERENCES tracks(id) ON DELETE CASCADE,
    first_beat_frame INTEGER NOT NULL DEFAULT 0,
    beats_per_bar INTEGER NOT NULL DEFAULT 4,
    downbeat_offset INTEGER NOT NULL DEFAULT 0,
    segments TEXT NOT NULL,         -- JSON [[startFrame,startBeat,bpm],...]
    manual INTEGER NOT NULL DEFAULT 0);  -- 1 if user-edited

CREATE TABLE IF NOT EXISTS waveforms(
    track_id INTEGER PRIMARY KEY REFERENCES tracks(id) ON DELETE CASCADE,
    channels INTEGER NOT NULL,
    num_frames INTEGER NOT NULL,
    sample_rate REAL NOT NULL,
    overview BLOB NOT NULL,         -- WaveformBin triplets × channels
    detail BLOB NOT NULL);

CREATE TABLE IF NOT EXISTS user_overrides(
    track_id INTEGER PRIMARY KEY REFERENCES tracks(id) ON DELETE CASCADE,
    bpm REAL,                       -- NULL = use detected
    key_index INTEGER,              -- NULL = use detected
    grid_first_beat_frame INTEGER,
    updated_at INTEGER NOT NULL DEFAULT 0);
)SQL",
};

bool Database::open(const std::string& path) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        close();
        return false;
    }
    exec("PRAGMA journal_mode=WAL");
    exec("PRAGMA synchronous=NORMAL");
    exec("PRAGMA foreign_keys=ON");
    return migrate();
}

void Database::close() {
    if (db_) { sqlite3_close(db_); db_ = nullptr; }
}

bool Database::exec(const char* sql) {
    return db_ && sqlite3_exec(db_, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

int64_t Database::lastInsertId() const {
    return db_ ? sqlite3_last_insert_rowid(db_) : -1;
}

bool Database::migrate() {
    if (!exec("CREATE TABLE IF NOT EXISTS schema_migrations("
              "version INTEGER PRIMARY KEY, applied_at INTEGER NOT NULL)"))
        return false;
    int applied = 0;
    {
        auto s = prepare("SELECT COALESCE(MAX(version),0) FROM schema_migrations");
        if (s.step()) applied = (int)s.colInt(0);
    }
    const int n = (int)(sizeof(kMigrations) / sizeof(kMigrations[0]));
    for (int v = applied + 1; v <= n; ++v) {
        if (!exec("BEGIN")) return false;
        if (!exec(kMigrations[v - 1])) { exec("ROLLBACK"); return false; }
        auto ins = prepare(
            "INSERT INTO schema_migrations(version, applied_at) VALUES(?,?)");
        ins.bind(1, (int64_t)v);
        ins.bind(2, (int64_t)::time(nullptr));
        if (!ins.step() && sqlite3_errcode(db_) != SQLITE_DONE) {
            ins.finalize(); exec("ROLLBACK"); return false;
        }
        ins.finalize();
        exec("COMMIT");
    }
    return true;
}

// ---------------------------------------------------------------- Stmt
bool Database::Stmt::prepare(sqlite3* db, const char* sql) {
    finalize();
    return db && sqlite3_prepare_v2(db, sql, -1, &s_, nullptr) == SQLITE_OK
           && s_ != nullptr;
}
void Database::Stmt::finalize() {
    if (s_) { sqlite3_finalize(s_); s_ = nullptr; }
}
bool Database::Stmt::bind(int i, int64_t v) {
    return s_ && sqlite3_bind_int64(s_, i, v) == SQLITE_OK;
}
bool Database::Stmt::bind(int i, double v) {
    return s_ && sqlite3_bind_double(s_, i, v) == SQLITE_OK;
}
bool Database::Stmt::bind(int i, const std::string& v) {
    return s_ && sqlite3_bind_text(s_, i, v.c_str(), -1,
                                   SQLITE_TRANSIENT) == SQLITE_OK;
}
bool Database::Stmt::bind(int i, const void* blob, int n) {
    return s_ && sqlite3_bind_blob(s_, i, blob, n, SQLITE_TRANSIENT) == SQLITE_OK;
}
bool Database::Stmt::bindNull(int i) {
    return s_ && sqlite3_bind_null(s_, i) == SQLITE_OK;
}
bool Database::Stmt::step() {
    return s_ && sqlite3_step(s_) == SQLITE_ROW;
}
int64_t Database::Stmt::colInt(int i) const {
    return s_ ? sqlite3_column_int64(s_, i) : 0;
}
double Database::Stmt::colDouble(int i) const {
    return s_ ? sqlite3_column_double(s_, i) : 0.0;
}
std::string Database::Stmt::colText(int i) const {
    const auto* p = s_ ? sqlite3_column_text(s_, i) : nullptr;
    return p ? (const char*)p : "";
}
std::vector<uint8_t> Database::Stmt::colBlob(int i) const {
    if (!s_) return {};
    const void* p = sqlite3_column_blob(s_, i);
    const int n = sqlite3_column_bytes(s_, i);
    if (!p || n <= 0) return {};
    const auto* b = (const uint8_t*)p;
    return std::vector<uint8_t>(b, b + n);
}
void Database::Stmt::reset() { if (s_) sqlite3_reset(s_); }

} // namespace onda
