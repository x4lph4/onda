// Phase 29 — professional library: import queue w/ cancel+resume,
// auto-import roots, missing ops, metadata write-back, tag normalize,
// fuzzy search, playlist meta/sort, smart snapshot, artwork LRU,
// session rollup, TrackReference, DB backup+version gate, semantic
// integrity, palette, 250k perf.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/library/MusicLibraryEngine.h"
#include "core/library/ImportQueue.h"
#include "core/library/ArtworkManager.h"
#include "core/library/TagNormalizer.h"
#include "core/library/SmartPlaylistEngine.h"

using namespace onda;
using namespace onda::library;
using Catch::Approx;
namespace fs = std::filesystem;

namespace {

fs::path tempRoot(const char* name) {
    fs::path d = fs::temp_directory_path() / "onda_libtest29" / name;
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

void writeWav(const fs::path& p, const std::vector<int16_t>& samples) {
    std::ofstream f(p, std::ios::binary);
    const uint32_t dataSize = (uint32_t)(samples.size() * sizeof(int16_t));
    auto w32 = [&](uint32_t v) { f.write((const char*)&v, 4); };
    auto w16 = [&](uint16_t v) { f.write((const char*)&v, 2); };
    f.write("RIFF", 4); w32(36 + dataSize); f.write("WAVE", 4);
    f.write("fmt ", 4); w32(16); w16(1); w16(2);
    w32(48000); w32(48000 * 2 * 2); w16(4); w16(16);
    f.write("data", 4); w32(dataSize);
    f.write((const char*)samples.data(), dataSize);
}

fs::path makeWav(const fs::path& dir, const std::string& name) {
    const int n = 48000 / 4;
    std::vector<int16_t> s((size_t)n * 2);
    for (int i = 0; i < n; ++i)
        s[(size_t)i * 2] = s[(size_t)i * 2 + 1] =
            (int16_t)(12000.0 * std::sin(6.28318530718 * 440.0 * i / 48000.0));
    const fs::path p = dir / name;
    writeWav(p, s);
    return p;
}

struct Lib {
    fs::path dir;
    MusicLibraryEngine engine;
    explicit Lib(const char* name) : dir(tempRoot(name)) {
        REQUIRE(engine.open((dir / "lib.db").string()));
    }
    int64_t add(const char* name, const char* artist,
                double bpm = 0.0, const char* genre = "House",
                int rating = 0) {
        Track t;
        t.path = (dir / name).string();
        t.filename = name;
        t.title = name; t.artist = artist; t.genre = genre;
        t.rating = rating;
        t.contentHash = 0x9E37 + (uint64_t)std::hash<std::string>{}(name);
        t.fingerprint = t.contentHash ^ 0xABCD;
        const int64_t id = engine.tracks().upsert(t);
        REQUIRE(id > 0);
        if (bpm > 0) {
            std::lock_guard lk(engine.db().mutex());
            auto s = engine.db().prepare(
                "INSERT OR REPLACE INTO analysis(track_id,bpm)"
                " VALUES(?,?)");
            s.bind(1, id); s.bind(2, bpm);
            s.step();
        }
        return id;
    }
};

} // namespace

// ===================================================== IMPORT QUEUE (§9-10)
TEST_CASE("ImportQueue: per-item states + progress + done", "[lib29]") {
    Lib lib("queue");
    lib.engine.setMetadataProvider(
        [](const std::string&, TrackMeta& m) { m.title = "x"; return true; });
    lib.engine.startWorker();

    const fs::path a = makeWav(lib.dir, "a.wav");
    const fs::path b = makeWav(lib.dir, "b.wav");
    int progressCalls = 0, doneCalls = 0;
    ImportResult last{};
    lib.engine.importQueue().setProgress(
        [&](const ImportItem&, int, int) { ++progressCalls; });
    lib.engine.importQueue().setDone(
        [&](const ImportResult& r) { ++doneCalls; last = r; });

    const int total = lib.engine.importQueue().enqueue(
        { a.string(), b.string(), "/nonexistent/dead.wav" });
    REQUIRE(total == 3);
    lib.engine.stopWorker();      // drains the posted steps inline

    REQUIRE(doneCalls == 1);
    REQUIRE(last.added == 2);
    REQUIRE(last.failed == 1);
    REQUIRE(progressCalls >= 3);

    const auto items = lib.engine.importQueue().items();
    REQUIRE(items.size() == 3);
    REQUIRE(items[0].state == ImportState::Completed);
    REQUIRE(items[0].trackId > 0);
    REQUIRE(items[1].state == ImportState::Completed);
    REQUIRE(items[2].state == ImportState::Failed);
}

TEST_CASE("ImportQueue: cancel halts, resume completes the rest", "[lib29]") {
    Lib lib("queuecancel");
    lib.engine.setMetadataProvider(
        [](const std::string&, TrackMeta& m) { m.title = "x"; return true; });
    // Standalone queue over the engine's importer with a captured post
    // — the test decides when queued steps run, so cancel is
    // deterministic without racing a real worker.
    std::vector<std::function<void()>> pending;
    ImportQueue q(lib.engine.importer(),
                  [&](std::function<void()> f) {
                      pending.push_back(std::move(f));
                  });
    auto drain = [&] {
        while (!pending.empty()) {
            auto f = std::move(pending.front());
            pending.erase(pending.begin());
            f();
        }
    };

    const fs::path a = makeWav(lib.dir, "c1.wav");
    const fs::path b = makeWav(lib.dir, "c2.wav");
    REQUIRE(q.enqueue({ a.string(), b.string() }) == 2);
    REQUIRE(pending.size() == 1);          // kick posted one step
    q.cancel();
    REQUIRE_FALSE(q.running());
    {
        int cancelled = 0;
        for (const auto& it : q.items())
            if (it.state == ImportState::Cancelled) ++cancelled;
        REQUIRE(cancelled == 2);   // nothing processed before cancel
    }
    drain();                             // stale steps are no-ops
    REQUIRE(lib.engine.trackCount() == 0);

    q.resume();
    drain();                             // each step re-posts the next
    {
        int completed = 0;
        for (const auto& it : q.items())
            if (it.state == ImportState::Completed) ++completed;
        REQUIRE(completed == 2);
        REQUIRE(lib.engine.trackCount() == 2);
    }
}

// ===================================================== AUTO IMPORT (§11-12)
TEST_CASE("Auto-import: per-root flag + persisted watch roots", "[lib29]") {
    Lib lib("autoimport");
    lib.engine.addWatchRoot(lib.dir.string(), true, true);
    REQUIRE(lib.engine.scanner().autoImportFor(
        (lib.dir / "new.wav").string()));
    REQUIRE(lib.engine.setAutoImport(lib.dir.string(), false));
    REQUIRE_FALSE(lib.engine.scanner().autoImportFor(
        (lib.dir / "new.wav").string()));
    REQUIRE_FALSE(lib.engine.setAutoImport("/no/such/root", true));

    // persisted: a fresh engine over the same db restores the roots
    lib.engine.close();
    MusicLibraryEngine e2;
    REQUIRE(e2.open((lib.dir / "lib.db").string()));
    REQUIRE(e2.restoreWatchRoots() == 1);
    REQUIRE(e2.scanner().roots().size() == 1);
    REQUIRE_FALSE(e2.scanner().autoImportFor(
        (lib.dir / "new.wav").string()));   // off flag restored
}

// ===================================================== MISSING OPS (§14)
TEST_CASE("MissingFileManager: ignore/unignore + remove", "[lib29]") {
    Lib lib("missing");
    const int64_t id = lib.add("gone.wav", "A");
    REQUIRE(lib.engine.missing().markMissing(id));
    REQUIRE(lib.engine.missing().missing().size() == 1);

    REQUIRE(lib.engine.missing().ignore(id));
    REQUIRE(lib.engine.missing().missing().empty());   // hidden from list
    Track t; REQUIRE(lib.engine.tracks().getById(id, t));
    REQUIRE(t.status == (int)TrackStatus::Invalid);

    REQUIRE(lib.engine.missing().unignore(id));
    REQUIRE(lib.engine.missing().missing().size() == 1);

    REQUIRE(lib.engine.missing().removeFromCollection(id));
    REQUIRE_FALSE(lib.engine.tracks().getById(id, t));
    REQUIRE(lib.engine.missing().removeMissing({999999}) == 0);
}

// ===================================================== METADATA WRITE (§19)
TEST_CASE("MetadataWriter: explicit write-back via injected provider",
          "[lib29]") {
    Lib lib("metawrite");
    const int64_t id = lib.add("m.wav", "OrigArtist");

    REQUIRE_FALSE(lib.engine.metadataWriter().canWrite());  // none yet
    int calls = 0; std::string wrotePath;
    lib.engine.metadataWriter().setWriter(
        [&](const std::string& p, const Track& t) {
            ++calls; wrotePath = p;
            return t.artist == "NewArtist";
        });
    REQUIRE(lib.engine.metadataWriter().canWrite());

    Track edited;
    REQUIRE(lib.engine.tracks().getById(id, edited));
    edited.artist = "NewArtist";
    REQUIRE(lib.engine.metadataWriter().writeBack(
        id, edited, EditedField::Artist));
    REQUIRE(calls == 1);
    REQUIRE(wrotePath == edited.path);

    Track cur;
    REQUIRE(lib.engine.tracks().getById(id, cur));
    REQUIRE(cur.artist == "NewArtist");                 // DB matches file
    REQUIRE((cur.editedFields & EditedField::Artist) != 0);

    // failed file write leaves the DB untouched
    lib.engine.metadataWriter().setWriter(
        [](const std::string&, const Track&) { return false; });
    edited.artist = "Ghost";
    REQUIRE_FALSE(lib.engine.metadataWriter().writeBack(
        id, edited, EditedField::Artist));
    REQUIRE(lib.engine.tracks().getById(id, cur));
    REQUIRE(cur.artist == "NewArtist");
}

// ===================================================== TAG NORMALIZER (§20)
TEST_CASE("TagNormalizer: whitespace / genre / featuring split", "[lib29]") {
    REQUIRE(TagNormalizer::text("  Daft   Punk  ") == "Daft Punk");
    REQUIRE(TagNormalizer::genre("drum & bass") == "Drum & Bass");
    REQUIRE(TagNormalizer::genre("  EDM ") == "EDM");   // unknown: kept
    const auto pr = TagNormalizer::splitFeaturing(
        "One More Time (feat. Roman Anthony)");
    REQUIRE(pr.first == "One More Time");
    REQUIRE(pr.second == "Roman Anthony");
    REQUIRE(TagNormalizer::splitFeaturing("Solo Cut").second.empty());
}

// ===================================================== FUZZY SEARCH (§22)
TEST_CASE("Fuzzy search: 'daftpunk' finds 'Daft Punk'", "[lib29]") {
    Lib lib("fuzzy");
    lib.add("one.wav", "Daft Punk");
    lib.add("two.wav", "Surgeon");
    lib.add("three.wav", "Daftside");

    const auto page = lib.engine.searchFuzzy("daftpunk");
    REQUIRE(page.total >= 1);
    REQUIRE(page.items[0].artist == "Daft Punk");

    // exact match still wins — fuzzy must not shadow it
    const auto exact = lib.engine.searchFuzzy("surgeon");
    REQUIRE(exact.items[0].artist == "Surgeon");
}

// ===================================================== PLAYLIST META (§29-30)
TEST_CASE("Playlist v10: description/color/sortMode persist + sort",
          "[lib29]") {
    Lib lib("plmeta");
    const int64_t a = lib.add("a.wav", "B", 128.0);
    const int64_t b = lib.add("b.wav", "A", 122.0);
    const int64_t pl = lib.engine.playlists().create("Set");
    lib.engine.playlists().addTrack(pl, a);
    lib.engine.playlists().addTrack(pl, b);

    std::string desc = "closing set";
    int color = 0xFFAA00, sort = (int)PlaylistSort::Bpm;
    REQUIRE(lib.engine.playlists().setMeta(pl, &desc, &color, &sort));

    auto lists = lib.engine.playlists().playlists();
    REQUIRE(lists.size() == 1);
    REQUIRE(lists[0].description == "closing set");
    REQUIRE(lists[0].color == 0xFFAA00);
    REQUIRE(lists[0].sortMode == (int)PlaylistSort::Bpm);

    // tracksView honors sort_mode: b.wav (122) before a.wav (128)
    QuerySpec q;
    auto page = lib.engine.playlists().tracksView(pl, q);
    REQUIRE(page.items.size() == 2);
    REQUIRE(page.items[0].filename == "b.wav");

    // manual keeps insertion order
    int manual = (int)PlaylistSort::Manual;
    REQUIRE(lib.engine.playlists().setMeta(pl, nullptr, nullptr, &manual));
    page = lib.engine.playlists().tracksView(pl, q);
    REQUIRE(page.items[0].filename == "a.wav");
}

// ===================================================== SMART SNAPSHOT (§35)
TEST_CASE("Smart playlist snapshot → static playlist", "[lib29]") {
    Lib lib("snap");
    lib.add("h1.wav", "A", 126.0, "House", 5);
    lib.add("h2.wav", "B", 124.0, "House", 4);
    lib.add("t1.wav", "C", 140.0, "Techno", 5);

    const std::string rules =
        R"({"match":"all","rules":[)"
        R"({"field":"genre","op":"eq","text":"House"},)"
        R"({"field":"bpm","op":"gt","a":123}]})";
    const int64_t smart = lib.engine.playlists().createSmart("peak", rules);
    REQUIRE(smart > 0);

    const int64_t snap =
        lib.engine.playlists().snapshotSmart(smart, "peak-freeze");
    REQUIRE(snap > 0);
    const auto ids = lib.engine.playlists().trackIds(snap);
    REQUIRE(ids.size() == 2);   // both House >123, techno excluded

    // snapshot is static: a new matching track does NOT enter it
    lib.add("h3.wav", "D", 125.0, "House", 5);
    REQUIRE(lib.engine.playlists().trackIds(snap).size() == 2);
    // ...but the live smart view does see it
    QuerySpec q;
    const auto live =
        SmartPlaylistEngine::evaluate(lib.engine.tracks(), rules, q);
    REQUIRE(live.total == 3);
}

// ===================================================== ARTWORK LRU (§41-43)
TEST_CASE("ArtworkManager: thumbnail via provider + bounded LRU",
          "[lib29]") {
    Lib lib("art");
    { std::ofstream c(lib.dir / "cover.jpg"); c << "jpg"; }
    const std::string cover = (lib.dir / "cover.jpg").string();

    int thumbCalls = 0;
    ArtworkManager am([&](int64_t) { return cover; });
    am.setThumbMaker(
        [&](const std::string&, const std::string& dstBase, int px) {
            ++thumbCalls;
            std::ofstream f(dstBase + ".jpg", std::ios::binary);
            const std::string blob((size_t)px * 10, 'x');
            f.write(blob.data(), (std::streamsize)blob.size());
            return std::string("jpg");
        });
    am.setCacheBudget(1024);

    const auto b1 = am.bytes(1, 64);
    REQUIRE(b1 != nullptr);
    REQUIRE_FALSE(b1->empty());
    const auto b2 = am.bytes(1, 64);
    REQUIRE(b2 == b1);                          // same cached object
    REQUIRE(thumbCalls == 1);                   // disk-thumb hit
    REQUIRE(am.cacheCount() == 1);

    // budget: 200-byte thumbs ×8 → oldest evicted under 1 KiB
    for (int64_t i = 1; i <= 8; ++i) am.bytes(i, 20);
    REQUIRE(am.cacheBytes() <= 1024);

    am.invalidate(1);
    const auto b3 = am.bytes(1, 64);
    REQUIRE(b3 != nullptr);
    REQUIRE(b3->size() == b1->size());          // re-read, no re-thumb
    REQUIRE(thumbCalls == 2);   // 64px + one 20px thumb; the other 7
                              // px-20 requests re-read the same file
}

// ===================================================== SESSION HISTORY (§47)
TEST_CASE("SessionHistory: rollup of one session", "[lib29]") {
    Lib lib("sess");
    const int64_t id = lib.add("s.wav", "A");
    const int64_t sess = lib.engine.history().beginSession("Friday");
    REQUIRE(sess > 0);
    lib.engine.history().recordPlay(id, 0, 1000, 200.0, sess);
    lib.engine.history().recordPlay(id, 1, 2000, 100.0, sess);
    REQUIRE(lib.engine.history().endSession(sess));

    const auto h = lib.engine.history().session(sess);
    REQUIRE(h.id == sess);
    REQUIRE(h.name == "Friday");
    REQUIRE(h.entries.size() == 2);
    REQUIRE(h.endedAt > 0);
}

// ===================================================== TRACK REFERENCE (§52-53)
TEST_CASE("TrackReference: deck hand-off carries identity not audio",
          "[lib29]") {
    Lib lib("tref");
    const int64_t id = lib.add("ref.wav", "RefArtist", 128.0);
    Track t;
    REQUIRE(lib.engine.tracks().getById(id, t));

    const auto r = lib.engine.trackRef(id);
    REQUIRE(r.trackId == id);
    REQUIRE(r.path == t.path);
    REQUIRE(r.artist == "RefArtist");
    REQUIRE(r.bpm == Approx(128.0));
    REQUIRE(lib.engine.trackRef(424242).trackId < 0);   // unknown → empty
}

// ===================================================== BACKUP DB + GATE (§56-57)
TEST_CASE("backupDb: consistent physical copy; importJson version gate",
          "[lib29]") {
    Lib lib("bkdb");
    lib.add("x.wav", "A");

    std::string err;
    const fs::path out = lib.dir / "backup.db";
    REQUIRE(lib.engine.backup().backupDb(out.string(), err));
    REQUIRE(fs::exists(out));
    REQUIRE(fs::file_size(out) > 0);

    // copy is a real, openable DB with the same content
    {
        Database d2;
        REQUIRE(d2.open(out.string()));
        std::lock_guard lk(d2.mutex());
        auto s = d2.prepare("SELECT COUNT(*) FROM tracks");
        REQUIRE(s.step());
        REQUIRE(s.colInt(0) == 1);
        d2.close();
    }

    // version gate: a backup stamped with a future version is refused
    const fs::path json = lib.dir / "b.json";
    REQUIRE(lib.engine.backup().exportJson(json.string(), err));
    {
        std::ifstream in(json); std::stringstream ss;
        ss << in.rdbuf();
        std::string doc = ss.str();
        const auto p = doc.find("\"version\":");
        REQUIRE(p != std::string::npos);
        const auto colon = doc.find(':', p);
        const auto end = doc.find_first_of(",}\n", colon);
        doc.replace(colon + 1, end - colon - 1, "9999");
        const fs::path bad = lib.dir / "b-future.json";
        std::ofstream o(bad); o << doc;
        const auto res = lib.engine.backup().importJson(
            bad.string(), BackupConflict::Replace);
        REQUIRE(res.errors >= 1);
        REQUIRE(res.tracks == 0);
    }
}

// ===================================================== INTEGRITY (§59)
TEST_CASE("Integrity: semantic checks catch broken playlist items",
          "[lib29]") {
    Lib lib("integrity");
    const int64_t id = lib.add("ok.wav", "A");
    const int64_t pl = lib.engine.playlists().create("P");
    lib.engine.playlists().addTrack(pl, id);

    std::string detail;
    REQUIRE(lib.engine.integrityCheck(detail));   // clean

    // inject an orphan playlist_items row (FK off — this is exactly
    // the corruption the checker is meant to find)
    {
        std::lock_guard lk(lib.engine.db().mutex());
        lib.engine.db().exec("PRAGMA foreign_keys=OFF");
        auto s = lib.engine.db().prepare(
            "INSERT INTO playlist_items(playlist_id,position,track_id)"
            " VALUES(?,0,999999)");
        s.bind(1, pl);
        s.step();
        lib.engine.db().exec("PRAGMA foreign_keys=ON");
    }
    REQUIRE_FALSE(lib.engine.integrityCheck(detail));
    REQUIRE(detail.find("playlist_items") != std::string::npos);
}

// ===================================================== PALETTE (§39)
TEST_CASE("Color palette: defaults + persisted override", "[lib29]") {
    Lib lib("palette");
    REQUIRE(lib.engine.colorPalette().size() == 8);      // shipped default
    {
        std::lock_guard lk(lib.engine.db().mutex());
        auto s = lib.engine.db().prepare(
            "INSERT OR REPLACE INTO kv(key,value) VALUES"
            "('palette.colors','16711680,65280')");
        s.step();
    }
    const auto pal = lib.engine.colorPalette();
    REQUIRE(pal.size() == 2);
    REQUIRE(pal[0] == 0xFF0000);
}

// ===================================================== PERF 250K (§60)
TEST_CASE("Large library: 250k — search/filter/sort stay usable",
          "[lib29][large250k]") {
    Lib lib("perf250k");
    const auto t0 = std::chrono::steady_clock::now();
    {
        std::lock_guard lk(lib.engine.db().mutex());
        auto ins = lib.engine.db().prepare(
            "INSERT INTO tracks(path,filename,fingerprint,content_hash,"
            "file_size,mtime,status,title,artist,album,genre,year,"
            "duration_sec,bitrate,channels,sample_rate,format,rating,"
            "color,label,play_count,last_played,date_added,updated_at)"
            " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        auto fts = lib.engine.db().prepare(
            "INSERT INTO tracks_fts(rowid,title,artist,album,genre,"
            "label,composer,filename,tags,comments)"
            " VALUES(?,?,?,?,?,?,'','',?,'')");
        auto ana = lib.engine.db().prepare(
            "INSERT INTO analysis(track_id,bpm,key_index) VALUES(?,?,?)");
        for (int start = 0; start < 250000; start += 10000) {
            Database::Tx tx(lib.engine.db());
            for (int i = start; i < start + 10000 && i < 250000; ++i) {
                char path[64], title[64], artist[64], album[64];
                std::snprintf(path, sizeof(path), "/m/%06d.wav", i);
                std::snprintf(title, sizeof(title), "Track %06d", i);
                std::snprintf(artist, sizeof(artist), "Artist %04d",
                              i % 1000);
                std::snprintf(album, sizeof(album), "Album %03d", i % 80);
                const char* genre = i % 3 ? "House" : "Techno";
                int b = 1;
                ins.reset();
                ins.bind(b++, path); ins.bind(b++, path + 3);
                ins.bind(b++, (int64_t)i); ins.bind(b++, (int64_t)(i * 7));
                ins.bind(b++, (int64_t)5000000); ins.bind(b++, (int64_t)1000);
                ins.bind(b++, (int64_t)0);
                ins.bind(b++, title); ins.bind(b++, artist);
                ins.bind(b++, album); ins.bind(b++, genre);
                ins.bind(b++, (int64_t)(1990 + i % 35));
                ins.bind(b++, 200.0 + (i % 60));
                ins.bind(b++, (int64_t)320000); ins.bind(b++, (int64_t)2);
                ins.bind(b++, 48000.0); ins.bind(b++, "wav");
                ins.bind(b++, (int64_t)(i % 6));
                ins.bind(b++, (int64_t)0xFF000000 | (i % 8));
                ins.bind(b++, i % 5 == 0 ? "MegaLabel" : "Indie");
                ins.bind(b++, (int64_t)(i % 40)); ins.bind(b++, (int64_t)0);
                ins.bind(b++, (int64_t)(1700000000 + i));
                ins.bind(b++, (int64_t)0);
                ins.step();
                const int64_t id = lib.engine.db().lastInsertId();
                fts.reset();
                fts.bind(1, id); fts.bind(2, title); fts.bind(3, artist);
                fts.bind(4, album); fts.bind(5, genre);
                fts.bind(6, path + 3);
                fts.step();
                ana.reset();
                ana.bind(1, id); ana.bind(2, 118.0 + (i % 25));
                ana.bind(3, (int64_t)(i % 24)); ana.step();
            }
            tx.commit();
        }
    }
    const double seedMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    REQUIRE(lib.engine.trackCount() == 250000);

    auto timed = [](const char* what, auto&& fn) {
        const auto a = std::chrono::steady_clock::now();
        auto r = fn();
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - a).count();
        std::printf("  [perf250k] %-34s %8.2f ms\n", what, ms);
        return std::pair{ r, ms };
    };

    auto [p1, ms1] = timed("fts 'artist 0499'", [&] {
        return lib.engine.search("artist 0499"); });
    REQUIRE(p1.total > 0);
    REQUIRE(ms1 < 2000);

    auto [p2, ms2] = timed("genre+bpm filter, bpm sort page", [&] {
        QuerySpec q;
        q.filters.push_back({ "genre", CmpOp::Eq, "House" });
        q.filters.push_back({ "bpm", CmpOp::Between, "", 120.0, 130.0 });
        q.sort = SortKey::Bpm; q.limit = 100;
        return lib.engine.tracks().query(q);
    });
    REQUIRE(p2.items.size() == 100);
    REQUIRE(ms2 < 3000);

    auto [p3, ms3] = timed("fuzzy 'artist499'", [&] {
        return lib.engine.searchFuzzy("artist499"); });
    REQUIRE(ms3 < 5000);

    std::error_code ec;
    const auto dbSize = fs::file_size(lib.dir / "lib.db", ec);
    std::printf("  [perf250k] seed: %.0f ms, db %.1f MB\n",
                seedMs, (double)dbSize / 1048576.0);
}
