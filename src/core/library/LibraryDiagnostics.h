#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

#include "PlaylistManager.h"
#include "TrackRepository.h"

namespace onda::library {

/// Diagnostics snapshot — POD, safe to copy across threads.
struct LibrarySnapshot {
    int tracks { 0 };
    int missing { 0 };
    int invalid { 0 };
    int unsupported { 0 };
    int metadataOnly { 0 };
    int analyzed { 0 };
    int analysisPending { 0 };
    int analysisFailed { 0 };
    int needsReview { 0 };
    int hasBeatgrid { 0 };
    int hasWaveform { 0 };
    int tags { 0 };
    int playlists { 0 };
    int smartPlaylists { 0 };
    int folders { 0 };
    int historyEntries { 0 };
    int ftsRows { 0 };
    int64_t dbBytes { 0 };
    bool integrityOk { false };
    std::string integrityDetail;
};

/// § diag — read-only rollup over the library DB. Never touches the
/// audio path; the count queries are indexed or table scans bounded
/// by library size — call from UI/worker threads.
class LibraryDiagnostics {
public:
    LibraryDiagnostics(TrackRepository& repo, PlaylistManager& pl,
                       const std::string& dbPath)
        : repo_(repo), pl_(pl), dbPath_(dbPath) {}

    LibrarySnapshot snapshot() {
        LibrarySnapshot s;
        Database& db = repo_.db();
        std::lock_guard lk(db.mutex());
        auto one = [&](const char* sql) {
            auto st = db.prepare(sql);
            return st.step() ? (int)st.colInt(0) : 0;
        };
        s.tracks          = one("SELECT COUNT(*) FROM tracks");
        s.missing         = one("SELECT COUNT(*) FROM tracks WHERE status=1");
        s.invalid         = one("SELECT COUNT(*) FROM tracks WHERE status=3");
        s.unsupported     = one("SELECT COUNT(*) FROM tracks WHERE play_state=1");
        s.metadataOnly    = one("SELECT COUNT(*) FROM tracks WHERE play_state=2");
        s.analyzed        = one("SELECT COUNT(*) FROM analysis");
        s.analysisPending = one("SELECT COUNT(*) FROM tracks"
                                " WHERE analysis_state IN (1,2)");
        s.analysisFailed  = one("SELECT COUNT(*) FROM tracks"
                                " WHERE analysis_state=4");
        s.needsReview     = one("SELECT COUNT(*) FROM analysis"
                                " WHERE needs_review!=0");
        s.hasBeatgrid     = one("SELECT COUNT(*) FROM beatgrids");
        s.hasWaveform     = one("SELECT COUNT(*) FROM waveforms");
        s.tags            = one("SELECT COUNT(*) FROM tags");
        s.playlists       = one("SELECT COUNT(*) FROM playlists WHERE kind=0");
        s.smartPlaylists  = one("SELECT COUNT(*) FROM playlists WHERE kind=1");
        s.folders         = one("SELECT COUNT(*) FROM folders");
        s.historyEntries  = one("SELECT COUNT(*) FROM history");
        s.ftsRows         = one("SELECT COUNT(*) FROM tracks_fts");
        std::error_code ec;
        s.dbBytes = (int64_t)std::filesystem::file_size(dbPath_, ec);
        if (ec) {
            // WAL: db file + -wal
            s.dbBytes = 0;
        }
        return s;
    }

    /// Integrity check (SQLite integrity_check + FK check) — runs the
    /// full scan; call from the worker, not on the UI clock.
    bool integrity(std::string& detail) {
        return repo_.integrityCheck(detail);
    }

    /// Human-readable one-liner report for the UI/diag copy command.
    std::string report() {
        LibrarySnapshot s = snapshot();
        std::string det;
        const bool ok = repo_.integrityCheck(det);
        s.integrityOk = ok;
        s.integrityDetail = det;
        char b[768];
        std::snprintf(b, sizeof(b),
            "tracks=%d missing=%d invalid=%d unsupported=%d "
            "metaOnly=%d analyzed=%d pending=%d failed=%d review=%d "
            "grid=%d wave=%d tags=%d playlists=%d smart=%d folders=%d "
            "history=%d fts=%d db=%lldKB integrity=%s",
            s.tracks, s.missing, s.invalid, s.unsupported,
            s.metadataOnly, s.analyzed, s.analysisPending,
            s.analysisFailed, s.needsReview, s.hasBeatgrid,
            s.hasWaveform, s.tags, s.playlists, s.smartPlaylists,
            s.folders, s.historyEntries, s.ftsRows,
            (long long)(s.dbBytes / 1024),
            ok ? "ok" : det.c_str());
        return b;
    }

private:
    TrackRepository& repo_;
    PlaylistManager& pl_;
    std::string dbPath_;
};

} // namespace onda::library
