#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../performance/PerformanceTypes.h"
#include "Database.h"

namespace onda {

/// Persistence for performance data: hot/memory/loop cues, saved loops
/// and per-session key-values (e.g. quantize settings). All writes are
/// small, write-through, and happen OFF the audio thread — the RT side
/// only ever sees resolved frame numbers.
class PerformanceStore {
public:
    explicit PerformanceStore(Database& db) : db_(db) {}

    // ---- cues ----
    void saveCue(int64_t trackId, const Cue& c) {
        auto s = db_.prepare(
            "INSERT OR REPLACE INTO cues(track_id,slot,type,frame,frame2,"
            "beat,beat2,argb,label,created_at,kind,enabled,quantized,"
            "comment) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        s.bind(1, trackId);           s.bind(2, (int64_t)c.slot);
        s.bind(3, (int64_t)c.type);   s.bind(4, c.frame);
        s.bind(5, c.frame2);          s.bind(6, c.beat);
        s.bind(7, c.beat2);           s.bind(8, (int64_t)c.argb);
        s.bind(9, c.label);           s.bind(10, c.createdAt);
        s.bind(11, (int64_t)c.kind);
        s.bind(12, (int64_t)(c.enabled ? 1 : 0));
        s.bind(13, (int64_t)(c.quantized ? 1 : 0));
        s.bind(14, c.comment);
        s.step(); s.finalize();
    }

    void deleteCue(int64_t trackId, int slot, int type) {
        auto s = db_.prepare(
            "DELETE FROM cues WHERE track_id=? AND slot=? AND type=?");
        s.bind(1, trackId); s.bind(2, (int64_t)slot); s.bind(3, (int64_t)type);
        s.step(); s.finalize();
    }

    void deleteAllCues(int64_t trackId) {
        auto s = db_.prepare("DELETE FROM cues WHERE track_id=?");
        s.bind(1, trackId); s.step(); s.finalize();
    }

    std::vector<Cue> loadCues(int64_t trackId) {
        std::vector<Cue> out;
        auto s = db_.prepare(
            "SELECT slot,type,frame,frame2,beat,beat2,argb,label,created_at,"
            "kind,enabled,quantized,comment"
            " FROM cues WHERE track_id=? ORDER BY slot");
        s.bind(1, trackId);
        while (s.step()) {
            Cue c;
            c.slot = (int)s.colInt(0); c.type = (CueType)(int)s.colInt(1);
            c.frame = s.colInt(2);     c.frame2 = s.colInt(3);
            c.beat = s.colDouble(4);   c.beat2 = s.colDouble(5);
            c.argb = (uint32_t)s.colInt(6);
            c.label = s.colText(7);    c.createdAt = s.colInt(8);
            c.kind = (HotKind)(int)s.colInt(9);
            c.enabled   = s.colInt(10) != 0;
            c.quantized = s.colInt(11) != 0;
            c.comment   = s.colText(12);
            out.push_back(std::move(c));
        }
        s.finalize();
        return out;
    }

    // ---- saved loops ----
    void saveLoop(int64_t trackId, const SavedLoop& l) {
        auto s = db_.prepare(
            "INSERT OR REPLACE INTO saved_loops(track_id,slot,in_frame,"
            "out_frame,in_beat,out_beat,argb,label) VALUES(?,?,?,?,?,?,?,?)");
        s.bind(1, trackId);        s.bind(2, (int64_t)l.slot);
        s.bind(3, l.inFrame);      s.bind(4, l.outFrame);
        s.bind(5, l.inBeat);       s.bind(6, l.outBeat);
        s.bind(7, (int64_t)l.argb); s.bind(8, l.label);
        s.step(); s.finalize();
    }

    void deleteLoop(int64_t trackId, int slot) {
        auto s = db_.prepare(
            "DELETE FROM saved_loops WHERE track_id=? AND slot=?");
        s.bind(1, trackId); s.bind(2, (int64_t)slot); s.step(); s.finalize();
    }

    std::vector<SavedLoop> loadLoops(int64_t trackId) {
        std::vector<SavedLoop> out;
        auto s = db_.prepare(
            "SELECT slot,in_frame,out_frame,in_beat,out_beat,argb,label"
            " FROM saved_loops WHERE track_id=? ORDER BY slot");
        s.bind(1, trackId);
        while (s.step()) {
            SavedLoop l;
            l.slot = (int)s.colInt(0);
            l.inFrame = s.colInt(1);   l.outFrame = s.colInt(2);
            l.inBeat = s.colDouble(3); l.outBeat = s.colDouble(4);
            l.argb = (uint32_t)s.colInt(5); l.label = s.colText(6);
            out.push_back(std::move(l));
        }
        s.finalize();
        return out;
    }

    // ---- key/value session settings ----
    void setKv(const std::string& key, const std::string& value) {
        auto s = db_.prepare(
            "INSERT OR REPLACE INTO kv(key,value) VALUES(?,?)");
        s.bind(1, key); s.bind(2, value); s.step(); s.finalize();
    }

    std::string getKv(const std::string& key,
                      const std::string& def = {}) {
        auto s = db_.prepare("SELECT value FROM kv WHERE key=?");
        s.bind(1, key);
        std::string v = def;
        if (s.step()) v = s.colText(0);
        s.finalize();
        return v;
    }

private:
    Database& db_;
};

} // namespace onda
