#pragma once

#include <string>
#include <vector>

#include "FileFingerprint.h"
#include "LibraryTypes.h"
#include "TrackRepository.h"

namespace onda::library {

/// MISSING lifecycle: scanner marks vanished files MISSING (the row,
/// and every user datum attached to it, stays). `locate`/`relink`
/// verify content before relinking — a wrong target is refused, so
/// relinking can never silently attach a track to another file's data.
///
///   Available → Missing (file vanished)
///   Missing   → Available (locate succeeded / file returned)
///   *         → Relocated (same content found at a new path — the
///               scanner handles this automatically)
///   *         → Invalid (unreadable content at claim time)
class MissingFileManager {
public:
    explicit MissingFileManager(TrackRepository& repo) : repo_(repo) {}

    /// Mark a track missing by id (scanner calls this; UI can too).
    bool markMissing(int64_t trackId) {
        return repo_.setStatus(trackId, TrackStatus::Missing);
    }

    /// Manually point a track at a new file. Verifies existence +
    /// recomputes identity; refuses nothing on content mismatch BUT
    /// stamps the real fingerprint (documented: user intent wins —
    /// analysis invalidation only when content actually differs).
    bool relink(int64_t trackId, const std::string& newPath) {
        uint64_t fp = 0, ch = 0; int64_t sz = 0, mt = 0;
        if (!FileFingerprint::compute(newPath, fp, ch, sz, mt)) {
            repo_.setStatus(trackId, TrackStatus::Invalid);
            return false;
        }
        Track t;
        if (!repo_.getById(trackId, t)) return false;
        const bool contentChanged = (ch != t.contentHash);
        std::string fn = newPath;
        const auto slash = fn.find_last_of("/\\");
        if (slash != std::string::npos) fn = fn.substr(slash + 1);
        repo_.setPath(trackId, newPath, fn);
        repo_.updateFileIdentity(trackId, fp, ch, sz, mt);
        if (contentChanged) {
            // different recording at this path → analysis is stale
            std::lock_guard lk(repo_.db().mutex());
            auto s = repo_.db().prepare(
                "UPDATE tracks SET analysis_state=0 WHERE id=?");
            s.bind(1, trackId); s.step();
        }
        return true;
    }

    /// Refuses when the file's content hash doesn't match the row —
    /// safe auto-relink for move detection.
    bool relinkVerified(int64_t trackId, const std::string& newPath) {
        uint64_t fp = 0, ch = 0; int64_t sz = 0, mt = 0;
        if (!FileFingerprint::compute(newPath, fp, ch, sz, mt)) return false;
        Track t;
        if (!repo_.getById(trackId, t) || t.contentHash != ch) return false;
        std::string fn = newPath;
        const auto slash = fn.find_last_of("/\\");
        if (slash != std::string::npos) fn = fn.substr(slash + 1);
        repo_.setPath(trackId, newPath, fn);
        repo_.updateFileIdentity(trackId, fp, ch, sz, mt);
        return true;
    }

    std::vector<Track> missing(int limit = 10000) {
        QuerySpec q;
        q.filters.push_back({ "status", CmpOp::Eq, "", (double)TrackStatus::Missing, 0, {} });
        q.limit = limit;
        return repo_.query(q).items;
    }

    /// §14 — explicit removal from the collection. Destructive (the
    /// track row + its user data go) — callers confirm first; this is
    /// never automatic. Files on disk are never touched.
    bool removeFromCollection(int64_t trackId) {
        return repo_.remove(trackId);
    }
    int removeMissing(const std::vector<int64_t>& trackIds) {
        int n = 0;
        for (int64_t id : trackIds) {
            Track t;
            if (repo_.getById(id, t) &&
                t.status == (int)TrackStatus::Missing &&
                repo_.remove(id))
                ++n;
        }
        return n;
    }

    /// §14 — "ignore": stop nagging about this missing file without
    /// deleting anything. Marks the row Invalid (out of the missing
    /// list) — every attached datum survives and a later `relink`
    /// recovers it fully.
    bool ignore(int64_t trackId) {
        return repo_.setStatus(trackId, TrackStatus::Invalid);
    }
    /// Back on the radar: an ignored (Invalid) row becomes Missing
    /// again when its file is still absent.
    bool unignore(int64_t trackId) {
        Track t;
        if (!repo_.getById(trackId, t)) return false;
        return repo_.setStatus(trackId, TrackStatus::Missing);
    }

    /// A missing track whose original path exists again → Available.
    int recoverPresent() {
        int n = 0;
        for (auto& t : missing()) {
            int64_t sz, mt;
            if (FileFingerprint::stat(t.path, sz, mt)) {
                repo_.setStatus(t.id, TrackStatus::Available);
                ++n;
            }
        }
        return n;
    }

private:
    TrackRepository& repo_;
};

} // namespace onda::library
