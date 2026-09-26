#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "TrackRepository.h"

namespace onda::library {

/// §23 — named tag surface over the repository's tag tables.
/// Global tag list with counts, rename (colliding name merges),
/// delete, merge, bulk add/remove. Track-side operations delegate to
/// TrackRepository (which keeps the FTS tags column in sync).
class TagManager {
public:
    explicit TagManager(TrackRepository& repo) : repo_(repo) {}

    struct TagInfo { std::string name; int count { 0 }; };

    /// Global tag list with per-tag track counts.
    std::vector<TagInfo> tagsWithCounts() {
        std::lock_guard lk(repo_.db().mutex());
        auto s = repo_.db().prepare(
            "SELECT g.name, COUNT(tt.track_id) FROM tags g"
            " LEFT JOIN track_tags tt ON tt.tag_id=g.id"
            " GROUP BY g.id ORDER BY g.name COLLATE NOCASE");
        std::vector<TagInfo> out;
        while (s.step()) out.push_back({ s.colText(0), (int)s.colInt(1) });
        return out;
    }

    std::vector<std::string> all() { return repo_.allTags(); }

    // ---- track-side ----
    bool add(int64_t trackId, const std::string& name) {
        return repo_.addTag(trackId, name);
    }
    bool remove(int64_t trackId, const std::string& name) {
        return repo_.removeTag(trackId, name);
    }
    bool set(int64_t trackId, const std::vector<std::string>& tags) {
        return repo_.setTags(trackId, tags);
    }
    std::vector<std::string> of(int64_t trackId) {
        return repo_.tagsOf(trackId);
    }
    /// Bulk: tag several tracks in one pass.
    int addToTracks(const std::vector<int64_t>& ids,
                    const std::string& name) {
        int n = 0;
        for (int64_t id : ids) n += repo_.addTag(id, name) ? 1 : 0;
        return n;
    }

    // ---- tag-side (global) ----
    /// Rename old→new. If `newName` already exists (NOCASE) the tags
    /// merge: memberships re-point to the existing tag and the old row
    /// is deleted.
    bool rename(const std::string& oldName, const std::string& newName) {
        std::lock_guard lk(repo_.db().mutex());
        const int64_t oldId = idOf(oldName);
        if (oldId < 0) return false;
        const int64_t newId = idOf(newName);
        Database::Tx tx(repo_.db());
        if (newId >= 0 && newId != oldId) {
            repoint(oldId, newId);
            deleteRow(oldId);
        } else {
            auto s = repo_.db().prepare(
                "UPDATE tags SET name=? WHERE id=?");
            s.bind(1, newName); s.bind(2, oldId);
            s.step();
        }
        tx.commit();
        refreshFtsForTag(oldId);
        refreshFtsForTag(newId);
        return true;
    }

    bool remove(const std::string& name) {
        std::lock_guard lk(repo_.db().mutex());
        const int64_t id = idOf(name);
        if (id < 0) return false;
        const auto tids = trackIdsFor(id);
        Database::Tx tx(repo_.db());
        deleteRow(id);
        tx.commit();
        refreshFtsFor(tids);
        return true;
    }

    /// src memberships move to dst (created when missing); src row
    /// is deleted.
    bool merge(const std::string& src, const std::string& dst) {
        std::lock_guard lk(repo_.db().mutex());
        const int64_t srcId = idOf(src);
        if (srcId < 0 || src == dst) return false;
        Database::Tx tx(repo_.db());
        const int64_t dstId = repo_.tagId(dst);
        if (dstId < 0) { tx.rollback(); return false; }
        repoint(srcId, dstId);
        deleteRow(srcId);
        tx.commit();
        refreshFtsForTag(dstId);
        return true;
    }

private:
    int64_t idOf(const std::string& name) {
        auto s = repo_.db().prepare("SELECT id FROM tags WHERE name=?");
        s.bind(1, name);
        return s.step() ? s.colInt(0) : -1;
    }

    std::vector<int64_t> trackIdsFor(int64_t tagId) {
        auto s = repo_.db().prepare(
            "SELECT track_id FROM track_tags WHERE tag_id=?");
        s.bind(1, tagId);
        std::vector<int64_t> out;
        while (s.step()) out.push_back(s.colInt(0));
        return out;
    }

    void repoint(int64_t from, int64_t to) {
        auto s = repo_.db().prepare(
            "UPDATE OR IGNORE track_tags SET tag_id=? WHERE tag_id=?");
        s.bind(1, to); s.bind(2, from);
        s.step();
        // pairs that now collide (track had both tags) must go
        auto d = repo_.db().prepare(
            "DELETE FROM track_tags WHERE tag_id=?");
        d.bind(1, from);
        d.step();
    }

    void deleteRow(int64_t id) {
        auto s = repo_.db().prepare("DELETE FROM tags WHERE id=?");
        s.bind(1, id);
        s.step();
    }

    /// Tag text lives in the FTS index — refresh affected tracks.
    void refreshFtsForTag(int64_t tagId) {
        if (tagId < 0) return;
        refreshFtsFor(trackIdsFor(tagId));
    }
    void refreshFtsFor(const std::vector<int64_t>& tids) {
        for (int64_t tid : tids) repo_.refreshIndex(tid);
    }

    TrackRepository& repo_;
};

} // namespace onda::library
