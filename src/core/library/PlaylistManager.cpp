#include "PlaylistManager.h"

#include <ctime>

namespace onda::library {

static int64_t now() { return (int64_t)std::time(nullptr); }

// ------------------------------------------------------------- folders
int64_t PlaylistManager::createFolder(const std::string& name,
                                      int64_t parentId) {
    std::lock_guard lk(repo_.db().mutex());
    auto s = repo_.db().prepare(
        "INSERT INTO folders(name,parent_id,created_at) VALUES(?,?,?)");
    s.bind(1, name);
    if (parentId > 0) s.bind(2, parentId); else s.bindNull(2);
    s.bind(3, now());
    s.step();
    return repo_.db().lastInsertId();
}

bool PlaylistManager::renameFolder(int64_t id, const std::string& name) {
    std::lock_guard lk(repo_.db().mutex());
    auto s = repo_.db().prepare("UPDATE folders SET name=? WHERE id=?");
    s.bind(1, name); s.bind(2, id);
    s.step();
    return true;
}

bool PlaylistManager::deleteFolder(int64_t id) {
    std::lock_guard lk(repo_.db().mutex());
    Database::Tx tx(repo_.db());
    {
        auto s = repo_.db().prepare(
            "UPDATE playlists SET folder_id=NULL WHERE folder_id=?");
        s.bind(1, id); s.step();
    }
    {
        auto s = repo_.db().prepare(
            "UPDATE folders SET parent_id=NULL WHERE parent_id=?");
        s.bind(1, id); s.step();
    }
    {
        auto s = repo_.db().prepare("DELETE FROM folders WHERE id=?");
        s.bind(1, id); s.step();
    }
    tx.commit();
    return true;
}

std::vector<FolderInfo> PlaylistManager::folders(int64_t parentId) {
    std::lock_guard lk(repo_.db().mutex());
    auto s = repo_.db().prepare(parentId < 0
        ? "SELECT id,name,parent_id FROM folders ORDER BY name COLLATE NOCASE"
        : "SELECT id,name,parent_id FROM folders WHERE parent_id IS ? "
          "ORDER BY name COLLATE NOCASE");
    if (parentId >= 0) { if (parentId == 0) s.bindNull(1); else s.bind(1, parentId); }
    std::vector<FolderInfo> out;
    while (s.step()) {
        FolderInfo f;
        f.id = s.colInt(0); f.name = s.colText(1); f.parentId = s.colInt(2);
        out.push_back(std::move(f));
    }
    return out;
}

// ------------------------------------------------------------ playlists
int64_t PlaylistManager::create(const std::string& name, int64_t folderId) {
    std::lock_guard lk(repo_.db().mutex());
    auto s = repo_.db().prepare(
        "INSERT INTO playlists(name,folder_id,kind,created_at,updated_at)"
        " VALUES(?,?,0,?,?)");
    s.bind(1, name);
    if (folderId > 0) s.bind(2, folderId); else s.bindNull(2);
    s.bind(3, now()); s.bind(4, now());
    s.step();
    return repo_.db().lastInsertId();
}

int64_t PlaylistManager::createSmart(const std::string& name,
                                     const std::string& rulesJson,
                                     int64_t folderId) {
    std::lock_guard lk(repo_.db().mutex());
    auto s = repo_.db().prepare(
        "INSERT INTO playlists(name,folder_id,kind,rules,created_at,"
        "updated_at) VALUES(?,?,1,?,?,?)");
    s.bind(1, name);
    if (folderId > 0) s.bind(2, folderId); else s.bindNull(2);
    s.bind(3, rulesJson); s.bind(4, now()); s.bind(5, now());
    s.step();
    return repo_.db().lastInsertId();
}

bool PlaylistManager::rename(int64_t id, const std::string& name) {
    std::lock_guard lk(repo_.db().mutex());
    auto s = repo_.db().prepare(
        "UPDATE playlists SET name=?,updated_at=? WHERE id=?");
    s.bind(1, name); s.bind(2, now()); s.bind(3, id);
    s.step();
    return true;
}

bool PlaylistManager::remove(int64_t id) {
    std::lock_guard lk(repo_.db().mutex());
    auto s = repo_.db().prepare("DELETE FROM playlists WHERE id=?");
    s.bind(1, id); s.step();
    return true;
}

int64_t PlaylistManager::duplicate(int64_t id, const std::string& name) {
    std::lock_guard lk(repo_.db().mutex());
    Database::Tx tx(repo_.db());
    const PlaylistInfo src = info(id);
    const int64_t nid = src.kind == PlaylistKind::Smart
        ? createSmart(name.empty() ? src.name + " copy" : name,
                      src.rulesJson, src.folderId)
        : create(name.empty() ? src.name + " copy" : name, src.folderId);
    if (nid <= 0) { tx.rollback(); return -1; }
    if (src.kind == PlaylistKind::Static) {
        auto s = repo_.db().prepare(
            "INSERT INTO playlist_items(playlist_id,position,track_id,"
            "added_at) SELECT ?,position,track_id,added_at FROM "
            "playlist_items WHERE playlist_id=? ORDER BY position");
        s.bind(1, nid); s.bind(2, id); s.step();
    }
    tx.commit();
    return nid;
}

bool PlaylistManager::setRules(int64_t id, const std::string& rulesJson) {
    std::lock_guard lk(repo_.db().mutex());
    auto s = repo_.db().prepare(
        "UPDATE playlists SET rules=?,updated_at=? WHERE id=? AND kind=1");
    s.bind(1, rulesJson); s.bind(2, now()); s.bind(3, id);
    s.step();
    return true;
}

PlaylistInfo PlaylistManager::info(int64_t id) {
    std::lock_guard lk(repo_.db().mutex());
    auto s = repo_.db().prepare(
        "SELECT p.id,p.name,p.folder_id,p.kind,p.rules,p.created_at,"
        "p.updated_at,(SELECT COUNT(*) FROM playlist_items i WHERE "
        "i.playlist_id=p.id) FROM playlists p WHERE p.id=?");
    s.bind(1, id);
    PlaylistInfo pi;
    if (!s.step()) return pi;
    pi.id = s.colInt(0); pi.name = s.colText(1);
    pi.folderId = s.colInt(2);
    pi.kind = (PlaylistKind)(int)s.colInt(3);
    pi.rulesJson = s.colText(4);
    pi.createdAt = s.colInt(5); pi.updatedAt = s.colInt(6);
    pi.itemCount = (int)s.colInt(7);
    return pi;
}

std::vector<PlaylistInfo> PlaylistManager::playlists(int64_t folderId) {
    std::lock_guard lk(repo_.db().mutex());
    auto s = repo_.db().prepare(folderId < 0
        ? "SELECT id FROM playlists ORDER BY name COLLATE NOCASE"
        : "SELECT id FROM playlists WHERE folder_id IS ? "
          "ORDER BY name COLLATE NOCASE");
    if (folderId >= 0) { if (folderId == 0) s.bindNull(1); else s.bind(1, folderId); }
    std::vector<int64_t> ids;
    while (s.step()) ids.push_back(s.colInt(0));
    std::vector<PlaylistInfo> out;
    for (auto id : ids) out.push_back(info(id));
    return out;
}

// ---------------------------------------------------------------- items
bool PlaylistManager::addTrack(int64_t playlistId, int64_t trackId) {
    std::lock_guard lk(repo_.db().mutex());
    Database::Tx tx(repo_.db());
    int64_t pos = 0;
    {
        auto s = repo_.db().prepare(
            "SELECT COALESCE(MAX(position)+1,0) FROM playlist_items "
            "WHERE playlist_id=?");
        s.bind(1, playlistId);
        if (s.step()) pos = s.colInt(0);
    }
    {
        auto s = repo_.db().prepare(
            "INSERT OR IGNORE INTO playlist_items(playlist_id,position,"
            "track_id,added_at) VALUES(?,?,?,?)");
        s.bind(1, playlistId); s.bind(2, pos);
        s.bind(3, trackId);    s.bind(4, now());
        s.step();
    }
    tx.commit();
    return true;
}

bool PlaylistManager::addTracks(int64_t playlistId,
                                const std::vector<int64_t>& ids) {
    std::lock_guard lk(repo_.db().mutex());
    Database::Tx tx(repo_.db());
    int64_t pos = 0;
    {
        auto s = repo_.db().prepare(
            "SELECT COALESCE(MAX(position)+1,0) FROM playlist_items "
            "WHERE playlist_id=?");
        s.bind(1, playlistId);
        if (s.step()) pos = s.colInt(0);
    }
    auto s = repo_.db().prepare(
        "INSERT OR IGNORE INTO playlist_items(playlist_id,position,"
        "track_id,added_at) VALUES(?,?,?,?)");
    for (int64_t id : ids) {
        s.reset();
        s.bind(1, playlistId); s.bind(2, pos++);
        s.bind(3, id);         s.bind(4, now());
        s.step();
    }
    s.finalize();
    tx.commit();
    return true;
}

bool PlaylistManager::removeTrack(int64_t playlistId, int64_t trackId) {
    std::lock_guard lk(repo_.db().mutex());
    Database::Tx tx(repo_.db());
    int64_t removedPos = -1;
    {
        auto s = repo_.db().prepare(
            "SELECT position FROM playlist_items WHERE playlist_id=? "
            "AND track_id=?");
        s.bind(1, playlistId); s.bind(2, trackId);
        if (s.step()) removedPos = s.colInt(0);
    }
    {
        auto s = repo_.db().prepare(
            "DELETE FROM playlist_items WHERE playlist_id=? AND track_id=?");
        s.bind(1, playlistId); s.bind(2, trackId);
        s.step();
    }
    if (removedPos >= 0) {
        auto s = repo_.db().prepare(
            "UPDATE playlist_items SET position=position-1 WHERE "
            "playlist_id=? AND position>?");
        s.bind(1, playlistId); s.bind(2, removedPos);
        s.step();
    }
    tx.commit();
    return true;
}

bool PlaylistManager::moveTrack(int64_t playlistId, int fromPos, int toPos) {
    if (fromPos == toPos) return true;
    std::lock_guard lk(repo_.db().mutex());
    Database::Tx tx(repo_.db());
    int64_t trackId = -1;
    {
        auto s = repo_.db().prepare(
            "SELECT track_id FROM playlist_items WHERE playlist_id=? "
            "AND position=?");
        s.bind(1, playlistId); s.bind(2, (int64_t)fromPos);
        if (!s.step() || (trackId = s.colInt(0)) < 0) {
            tx.rollback(); return false;
        }
    }
    if (fromPos < toPos) {
        auto s = repo_.db().prepare(
            "UPDATE playlist_items SET position=position-1 WHERE "
            "playlist_id=? AND position>? AND position<=?");
        s.bind(1, playlistId); s.bind(2, (int64_t)fromPos);
        s.bind(3, (int64_t)toPos); s.step();
    } else {
        auto s = repo_.db().prepare(
            "UPDATE playlist_items SET position=position+1 WHERE "
            "playlist_id=? AND position>=? AND position<?");
        s.bind(1, playlistId); s.bind(2, (int64_t)toPos);
        s.bind(3, (int64_t)fromPos); s.step();
    }
    {
        auto s = repo_.db().prepare(
            "UPDATE playlist_items SET position=? WHERE playlist_id=? "
            "AND track_id=?");
        s.bind(1, (int64_t)toPos); s.bind(2, playlistId);
        s.bind(3, trackId); s.step();
    }
    tx.commit();
    return true;
}

bool PlaylistManager::setOrder(int64_t playlistId,
                               const std::vector<int64_t>& orderedIds) {
    std::lock_guard lk(repo_.db().mutex());
    Database::Tx tx(repo_.db());
    {
        auto s = repo_.db().prepare(
            "DELETE FROM playlist_items WHERE playlist_id=?");
        s.bind(1, playlistId); s.step();
    }
    auto s = repo_.db().prepare(
        "INSERT INTO playlist_items(playlist_id,position,track_id,"
        "added_at) VALUES(?,?,?,?)");
    int64_t pos = 0;
    for (int64_t id : orderedIds) {
        s.reset();
        s.bind(1, playlistId); s.bind(2, pos++);
        s.bind(3, id);         s.bind(4, now());
        s.step();
    }
    s.finalize();
    tx.commit();
    return true;
}

std::vector<int64_t> PlaylistManager::trackIds(int64_t playlistId) {
    std::lock_guard lk(repo_.db().mutex());
    auto s = repo_.db().prepare(
        "SELECT track_id FROM playlist_items WHERE playlist_id=? "
        "ORDER BY position");
    s.bind(1, playlistId);
    std::vector<int64_t> out;
    while (s.step()) out.push_back(s.colInt(0));
    return out;
}

TrackPage PlaylistManager::tracks(int64_t playlistId, int offset, int limit) {
    const auto ids = trackIds(playlistId);
    TrackPage p;
    p.total = (int)ids.size();
    const int64_t end = std::min<int64_t>((int64_t)ids.size(),
                                          offset + (int64_t)limit);
    for (int64_t i = offset; i < end; ++i) {
        Track t;
        if (repo_.getById(ids[(size_t)i], t)) p.items.push_back(std::move(t));
    }
    return p;
}

} // namespace onda::library
