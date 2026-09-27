#include "AnalysisStore.h"

#include "../analysis/KeyAnalyzer.h"
#include "../harmonic/KeyAnalysisCache.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace onda {

static std::string jnum(double v) { // minimal-alloc number formatter
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.4f", v);
    return buf;
}

uint64_t AnalysisStore::fingerprint(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) return 0;
    const auto mt = fs::last_write_time(path, ec);
    uint64_t h = 1469598103934665603ull;
    auto fnv = [&h](const uint8_t* p, size_t n) {
        for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    };
    fnv((const uint8_t*)&size, sizeof(size));
    if (!ec) {
        const long long mtl = mt.time_since_epoch().count();
        fnv((const uint8_t*)&mtl, sizeof(mtl));
    }
    std::ifstream f(path, std::ios::binary);
    if (f) {
        std::vector<uint8_t> buf(65536);
        f.read((char*)buf.data(), (std::streamsize)buf.size());
        fnv(buf.data(), (size_t)f.gcount());
        if (size > 65536) {
            f.clear();
            f.seekg((std::streamoff)(size - 65536), std::ios::beg);
            f.read((char*)buf.data(), (std::streamsize)buf.size());
            fnv(buf.data(), (size_t)f.gcount());
        }
    }
    return h;
}

int64_t AnalysisStore::trackIdFor(const std::string& path) {
    {
        auto s = db_.prepare("SELECT id FROM tracks WHERE path=?");
        s.bind(1, path);
        if (s.step()) return s.colInt(0);
    }
    auto ins = db_.prepare(
        "INSERT INTO tracks(path,fingerprint,updated_at) VALUES(?,0,?)");
    ins.bind(1, path);
    ins.bind(2, (int64_t)::time(nullptr));
    ins.step();
    ins.finalize();
    return db_.lastInsertId();
}

std::optional<std::string> AnalysisStore::pathOf(int64_t trackId) {
    auto s = db_.prepare("SELECT path FROM tracks WHERE id=?");
    s.bind(1, trackId);
    if (s.step()) return s.colText(0);
    return std::nullopt;
}

AnalysisState AnalysisStore::stateFor(const std::string& path) {
    auto s = db_.prepare(
        "SELECT analysis_state, analysis_version, fingerprint FROM tracks WHERE path=?");
    s.bind(1, path);
    if (!s.step()) return AnalysisState::NotAnalyzed;
    const int state = (int)s.colInt(0);
    const int ver = (int)s.colInt(1);
    const uint64_t fp = (uint64_t)s.colInt(2);
    if (state != (int)AnalysisState::Analyzed)
        return (AnalysisState)state;
    if (ver != kAnalysisVersion || fp != fingerprint(path))
        return AnalysisState::Outdated;
    return AnalysisState::Analyzed;
}

std::string AnalysisStore::serializeGrid(const BeatGrid& g) const {
    std::string s = "{\"fb\":" + std::to_string(g.firstBeatFrame)
        + ",\"bpb\":" + std::to_string(g.beatsPerBar)
        + ",\"dbo\":" + std::to_string(g.downbeatOffset)
        + ",\"segs\":[";
    for (size_t i = 0; i < g.segments.size(); ++i) {
        const auto& t = g.segments[i];
        s += "[" + std::to_string(t.startFrame) + ","
             + std::to_string(t.startBeat) + "," + jnum(t.bpm) + "]";
        if (i + 1 < g.segments.size()) s += ",";
    }
    s += "]}";
    return s;
}

BeatGrid AnalysisStore::deserializeGrid(const std::string& s, double rate) const {
    BeatGrid g;
    g.setRate(rate);
    // Tolerant JSON-lite parse: numbers in order
    // {"fb":N,"bpb":N,"dbo":N,"segs":[[f,b,bpm],...]}
    auto eat = [&](const char* tag) -> double {
        const char* p = std::strstr(s.c_str(), tag);
        return p ? std::strtod(p + std::strlen(tag), nullptr) : 0.0;
    };
    g.firstBeatFrame = (int64_t)eat("\"fb\":");
    g.beatsPerBar    = (int)eat("\"bpb\":");
    g.downbeatOffset = (int)eat("\"dbo\":");
    if (g.beatsPerBar <= 0) g.beatsPerBar = 4;
    const char* segs = std::strstr(s.c_str(), "\"segs\":[");
    if (segs) {
        const char* p = segs + 8;
        while (*p) {
            while (*p && *p != '[' && *p != ']') ++p;
            if (*p != '[') break;
            ++p;
            TempoSegment t;
            t.startFrame = (int64_t)std::strtoll(p, (char**)&p, 10);
            while (*p == ' ' || *p == ',') ++p;
            t.startBeat  = (int64_t)std::strtoll(p, (char**)&p, 10);
            while (*p == ' ' || *p == ',') ++p;
            t.bpm        = std::strtod(p, (char**)&p);
            g.segments.push_back(t);
            while (*p && *p != ']') ++p;
            ++p;
        }
    }
    return g;
}

void AnalysisStore::save(const std::string& path, const AnalysisBundle& b,
                         const std::string& title, const std::string& artist) {
    const int64_t id = trackIdFor(path);
    const uint64_t fp = fingerprint(path);
    const auto& r = b.result;

    {
        auto s = db_.prepare(
            "UPDATE tracks SET fingerprint=?,title=?,artist=?,duration_sec=?,"
            "sample_rate=?,analysis_state=?,analysis_version=?,error=?,updated_at=?"
            " WHERE id=?");
        s.bind(1, (int64_t)fp);
        s.bind(2, title); s.bind(3, artist);
        s.bind(4, r.durationSec); s.bind(5, r.sampleRate);
        s.bind(6, (int64_t)r.state);
        s.bind(7, (int64_t)kAnalysisVersion);
        s.bind(8, r.error);
        s.bind(9, (int64_t)::time(nullptr));
        s.bind(10, id);
        s.step(); s.finalize();
    }
    if (r.state != AnalysisState::Analyzed) return;

    std::string cands = "[";
    for (size_t i = 0; i < r.bpm.candidates.size(); ++i) {
        cands += "[" + jnum(r.bpm.candidates[i].bpm) + ","
               + jnum(r.bpm.candidates[i].score) + "]";
        if (i + 1 < r.bpm.candidates.size()) cands += ",";
    }
    cands += "]";
    std::string sils = "[";
    for (size_t i = 0; i < r.silences.size(); ++i) {
        sils += "[" + std::to_string(r.silences[i].startFrame) + ","
              + std::to_string(r.silences[i].endFrame) + "]";
        if (i + 1 < r.silences.size()) sils += ",";
    }
    sils += "]";

    // compact JSON packs for the new Phase-14 columns
    std::string trans = "[";
    for (size_t i = 0; i < r.transients.size(); ++i) {
        trans += "[" + std::to_string(r.transients[i].frame) + ","
               + jnum(r.transients[i].strength) + "]";
        if (i + 1 < r.transients.size()) trans += ",";
    }
    trans += "]";
    int64_t compMask = 0;
    for (int c = 0; c < kNumComponents; ++c)
        if (r.components[c].state == (int)AnalysisState::Analyzed)
            compMask |= (int64_t)1 << c;

    {
        auto s = db_.prepare(
            "INSERT INTO analysis(track_id,bpm,bpm_confidence,bpm_stability,"
            "bpm_variable,bpm_candidates,key_index,key_confidence,"
            "first_sound_frame,last_sound_frame,silences,first_onset_frame,"
            "peak,rms,lufs_i,lra,transients,comp_status,needs_review)"
            " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"
            " ON CONFLICT(track_id) DO UPDATE SET bpm=excluded.bpm,"
            "bpm_confidence=excluded.bpm_confidence,"
            "bpm_stability=excluded.bpm_stability,"
            "bpm_variable=excluded.bpm_variable,"
            "bpm_candidates=excluded.bpm_candidates,"
            "key_index=excluded.key_index,"
            "key_confidence=excluded.key_confidence,"
            "first_sound_frame=excluded.first_sound_frame,"
            "last_sound_frame=excluded.last_sound_frame,"
            "silences=excluded.silences,"
            "first_onset_frame=excluded.first_onset_frame,"
            "peak=excluded.peak,rms=excluded.rms,lufs_i=excluded.lufs_i,"
            "lra=excluded.lra,transients=excluded.transients,"
            "comp_status=excluded.comp_status,"
            "needs_review=excluded.needs_review");
        s.bind(1, id);
        s.bind(2, r.bpm.bpm); s.bind(3, (double)r.bpm.confidence);
        s.bind(4, (double)r.bpm.stability);
        s.bind(5, (int64_t)(r.bpm.variable ? 1 : 0));
        s.bind(6, cands);
        s.bind(7, (int64_t)r.key.key); s.bind(8, (double)r.key.confidence);
        s.bind(9, r.firstSoundFrame); s.bind(10, r.lastSoundFrame);
        s.bind(11, sils); s.bind(12, r.bpm.firstOnsetFrame);
        s.bind(13, (double)r.loudness.peak);
        s.bind(14, (double)r.loudness.rms);
        s.bind(15, (double)r.loudness.integratedLufs);
        s.bind(16, (double)r.loudness.loudnessRange);
        s.bind(17, trans);
        s.bind(18, compMask);
        s.bind(19, (int64_t)(r.needsReview ? 1 : 0));
        s.step(); s.finalize();
    }
    saveGrid(id, b.grid, false);

    // Phase 22 — rich key detail (algo/version/alts/segments + history)
    if (b.keyDetail.analyzedAt > 0 || b.keyDetail.primary.valid()) {
        harmonic::KeyAnalysisResult kd = b.keyDetail;
        if (kd.algorithm.empty()) {
            kd.algorithm = "onda.ks"; kd.version = "1.0.0";
            kd.analyzedAt = (int64_t)::time(nullptr);
            if (!kd.primary.valid()) {
                kd.primary = harmonic::MusicalKey::fromIndex(
                    r.key.key, r.key.confidence);
            }
        }
        harmonic::KeyAnalysisCache(db_).save(id, kd);
    }

    // ---- waveform blobs (3-level pyramid) ----
    auto& w = b.waveform;
    const size_t ovBytes = w.overview.size() * sizeof(WaveformBin);
    const size_t mdBytes = w.mid.size() * sizeof(WaveformBin);
    const size_t dtBytes = w.detail.size() * sizeof(WaveformBin);
    {
        auto s = db_.prepare(
            "INSERT INTO waveforms(track_id,channels,num_frames,sample_rate,overview,mid,detail)"
            " VALUES(?,?,?,?,?,?,?)"
            " ON CONFLICT(track_id) DO UPDATE SET channels=excluded.channels,"
            "num_frames=excluded.num_frames,sample_rate=excluded.sample_rate,"
            "overview=excluded.overview,mid=excluded.mid,detail=excluded.detail");
        s.bind(1, id); s.bind(2, (int64_t)w.channels);
        s.bind(3, w.numFrames); s.bind(4, w.sampleRate);
        s.bind(5, w.overview.data(), (int)ovBytes);
        s.bind(6, w.mid.data(), (int)mdBytes);
        s.bind(7, w.detail.data(), (int)dtBytes);
        s.step(); s.finalize();
    }
}

void AnalysisStore::saveGrid(int64_t trackId, const BeatGrid& g, bool manual) {
    auto s = db_.prepare(
        "INSERT INTO beatgrids(track_id,first_beat_frame,beats_per_bar,"
        "downbeat_offset,segments,manual) VALUES(?,?,?,?,?,?)"
        " ON CONFLICT(track_id) DO UPDATE SET first_beat_frame=excluded.first_beat_frame,"
        "beats_per_bar=excluded.beats_per_bar,"
        "downbeat_offset=excluded.downbeat_offset,"
        "segments=excluded.segments,"
        "manual=excluded.manual");
    s.bind(1, trackId); s.bind(2, g.firstBeatFrame);
    s.bind(3, (int64_t)g.beatsPerBar); s.bind(4, (int64_t)g.downbeatOffset);
    s.bind(5, serializeGrid(g)); s.bind(6, (int64_t)(manual ? 1 : 0));
    s.step(); s.finalize();
}

void AnalysisStore::markGridManual(int64_t trackId) {
    auto s = db_.prepare("UPDATE beatgrids SET manual=1 WHERE track_id=?");
    s.bind(1, trackId); s.step(); s.finalize();
}

void AnalysisStore::saveUserGrid(int64_t trackId, const BeatGrid& g) {
    auto s = db_.prepare(
        "INSERT INTO user_overrides(track_id,grid_json,downbeat_offset,updated_at)"
        " VALUES(?,?,?,?)"
        " ON CONFLICT(track_id) DO UPDATE SET grid_json=excluded.grid_json,"
        "downbeat_offset=excluded.downbeat_offset,"
        "updated_at=excluded.updated_at");
    s.bind(1, trackId);
    s.bind(2, serializeGrid(g));
    s.bind(3, (int64_t)g.downbeatOffset);
    s.bind(4, (int64_t)::time(nullptr));
    s.step(); s.finalize();
    markGridManual(trackId); // flag on detected row: user grid in effect
}

void AnalysisStore::clearUserGrid(int64_t trackId) {
    auto s = db_.prepare(
        "INSERT INTO user_overrides(track_id,grid_json,downbeat_offset,updated_at)"
        " VALUES(?,NULL,NULL,?)"
        " ON CONFLICT(track_id) DO UPDATE SET grid_json=NULL,"
        "downbeat_offset=NULL,updated_at=excluded.updated_at");
    s.bind(1, trackId);
    s.bind(2, (int64_t)::time(nullptr));
    s.step(); s.finalize();
    auto u = db_.prepare("UPDATE beatgrids SET manual=0 WHERE track_id=?");
    u.bind(1, trackId); u.step(); u.finalize();
}

std::shared_ptr<AnalysisBundle> AnalysisStore::load(const std::string& path) {
    const int64_t id = trackIdFor(path);
    if (stateFor(path) != AnalysisState::Analyzed) return nullptr;

    auto b = std::make_shared<AnalysisBundle>();
    auto& r = b->result;
    r.state = AnalysisState::Analyzed;
    {
        auto s = db_.prepare(
            "SELECT duration_sec,sample_rate FROM tracks WHERE id=?");
        s.bind(1, id);
        if (s.step()) { r.durationSec = s.colDouble(0); r.sampleRate = s.colDouble(1); }
        s.finalize();
    }
    {
        auto s = db_.prepare(
            "SELECT bpm,bpm_confidence,bpm_stability,bpm_variable,"
            "key_index,key_confidence,first_sound_frame,last_sound_frame,"
            "first_onset_frame,peak,rms,lufs_i,lra,transients,"
            "comp_status,needs_review FROM analysis WHERE track_id=?");
        s.bind(1, id);
        if (s.step()) {
            r.bpm.bpm = s.colDouble(0); r.bpm.confidence = (float)s.colDouble(1);
            r.bpm.stability = (float)s.colDouble(2);
            r.bpm.variable = s.colInt(3) != 0;
            r.key.key = (int)s.colInt(4); r.key.confidence = (float)s.colDouble(5);
            r.key.standard = KeyAnalyzer::standardName(r.key.key);
            r.key.camelot  = KeyAnalyzer::camelotName(r.key.key);
            r.firstSoundFrame = s.colInt(6); r.lastSoundFrame = s.colInt(7);
            r.bpm.firstOnsetFrame = s.colInt(8);
            r.loudness.peak = (float)s.colDouble(9);
            r.loudness.rms = (float)s.colDouble(10);
            r.loudness.integratedLufs = (float)s.colDouble(11);
            r.loudness.loudnessRange = (float)s.colDouble(12);
            // transients JSON-lite parse: [[frame,strength],…]
            const std::string t = s.colText(13);
            const char* p = t.c_str();
            while ((p = std::strstr(p, "[")) && p[1] != ']') {
                ++p;
                TransientEvent tr;
                tr.frame = std::strtoll(p, (char**)&p, 10);
                while (*p == ' ' || *p == ',') ++p;
                tr.strength = (float)std::strtod(p, (char**)&p);
                if (tr.frame > 0) r.transients.push_back(tr);
            }
            const int64_t mask = s.colInt(14);
            for (int c = 0; c < kNumComponents; ++c)
                if (mask & ((int64_t)1 << c))
                    r.components[c].state = (int)AnalysisState::Analyzed;
            r.needsReview = s.colInt(15) != 0;
        }
        s.finalize();
    }
    {
        auto s = db_.prepare("SELECT segments FROM beatgrids WHERE track_id=?");
        s.bind(1, id);
        if (s.step()) b->grid = deserializeGrid(s.colText(0), r.sampleRate);
        s.finalize();
    }
    {
        auto s = db_.prepare(
            "SELECT channels,num_frames,sample_rate,overview,mid,detail FROM waveforms WHERE track_id=?");
        s.bind(1, id);
        if (s.step()) {
            auto& w = b->waveform;
            w.channels = (int)s.colInt(0); w.numFrames = s.colInt(1);
            w.sampleRate = s.colDouble(2);
            const auto ov = s.colBlob(3), md = s.colBlob(4), dt = s.colBlob(5);
            w.overview.resize(ov.size() / sizeof(WaveformBin));
            w.mid.resize(md.size() / sizeof(WaveformBin));
            w.detail.resize(dt.size() / sizeof(WaveformBin));
            std::memcpy(w.overview.data(), ov.data(), ov.size());
            std::memcpy(w.mid.data(), md.data(), md.size());
            std::memcpy(w.detail.data(), dt.data(), dt.size());
        }
        s.finalize();
    }
    // user overrides — separate table, detected data untouched
    {
        auto s = db_.prepare(
            "SELECT bpm,key_index,grid_json,downbeat_offset FROM user_overrides WHERE track_id=?");
        s.bind(1, id);
        if (s.step()) {
            const double ubpm = s.colDouble(0);
            if (ubpm > 0.0) b->overrides.bpm = ubpm;
            const int64_t ukey = s.colInt(1);
            if (ukey >= 0) b->overrides.keyIndex = (int)ukey;
            const std::string g = s.colText(2);
            if (!g.empty())
                b->overrides.grid = deserializeGrid(g, r.sampleRate);
            const int64_t dbo = s.colInt(3);
            if (dbo > 0 && b->overrides.grid)
                b->overrides.grid->downbeatOffset = (int)dbo;
        }
        s.finalize();
    }
    r.numFrames = b->waveform.numFrames;
    return b;
}

std::optional<harmonic::KeyAnalysisResult> AnalysisStore::loadKeyDetail(
    int64_t trackId) {
    return harmonic::KeyAnalysisCache(db_).load(trackId);
}

void AnalysisStore::setUserBpm(int64_t trackId, double bpm) {
    auto s = db_.prepare(
        "INSERT INTO user_overrides(track_id,bpm,updated_at) VALUES(?,?,?)"
        " ON CONFLICT(track_id) DO UPDATE SET bpm=excluded.bpm,"
        "updated_at=excluded.updated_at");
    s.bind(1, trackId);
    if (bpm > 0.0) s.bind(2, bpm); else s.bindNull(2);
    s.bind(3, (int64_t)::time(nullptr));
    s.step(); s.finalize();
}

void AnalysisStore::setUserKey(int64_t trackId, int keyIndex) {
    auto s = db_.prepare(
        "INSERT INTO user_overrides(track_id,key_index,updated_at) VALUES(?,?,?)"
        " ON CONFLICT(track_id) DO UPDATE SET key_index=excluded.key_index,"
        "updated_at=excluded.updated_at");
    s.bind(1, trackId);
    if (keyIndex >= 0) s.bind(2, (int64_t)keyIndex); else s.bindNull(2);
    s.bind(3, (int64_t)::time(nullptr));
    s.step(); s.finalize();
}

std::optional<double> AnalysisStore::userBpm(int64_t trackId) const {
    Database& d = const_cast<Database&>(db_);
    auto s = d.prepare("SELECT bpm FROM user_overrides WHERE track_id=?");
    s.bind(1, trackId);
    if (s.step()) {
        // column may be NULL -> sqlite reports 0; distinguish by count
        const double v = s.colDouble(0);
        if (v > 0.0) return v;
    }
    return std::nullopt;
}

double AnalysisStore::effectiveBpm(int64_t trackId, double detected) const {
    const auto u = userBpm(trackId);
    return u.has_value() ? *u : detected;
}

int AnalysisStore::invalidateStale() {
    int n = 0;
    auto s = db_.prepare(
        "SELECT id,path,fingerprint,analysis_version FROM tracks WHERE analysis_state=2");
    std::vector<std::pair<int64_t, std::string>> rows;
    std::vector<int64_t> fps, vers;
    while (s.step()) {
        rows.emplace_back(s.colInt(0), s.colText(1));
        fps.push_back(s.colInt(2)); vers.push_back(s.colInt(3));
    }
    s.finalize();
    for (size_t i = 0; i < rows.size(); ++i) {
        const bool stale = vers[i] != kAnalysisVersion ||
                           (uint64_t)fps[i] != fingerprint(rows[i].second);
        if (stale) {
            auto u = db_.prepare(
                "UPDATE tracks SET analysis_state=4 WHERE id=?"); // Outdated
            u.bind(1, rows[i].first); u.step(); u.finalize();
            ++n;
        }
    }
    return n;
}

} // namespace onda
