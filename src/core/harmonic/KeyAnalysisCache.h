#pragma once

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <optional>
#include <string>
#include <vector>

#include "../data/Database.h"
#include "HarmonicTypes.h"

namespace onda::harmonic {

/// ------------------------------------------------------------------
/// §21/§40 — key-analysis persistence (schema v8). The rich result
/// lives on the `analysis` row's key_* columns; every write also
/// appends to `key_history` for debugging/re-analysis.
///
/// Validity identity = (key_algo, key_version) on the row vs the
/// caller's current detector identity — an algorithm upgrade makes
/// old rows stale without a schema change.
///
/// Detected values ONLY. User overrides live in `user_overrides` and
/// are never touched here (§38).
/// ------------------------------------------------------------------
class KeyAnalysisCache {
public:
    explicit KeyAnalysisCache(Database& db) : db_(db) {}

    /// Persist a result for a track (also updates the flat
    /// key_index/key_confidence columns every legacy reader uses).
    void save(int64_t trackId, const KeyAnalysisResult& r) {
        auto s = db_.prepare(
            "UPDATE analysis SET key_index=?, key_confidence=?,"
            " key_algo=?, key_version=?, key_at=?, key_alt_json=?,"
            " key_segments_json=?, key_ambiguous=? WHERE track_id=?");
        s.bind(1, (int64_t)r.primary.index());
        s.bind(2, (double)r.primary.confidence);
        s.bind(3, r.algorithm);
        s.bind(4, r.version);
        s.bind(5, r.analyzedAt > 0 ? r.analyzedAt
                                 : (int64_t)::time(nullptr));
        s.bind(6, alternativesJson(r));
        s.bind(7, segmentsJson(r));
        s.bind(8, (int64_t)(r.ambiguous ? 1 : 0));
        s.bind(9, trackId);
        s.step(); s.finalize();
        appendHistory(trackId, r);
    }

    /// Load the persisted detail. `algo`/`version` non-empty → caller
    /// checks identity match themselves via `valid()`.
    std::optional<KeyAnalysisResult> load(int64_t trackId) {
        auto s = db_.prepare(
            "SELECT key_index,key_confidence,key_algo,key_version,key_at,"
            "key_alt_json,key_segments_json,key_ambiguous"
            " FROM analysis WHERE track_id=?");
        s.bind(1, trackId);
        if (!s.step()) { s.finalize(); return std::nullopt; }
        KeyAnalysisResult r;
        r.primary = MusicalKey::fromIndex((int)s.colInt(0),
                                        (float)s.colDouble(1));
        r.algorithm  = s.colText(2);
        r.version    = s.colText(3);
        r.analyzedAt = s.colInt(4);
        parseAlternatives(s.colText(5), r);
        parseSegments(s.colText(6), r);
        r.ambiguous = s.colInt(7) != 0;
        s.finalize();
        return r;
    }

    /// §21 — cache-validity test: identity match on algo+version, and
    /// a real row was produced (analyzed_at > 0).
    static bool valid(const KeyAnalysisResult& r,
                      const std::string& algo, const std::string& version) {
        return r.analyzedAt > 0 && r.algorithm == algo
            && r.version == version;
    }

    /// §40 — full history for a track, newest first.
    struct HistoryEntry {
        int64_t     analyzedAt { 0 };
        std::string algo, version;
        int         quality   { 0 };
        int         keyIndex  { -1 };
        float       confidence { 0.0f };
        bool        ambiguous { false };
    };
    std::vector<HistoryEntry> history(int64_t trackId) {
        std::vector<HistoryEntry> out;
        auto s = db_.prepare(
            "SELECT analyzed_at,algo,version,quality,key_index,confidence,"
            "ambiguous FROM key_history WHERE track_id=?"
            " ORDER BY analyzed_at DESC");
        s.bind(1, trackId);
        while (s.step()) {
            HistoryEntry h;
            h.analyzedAt = s.colInt(0);
            h.algo       = s.colText(1);
            h.version    = s.colText(2);
            h.quality    = (int)s.colInt(3);
            h.keyIndex   = (int)s.colInt(4);
            h.confidence = (float)s.colDouble(5);
            h.ambiguous  = s.colInt(6) != 0;
            out.push_back(h);
        }
        s.finalize();
        return out;
    }

private:
    static std::string alternativesJson(const KeyAnalysisResult& r) {
        if (r.alternatives.empty()) return {};
        std::string s = "[";
        for (size_t i = 0; i < r.alternatives.size(); ++i) {
            const auto& a = r.alternatives[i];
            char buf[48];
            std::snprintf(buf, sizeof buf, "[%d,%.4f]",
                          a.key.index(), (double)a.correlation);
            s += buf;
            if (i + 1 < r.alternatives.size()) s += ",";
        }
        return s + "]";
    }
    static std::string segmentsJson(const KeyAnalysisResult& r) {
        if (r.segments.empty()) return {};
        std::string s = "[";
        for (size_t i = 0; i < r.segments.size(); ++i) {
            const auto& g = r.segments[i];
            char buf[80];
            std::snprintf(buf, sizeof buf, "[%lld,%lld,%d,%.4f]",
                          (long long)g.startFrame, (long long)g.endFrame,
                          g.key.index(), (double)g.key.confidence);
            s += buf;
            if (i + 1 < r.segments.size()) s += ",";
        }
        return s + "]";
    }
    static void parseAlternatives(const std::string& j,
                                  KeyAnalysisResult& r) {
        const char* p = j.c_str();
        while ((p = std::strstr(p, "[")) && p[1] != ']') {
            ++p;
            if (!std::isdigit((unsigned char)*p) && *p != '-') continue;
            const int idx = (int)std::strtoll(p, (char**)&p, 10);
            while (*p == ' ' || *p == ',') ++p;
            const double corr = std::strtod(p, (char**)&p);
            r.alternatives.push_back(
                { MusicalKey::fromIndex(idx), (float)corr });
        }
    }
    static void parseSegments(const std::string& j, KeyAnalysisResult& r) {
        const char* p = j.c_str();
        while ((p = std::strstr(p, "[")) && p[1] != ']') {
            ++p;
            if (!std::isdigit((unsigned char)*p) && *p != '-') continue;
            TonalSegment g;
            g.startFrame = std::strtoll(p, (char**)&p, 10);
            while (*p == ' ' || *p == ',') ++p;
            g.endFrame = std::strtoll(p, (char**)&p, 10);
            while (*p == ' ' || *p == ',') ++p;
            const int idx = (int)std::strtoll(p, (char**)&p, 10);
            while (*p == ' ' || *p == ',') ++p;
            const double conf = std::strtod(p, (char**)&p);
            g.key = MusicalKey::fromIndex(idx, (float)conf);
            r.segments.push_back(g);
        }
    }

    void appendHistory(int64_t trackId, const KeyAnalysisResult& r) {
        auto s = db_.prepare(
            "INSERT INTO key_history(track_id,algo,version,quality,"
            "key_index,confidence,ambiguous,analyzed_at,detail_json)"
            " VALUES(?,?,?,?,?,?,?,?,?)");
        s.bind(1, trackId);
        s.bind(2, r.algorithm);
        s.bind(3, r.version);
        s.bind(4, (int64_t)r.quality);
        s.bind(5, (int64_t)r.primary.index());
        s.bind(6, (double)r.primary.confidence);
        s.bind(7, (int64_t)(r.ambiguous ? 1 : 0));
        s.bind(8, r.analyzedAt > 0 ? r.analyzedAt
                                 : (int64_t)::time(nullptr));
        s.bind(9, alternativesJson(r));
        s.step(); s.finalize();
    }

    Database& db_;
};

} // namespace onda::harmonic
