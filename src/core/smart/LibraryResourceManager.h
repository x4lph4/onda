#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include "../data/Database.h"

namespace onda::smart {

/// Phase 23 §41-44 — user-visible resource policy + cache cleanup.
///
/// Persisted in `kv` (same store other settings use). Cleanup only
/// ever touches ANALYSIS DERIVED data: waveforms/analysis-derived
/// columns can be regenerated; user metadata (cues, loops, ratings,
/// tags, comments, playlists, history) is NEVER removed (§44).
struct ResourceLimits {
    int  analysisWorkers { 2 };         // requested worker count
    bool gpuAllowed      { false };     // §35 — opt-in, never required
    int64_t artworkCacheMb  { 256 };    // soft cap for artwork dir
    int64_t analysisCacheMb { 1024 };   // stems+waveforms soft cap
    int  analyzeOnImport { 1 };         // §42: 1=on import, 0=manual,
                                        //      2=when idle
};

class LibraryResourceManager {
public:
    explicit LibraryResourceManager(Database& db) : db_(db) {}

    // ---- settings (kv) -------------------------------------------
    ResourceLimits limits() {
        std::lock_guard lk(db_.mutex());
        ResourceLimits l;
        l.analysisWorkers  = getInt("res.analysis_workers", 2);
        l.gpuAllowed       = getInt("res.gpu_allowed", 0) != 0;
        l.artworkCacheMb   = getInt("res.artwork_cache_mb", 256);
        l.analysisCacheMb  = getInt("res.analysis_cache_mb", 1024);
        l.analyzeOnImport  = getInt("res.analyze_on_import", 1);
        return l;
    }
    void setLimits(const ResourceLimits& l) {
        std::lock_guard lk(db_.mutex());
        setInt("res.analysis_workers", l.analysisWorkers);
        setInt("res.gpu_allowed", l.gpuAllowed ? 1 : 0);
        setInt("res.artwork_cache_mb", l.artworkCacheMb);
        setInt("res.analysis_cache_mb", l.analysisCacheMb);
        setInt("res.analyze_on_import", l.analyzeOnImport);
    }

    // ---- §44 cache hygiene (analysis data only) -------------------
    struct CacheStats {
        int64_t waveforms { 0 }, analysis { 0 }, stemSets { 0 };
        int64_t autoTags { 0 }, keyHistory { 0 };
    };
    CacheStats cacheStats() {
        std::lock_guard lk(db_.mutex());
        CacheStats c;
        auto one = [&](const char* s) {
            auto st = db_.prepare(s);
            return st.step() ? st.colInt(0) : 0;
        };
        c.waveforms  = one("SELECT COUNT(*) FROM waveforms");
        c.analysis   = one("SELECT COUNT(*) FROM analysis");
        c.stemSets   = one("SELECT COUNT(*) FROM stem_sets");
        c.autoTags   = one("SELECT COUNT(*) FROM auto_tags");
        c.keyHistory = one("SELECT COUNT(*) FROM key_history");
        return c;
    }

    /// Clear stale/orphaned analysis rows: analysis + waveform rows
    /// whose track no longer exists. User data is not in these
    /// tables by construction.
    int cleanOrphanedAnalysis() {
        std::lock_guard lk(db_.mutex());
        Database::Tx tx(db_);
        auto a = db_.prepare(
            "DELETE FROM analysis WHERE track_id NOT IN"
            " (SELECT id FROM tracks)");
        a.step();
        const int nA = changes();
        auto w = db_.prepare(
            "DELETE FROM waveforms WHERE track_id NOT IN"
            " (SELECT id FROM tracks)");
        w.step();
        const int nW = changes();
        auto k = db_.prepare(
            "DELETE FROM key_history WHERE track_id NOT IN"
            " (SELECT id FROM tracks)");
        k.step();
        tx.commit();
        return nA + nW + (int)changes();
    }

private:
    int64_t changes() {
        auto s = db_.prepare("SELECT changes()");
        return s.step() ? s.colInt(0) : 0;
    }
    int64_t getInt(const char* k, int64_t def) {
        auto s = db_.prepare("SELECT value FROM kv WHERE key=?");
        s.bind(1, k);
        return s.step() ? s.colInt(0) : def;
    }
    void setInt(const char* k, int64_t v) {
        auto s = db_.prepare(
            "INSERT INTO kv(key,value) VALUES(?,?)"
            " ON CONFLICT(key) DO UPDATE SET value=excluded.value");
        s.bind(1, k); s.bind(2, v);
        s.step();
    }
    Database& db_;
};

} // namespace onda::smart
