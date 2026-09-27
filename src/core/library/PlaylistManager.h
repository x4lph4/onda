#pragma once

#include <string>
#include <vector>

#include "LibraryTypes.h"
#include "TrackRepository.h"

namespace onda::library {

/// Playlists + virtual folders (crates).
///
///   Folder (tree, parent_id)
///     └── Playlist (static: explicit position order | smart: rules)
///           └── playlist_items (playlist_id, position, track_id)
///
/// A track can live in any number of playlists; rows, never files.
/// Ordering is an explicit `position` column — independent of any
/// other ordering in the DB. Reorders run inside one transaction.
///
/// Smart playlists store a FilterSpec (JSON serialization is handled
/// by the engine, which owns the rule codec) and evaluate it live
/// via the repository query — no items are materialized.
class PlaylistManager {
public:
    explicit PlaylistManager(TrackRepository& repo) : repo_(repo) {}

    // ---- folders ----
    int64_t createFolder(const std::string& name, int64_t parentId = 0);
    bool renameFolder(int64_t id, const std::string& name);
    bool deleteFolder(int64_t id);               // children get parent=null
    std::vector<FolderInfo> folders(int64_t parentId = -1);

    // ---- playlists ----
    int64_t create(const std::string& name, int64_t folderId = 0);
    int64_t createSmart(const std::string& name, const std::string& rulesJson,
                        int64_t folderId = 0);
    bool rename(int64_t id, const std::string& name);
    bool remove(int64_t id);
    int64_t duplicate(int64_t id, const std::string& name);
    bool setRules(int64_t id, const std::string& rulesJson);
    /// §29 — presentation fields (description/color/sortMode). All
    /// optional — pass nullptr to keep a field unchanged.
    bool setMeta(int64_t id, const std::string* description,
                 const int* color, const int* sortMode);
    PlaylistInfo info(int64_t id);
    std::vector<PlaylistInfo> playlists(int64_t folderId = -1);

    // ---- items (static playlists) ----
    bool addTrack(int64_t playlistId, int64_t trackId);
    bool addTracks(int64_t playlistId, const std::vector<int64_t>& ids);
    bool removeTrack(int64_t playlistId, int64_t trackId);
    /// Move the item at `fromPos` to `toPos` (0-based explicit order).
    bool moveTrack(int64_t playlistId, int fromPos, int toPos);
    /// Replace the whole order in one transaction.
    bool setOrder(int64_t playlistId, const std::vector<int64_t>& orderedIds);
    std::vector<int64_t> trackIds(int64_t playlistId);
    TrackPage tracks(int64_t playlistId, int offset = 0, int limit = 200);
    /// §30 — membership viewed through the playlist's stored
    /// sort_mode (or `q.sort` when the caller overrides). Stored
    /// positions are never rewritten by sorting.
    TrackPage tracksView(int64_t playlistId, const QuerySpec& q);
    /// §35 — materialize a smart playlist's CURRENT result into a
    /// new static playlist. The source smart playlist and the
    /// collection are never modified; the copy is a plain static
    /// playlist from then on.
    int64_t snapshotSmart(int64_t smartId, const std::string& name);

    /// Shared DB handle for sibling managers (CrateManager depth
    /// walk). Still guarded by the same recursive mutex.
    Database& db() { return repo_.db(); }

private:
    TrackRepository& repo_;
};

} // namespace onda::library
