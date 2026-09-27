#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "../data/Database.h"
#include "SmartLibraryTypes.h"

namespace onda::smart {

/// Phase 23 §34-36 — read-only aggregate reports over the library.
/// Every number is an indexed SQL aggregate; nothing is estimated.
/// Energy stats describe the analysis feature, not musical quality.
struct LibraryStats {
    int64_t totalTracks { 0 };
    double  totalDurationSec { 0.0 };
    int64_t artists { 0 }, albums { 0 }, genres { 0 };
    double  avgBpm { 0.0 }, minBpm { 0.0 }, maxBpm { 0.0 };
    int64_t analyzed { 0 }, unanalyzed { 0 }, needsReview { 0 };
    int64_t missing { 0 }, duplicateGroups { 0 };
    int64_t stemsReady { 0 };

    /// key distribution: 24 slots (maj 0-11, min 12-23) + unknown.
    int64_t keyCounts[25] {};
    /// energy distribution per EnergyLevels bucket + unknown.
    int64_t energyCounts[6] {};          // unknown,verylow..veryhigh
    double  avgEnergy { -1.0 };          // <0 ⇒ no analyzed energy

    int64_t userTags { 0 }, autoTagsSuggested { 0 },
            autoTagsAccepted { 0 }, autoTagsRejected { 0 };
};

class LibraryStatistics {
public:
    explicit LibraryStatistics(Database& db) : db_(db) {}

    LibraryStats compute(const EnergyLevels& lv = {}) {
        std::lock_guard lk(db_.mutex());
        LibraryStats s;
        auto one = [&](const char* sql) {
            auto st = db_.prepare(sql);
            return st.step() ? st.colInt(0) : 0;
        };
        s.totalTracks = one("SELECT COUNT(*) FROM tracks");
        {
            auto st = db_.prepare(
                "SELECT COALESCE(SUM(duration_sec),0) FROM tracks");
            if (st.step()) s.totalDurationSec = st.colDouble(0);
        }
        s.artists = one(
            "SELECT COUNT(DISTINCT artist) FROM tracks"
            " WHERE artist IS NOT NULL AND artist != ''");
        s.albums  = one(
            "SELECT COUNT(DISTINCT album) FROM tracks"
            " WHERE album IS NOT NULL AND album != ''");
        s.genres  = one(
            "SELECT COUNT(DISTINCT genre) FROM tracks"
            " WHERE genre IS NOT NULL AND genre != ''");
        {
            auto st = db_.prepare(
                "SELECT AVG(bpm),MIN(bpm),MAX(bpm) FROM analysis"
                " WHERE bpm > 0");
            if (st.step()) {
                s.avgBpm = st.colDouble(0);
                s.minBpm = st.colDouble(1);
                s.maxBpm = st.colDouble(2);
            }
        }
        s.analyzed   = one("SELECT COUNT(*) FROM tracks"
                           " WHERE analysis_state = 3");   // Analyzed
        s.unanalyzed = one("SELECT COUNT(*) FROM tracks"
                           " WHERE analysis_state = 0");
        s.needsReview = one("SELECT COUNT(*) FROM analysis"
                            " WHERE needs_review != 0");
        s.missing  = one("SELECT COUNT(*) FROM tracks WHERE status = 1");
        s.duplicateGroups = one(
            "SELECT COUNT(*) FROM (SELECT content_hash FROM tracks"
            " WHERE content_hash != 0 GROUP BY content_hash"
            " HAVING COUNT(*) > 1)");
        s.stemsReady = one(
            "SELECT COUNT(*) FROM stem_sets WHERE state = 1");

        {
            auto st = db_.prepare(
                "SELECT key_index,COUNT(*) FROM analysis"
                " GROUP BY key_index");
            while (st.step()) {
                const int k = (int)st.colInt(0);
                s.keyCounts[k >= 0 && k < 24 ? k : 24] += st.colInt(1);
            }
        }
        {
            auto st = db_.prepare(
                "SELECT energy,COUNT(*) FROM analysis"
                " WHERE energy >= 0 GROUP BY energy");
            int64_t n = 0; double sum = 0.0;
            while (st.step()) {
                const float e = (float)st.colDouble(0);
                const int64_t cnt = st.colInt(1);
                const auto l = lv.levelFor(e);
                s.energyCounts[(int)l + 1] += cnt;
                sum += e * (double)cnt; n += cnt;
            }
            if (n > 0) s.avgEnergy = sum / (double)n;
        }
        s.energyCounts[0] = s.totalTracks > 0
            ? s.totalTracks - (s.energyCounts[1] + s.energyCounts[2]
                             + s.energyCounts[3] + s.energyCounts[4]
                             + s.energyCounts[5])
            : 0;

        s.userTags = one(
            "SELECT COUNT(*) FROM track_tags");
        s.autoTagsSuggested = one(
            "SELECT COUNT(*) FROM auto_tags WHERE status = 0");
        s.autoTagsAccepted  = one(
            "SELECT COUNT(*) FROM auto_tags WHERE status = 1");
        s.autoTagsRejected  = one(
            "SELECT COUNT(*) FROM auto_tags WHERE status = 2");
        return s;
    }

    /// §36 — top tags both sides for the stats view.
    std::vector<std::pair<std::string, int>> topUserTags(int n = 20) {
        std::lock_guard lk(db_.mutex());
        auto s = db_.prepare(
            "SELECT g.name,COUNT(tt.track_id) c FROM tags g"
            " JOIN track_tags tt ON tt.tag_id=g.id"
            " GROUP BY g.id ORDER BY c DESC,g.name LIMIT ?");
        s.bind(1, (int64_t)n);
        std::vector<std::pair<std::string, int>> out;
        while (s.step()) out.push_back({ s.colText(0), (int)s.colInt(1) });
        return out;
    }
    /// §36 — user tags with zero memberships (cleanup candidates).
    std::vector<std::string> unusedUserTags() {
        std::lock_guard lk(db_.mutex());
        auto s = db_.prepare(
            "SELECT g.name FROM tags g LEFT JOIN track_tags tt"
            " ON tt.tag_id=g.id WHERE tt.track_id IS NULL"
            " ORDER BY g.name");
        std::vector<std::string> out;
        while (s.step()) out.push_back(s.colText(0));
        return out;
    }

private:
    Database& db_;
};

} // namespace onda::smart
