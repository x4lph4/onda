#pragma once

#include <algorithm>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

#include "../data/Database.h"
#include "SmartLibraryTypes.h"

namespace onda::smart {

/// Phase 23 §9-12 — rule-based auto tagging. Tags are proposed from
/// REAL, already-measured columns only: analysis energy/loudness/
/// first-sound + bpm. Every suggestion carries a confidence
/// (margin past the rule threshold) and the rule that produced it.
///
/// Deliberately NOT implemented: vocal/instrumental detection — no
/// honest signal exists yet (a stems/ML source can register a rule
/// later; the engine never invents tags it can't back).
///
/// `auto_tags` is a separate table from user `tags`/`track_tags`:
/// re-analysis can never delete or overwrite a user tag (§10).
class AutoTagEngine {
public:
    explicit AutoTagEngine(Database& db) : db_(db) {}

    // ---- rule evaluation (pure) ---------------------------------
    /// Suggestions for one analysis row. `energy` -1 ⇒ none.
    static std::vector<AutoTagSuggestion> suggest(
        float energy, double bpm, double firstSoundSec,
        const EnergyLevels& lv = {}) {
        std::vector<AutoTagSuggestion> out;
        if (energy < 0.0f) return out;

        auto conf = [](float v, float edge) {
            // confidence = distance past the threshold, squashed
            return std::clamp(0.5f + std::fabs(v - edge) * 1.5f,
                              0.0f, 1.0f);
        };
        const float high = lv.high, low = lv.low;

        if (energy >= high)
            out.push_back({ "High Energy", conf(energy, high),
                            "energy>=high" });
        else if (energy < low)
            out.push_back({ "Low Energy", conf(energy, low),
                            "energy<low" });

        if (bpm > 0.0) {
            if (energy < low && bpm <= 122.0)
                out.push_back({ "Warmup",
                                conf(energy, low) * 0.9f,
                                "low-energy+bpm<=122" });
            if (energy >= 0.75f && bpm >= 120.0 && bpm <= 135.0)
                out.push_back({ "Peak Time",
                                conf(energy, 0.75f),
                                "energy>=0.75+bpm-120..135" });
        }
        if (firstSoundSec >= 0.0) {
            if (firstSoundSec <= 4.0)
                out.push_back({ "Short Intro", 0.85f,
                                "first-sound<=4s" });
            else if (firstSoundSec >= 16.0)
                out.push_back({ "Long Intro", 0.85f,
                                "first-sound>=16s" });
        }
        return out;
    }

    // ---- suggestion pass (writes only `auto_tags`) ---------------
    /// Evaluate stored analysis for `ids` and upsert suggestions.
    /// Rejected pairs are never re-suggested (§12); Accepted rows
    /// are left untouched; confidence only updates on change.
    int run(const std::vector<int64_t>& ids,
            const EnergyLevels& lv = {}) {
        std::lock_guard lk(db_.mutex());
        int written = 0;
        auto row = db_.prepare(
            "SELECT a.energy,a.bpm,a.first_sound_frame,t.sample_rate"
            " FROM analysis a JOIN tracks t ON t.id=a.track_id"
            " WHERE a.track_id=?");
        auto st = db_.prepare(
            "SELECT status FROM auto_tags WHERE track_id=? AND name=?");
        auto up = db_.prepare(
            "INSERT INTO auto_tags(track_id,name,confidence,status,"
            "source,created_at,updated_at) VALUES(?,?,?,0,?,?,?)"
            " ON CONFLICT(track_id,name) DO UPDATE SET"
            " confidence=excluded.confidence,source=excluded.source,"
            "updated_at=excluded.updated_at");
        const int64_t now = (int64_t)std::time(nullptr);
        for (const int64_t id : ids) {
            row.bind(1, id);
            if (!row.step()) { row.reset(); continue; }
            const float energy   = (float)row.colDouble(0);
            const double bpm     = row.colDouble(1);
            const double fsf     = (double)row.colInt(2);
            const double rate    = row.colDouble(3);
            row.reset();
            const double firstSec =
                fsf >= 0.0 && rate > 0.0 ? fsf / rate : -1.0;

            for (const auto& sg : suggest(energy, bpm, firstSec, lv)) {
                st.bind(1, id); st.bind(2, sg.name);
                const int prev = st.step() ? (int)st.colInt(0) : -1;
                st.reset();
                if (prev == (int)AutoTagStatus::Rejected
                    || prev == (int)AutoTagStatus::Accepted)
                    continue;                        // §12
                up.bind(1, id); up.bind(2, sg.name);
                up.bind(3, (double)sg.confidence);
                up.bind(4, sg.source);
                up.bind(5, now); up.bind(6, now);
                up.step(); up.reset();
                ++written;
            }
        }
        return written;
    }

    /// All pending suggestions for the whole library.
    int runAll(const EnergyLevels& lv = {}) {
        auto s = db_.prepare(
            "SELECT track_id FROM analysis WHERE energy >= 0");
        std::vector<int64_t> ids;
        while (s.step()) ids.push_back(s.colInt(0));
        s.finalize();
        return run(ids, lv);
    }

    // ---- verdicts (§12) ------------------------------------------
    bool setStatus(int64_t trackId, const std::string& name,
                   AutoTagStatus status) {
        std::lock_guard lk(db_.mutex());
        auto s = db_.prepare(
            "UPDATE auto_tags SET status=?,updated_at=?"
            " WHERE track_id=? AND name=?");
        s.bind(1, (int64_t)status);
        s.bind(2, (int64_t)std::time(nullptr));
        s.bind(3, trackId); s.bind(4, name);
        s.step();
        return true;
    }
    bool accept(int64_t t, const std::string& n) {
        return setStatus(t, n, AutoTagStatus::Accepted);
    }
    bool reject(int64_t t, const std::string& n) {
        return setStatus(t, n, AutoTagStatus::Rejected);
    }
    bool ignore(int64_t t, const std::string& n) {
        return setStatus(t, n, AutoTagStatus::Ignored);
    }

    // ---- reads ----------------------------------------------------
    std::vector<AutoTag> of(int64_t trackId,
                            AutoTagStatus filter =
                                AutoTagStatus::Suggested,
                            bool anyStatus = false) {
        std::lock_guard lk(db_.mutex());
        auto s = db_.prepare(
            anyStatus
            ? "SELECT id,name,confidence,status,source,created_at,"
              "updated_at FROM auto_tags WHERE track_id=?"
              " ORDER BY name"
            : "SELECT id,name,confidence,status,source,created_at,"
              "updated_at FROM auto_tags WHERE track_id=? AND status=?"
              " ORDER BY name");
        s.bind(1, trackId);
        if (!anyStatus) s.bind(2, (int64_t)filter);
        std::vector<AutoTag> out;
        while (s.step())
            out.push_back({ s.colInt(0), trackId, s.colText(1),
                            (float)s.colDouble(2),
                            (AutoTagStatus)s.colInt(3), s.colText(4),
                            s.colInt(5), s.colInt(6) });
        return out;
    }

    int countByStatus(AutoTagStatus status) {
        std::lock_guard lk(db_.mutex());
        auto s = db_.prepare(
            "SELECT COUNT(*) FROM auto_tags WHERE status=?");
        s.bind(1, (int64_t)status);
        return s.step() ? (int)s.colInt(0) : 0;
    }

    /// Tag names + counts per status for §36 statistics.
    std::vector<std::pair<std::string, int>> tagStats(
        AutoTagStatus status) {
        std::lock_guard lk(db_.mutex());
        auto s = db_.prepare(
            "SELECT name,COUNT(*) FROM auto_tags WHERE status=?"
            " GROUP BY name ORDER BY COUNT(*) DESC");
        s.bind(1, (int64_t)status);
        std::vector<std::pair<std::string, int>> out;
        while (s.step()) out.push_back({ s.colText(0), (int)s.colInt(1) });
        return out;
    }

    /// Manual cleanup of a single auto-tag row (never user tags §44).
    bool clearTrack(int64_t trackId) {
        std::lock_guard lk(db_.mutex());
        auto s = db_.prepare("DELETE FROM auto_tags WHERE track_id=?");
        s.bind(1, trackId); s.step();
        return true;
    }

private:
    Database& db_;
};

} // namespace onda::smart
