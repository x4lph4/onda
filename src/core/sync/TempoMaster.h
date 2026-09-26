#pragma once

#include <array>

#include "SyncTypes.h"

namespace onda::sync {

/// Tempo-master selection (§4-5, §23). Pure policy — the engine feeds
/// it plain flags and it returns the resolved master deck (or -1).
///
/// AUTO rules, in order:
///   1. the lowest-indexed *playing* deck that is sync-enabled wins;
///   2. else the first sync-enabled deck;
///   3. else none.
/// The resolver never re-picks while the current master remains valid
/// (playing + synced) — no surprise switches mid-performance.
struct TempoMaster {
    MasterSource      source { MasterSource::Auto };
    MasterPausePolicy pause  { MasterPausePolicy::ClockHolds };

    /// Deck index for explicit sources; Auto/External/None resolve to
    /// -1 here (Auto goes through resolveAuto, External/None → no deck).
    int pinnedDeck() const {
        const int s = (int)source;
        return s >= 0 && s <= 3 ? s : -1;
    }
    bool isExternal() const { return source == MasterSource::External; }
    bool isNone()     const { return source == MasterSource::None; }

    /// AUTO evaluation. `synced[d]` = sync engaged, `playing[d]` =
    /// deck transport running, `current` = current resolved master.
    int resolveAuto(const std::array<bool, 4>& synced,
                    const std::array<bool, 4>& playing,
                    int current) const {
        // keep a still-valid master — stability first
        if (current >= 0 && current < 4 &&
            synced[(size_t)current] && playing[(size_t)current])
            return current;
        for (int d = 0; d < 4; ++d)
            if (synced[(size_t)d] && playing[(size_t)d]) return d;
        for (int d = 0; d < 4; ++d)
            if (synced[(size_t)d]) return d;
        return -1;
    }

    /// Full resolution for the configured source.
    int resolve(const std::array<bool, 4>& synced,
                const std::array<bool, 4>& playing,
                int current) const {
        if (isExternal() || isNone()) return -1;
        // a pinned deck is the master as a ROLE — its own sync state
        // is irrelevant (the RT layer guards playing/grid anyway).
        if (const int p = pinnedDeck(); p >= 0) return p;
        return resolveAuto(synced, playing, current);
    }
};

} // namespace onda::sync
