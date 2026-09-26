#include "TrackRepository.h"

#include <ctime>
#include <filesystem>

namespace onda::library {

static int64_t now() { return (int64_t)std::time(nullptr); }

static const char* kCols =
    "t.id,t.path,t.filename,t.fingerprint,t.content_hash,t.file_size,"
    "t.mtime,t.status,t.title,t.artist,t.album,t.album_artist,t.genre,"
    "t.year,t.track_num,t.disc_num,t.duration_sec,t.bitrate,t.channels,"
    "t.sample_rate,t.format,t.artwork_hash,t.rating,t.color,t.energy,"
    "t.comments,t.play_count,t.last_played,t.date_added,"
    "t.analysis_state,t.analysis_version,"
    "a.bpm,a.bpm_confidence,a.key_index,a.key_confidence";

static const char* kFrom =
    " FROM tracks t LEFT JOIN analysis a ON a.track_id = t.id ";

// ------------------------------------------------------------------ write
int64_t TrackRepository::upsert(Track& t) {
    std::lock_guard lk(db_.mutex());
    Database::Tx tx(db_);
    const int64_t existing = idForPath(t.path);
    if (existing > 0) {
        auto u = db_.prepare(
            "UPDATE tracks SET fingerprint=?,content_hash=?,file_size=?,"
            "mtime=?,status=?,filename=?,title=?,artist=?,album=?,"
            "album_artist=?,genre=?,year=?,track_num=?,disc_num=?,"
            "duration_sec=?,bitrate=?,channels=?,sample_rate=?,format=?,"
            "artwork_hash=?,updated_at=? WHERE id=?");
        int i = 1;
        u.bind(i++, (int64_t)t.fingerprint); u.bind(i++, (int64_t)t.contentHash);
        u.bind(i++, t.fileSize); u.bind(i++, t.mtime);
        u.bind(i++, (int64_t)t.status);   u.bind(i++, t.filename);
        u.bind(i++, t.title);      u.bind(i++, t.artist);
        u.bind(i++, t.album);      u.bind(i++, t.albumArtist);
        u.bind(i++, t.genre);      u.bind(i++, (int64_t)t.year);
        u.bind(i++, (int64_t)t.trackNum); u.bind(i++, (int64_t)t.discNum);
        u.bind(i++, t.durationSec); u.bind(i++, (int64_t)t.bitrate);
        u.bind(i++, (int64_t)t.channels); u.bind(i++, t.sampleRate);
        u.bind(i++, t.format);     u.bind(i++, (int64_t)t.artworkHash);
        u.bind(i++, now());        u.bind(i++, existing);
        u.step();
        t.id = existing;
    } else {
        auto ins = db_.prepare(
            "INSERT INTO tracks(path,fingerprint,content_hash,file_size,"
            "mtime,status,filename,title,artist,album,album_artist,genre,"
            "year,track_num,disc_num,duration_sec,bitrate,channels,"
            "sample_rate,format,artwork_hash,rating,color,energy,comments,"
            "play_count,last_played,date_added,analysis_state,"
            "analysis_version,updated_at) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,"
            "?,?,?,?,?)");
        int i = 1;
        ins.bind(i++, t.path);
        ins.bind(i++, (int64_t)t.fingerprint);
        ins.bind(i++, (int64_t)t.contentHash);
        ins.bind(i++, t.fileSize); ins.bind(i++, t.mtime);
        ins.bind(i++, (int64_t)t.status); ins.bind(i++, t.filename);
        ins.bind(i++, t.title);     ins.bind(i++, t.artist);
        ins.bind(i++, t.album);     ins.bind(i++, t.albumArtist);
        ins.bind(i++, t.genre);     ins.bind(i++, (int64_t)t.year);
        ins.bind(i++, (int64_t)t.trackNum); ins.bind(i++, (int64_t)t.discNum);
        ins.bind(i++, t.durationSec); ins.bind(i++, (int64_t)t.bitrate);
        ins.bind(i++, (int64_t)t.channels); ins.bind(i++, t.sampleRate);
        ins.bind(i++, t.format);    ins.bind(i++, (int64_t)t.artworkHash);
        ins.bind(i++, (int64_t)t.rating); ins.bind(i++, (int64_t)t.color);
        ins.bind(i++, (int64_t)t.energy); ins.bind(i++, t.comments);
        ins.bind(i++, (int64_t)t.playCount); ins.bind(i++, t.lastPlayed);
        ins.bind(i++, t.dateAdded > 0 ? t.dateAdded : now());
        ins.bind(i++, (int64_t)t.analysisState);
        ins.bind(i++, (int64_t)t.analysisVersion);
        ins.bind(i++, now());
        if (!ins.step()) { /* DONE */ }
        t.id = db_.lastInsertId();
    }
    if (t.id <= 0) { tx.rollback(); return -1; }
    ftsUpdate(t.id);
    tx.commit();
    return t.id;
}

bool TrackRepository::remove(int64_t id) {
    std::lock_guard lk(db_.mutex());
    Database::Tx tx(db_);
    ftsRemove(id);
    auto s = db_.prepare("DELETE FROM tracks WHERE id=?");
    s.bind(1, id);
    s.step();
    tx.commit();
    return true;
}

bool TrackRepository::setStatus(int64_t id, TrackStatus s) {
    std::lock_guard lk(db_.mutex());
    auto st = db_.prepare("UPDATE tracks SET status=? WHERE id=?");
    st.bind(1, (int64_t)s); st.bind(2, id);
    st.step();
    return true;
}

bool TrackRepository::setPath(int64_t id, const std::string& newPath,
                              const std::string& newFilename) {
    std::lock_guard lk(db_.mutex());
    Database::Tx tx(db_);
    auto st = db_.prepare(
        "UPDATE tracks SET path=?,filename=?,status=?,updated_at=? WHERE id=?");
    st.bind(1, newPath); st.bind(2, newFilename);
    st.bind(3, (int64_t)TrackStatus::Available);
    st.bind(4, now());   st.bind(5, id);
    st.step();
    ftsUpdate(id);
    tx.commit();
    return true;
}

bool TrackRepository::updateFileIdentity(int64_t id, uint64_t fp, uint64_t ch,
                                         int64_t size, int64_t mtime) {
    std::lock_guard lk(db_.mutex());
    auto st = db_.prepare(
        "UPDATE tracks SET fingerprint=?,content_hash=?,file_size=?,mtime=?,"
        "updated_at=? WHERE id=?");
    st.bind(1, (int64_t)fp); st.bind(2, (int64_t)ch);
    st.bind(3, size); st.bind(4, mtime); st.bind(5, now()); st.bind(6, id);
    st.step();
    return true;
}

bool TrackRepository::setUserFields(int64_t id, int rating, int color,
                                    int energy, const std::string& comments) {
    std::lock_guard lk(db_.mutex());
    Database::Tx tx(db_);
    auto st = db_.prepare(
        "UPDATE tracks SET rating=?,color=?,energy=?,comments=?,updated_at=?"
        " WHERE id=?");
    st.bind(1, (int64_t)rating); st.bind(2, (int64_t)color);
    st.bind(3, (int64_t)energy); st.bind(4, comments);
    st.bind(5, now()); st.bind(6, id);
    st.step();
    ftsUpdate(id);                 // comments is an FTS column
    tx.commit();
    return true;
}

bool TrackRepository::bumpPlayStats(int64_t id, double playedSec,
                                    int64_t when, bool counted) {
    std::lock_guard lk(db_.mutex());
    auto st = db_.prepare(counted
        ? "UPDATE tracks SET play_count=play_count+1,last_played=? WHERE id=?"
        : "UPDATE tracks SET last_played=? WHERE id=?");
    st.bind(1, when); st.bind(2, id);
    st.step();
    return true;
}

// ------------------------------------------------------------------ read
Track TrackRepository::rowToTrack(Database::Stmt& s) {
    Track t;
    t.id           = s.colInt(0);
    t.path         = s.colText(1);
    t.filename     = s.colText(2);
    t.fingerprint  = (uint64_t)s.colInt(3);
    t.contentHash  = (uint64_t)s.colInt(4);
    t.fileSize     = s.colInt(5);
    t.mtime        = s.colInt(6);
    t.status       = (int)s.colInt(7);
    t.title        = s.colText(8);
    t.artist       = s.colText(9);
    t.album        = s.colText(10);
    t.albumArtist  = s.colText(11);
    t.genre        = s.colText(12);
    t.year         = (int)s.colInt(13);
    t.trackNum     = (int)s.colInt(14);
    t.discNum      = (int)s.colInt(15);
    t.durationSec  = s.colDouble(16);
    t.bitrate      = (int)s.colInt(17);
    t.channels     = (int)s.colInt(18);
    t.sampleRate   = s.colDouble(19);
    t.format       = s.colText(20);
    t.artworkHash  = (uint64_t)s.colInt(21);
    t.rating       = (int)s.colInt(22);
    t.color        = (int)s.colInt(23);
    t.energy       = (int)s.colInt(24);
    t.comments     = s.colText(25);
    t.playCount    = (int)s.colInt(26);
    t.lastPlayed   = s.colInt(27);
    t.dateAdded    = s.colInt(28);
    t.analysisState   = (int)s.colInt(29);
    t.analysisVersion = (int)s.colInt(30);
    t.bpm          = s.colDouble(31);
    t.bpmConfidence = (float)s.colDouble(32);
    t.keyIndex     = (int)s.colInt(33);
    t.keyConfidence = (float)s.colDouble(34);
    return t;
}

bool TrackRepository::getById(int64_t id, Track& out) {
    std::lock_guard lk(db_.mutex());
    auto s = db_.prepare((std::string("SELECT ") + kCols + kFrom +
                          "WHERE t.id=?").c_str());
    s.bind(1, id);
    if (!s.step()) return false;
    out = rowToTrack(s);
    out.tags = tagsOf(id);
    return true;
}

bool TrackRepository::getByPath(const std::string& path, Track& out) {
    const int64_t id = idForPath(path);
    return id > 0 && getById(id, out);
}

int64_t TrackRepository::idForPath(const std::string& path) {
    std::lock_guard lk(db_.mutex());
    auto s = db_.prepare("SELECT id FROM tracks WHERE path=?");
    s.bind(1, path);
    return s.step() ? s.colInt(0) : -1;
}

std::vector<int64_t> TrackRepository::idsByContentHash(uint64_t ch) {
    std::lock_guard lk(db_.mutex());
    auto s = db_.prepare("SELECT id FROM tracks WHERE content_hash=?");
    s.bind(1, (int64_t)ch);
    std::vector<int64_t> out;
    while (s.step()) out.push_back(s.colInt(0));
    return out;
}

std::vector<Track> TrackRepository::byIds(const std::vector<int64_t>& ids) {
    std::vector<Track> out;
    out.reserve(ids.size());
    for (int64_t id : ids) { Track t; if (getById(id, t)) out.push_back(std::move(t)); }
    return out;
}

// ------------------------------------------------------------------ query
const char* TrackRepository::orderColumn(SortKey k) {
    switch (k) {
        case SortKey::Title:      return "t.title COLLATE NOCASE";
        case SortKey::Artist:     return "t.artist COLLATE NOCASE";
        case SortKey::Album:      return "t.album COLLATE NOCASE";
        case SortKey::Bpm:        return "a.bpm";
        case SortKey::Key:        return "a.key_index";
        case SortKey::Duration:   return "t.duration_sec";
        case SortKey::Rating:     return "t.rating";
        case SortKey::DateAdded:  return "t.date_added";
        case SortKey::PlayCount:  return "t.play_count";
        case SortKey::LastPlayed: return "t.last_played";
        case SortKey::Year:       return "t.year";
        case SortKey::Genre:      return "t.genre COLLATE NOCASE";
        case SortKey::Path:       return "t.path";
    }
    return "t.artist COLLATE NOCASE";
}

std::string TrackRepository::fullWhere(const QuerySpec& q,
                                       std::vector<std::string>& binds) {
    std::string w = FilterEngine::where(q.filters, binds);
    const std::string m = SearchEngine::matchExpr(q.search);
    if (!m.empty()) {
        binds.push_back(m);
        const std::string fts =
            "t.id IN (SELECT rowid FROM tracks_fts WHERE tracks_fts MATCH ?)";
        w = w.empty() ? fts : w + " AND " + fts;
    }
    return w;
}

int TrackRepository::countWhere(const std::string& where,
                                const std::vector<std::string>& binds) {
    std::lock_guard lk(db_.mutex());
    std::string sql = "SELECT COUNT(*)" + std::string(kFrom);
    if (!where.empty()) sql += "WHERE " + where;
    auto s = db_.prepare(sql.c_str());
    for (size_t i = 0; i < binds.size(); ++i) s.bind((int)i + 1, binds[i]);
    return s.step() ? (int)s.colInt(0) : 0;
}

TrackPage TrackRepository::query(const QuerySpec& q) {
    std::vector<std::string> binds;
    const std::string w = fullWhere(q, binds);
    TrackPage page;
    page.total = countWhere(w, binds);
    std::lock_guard lk(db_.mutex());
    std::string sql = std::string("SELECT ") + kCols + kFrom;
    if (!w.empty()) sql += "WHERE " + w;
    sql += " ORDER BY ";
    sql += orderColumn(q.sort);
    sql += q.ascending ? " ASC, t.id ASC" : " DESC, t.id ASC";
    sql += " LIMIT ? OFFSET ?";
    auto s = db_.prepare(sql.c_str());
    int bi = 1;
    for (auto& b : binds) s.bind(bi++, b);
    s.bind(bi++, (int64_t)q.limit);
    s.bind(bi++, (int64_t)q.offset);
    while (s.step()) page.items.push_back(rowToTrack(s));
    // tag fetch is O(1)-indexed per row — only for the returned page
    for (auto& t : page.items) t.tags = tagsOf(t.id);
    return page;
}

// ------------------------------------------------------------------ tags
int64_t TrackRepository::tagId(const std::string& name) {
    {
        auto s = db_.prepare("SELECT id FROM tags WHERE name=?");
        s.bind(1, name);
        if (s.step()) return s.colInt(0);
    }
    auto ins = db_.prepare("INSERT OR IGNORE INTO tags(name) VALUES(?)");
    ins.bind(1, name);
    ins.step();
    auto s = db_.prepare("SELECT id FROM tags WHERE name=?");
    s.bind(1, name);
    return s.step() ? s.colInt(0) : -1;
}

bool TrackRepository::addTag(int64_t trackId, const std::string& name) {
    std::lock_guard lk(db_.mutex());
    Database::Tx tx(db_);
    const int64_t tid = tagId(name);
    if (tid < 0) { tx.rollback(); return false; }
    auto s = db_.prepare(
        "INSERT OR IGNORE INTO track_tags(track_id,tag_id) VALUES(?,?)");
    s.bind(1, trackId); s.bind(2, tid);
    s.step();
    ftsUpdate(trackId);
    tx.commit();
    return true;
}

bool TrackRepository::removeTag(int64_t trackId, const std::string& name) {
    std::lock_guard lk(db_.mutex());
    Database::Tx tx(db_);
    auto s = db_.prepare(
        "DELETE FROM track_tags WHERE track_id=? AND tag_id IN "
        "(SELECT id FROM tags WHERE name=?)");
    s.bind(1, trackId); s.bind(2, name);
    s.step();
    ftsUpdate(trackId);
    tx.commit();
    return true;
}

bool TrackRepository::setTags(int64_t trackId,
                              const std::vector<std::string>& tags) {
    std::lock_guard lk(db_.mutex());
    Database::Tx tx(db_);
    {
        auto d = db_.prepare("DELETE FROM track_tags WHERE track_id=?");
        d.bind(1, trackId);
        d.step();
    }
    for (const auto& name : tags) {
        const int64_t tid = tagId(name);
        if (tid < 0) continue;
        auto s = db_.prepare(
            "INSERT OR IGNORE INTO track_tags(track_id,tag_id) VALUES(?,?)");
        s.bind(1, trackId); s.bind(2, tid);
        s.step();
    }
    ftsUpdate(trackId);
    tx.commit();
    return true;
}

std::vector<std::string> TrackRepository::tagsOf(int64_t trackId) {
    std::lock_guard lk(db_.mutex());
    auto s = db_.prepare(
        "SELECT g.name FROM tags g JOIN track_tags tt ON tt.tag_id=g.id "
        "WHERE tt.track_id=? ORDER BY g.name COLLATE NOCASE");
    s.bind(1, trackId);
    std::vector<std::string> out;
    while (s.step()) out.push_back(s.colText(0));
    return out;
}

std::vector<std::string> TrackRepository::allTags() {
    std::lock_guard lk(db_.mutex());
    auto s = db_.prepare("SELECT name FROM tags ORDER BY name COLLATE NOCASE");
    std::vector<std::string> out;
    while (s.step()) out.push_back(s.colText(0));
    return out;
}

// ------------------------------------------------------------------ FTS
std::string TrackRepository::tagsText(int64_t id) {
    std::string out;
    for (auto& t : tagsOf(id)) { if (!out.empty()) out += ' '; out += t; }
    return out;
}

void TrackRepository::ftsRemove(int64_t id) {
    auto s = db_.prepare("DELETE FROM tracks_fts WHERE rowid=?");
    s.bind(1, id);
    s.step();
}

void TrackRepository::ftsUpdate(int64_t id) {
    ftsRemove(id);
    Track t;
    if (!getById(id, t)) return;
    auto s = db_.prepare(
        "INSERT INTO tracks_fts(rowid,title,artist,album,genre,filename,"
        "tags,comments) VALUES(?,?,?,?,?,?,?,?)");
    s.bind(1, id);
    s.bind(2, t.title);   s.bind(3, t.artist);
    s.bind(4, t.album);   s.bind(5, t.genre);
    s.bind(6, t.filename); s.bind(7, tagsText(id));
    s.bind(8, t.comments);
    s.step();
}

// ------------------------------------------------------------------ integrity
bool TrackRepository::integrityCheck(std::string& detail) {
    std::lock_guard lk(db_.mutex());
    bool ok = true;
    {
        auto s = db_.prepare("PRAGMA integrity_check");
        while (s.step()) {
            const std::string r = s.colText(0);
            if (r != "ok") { ok = false; detail += r + "; "; }
        }
    }
    {
        auto s = db_.prepare("PRAGMA foreign_key_check");
        while (s.step()) {
            ok = false;
            detail += "fk violation in " + s.colText(0) + "; ";
        }
    }
    if (ok && detail.empty()) detail = "ok";
    return ok;
}

int TrackRepository::purgeOrphanTags() {
    std::lock_guard lk(db_.mutex());
    auto s = db_.prepare(
        "DELETE FROM tags WHERE id NOT IN (SELECT DISTINCT tag_id FROM track_tags)");
    s.step();
    return 0;
}

} // namespace onda::library
