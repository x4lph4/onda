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
    LibraryScanner(TrackRepository& repo, TrackImporter& importer)
        : repo_(repo), importer_(importer) {}

    void addRoot(const std::string& dir, bool recursive = true);
    bool removeRoot(const std::string& dir);
    std::vector<std::pair<std::string, bool>> roots() const;

    /// Full scan: hash every file, reconcile, import new/modified.
    /// Returns the diff applied. Runs on the worker.
    ScanDiff scan();

    /// Cheap pass: stat-only; returns paths whose size/mtime differs
    /// from the DB row (candidates for a real fingerprint pass).
    std::vector<std::string> quickDiff();

private:
    TrackRepository& repo_;
    TrackImporter&   importer_;
    std::vector<std::pair<std::string, bool>> roots_;
};

} // namespace onda::library
