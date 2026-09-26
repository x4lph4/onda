#pragma once

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include "MiniJson.h"
#include "PlaylistManager.h"
#include "TrackRepository.h"

namespace onda::library {

/// §48-50 — library backup / export / import.
///
/// Export never bundles audio files — only library metadata:
/// user fields (rating/color/energy/comments/edited flags/tags),
/// file metadata snapshot, playlists+crates with item order,
/// smart-playlist rules, folders, and the global tag list.
///
/// Import matches rows by content_hash first, then by path, and
/// applies the chosen conflict policy — it never deletes anything.
enum class BackupConflict { Skip, Merge, Replace };

struct BackupResult {
    int tracks { 0 }, applied { 0 }, skipped { 0 },
        playlists { 0 }, errors { 0 };
    std::string detail;
};

class LibraryBackup {
public:
    static constexpr int kVersion = 1;

    LibraryBackup(TrackRepository& repo, PlaylistManager& pl)
        : repo_(repo), pl_(pl) {}

    // ============================================================ JSON
    /// Full library dump → pretty-printed JSON file.
    bool exportJson(const std::string& outPath, std::string& err) {
        MiniJson root = MiniJson::object();
        root.set("format", "onda-library");
        root.set("version", kVersion);
        root.set("exported_at", (int64_t)std::time(nullptr));

        // ---- tracks (all, paged internally)
        MiniJson trs = MiniJson::array();
        QuerySpec q; q.limit = 5000;
        for (int off = 0;; off += q.limit) {
            TrackPage page = repo_.query(q);
            for (const auto& t : page.items)
                trs.push(trackToJson(t));
            if ((int)page.items.size() < q.limit) break;
            q.offset = off + q.limit;
        }
        root.set("tracks", std::move(trs));

        // ---- folders + playlists
        MiniJson fols = MiniJson::array();
        for (const auto& f : pl_.folders(-1)) {
            MiniJson o = MiniJson::object();
            o.set("id", f.id); o.set("parent_id", f.parentId);
            o.set("name", f.name);
            fols.push(std::move(o));
        }
        root.set("folders", std::move(fols));

        MiniJson pls = MiniJson::array();
        for (const auto& p : pl_.playlists(-1)) {
            MiniJson o = MiniJson::object();
            o.set("name", p.name);
            o.set("folder_id", p.folderId);
            o.set("kind", p.kind == PlaylistKind::Smart ? "smart" : "static");
            if (p.kind == PlaylistKind::Smart)
                o.set("rules", MiniJson::parse(p.rulesJson));
            else {
                MiniJson items = MiniJson::array();
                for (int64_t tid : pl_.trackIds(p.id)) {
                    Track t;
                    if (repo_.getById(tid, t)) {
                        MiniJson ref = MiniJson::object();
                        ref.set("content_hash", u64str(t.contentHash));
                        ref.set("path", t.path);
                        items.push(std::move(ref));
                    }
                }
                o.set("items", std::move(items));
            }
            pls.push(std::move(o));
        }
        root.set("playlists", std::move(pls));

        return writeFile(outPath, root.dump(1), err);
    }

    /// Import a previously exported library. Conflict policy applies
    /// when a matching track already exists (matched by content hash,
    /// then path). Never deletes rows/files — §50.
    BackupResult importJson(const std::string& inPath,
                            BackupConflict policy) {
        BackupResult res;
        std::string data;
        if (!readFile(inPath, data)) { res.detail = "cannot read file"; ++res.errors; return res; }
        bool ok = false;
        const MiniJson root = MiniJson::parse(data, ok);
        if (!ok || root.at("format").str() != "onda-library") {
            res.detail = "not an onda-library export";
            ++res.errors;
            return res;
        }

        // ---- tracks
        for (const auto& e : root.at("tracks").arr()) {
            ++res.tracks;
            const uint64_t ch = u64val(e.at("content_hash"));
            const std::string path = e.at("path").str();
            Track t;
            const bool found = findTrack(ch, path, t);
            if (!found) { ++res.skipped; continue; }

            const bool hasUser = e.at("rating").isNum()
                || e.at("color").isNum() || e.at("comments").isStr();
            if (policy == BackupConflict::Skip && hasUser) {
                ++res.skipped; continue;
            }
            applyUserFields(t, e, policy == BackupConflict::Merge);
            if (e.at("tags").isArr()) {
                std::vector<std::string> tags;
                if (policy == BackupConflict::Merge)
                    tags = repo_.tagsOf(t.id);
                for (const auto& tv : e.at("tags").arr()) {
                    const std::string n = tv.str();
                    bool dup = false;
                    for (auto& x : tags) if (x == n) { dup = true; break; }
                    if (!dup) tags.push_back(n);
                }
                repo_.setTags(t.id, tags);
            }
            ++res.applied;
        }

        // ---- playlists (created if missing; items append)
        for (const auto& e : root.at("playlists").arr()) {
            ++res.playlists;
            const std::string name = e.at("name").str();
            const bool smart = e.at("kind").str() == "smart";
            int64_t pid = playlistIdFor(name);
            if (pid < 0) {
                pid = smart
                    ? pl_.createSmart(name, e.at("rules").dump())
                    : pl_.create(name);
            }
            if (pid < 0) continue;
            if (smart) {
                if (policy == BackupConflict::Replace)
                    pl_.setRules(pid, e.at("rules").dump());
                continue;
            }
            if (policy == BackupConflict::Replace)
                pl_.setOrder(pid, {});
            for (const auto& it : e.at("items").arr()) {
                const int64_t tid = idForRef(it);
                if (tid >= 0) pl_.addTrack(pid, tid);
            }
        }
        res.detail = "ok";
        return res;
    }

    // ============================================================= CSV
    /// Flat per-track metadata export (spreadsheet-friendly).
    /// One row per track; tags joined with '|'; proper CSV quoting.
    bool exportCsv(const std::string& outPath, std::string& err) {
        std::string out =
            "path,title,artist,album,album_artist,genre,label,composer,"
            "year,track_num,disc_num,duration_sec,bpm,key,rating,color,"
            "energy,tags,comments,play_count,last_played,date_added,"
            "status,play_state,format\n";
        QuerySpec q; q.limit = 5000;
        for (int off = 0;; off += q.limit) {
            TrackPage page = repo_.query(q);
            for (const auto& t : page.items) {
                out += csv(t.path) + ',' + csv(t.title) + ','
                     + csv(t.artist) + ',' + csv(t.album) + ','
                     + csv(t.albumArtist) + ',' + csv(t.genre) + ','
                     + csv(t.label) + ',' + csv(t.composer) + ','
                     + std::to_string(t.year) + ','
                     + std::to_string(t.trackNum) + ','
                     + std::to_string(t.discNum) + ','
                     + numStr(t.durationSec) + ','
                     + numStr(t.bpm) + ','
                     + std::to_string(t.keyIndex) + ','
                     + std::to_string(t.rating) + ','
                     + std::to_string(t.color) + ','
                     + std::to_string(t.energy) + ','
                     + csv(tagsJoined(t.tags)) + ','
                     + csv(t.comments) + ','
                     + std::to_string(t.playCount) + ','
                     + std::to_string(t.lastPlayed) + ','
                     + std::to_string(t.dateAdded) + ','
                     + std::to_string(t.status) + ','
                     + std::to_string(t.playState) + ','
                     + csv(t.format) + '\n';
            }
            if ((int)page.items.size() < q.limit) break;
            q.offset = off + q.limit;
        }
        return writeFile(outPath, out, err);
    }

private:
    // ---- track <-> json
    static MiniJson trackToJson(const Track& t) {
        MiniJson o = MiniJson::object();
        o.set("path", t.path);
        o.set("filename", t.filename);
        // 64-bit ids as decimal strings — JSON doubles would lose
        // low bits and break hash matching on import
        o.set("content_hash", u64str(t.contentHash));
        o.set("fingerprint", u64str(t.fingerprint));
        o.set("title", t.title);       o.set("artist", t.artist);
        o.set("album", t.album);       o.set("album_artist", t.albumArtist);
        o.set("genre", t.genre);       o.set("label", t.label);
        o.set("composer", t.composer);
        o.set("year", t.year);         o.set("track_num", t.trackNum);
        o.set("disc_num", t.discNum);
        o.set("duration_sec", t.durationSec);
        o.set("bpm", t.bpm);           o.set("key_index", t.keyIndex);
        o.set("rating", t.rating);     o.set("color", t.color);
        o.set("energy", t.energy);     o.set("comments", t.comments);
        o.set("edited_fields", (int64_t)t.editedFields);
        o.set("play_count", t.playCount);
        o.set("last_played", t.lastPlayed);
        o.set("date_added", t.dateAdded);
        MiniJson tags = MiniJson::array();
        for (const auto& n : t.tags) tags.push(n);
        o.set("tags", std::move(tags));
        return o;
    }

    bool findTrack(uint64_t contentHash, const std::string& path,
                   Track& out) {
        if (contentHash != 0) {
            const auto ids = repo_.idsByContentHash(contentHash);
            if (!ids.empty()) return repo_.getById(ids[0], out);
        }
        return repo_.getByPath(path, out);
    }

    int64_t idForRef(const MiniJson& ref) {
        const uint64_t ch = u64val(ref.at("content_hash"));
        if (ch != 0) {
            const auto ids = repo_.idsByContentHash(ch);
            if (!ids.empty()) return ids[0];
        }
        return repo_.idForPath(ref.at("path").str());
    }

    /// Write user fields; `merge` keeps non-zero local values instead
    /// of overwriting them.
    void applyUserFields(Track& t, const MiniJson& e, bool merge) {
        const int rating  = merge && t.rating != 0 ? t.rating
                          : (int)e.at("rating").integer();
        const int color   = merge && t.color != 0 ? t.color
                          : (int)e.at("color").integer();
        const int energy  = merge && t.energy != 0 ? t.energy
                          : (int)e.at("energy").integer();
        const std::string comments = merge && !t.comments.empty()
                          ? t.comments : e.at("comments").str();
        repo_.setUserFields(t.id, rating, color, energy, comments);
        // play_count/last_played derive from history rows — not
        // re-synthesised on import (documented limit).
    }

    int64_t playlistIdFor(const std::string& name) {
        for (const auto& p : pl_.playlists(-1))
            if (p.name == name) return p.id;
        return -1;
    }

    // ---- io helpers
    static std::string u64str(uint64_t v) {
        char b[24]; std::snprintf(b, sizeof(b), "%llu",
                                  (unsigned long long)v);
        return b;
    }
    /// Hash may arrive as a decimal string (v1 files) or a number.
    static uint64_t u64val(const MiniJson& v) {
        if (v.isStr())
            return (uint64_t)std::strtoull(v.str().c_str(), nullptr, 10);
        return (uint64_t)v.integer();
    }
    static std::string csv(const std::string& s) {
        if (s.find_first_of(",\"\n\r") == std::string::npos) return s;
        std::string out = "\"";
        for (char c : s) { if (c == '"') out += '"'; out += c; }
        return out + '"';
    }
    static std::string numStr(double d) {
        char b[32]; std::snprintf(b, sizeof(b), "%.3f", d);
        return b;
    }
    static std::string tagsJoined(const std::vector<std::string>& tags) {
        std::string out;
        for (const auto& t : tags) {
            if (!out.empty()) out += '|';
            out += t;
        }
        return out;
    }
    static bool writeFile(const std::string& path,
                          const std::string& data, std::string& err) {
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path(path).parent_path(), ec);
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) { err = "cannot write " + path; return false; }
        const size_t w = std::fwrite(data.data(), 1, data.size(), f);
        std::fclose(f);
        if (w != data.size()) { err = "short write"; return false; }
        return true;
    }
    static bool readFile(const std::string& path, std::string& out) {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        out.resize((size_t)std::max<long>(n, 0));
        const size_t r = std::fread(out.data(), 1, out.size(), f);
        std::fclose(f);
        out.resize(r);
        return true;
    }

    TrackRepository& repo_;
    PlaylistManager& pl_;
};

} // namespace onda::library
