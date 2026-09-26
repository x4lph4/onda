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

    // v2 — performance data (cues, saved loops, session key-values)
    R"SQL(
CREATE TABLE IF NOT EXISTS cues(
    track_id INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,
    slot INTEGER NOT NULL,
    type INTEGER NOT NULL DEFAULT 0,       -- CueType
    frame INTEGER NOT NULL,
    frame2 INTEGER NOT NULL DEFAULT -1,    -- loop-out for loop cues
    beat REAL NOT NULL DEFAULT -1,
    beat2 REAL NOT NULL DEFAULT -1,
    argb INTEGER NOT NULL DEFAULT 0,
    label TEXT,
    created_at INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY(track_id, slot, type));

CREATE TABLE IF NOT EXISTS saved_loops(
    track_id INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,
    slot INTEGER NOT NULL,
    in_frame INTEGER NOT NULL,
    out_frame INTEGER NOT NULL,
    in_beat REAL NOT NULL DEFAULT -1,
    out_beat REAL NOT NULL DEFAULT -1,
    argb INTEGER NOT NULL DEFAULT 0,
    label TEXT,
    PRIMARY KEY(track_id, slot));

CREATE TABLE IF NOT EXISTS kv(
    key TEXT PRIMARY KEY,
    value TEXT);
)SQL",

    // v3 — professional library: rich track metadata, user data,
    // tags, playlists/crates, smart playlists, sessions/history,
    // missing-file status, FTS index, query indexes
    R"SQL(
ALTER TABLE tracks ADD COLUMN filename TEXT;
ALTER TABLE tracks ADD COLUMN album TEXT;
ALTER TABLE tracks ADD COLUMN album_artist TEXT;
ALTER TABLE tracks ADD COLUMN genre TEXT;
ALTER TABLE tracks ADD COLUMN year INTEGER NOT NULL DEFAULT 0;
ALTER TABLE tracks ADD COLUMN track_num INTEGER NOT NULL DEFAULT 0;
ALTER TABLE tracks ADD COLUMN disc_num INTEGER NOT NULL DEFAULT 0;
ALTER TABLE tracks ADD COLUMN bitrate INTEGER NOT NULL DEFAULT 0;
ALTER TABLE tracks ADD COLUMN channels INTEGER NOT NULL DEFAULT 0;
ALTER TABLE tracks ADD COLUMN format TEXT;
ALTER TABLE tracks ADD COLUMN file_size INTEGER NOT NULL DEFAULT 0;
ALTER TABLE tracks ADD COLUMN mtime INTEGER NOT NULL DEFAULT 0;
ALTER TABLE tracks ADD COLUMN content_hash INTEGER NOT NULL DEFAULT 0;
ALTER TABLE tracks ADD COLUMN artwork_hash INTEGER NOT NULL DEFAULT 0;
ALTER TABLE tracks ADD COLUMN rating INTEGER NOT NULL DEFAULT 0;
ALTER TABLE tracks ADD COLUMN color INTEGER NOT NULL DEFAULT 0;
ALTER TABLE tracks ADD COLUMN energy INTEGER NOT NULL DEFAULT 0;
ALTER TABLE tracks ADD COLUMN comments TEXT;
ALTER TABLE tracks ADD COLUMN play_count INTEGER NOT NULL DEFAULT 0;
ALTER TABLE tracks ADD COLUMN last_played INTEGER NOT NULL DEFAULT 0;
ALTER TABLE tracks ADD COLUMN date_added INTEGER NOT NULL DEFAULT 0;
ALTER TABLE tracks ADD COLUMN status INTEGER NOT NULL DEFAULT 0;

CREATE TABLE IF NOT EXISTS tags(
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL COLLATE NOCASE UNIQUE);

CREATE TABLE IF NOT EXISTS track_tags(
    track_id INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,
    tag_id INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE,
    PRIMARY KEY(track_id, tag_id));

CREATE TABLE IF NOT EXISTS folders(
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    parent_id INTEGER REFERENCES folders(id) ON DELETE CASCADE,
    created_at INTEGER NOT NULL DEFAULT 0);

CREATE TABLE IF NOT EXISTS playlists(
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    folder_id INTEGER REFERENCES folders(id) ON DELETE SET NULL,
    kind INTEGER NOT NULL DEFAULT 0,      -- 0 static, 1 smart
    rules TEXT,                           -- smart: FilterSpec JSON
    created_at INTEGER NOT NULL DEFAULT 0,
    updated_at INTEGER NOT NULL DEFAULT 0);

CREATE TABLE IF NOT EXISTS playlist_items(
    playlist_id INTEGER NOT NULL REFERENCES playlists(id) ON DELETE CASCADE,
    position INTEGER NOT NULL,
    track_id INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,
    added_at INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY(playlist_id, track_id));
CREATE INDEX IF NOT EXISTS idx_pl_items ON playlist_items(playlist_id, position);

CREATE TABLE IF NOT EXISTS sessions(
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT,
    started_at INTEGER NOT NULL DEFAULT 0,
    ended_at INTEGER NOT NULL DEFAULT 0);

CREATE TABLE IF NOT EXISTS history(
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    track_id INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,
    session_id INTEGER REFERENCES sessions(id) ON DELETE SET NULL,
    deck INTEGER NOT NULL DEFAULT 0,
    started_at INTEGER NOT NULL DEFAULT 0,
    played_sec REAL NOT NULL DEFAULT 0,
    counted INTEGER NOT NULL DEFAULT 0);
CREATE INDEX IF NOT EXISTS idx_history_track ON history(track_id);
CREATE INDEX IF NOT EXISTS idx_history_session ON history(session_id);

CREATE VIRTUAL TABLE IF NOT EXISTS tracks_fts USING fts5(
    title, artist, album, genre, filename, tags, comments,
    content='');

CREATE INDEX IF NOT EXISTS idx_tracks_title ON tracks(title);
CREATE INDEX IF NOT EXISTS idx_tracks_artist ON tracks(artist);
CREATE INDEX IF NOT EXISTS idx_tracks_genre ON tracks(genre);
CREATE INDEX IF NOT EXISTS idx_tracks_rating ON tracks(rating);
CREATE INDEX IF NOT EXISTS idx_tracks_added ON tracks(date_added);
CREATE INDEX IF NOT EXISTS idx_tracks_status ON tracks(status);
CREATE INDEX IF NOT EXISTS idx_tracks_hash ON tracks(content_hash);
CREATE INDEX IF NOT EXISTS idx_analysis_bpm ON analysis(bpm);
CREATE INDEX IF NOT EXISTS idx_analysis_key ON analysis(key_index);
)SQL",

    // v4 — Phase 14 analysis deepening: loudness + transients +
    // component status + review flag (compact columns, not per-row
    // beats/transients — see docs/analysis/music-analysis-engine.md)
    R"SQL(
ALTER TABLE analysis ADD COLUMN peak REAL NOT NULL DEFAULT 0;
ALTER TABLE analysis ADD COLUMN rms REAL NOT NULL DEFAULT 0;
ALTER TABLE analysis ADD COLUMN lufs_i REAL NOT NULL DEFAULT -70;
ALTER TABLE analysis ADD COLUMN lra REAL NOT NULL DEFAULT 0;
ALTER TABLE analysis ADD COLUMN transients TEXT;   -- JSON [[frame,strength],…]
ALTER TABLE analysis ADD COLUMN comp_status INTEGER NOT NULL DEFAULT 0;
ALTER TABLE analysis ADD COLUMN needs_review INTEGER NOT NULL DEFAULT 0;
ALTER TABLE waveforms ADD COLUMN mid BLOB;         -- 1024-frame pyramid level
ALTER TABLE user_overrides ADD COLUMN grid_json TEXT;    -- edited grid (detected kept)
ALTER TABLE user_overrides ADD COLUMN downbeat_offset INTEGER;
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
