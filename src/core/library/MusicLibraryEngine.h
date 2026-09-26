#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../data/Database.h"
#include "CrateManager.h"
#include "DuplicateDetector.h"
#include "FileWatcher.h"
#include "HistoryManager.h"
#include "LibraryBackup.h"
#include "LibraryDiagnostics.h"
#include "LibraryScanner.h"
#include "LibraryTypes.h"
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

    /// Diagnostics rollup (track/missing/analysis counts, db size).
    /// Constructed on demand — reads are cheap indexed counts.
    LibraryDiagnostics diagnostics() {
        return LibraryDiagnostics(repo_, playlists_, dbPath_);
    }

    // ---- convenience queries (sync, cheap) ----
    TrackPage query(const QuerySpec& q) { return repo_.query(q); }
    int  trackCount() { QuerySpec q; q.limit = 1; return repo_.query(q).total; }

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
