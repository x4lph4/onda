#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "TrackRepository.h"

namespace onda::library {

/// §24-26 — validated user-metadata surface: 0–5 ratings, color
/// tags, user comments. All state lives in the tracks row's user
/// columns — separate from file metadata, preserved by rescans.
class RatingManager {
public:
    explicit RatingManager(TrackRepository& repo) : repo_(repo) {}

    /// §25 — set 0..5 star rating. Values outside clamp.
    bool setRating(int64_t id, int stars) {
        Track t;
        if (!repo_.getById(id, t)) return false;
        if (stars < 0) stars = 0;
        if (stars > 5) stars = 5;
        return repo_.setUserFields(id, stars, t.color, t.energy,
                                   t.comments);
    }

    /// §24 — ARGB color tag; 0 clears.
    bool setColor(int64_t id, int argb) {
        Track t;
        if (!repo_.getById(id, t)) return false;
        return repo_.setUserFields(id, t.rating, argb, t.energy,
                                   t.comments);
    }

    /// Named palette (UI maps to ARGB) — kept as a fixed list so
    /// exports/skins can rely on stable indices.
    static const char* colorName(int slot) {
        static const char* names[] = {
            "none", "red", "orange", "yellow",
            "green", "blue", "purple", "pink", "gray" };
        return slot >= 0 && slot < 9 ? names[slot] : "none";
    }

    /// §26 — user comment (independent of the file's embedded
    /// comment once set — see EditedField::Comments).
    bool setComments(int64_t id, const std::string& comments) {
        Track t;
        if (!repo_.getById(id, t)) return false;
        return repo_.setUserFields(id, t.rating, t.color, t.energy,
                                   comments);
    }

    /// Energy 0..10 — user-facing "drive" tag.
    bool setEnergy(int64_t id, int energy) {
        Track t;
        if (!repo_.getById(id, t)) return false;
        if (energy < 0) energy = 0;
        if (energy > 10) energy = 10;
        return repo_.setUserFields(id, t.rating, t.color, energy,
                                   t.comments);
    }

    /// Bulk rating over many tracks — used by multi-select edits.
    int setRatingBulk(const std::vector<int64_t>& ids, int stars) {
        int n = 0;
        for (int64_t id : ids) n += setRating(id, stars) ? 1 : 0;
        return n;
    }
    int setColorBulk(const std::vector<int64_t>& ids, int argb) {
        int n = 0;
        for (int64_t id : ids) n += setColor(id, argb) ? 1 : 0;
        return n;
    }

private:
    TrackRepository& repo_;
};

} // namespace onda::library
