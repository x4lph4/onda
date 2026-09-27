#pragma once

#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include "../harmonic/CamelotWheel.h"
#include "../harmonic/Compatibility.h"
#include "../harmonic/KeyShiftEngine.h"
#include "../library/LibraryTypes.h"
#include "../library/SearchQuery.h"
#include "../library/TrackRepository.h"
#include "SmartLibraryTypes.h"

namespace onda::smart {

/// Phase 23 §19-26 — criteria-driven discovery. Every result carries
/// the reasons it matched, derived from real row data — never an
/// opaque score (§21/22/52). Nothing auto-loads or auto-plays (§26/27).
///
/// Reference semantics: `keyRelation` picks the KEY filter:
///   0 same | 1 relative maj/min | 2 camelot-adjacent (strict wheel)
///   3 broad compatible | 4 any
struct DiscoveryCriteria {
    int64_t excludeTrackId { -1 };

    bool   useBpm { false };
    double bpm { 0.0 };
    double bpmTolerance { 4.0 };          // ±N BPM absolute (§23)

    int    keyRelation { 4 };
    int    keyIndex { -1 };

    std::string genre;                    // "" = any
    std::vector<std::string> tags;        // all must be present
    std::string artist;                   // "" = any
    int    minRating { 0 };               // 0 = any

    bool   useEnergy { false };
    int    energyMode { 0 };              // 0 similar | 1 higher | 2 lower | 3 range
    float  energyRef { -1.0f };
    float  energyRange { 0.15f };         // ± for similar
    float  energyMin { 0.0f }, energyMax { 1.0f }; // mode 3

    int    historyMode { 0 };             // 0 any | 1 played | 2 unplayed
    int    limit { 64 };
};

/// One explainable match reason: field + human text (§21).
struct MatchReason {
    std::string field;
    std::string detail;
};

struct DiscoveryMatch {
    library::Track track;
    std::vector<MatchReason> reasons;
};

class TrackDiscoveryEngine {
public:
    /// Build the QuerySpec that encodes the hard criteria — the
    /// same FilterEngine the browser uses, so results are explainable
    /// by construction (criteria → clause → row).
    static library::QuerySpec specFor(const DiscoveryCriteria& c) {
        library::QuerySpec q;
        auto add = [&](const char* f, library::CmpOp op, double a,
                       double b = 0.0, const std::string& t = "") {
            library::FilterCond fc;
            fc.field = f; fc.op = op; fc.a = a; fc.b = b; fc.text = t;
            q.filters.push_back(fc);
        };
        if (c.useBpm && c.bpm > 0.0)
            add("bpm", library::CmpOp::Between,
                c.bpm - c.bpmTolerance, c.bpm + c.bpmTolerance);

        switch (c.keyRelation) {
            case 0: if (c.keyIndex >= 0)
                        add("key_index", library::CmpOp::Eq, c.keyIndex);
                    break;
            case 1: if (c.keyIndex >= 0 && c.keyIndex < 24) {
                        // relative major/minor = same pc, mode flip
                        const int rel = c.keyIndex < 12
                            ? c.keyIndex + 12 : c.keyIndex - 12;
                        library::FilterCond fc;
                        fc.field = "key_index";
                        fc.op = library::CmpOp::In;
                        fc.ints = { c.keyIndex, rel };
                        q.filters.push_back(fc);
                    }
                    break;
            case 2: if (c.keyIndex >= 0)
                        add("key_camelot_compatible",
                            library::CmpOp::Eq, c.keyIndex);
                    break;
            case 3: if (c.keyIndex >= 0)
                        add("key_compatible", library::CmpOp::Eq,
                            c.keyIndex);
                    break;
            default: break;               // any key
        }
        if (!c.genre.empty())
            add("genre", library::CmpOp::Contains, 0.0, 0.0, c.genre);
        for (const auto& t : c.tags)
            add("tags", library::CmpOp::Contains, 0.0, 0.0, t);
        if (!c.artist.empty())
            add("artist", library::CmpOp::Contains, 0.0, 0.0, c.artist);
        if (c.minRating > 0)
            add("rating", library::CmpOp::Gte, c.minRating);

        if (c.useEnergy) {
            switch (c.energyMode) {
                case 0: if (c.energyRef >= 0.0f)
                            add("energy_score", library::CmpOp::Between,
                                c.energyRef - c.energyRange,
                                c.energyRef + c.energyRange);
                        break;
                case 1: if (c.energyRef >= 0.0f)
                            add("energy_score", library::CmpOp::Gt,
                                c.energyRef);
                        break;
                case 2: if (c.energyRef >= 0.0f)
                            add("energy_score", library::CmpOp::Lt,
                                c.energyRef);
                        break;
                default: add("energy_score", library::CmpOp::Between,
                             c.energyMin, c.energyMax);
                        break;
            }
        }
        if (c.historyMode == 1)
            add("unplayed", library::CmpOp::Eq, 0.0);
        else if (c.historyMode == 2)
            add("unplayed", library::CmpOp::Eq, 1.0);

        q.sort = library::SortKey::Bpm;
        q.limit = c.limit > 0 ? c.limit * 4 : 256;   // over-fetch for reasons+exclusion
        return q;
    }

    /// Compute why `t` appears under `c` — derived from row data only.
    static std::vector<MatchReason> explain(const DiscoveryCriteria& c,
                                            const library::Track& t) {
        std::vector<MatchReason> out;
        auto push = [&](std::string f, std::string d) {
            out.push_back({ std::move(f), std::move(d) });
        };
        if (c.useBpm && c.bpm > 0.0 && t.bpm > 0.0) {
            const double d = t.bpm - c.bpm;
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%+.1f BPM", d);
            push("bpm", buf);
        }
        if (c.keyIndex >= 0 && t.keyIndex >= 0) {
            const auto rel = harmonic::HarmonicCompatibilityEngine::
                relate(harmonic::MusicalKey::fromIndex(c.keyIndex),
                       harmonic::MusicalKey::fromIndex(t.keyIndex));
            push("key", rel.explanation);
        } else if (t.keyIndex < 0) {
            push("key", "unknown");
        }
        if (!c.genre.empty())
            push("genre", t.genre.empty() ? "no genre" : t.genre);
        for (const auto& tg : c.tags)
            push("tag", tg);
        if (!c.artist.empty())
            push("artist", t.artist.empty() ? "unknown" : t.artist);
        if (c.minRating > 0)
            push("rating", std::to_string(t.rating) + "/5");
        if (c.useEnergy) {
            if (t.energyScore >= 0.0f) {
                char buf[80];
                std::snprintf(buf, sizeof(buf), "energy %.2f (%s)",
                              (double)t.energyScore,
                              EnergyLevels::name(
                                  EnergyLevels().levelFor(
                                      t.energyScore)));
                push("energy", buf);
            } else {
                push("energy", "not analyzed");
            }
        }
        if (c.historyMode != 0)
            push("history", t.playCount > 0
                 ? "played " + std::to_string(t.playCount) + "x"
                 : "never played");
        return out;
    }

    /// Full pass: query → exclude reference → per-track reasons.
    static std::vector<DiscoveryMatch> discover(
        const DiscoveryCriteria& c, library::TrackRepository& repo) {
        auto page = repo.query(specFor(c));
        std::vector<DiscoveryMatch> out;
        out.reserve(page.items.size());
        for (const auto& t : page.items) {
            if (t.id == c.excludeTrackId) continue;
            DiscoveryMatch m;
            m.track   = t;
            m.reasons = explain(c, t);
            out.push_back(std::move(m));
            if ((int)out.size() >= (c.limit > 0 ? c.limit : 64)) break;
        }
        return out;
    }
};

} // namespace onda::smart
