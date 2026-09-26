#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../data/Database.h"
#include "LibraryTypes.h"
#include "FilterEngine.h"
#include "SearchEngine.h"

namespace onda::library {

/// Track CRUD + indexed queries. Every method locks the shared
/// Database mutex — safe to call from UI/worker threads concurrently
/// with the library worker. Never called from the audio thread.
///
/// Query shape:
///   tracks t LEFT JOIN analysis a
///   [AND t.id IN (SELECT rowid FROM tracks_fts WHERE MATCH ?)]
///   [AND (filter clauses)]
///   ORDER BY <whitelisted column> LIMIT/OFFSET
/// A separate COUNT(*) with the same WHERE feeds TrackPage.total.
class TrackRepository {
public:
    explicit TrackRepository(Database& db) : db_(db) {}

    // ---- write ----
    /// Insert or update by unique path. Maintains the FTS row.
    /// Returns the track id (<0 on failure).
    int64_t upsert(Track& t);
    bool remove(int64_t id);
    bool setStatus(int64_t id, TrackStatus s);
    bool setPath(int64_t id, const std::string& newPath,
                 const std::string& newFilename);
    bool updateFileIdentity(int64_t id, uint64_t fp, uint64_t ch,
                            int64_t size, int64_t mtime);
    bool setUserFields(int64_t id, int rating, int color, int energy,
                       const std::string& comments);
    bool bumpPlayStats(int64_t id, double playedSec, int64_t when,
                       bool counted);

    // ---- read ----
    bool getById(int64_t id, Track& out);
    bool getByPath(const std::string& path, Track& out);
    int64_t idForPath(const std::string& path);
    std::vector<int64_t> idsByContentHash(uint64_t ch);
    std::vector<Track> byIds(const std::vector<int64_t>& ids);
    TrackPage query(const QuerySpec& q);
    int  countWhere(const std::string& where,
                    const std::vector<std::string>& binds = {});

    // ---- tags ----
    int64_t tagId(const std::string& name);          // creates if needed
    bool addTag(int64_t trackId, const std::string& name);
    bool removeTag(int64_t trackId, const std::string& name);
    bool setTags(int64_t trackId, const std::vector<std::string>& tags);
    std::vector<std::string> tagsOf(int64_t trackId);
    std::vector<std::string> allTags();

    // ---- integrity ----
    bool integrityCheck(std::string& detail);
    int  purgeOrphanTags();   // tags no track references

    Database& db() { return db_; }

private:
    Track rowToTrack(Database::Stmt& s);
    void ftsUpdate(int64_t id);
    void ftsRemove(int64_t id);
    std::string tagsText(int64_t id);
    static const char* orderColumn(SortKey k);
    static std::string fullWhere(const QuerySpec& q,
                                 std::vector<std::string>& binds);

    Database& db_;
};

} // namespace onda::library
