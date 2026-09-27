#pragma once

#include <string>
#include <vector>

#include "../library/CrateManager.h"
#include "../library/PlaylistManager.h"
#include "../library/SmartPlaylistEngine.h"
#include "../library/TrackRepository.h"

namespace onda::smart {

/// Phase 23 §6 — a Smart Crate is a rule-driven crate. Crates and
/// playlists share the same table/engine already (kind=smart rows
/// under crate folders); this surface gives crates first-class
/// naming without duplicating the query engine.
class SmartCrateEngine {
public:
    SmartCrateEngine(library::PlaylistManager& pl,
                     library::TrackRepository& repo)
        : pl_(pl), repo_(repo) {}

    int64_t create(const std::string& name,
                   const library::SmartRules& rules,
                   int64_t folderId = 0) {
        return pl_.createSmart(
            name, library::SmartPlaylistEngine::toJson(rules), folderId);
    }
    bool setRules(int64_t id, const library::SmartRules& rules) {
        return pl_.setRules(
            id, library::SmartPlaylistEngine::toJson(rules));
    }
    bool rename(int64_t id, const std::string& n) {
        return pl_.rename(id, n);
    }
    bool remove(int64_t id) { return pl_.remove(id); }

    /// Live evaluation — identical engine to smart playlists (§6).
    library::TrackPage tracks(int64_t id, int offset = 0,
                              int limit = 200) {
        return pl_.tracks(id, offset, limit);
    }
    /// §7 — preview without persisting anything.
    library::TrackPage preview(const library::SmartRules& rules,
                               const library::QuerySpec& q) {
        return library::SmartPlaylistEngine::evaluate(
            repo_, library::SmartPlaylistEngine::toJson(rules), q);
    }
    int previewCount(const library::SmartRules& rules) {
        library::QuerySpec q; q.limit = 1;
        return preview(rules, q).total;
    }

private:
    library::PlaylistManager& pl_;
    library::TrackRepository& repo_;
};

} // namespace onda::smart
