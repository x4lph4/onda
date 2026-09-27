#pragma once

#include <functional>
#include <string>
#include <vector>

#include "../library/LibraryTypes.h"
#include "Compatibility.h"

namespace onda::harmonic {

/// ------------------------------------------------------------------
/// §26 — related-track suggestions. Given a reference track (key+bpm)
/// it runs ONE library query with the configured criteria, then
/// describes each hit's relationship. Suggestions ONLY — never loads
/// or decides (§27).
/// ------------------------------------------------------------------
struct RelatedCriteria {
    bool  sameKeyOnly       { false }; // exact key only
    bool  strictCamelot     { true };  // wheel adjacency (else broad set)
    bool  useBpm            { true };
    double bpmTolerancePct  { 6.0 };   // ±% around reference bpm
    float minKeyConfidence  { 0.0f };  // 0 = don't filter on confidence
    int   limit             { 64 };
    int64_t excludeTrackId  { -1 };    // the reference track itself
};

struct RelatedResult {
    library::Track   track;
    HarmonicRelation keyRel;
    TempoRelation    tempoRel;
    std::string      explanation;      // criteria that produced the hit
};

class RelatedTracksEngine {
public:
    /// `queryFn` runs a QuerySpec against the library — injected so the
    /// engine stays UI/DB-agnostic and test-friendly.
    using QueryFn =
        std::function<library::TrackPage(const library::QuerySpec&)>;

    static std::vector<RelatedResult> suggest(
        const MusicalKey& refKey, double refBpm,
        const RelatedCriteria& c, const QueryFn& queryFn) {
        std::vector<RelatedResult> out;
        if (!refKey.valid() || !queryFn) return out;

        library::QuerySpec q;
        q.limit = c.limit;
        library::FilterCond f;
        // key set — strict wheel or the broader harmonic set
        const auto keys = c.sameKeyOnly
            ? std::vector<int>{ refKey.index() }
            : (c.strictCamelot
                 ? CamelotWheel::compatibleIndices(refKey.index())
                 : broadKeys(refKey.index()));
        if (keys.empty()) return out;
        f.field = "key_index"; f.op = library::CmpOp::In; f.ints = keys;
        q.filters.push_back(f);
        if (c.useBpm && refBpm > 0.0) {
            const double lo = refBpm * (1.0 - c.bpmTolerancePct / 100.0);
            const double hi = refBpm * (1.0 + c.bpmTolerancePct / 100.0);
            f = {}; f.field = "bpm"; f.op = library::CmpOp::Between;
            f.a = lo; f.b = hi;
            q.filters.push_back(f);
        }
        if (c.minKeyConfidence > 0.0f) {
            f = {}; f.field = "keyconf"; f.op = library::CmpOp::Gte;
            f.a = c.minKeyConfidence;
            q.filters.push_back(f);
        }
        auto page = queryFn(q);
        for (auto& t : page.items) {
            if (t.id == c.excludeTrackId) continue;
            RelatedResult r;
            r.track = t;
            const auto cand = MusicalKey::fromIndex(t.keyIndex,
                                                    t.keyConfidence);
            r.keyRel   = HarmonicCompatibilityEngine::relate(refKey, cand);
            r.tempoRel = TempoCompatibility::relate(refBpm, t.bpm);
            // §26 — surface the criteria that produced the row.
            r.explanation = r.keyRel.explanation;
            if (c.useBpm) r.explanation += " · " + r.tempoRel.explanation;
            out.push_back(std::move(r));
        }
        return out;
    }

private:
    /// The broader pre-Phase-22 neighbourhood (semitone/fifth/relative).
    static std::vector<int> broadKeys(int keyIndex) {
        if (keyIndex < 0 || keyIndex > 23) return {};
        const bool minor = keyIndex >= 12;
        const int  pc    = keyIndex % 12;
        auto rel = [&](int p, bool m) { return m ? p + 12 : p; };
        return { keyIndex, rel((pc + 1) % 12, minor),
                 rel((pc + 11) % 12, minor), rel((pc + 7) % 12, minor),
                 rel((pc + 5) % 12, minor), rel(pc, !minor) };
    }
};

} // namespace onda::harmonic
