#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../data/Database.h"
#include "ArtworkManager.h"
#include "CrateManager.h"
#include "DuplicateDetector.h"
#include "FileWatcher.h"
#include "HistoryManager.h"
#include "ImportQueue.h"
#include "LibraryBackup.h"
#include "LibraryDiagnostics.h"
#include "LibraryScanner.h"
#include "LibraryTypes.h"
#include "MetadataWriter.h"
#include "MissingFileManager.h"
#include "PlaylistManager.h"
#include "RatingManager.h"
#include "SearchQuery.h"
#include "SmartPlaylistEngine.h"
#include "TagManager.h"
#include "TrackImporter.h"
#include "TrackRepository.h"

namespace onda {
class AnalysisService;
}

namespace onda::library {

/// Library facade: owns the Database, every manager, and the single
/// library worker thread.
///
/// Threading contract (documented):
///   - the AUDIO thread never calls anything here
///   - queries are synchronous and cheap (indexed; hold the DB mutex
///     briefly) — UI may call them directly
///   - long ops (import, scan, history writes) go through post()/the
///     async entry points and run on the worker
///   - AnalysisService is optional: when set, imported tracks are
///     enqueued with priority 0 and the engine mirrors analysis
///     states (pending/analyzing/done/failed) onto the track rows
class MusicLibraryEngine {
public:
    MusicLibraryEngine();
    ~MusicLibraryEngine();

    bool open(const std::string& dbPath);
    void close();
    bool isOpen() const { return db_.isOpen(); }

    // ---- wiring ----
    void setMetadataProvider(TrackImporter::MetadataProvider p);
    void setAnalysisService(AnalysisService* svc);
    /// Artwork: (path) -> writes image to `destPath`, returns ext or "".
    void setArtworkProvider(
        std::function<std::string(const std::string& path,
                                  const std::string& destDir)> p);

    // ---- sub-managers (all share the guarded connection) ----
    TrackRepository&    tracks()    { return repo_; }
    TrackImporter&      importer()  { return importer_; }
    LibraryScanner&     scanner()   { return scanner_; }
    PlaylistManager&    playlists() { return playlists_; }
    HistoryManager&     history()   { return history_; }
    DuplicateDetector&  duplicates(){ return dup_; }
    MissingFileManager& missing()   { return missing_; }
    FileWatcher&        watcher()   { return watcher_; }
    Database&           db()        { return db_; }
    // Phase 18 surfaces (all share the guarded connection)
    CrateManager&       crates()    { return crates_; }
    TagManager&         tagManager(){ return tags_; }
    RatingManager&      ratings()   { return ratings_; }
    LibraryBackup&      backup()    { return backup_; }
    // Phase 29 surfaces
    ImportQueue&        importQueue()   { return importQueue_; }
    MetadataWriter&     metadataWriter(){ return metaWriter_; }
    ArtworkManager&     artwork()       { return artwork_; }

    /// Diagnostics rollup (track/missing/analysis counts, db size).
    /// Constructed on demand — reads are cheap indexed counts.
    LibraryDiagnostics diagnostics() {
        return LibraryDiagnostics(repo_, playlists_, dbPath_);
    }

    // ---- convenience queries (sync, cheap) ----
    TrackPage query(const QuerySpec& q) { return repo_.query(q); }
    int  trackCount() { QuerySpec q; q.limit = 1; return repo_.query(q).total; }

    /// §52-53 — deck hand-off: the library produces ONLY this
    /// reference; the deck/loader resolves audio from `path` and
    /// attaches cues/analysis via `trackId`. Playback control stays
    /// outside the library by construction.
    TrackReference trackRef(int64_t trackId) {
        TrackReference r;
        Track t;
        if (!repo_.getById(trackId, t)) return r;
        r.trackId = t.id; r.path = t.path;
        r.title = t.title; r.artist = t.artist;
        r.durationSec = t.durationSec;
        r.bpm = t.bpm; r.keyIndex = t.keyIndex;
        return r;
    }

    /// §11-12 — register a watched root AND persist it (kv), so
    /// watch configuration survives restarts. `autoImport` = new files
    /// appearing under it are picked up automatically.
    void addWatchRoot(const std::string& dir, bool recursive = true,
                      bool autoImport = false) {
        scanner_.addRoot(dir, recursive, autoImport);
        saveSetting("watch.root." + dir,
                    std::string(recursive ? "1" : "0") +
                    (autoImport ? ":a" : ""));
    }
    /// §12 — auto-import toggle for a watched root (persisted in kv).
    bool setAutoImport(const std::string& dir, bool on) {
        const bool ok = scanner_.setAutoImport(dir, on);
        if (!ok) return false;
        bool rec = true;
        std::error_code ec;
        const std::string abs = std::filesystem::absolute(
            dir, ec).lexically_normal().string();
        for (const auto& r : scanner_.roots())
            if (r.path == abs) { rec = r.recursive; break; }
        saveSetting("watch.root." + dir,
                    std::string(rec ? "1" : "0") + (on ? ":a" : ""));
        return true;
    }
    /// Re-register every persisted watch root (call after open()).
    int restoreWatchRoots() {
        std::lock_guard lk(db_.mutex());
        auto s = db_.prepare(
            "SELECT key,value FROM kv WHERE key LIKE 'watch.root.%'");
        int n = 0;
        while (s.step()) {
            const std::string k = s.colText(0);
            const std::string v = s.colText(1);
            const std::string dir = k.substr(11);
            const bool autoImport = v.find(":a") != std::string::npos;
            scanner_.addRoot(dir, v.empty() || v[0] != '0', autoImport);
            ++n;
        }
        return n;
    }

    /// §39 — configurable color palette (argb list in kv; comma-sep).
    /// UI reads this to render color-tag pickers — never hardcoded.
    std::vector<int> colorPalette() {
        const std::string raw =
            setting("palette.colors",
                    "4294901760,4294934528,4278255360,4278235145,"
                    "4280391411,4286578816,4294967040,4289574000");
        std::vector<int> out;
        size_t p = 0;
        while (p <= raw.size()) {
            const size_t c = raw.find(',', p);
            const std::string tok =
                raw.substr(p, c == std::string::npos ? c : c - p);
            if (!tok.empty()) out.push_back((int)std::strtoll(
                tok.c_str(), nullptr, 10));
            if (c == std::string::npos) break;
            p = c + 1;
        }
        return out;
    }

    /// §13 — search-box entry point: parses `field:value` tokens into
    /// filters + FTS text, then runs the normal indexed query.
    TrackPage search(const std::string& text, int offset = 0,
                     int limit = 200) {
        QuerySpec q = SearchQuery::toSpec(text);
        q.offset = offset; q.limit = limit;
        return repo_.query(q);
    }

    /// §20 — live smart-playlist page (rules stored on the playlist).
    TrackPage smartTracks(int64_t playlistId, int offset = 0,
                          int limit = 200) {
        return playlists_.tracks(playlistId, offset, limit);
    }

    /// §22 — typo-tolerant search. First runs the exact indexed query;
    /// when it returns nothing, an OR-prefix candidate net fetches a
    /// bounded set and each candidate is re-scored by banded edit
    /// distance over squashed (folded, space-stripped) metadata —
    /// "daftpunk" → "Daft Punk". Original metadata is never modified.
    /// Bounded work: at most `candCap` rows are scored per query.
    TrackPage searchFuzzy(const std::string& text, int offset = 0,
                          int limit = 200, int candCap = 500) {
        TrackPage exact = search(text, offset, limit);
        if (exact.total > 0 || text.empty()) return exact;
        const std::string fx = SearchEngine::fuzzyExpr(text);
        TrackPage out;
        if (fx.empty()) return out;
        std::vector<std::string> binds { fx };
        QuerySpec q; q.limit = candCap; q.offset = 0;
        TrackPage cands = repo_.queryWhere(
            "t.id IN (SELECT rowid FROM tracks_fts"
            " WHERE tracks_fts MATCH ?)", binds, q);
        const int maxD = SearchEngine::fuzzyThreshold(text);
        struct Scored { Track t; int d; };
        std::vector<Scored> scored;
        scored.reserve(cands.items.size());
        for (auto& t : cands.items) {
            const std::string hay = t.title + " " + t.artist + " " +
                                    t.album + " " + t.filename;
            const int d = SearchEngine::fuzzyDistance(text, hay, maxD);
            if (d <= maxD) scored.push_back({ std::move(t), d });
        }
        std::stable_sort(scored.begin(), scored.end(),
                         [](const Scored& a, const Scored& b) {
                             return a.d < b.d; });
        out.total = (int)scored.size();
        for (int i = offset;
             i < (int)scored.size() && (int)out.items.size() < limit; ++i)
            out.items.push_back(std::move(scored[(size_t)i].t));
        return out;
    }

    // ---- worker ----
    /// Queue a callable on the library worker. All heavy ops use this.
    void post(std::function<void()> fn);
    /// Drain synchronously (tests): runs queued work inline.
    int runPending();
    void startWorker();
    void stopWorker();
    bool workerRunning() const { return worker_.joinable(); }

    // ---- async facades ----
    void importPathsAsync(const std::vector<std::string>& paths,
                          std::function<void(const ImportResult&)> done = {});
    void scanAsync(std::function<void(const ScanDiff&)> done = {});
    void recordPlayAsync(int64_t trackId, int deck, int64_t startedAt,
                         double playedSec, int64_t sessionId = -1);

    // ---- artwork ----
    /// Cached artwork file path for a track, or "" when none/error.
    /// Extraction is lazy: first call extracts via the provider into
    /// `<dbDir>/artwork/<hash>.<ext>`; later calls hit the disk cache.
    std::string artworkPath(int64_t trackId);

    // ---- integrity / maintenance ----
    bool integrityCheck(std::string& detail) {
        return repo_.integrityCheck(detail);
    }

    /// Watch settings persisted in kv (scan roots, play-count rule).
    void saveSetting(const std::string& k, const std::string& v);
    std::string setting(const std::string& k,
                        const std::string& def = "");

private:
    void workerMain();
    std::string dbDir() const;

    Database          db_;
    std::string       dbPath_;
    TrackRepository   repo_;
    TrackImporter     importer_;
    LibraryScanner    scanner_;
    PlaylistManager   playlists_;
    HistoryManager    history_;
    DuplicateDetector dup_;
    MissingFileManager missing_;
    FileWatcher       watcher_;
    CrateManager      crates_;
    TagManager        tags_;
    RatingManager     ratings_;
    LibraryBackup     backup_;
    ImportQueue       importQueue_;
    MetadataWriter    metaWriter_;
    ArtworkManager    artwork_;
    AnalysisService*  analysis_ { nullptr };
    std::function<std::string(const std::string&, const std::string&)>
        artworkProvider_;

    std::thread worker_;
    std::mutex wq_;
    std::condition_variable wcv_;
    std::deque<std::function<void()>> tasks_;
    std::atomic<bool> exit_ { false };
};

} // namespace onda::library
