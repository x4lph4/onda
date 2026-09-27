#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "LibraryTypes.h"
#include "TrackImporter.h"
#include "TrackRepository.h"

namespace onda::library {

/// Folder scan: enumerates audio files under configured roots and
/// reconciles them with the DB.
///
/// Rules (documented):
///   new file      → import
///   file gone     → mark MISSING, never delete the row — user data
///                   (rating/tags/cues) survives and relinks cleanly
///   fingerprint changed → re-import + invalidate analysis
///   missing row + unseen file share contentHash → file MOVED:
///                   relink the row to the new path (RELOCATED→Available),
///                   keeping the same track id and every attached datum
///
/// Cheap pass mode (`diffOnly`) stats files without hashing — the
/// file watcher polls that and only escalates changed candidates.
class LibraryScanner {
public:
    /// §11-12 — a watched folder. `autoImport` gates ONLY the
    /// watcher's new-file pickup: scans always reconcile every root
    /// (missing/relocated apply library-wide) but a file that appears
    /// under a non-autoImport root is left alone until the user
    /// imports or scans explicitly.
    struct WatchRoot {
        std::string path;
        bool recursive  { true };
        bool autoImport { false };
    };

    LibraryScanner(TrackRepository& repo, TrackImporter& importer)
        : repo_(repo), importer_(importer) {}

    void addRoot(const std::string& dir, bool recursive = true,
                 bool autoImport = false);
    bool removeRoot(const std::string& dir);
    std::vector<WatchRoot> roots() const;

    /// §12 — toggle auto-import for a root (returns false for unknown).
    bool setAutoImport(const std::string& dir, bool on);
    /// Is `path` under a root that auto-imports?
    bool autoImportFor(const std::string& path) const;

    /// Full scan: hash every file, reconcile, import new/modified.
    /// Returns the diff applied. Runs on the worker.
    ScanDiff scan();

    /// Cheap pass: stat-only; returns paths whose size/mtime differs
    /// from the DB row (candidates for a real fingerprint pass).
    std::vector<std::string> quickDiff();

private:
    TrackRepository& repo_;
    TrackImporter&   importer_;
    std::vector<WatchRoot> roots_;
};

} // namespace onda::library
