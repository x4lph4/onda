#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "PlaylistManager.h"

namespace onda::library {

/// §16-17 — crates are *manual collections* implemented as static
/// playlists; crate folders nest them (Crate → Subcrate) up to a
/// fixed depth. Deleting a crate removes membership rows only —
/// never audio files or track rows.
///
/// CrateManager is a thin named surface over PlaylistManager so the
/// library UI/controller layer addresses crates in their own terms.
class CrateManager {
public:
    static constexpr int kMaxDepth = 8;   // nested crate folders

    explicit CrateManager(PlaylistManager& pl) : pl_(pl) {}

    // ---- crates (manual, ordered collections) ----
    int64_t createCrate(const std::string& name,
                        int64_t parentFolderId = 0) {
        return pl_.create(name, parentFolderId);
    }
    bool renameCrate(int64_t id, const std::string& name) {
        return pl_.rename(id, name);
    }
    /// Removes the crate + its membership rows. Audio files are
    /// untouched (§16).
    bool deleteCrate(int64_t id) { return pl_.remove(id); }
    int64_t duplicateCrate(int64_t id, const std::string& name) {
        return pl_.duplicate(id, name);
    }

    bool addTrack(int64_t crateId, int64_t trackId) {
        return pl_.addTrack(crateId, trackId);
    }
    bool addTracks(int64_t crateId, const std::vector<int64_t>& ids) {
        return pl_.addTracks(crateId, ids);
    }
    bool removeTrack(int64_t crateId, int64_t trackId) {
        return pl_.removeTrack(crateId, trackId);
    }
    /// Explicit ordering (§19) — crates keep position like playlists.
    bool moveTrack(int64_t crateId, int from, int to) {
        return pl_.moveTrack(crateId, from, to);
    }
    bool setOrder(int64_t crateId, const std::vector<int64_t>& ids) {
        return pl_.setOrder(crateId, ids);
    }
    std::vector<int64_t> trackIds(int64_t crateId) {
        return pl_.trackIds(crateId);
    }
    TrackPage tracks(int64_t crateId, int offset = 0, int limit = 200) {
        return pl_.tracks(crateId, offset, limit);
    }
    std::vector<PlaylistInfo> crates(int64_t folderId = -1) {
        return pl_.playlists(folderId);
    }

    // ---- nesting (§17) ----
    int64_t createCrateFolder(const std::string& name,
                              int64_t parentFolderId = 0) {
        if (depth(parentFolderId) >= kMaxDepth) return -1;
        return pl_.createFolder(name, parentFolderId);
    }
    bool renameCrateFolder(int64_t id, const std::string& name) {
        return pl_.renameFolder(id, name);
    }
    /// Children are re-parented up one level (existing behaviour) —
    /// contents are never dropped silently.
    bool deleteCrateFolder(int64_t id) { return pl_.deleteFolder(id); }
    std::vector<FolderInfo> crateFolders(int64_t parentId = -1) {
        return pl_.folders(parentId);
    }

    /// Nesting depth of a folder id (0 = root). Stops at kMaxDepth+1.
    int depth(int64_t folderId) {
        int d = 0;
        int64_t cur = folderId;
        std::lock_guard lk(pl_.db().mutex());
        while (cur > 0 && d <= kMaxDepth + 1) {
            auto s = pl_.db().prepare(
                "SELECT parent_id FROM folders WHERE id=?");
            s.bind(1, cur);
            if (!s.step()) break;
            cur = s.colInt(0);
            s.finalize();
            ++d;
        }
        return d;
    }

private:
    PlaylistManager& pl_;
};

} // namespace onda::library
