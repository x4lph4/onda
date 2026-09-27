#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "../library/MusicLibraryEngine.h"
#include "../library/SmartPlaylistEngine.h"
#include "AutoTagEngine.h"
#include "LibraryBatchEngine.h"
#include "LibraryPresets.h"
#include "LibraryResourceManager.h"
#include "LibraryStatistics.h"
#include "SmartCrateEngine.h"
#include "SmartQueryValidator.h"
#include "TrackDiscoveryEngine.h"

namespace onda::smart {

/// Phase 23 — coordinator for the intelligent library layer.
///
/// Thin facade over MusicLibraryEngine's shared guarded connection:
/// queries stay synchronous and cheap (indexed), heavy work is
/// posted to the library worker. The audio thread never calls any
/// of this; UI calls the read/query surfaces directly.
///
///       SmartLibraryEngine
///       ├── SmartPlaylistEngine  (existing — shared rules engine)
///       ├── SmartCrateEngine
///       ├── AutoTagEngine
///       ├── EnergyAnalysisEngine (pure, fills analysis columns)
///       ├── TrackDiscoveryEngine (criteria + match reasons)
///       ├── RelatedTracksEngine  (harmonic — Phase 22, reused)
///       ├── LibraryPresets + SessionLibraryQuery
///       ├── LibraryBatchEngine   (async + undo)
///       ├── LibraryStatistics
///       ├── LibraryResourceManager
///       └── SmartLibraryDiagnostics
///
/// §1 contract: these tools INFORM and ORGANIZE — no automatic
/// creative decisions (no auto-load, no auto-mix, no auto-delete).
class SmartLibraryEngine {
public:
    explicit SmartLibraryEngine(library::MusicLibraryEngine& lib)
        : lib_(lib)
        , crates_(lib.playlists(), lib.tracks())
        , autoTags_(lib.db())
        , session_(lib.tracks())
        , stats_(lib.db())
        , batch_(lib.db(), lib.tracks())
        , resources_(lib.db()) {}

    library::MusicLibraryEngine& library() { return lib_; }

    // ---- sub-engines ---------------------------------------------
    SmartCrateEngine&       crates()     { return crates_; }
    AutoTagEngine&          autoTags()   { return autoTags_; }
    SessionLibraryQuery&    sessions()   { return session_; }
    LibraryStatistics&      statistics() { return stats_; }
    LibraryBatchEngine&     batch()      { return batch_; }
    LibraryResourceManager& resources()  { return resources_; }

    // ---- smart playlists (existing engine surfaced) ---------------
    library::PlaylistManager& playlists() { return lib_.playlists(); }

    /// §7 — dry-run a query: count + first page, nothing persisted.
    library::TrackPage preview(const library::SmartRules& rules,
                               const library::QuerySpec& q = {}) {
        return library::SmartPlaylistEngine::evaluate(
            lib_.tracks(),
            library::SmartPlaylistEngine::toJson(rules), q);
    }
    int previewCount(const library::SmartRules& rules) {
        library::QuerySpec q; q.limit = 1;
        return preview(rules, q).total;
    }
    /// §8 — validation before save.
    std::vector<SmartQueryValidator::Issue> validate(
        const library::SmartRules& rules) {
        return SmartQueryValidator::validate(rules);
    }

    // ---- §48 save surfaces -----------------------------------------
    int64_t saveAsPlaylist(const std::string& name,
                           const library::SmartRules& rules,
                           int64_t folderId = 0) {
        return lib_.playlists().createSmart(
            name, library::SmartPlaylistEngine::toJson(rules),
            folderId);
    }
    int64_t saveAsCrate(const std::string& name,
                        const library::SmartRules& rules,
                        int64_t folderId = 0) {
        return crates_.create(name, rules, folderId);
    }

    // ---- discovery ---------------------------------------------------
    std::vector<DiscoveryMatch> discover(
        const DiscoveryCriteria& c) {
        return TrackDiscoveryEngine::discover(c, lib_.tracks());
    }

    // ---- presets ------------------------------------------------------
    library::TrackPage preset(LibraryPresets::Id id,
                              int64_t sessionId = -1,
                              int limit = 200) {
        library::QuerySpec q;
        switch (id) {
            case LibraryPresets::Id::RecentlyAdded:
                q = LibraryPresets::recentlyAdded(limit); break;
            case LibraryPresets::Id::RecentlyPlayed:
                q = LibraryPresets::recentlyPlayed(limit); break;
            case LibraryPresets::Id::MostPlayed:
                q = LibraryPresets::mostPlayed(1, limit); break;
            case LibraryPresets::Id::Unplayed:
                q = LibraryPresets::unplayed(limit); break;
            case LibraryPresets::Id::NotPlayedRecently:
                q = LibraryPresets::notPlayedRecently(30, limit); break;
            case LibraryPresets::Id::PlayedThisSession:
            case LibraryPresets::Id::PlayedInSession:
                q = LibraryPresets::playedInSession(
                    sessionId < 0 ? lib_.history().currentSession()
                                  : sessionId, limit);
                break;
            case LibraryPresets::Id::MissingFiles:
                q = LibraryPresets::missingFiles(limit); break;
            case LibraryPresets::Id::Unanalyzed:
                q = LibraryPresets::unanalyzed(limit); break;
        }
        return lib_.tracks().query(q);
    }

    /// §37 — possible-duplicate view: flattened group members.
    /// Detector-driven, never deletes anything automatically.
    std::vector<library::DuplicateGroup> duplicatesView(
        int maxGroups = 1000) {
        return lib_.duplicates().findAll(maxGroups);
    }

    // ---- diagnostics --------------------------------------------------
    struct SmartLibraryDiagnostics {
        LibraryStats stats;
        LibraryResourceManager::CacheStats caches;
    };
    SmartLibraryDiagnostics diagnostics() {
        return { stats_.compute(), resources_.cacheStats() };
    }

private:
    library::MusicLibraryEngine& lib_;
    SmartCrateEngine      crates_;
    AutoTagEngine         autoTags_;
    SessionLibraryQuery   session_;
    LibraryStatistics     stats_;
    LibraryBatchEngine    batch_;
    LibraryResourceManager resources_;
};

} // namespace onda::smart
