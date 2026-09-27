#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include "../data/Database.h"
#include "StemTypes.h"

namespace onda::stems {

/// ------------------------------------------------------------------
/// §29/§48 — stem-set metadata in the library DB (schema v7). Holds
/// ONLY availability records: model/version/quality/format/state +
/// the cache key. Audio lives in the StemCache files — no blobs here.
///
/// Lookup key is `content_hash` (the track's content identity), so a
/// relocated/re-imported file still resolves its stems (§40).
/// ------------------------------------------------------------------
class StemStore {
public:
    enum SetState : int { Pending = 0, Ready = 1, Failed = 2, Orphaned = 3 };

    explicit StemStore(Database& db) : db_(db) {}

    /// Insert-or-replace a set record (content_hash+cache_key unique).
    void save(const StemSetMeta& m, int state) {
        const int64_t now = (int64_t)::time(nullptr);
        auto s = db_.prepare(
            "INSERT OR REPLACE INTO stem_sets(track_id,content_hash,"
            "model_id,model_version,quality,sample_rate,channels,frames,"
            "stem_mask,state,cache_key,created_at,updated_at)"
            " VALUES(?,?,?,?,?,?,?,?,?,?,?,"
            " COALESCE((SELECT created_at FROM stem_sets"
            "          WHERE content_hash=? AND cache_key=?),?),?)");
        s.bind(1, m.trackId);
        s.bind(2, (int64_t)m.contentHash);
        s.bind(3, m.modelId);      s.bind(4, m.modelVersion);
        s.bind(5, (int64_t)m.quality);
        s.bind(6, m.rate);         s.bind(7, (int64_t)m.channels);
        s.bind(8, m.frames);       s.bind(9, (int64_t)m.mask);
        s.bind(10, (int64_t)state); s.bind(11, m.key);
        s.bind(12, (int64_t)m.contentHash); s.bind(13, m.key);
        s.bind(14, now);           s.bind(15, now);
        s.step(); s.finalize();
    }

    /// Latest record for a track identity — any state.
    bool find(uint64_t contentHash, StemSetMeta& out, int* stateOut = nullptr) {
        auto s = db_.prepare(
            "SELECT track_id,content_hash,model_id,model_version,quality,"
            "sample_rate,channels,frames,stem_mask,state,cache_key,created_at"
            " FROM stem_sets WHERE content_hash=?"
            " ORDER BY updated_at DESC LIMIT 1");
        s.bind(1, (int64_t)contentHash);
        if (!s.step()) { s.finalize(); return false; }
        fill(s, out);
        if (stateOut) *stateOut = (int)s.colInt(9);
        s.finalize();
        return true;
    }

    /// Exact-match lookup — same parameters the cache key encodes.
    bool findKey(uint64_t contentHash, const std::string& key,
                 StemSetMeta& out, int* stateOut = nullptr) {
        auto s = db_.prepare(
            "SELECT track_id,content_hash,model_id,model_version,quality,"
            "sample_rate,channels,frames,stem_mask,state,cache_key,created_at"
            " FROM stem_sets WHERE content_hash=? AND cache_key=?");
        s.bind(1, (int64_t)contentHash); s.bind(2, key);
        if (!s.step()) { s.finalize(); return false; }
        fill(s, out);
        if (stateOut) *stateOut = (int)s.colInt(9);
        s.finalize();
        return true;
    }

    void markState(uint64_t contentHash, const std::string& key, int state) {
        auto s = db_.prepare(
            "UPDATE stem_sets SET state=?, updated_at=? "
            "WHERE content_hash=? AND cache_key=?");
        s.bind(1, (int64_t)state);
        s.bind(2, (int64_t)::time(nullptr));
        s.bind(3, (int64_t)contentHash); s.bind(4, key);
        s.step(); s.finalize();
    }

    void deleteKey(uint64_t contentHash, const std::string& key) {
        auto s = db_.prepare(
            "DELETE FROM stem_sets WHERE content_hash=? AND cache_key=?");
        s.bind(1, (int64_t)contentHash); s.bind(2, key);
        s.step(); s.finalize();
    }

    void deleteAll() {
        auto s = db_.prepare("DELETE FROM stem_sets");
        s.step(); s.finalize();
    }

    /// All recorded cache keys — the storage manager walks these to
    /// reconcile files vs rows.
    std::vector<std::string> allKeys() {
        std::vector<std::string> out;
        auto s = db_.prepare("SELECT DISTINCT cache_key FROM stem_sets");
        while (s.step()) out.push_back(s.colText(0));
        s.finalize();
        return out;
    }

    /// Counts by state for the library UI (§47).
    int countByState(int state) {
        auto s = db_.prepare(
            "SELECT COUNT(*) FROM stem_sets WHERE state=?");
        s.bind(1, (int64_t)state);
        const int n = s.step() ? (int)s.colInt(0) : 0;
        s.finalize();
        return n;
    }

    /// Track-facing status rollup (§47): best state across keys —
    /// Ready > Pending > Failed; -1 = no record.
    int statusForTrack(uint64_t contentHash) {
        auto q = db_.prepare(
            "SELECT state FROM stem_sets WHERE content_hash=?");
        q.bind(1, (int64_t)contentHash);
        bool any = false, ready = false, pending = false;
        while (q.step()) {
            any = true;
            const int v = (int)q.colInt(0);
            ready   |= v == Ready;
            pending |= v == Pending;
        }
        q.finalize();
        if (!any)    return -1;
        if (ready)   return Ready;
        if (pending) return Pending;
        return Failed;
    }

private:
    void fill(Database::Stmt& s, StemSetMeta& m) {
        m.trackId      = s.colInt(0);
        m.contentHash  = (uint64_t)s.colInt(1);
        m.modelId      = s.colText(2);
        m.modelVersion = s.colText(3);
        m.quality      = (int)s.colInt(4);
        m.rate         = s.colDouble(5);
        m.channels     = (int)s.colInt(6);
        m.frames       = s.colInt(7);
        m.mask         = (uint32_t)s.colInt(8);
        m.key          = s.colText(10);
        m.createdAt    = s.colInt(11);
    }

    Database& db_;
};

} // namespace onda::stems
