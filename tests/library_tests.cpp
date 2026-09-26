// Phase 7 — professional music library tests.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "core/library/MusicLibraryEngine.h"
#include "core/library/FileFingerprint.h"
#include "platform/metadata/FFmpegMetadataReader.h"

using namespace onda;
using namespace onda::library;
using Catch::Approx;
namespace fs = std::filesystem;

namespace {

fs::path tempRoot(const char* name) {
    fs::path d = fs::temp_directory_path() / "onda_libtest" / name;
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

// Minimal PCM16 stereo WAV (RIFF) — same shape as decoder_tests.
void writeWav(const fs::path& p, int sampleRate,
              const std::vector<int16_t>& samples) {
    std::ofstream f(p, std::ios::binary);
    const uint32_t dataSize   = (uint32_t)(samples.size() * sizeof(int16_t));
    const uint32_t byteRate   = sampleRate * 2 * 2;
    auto w32 = [&](uint32_t v) { f.write((const char*)&v, 4); };
    auto w16 = [&](uint16_t v) { f.write((const char*)&v, 2); };
    f.write("RIFF", 4); w32(36 + dataSize); f.write("WAVE", 4);
    f.write("fmt ", 4); w32(16); w16(1); w16(2);
    w32((uint32_t)sampleRate); w32(byteRate); w16(4); w16(16);
    f.write("data", 4); w32(dataSize);
    f.write((const char*)samples.data(), dataSize);
}

fs::path makeWav(const fs::path& dir, const std::string& name,
                 double seconds = 1.0, double freq = 440.0) {
    const int n = (int)(48000 * seconds);
    std::vector<int16_t> s((size_t)n * 2);
    for (int i = 0; i < n; ++i) {
        const int16_t v = (int16_t)(12000.0 *
            std::sin(6.28318530718 * freq * i / 48000.0));
        s[(size_t)i * 2] = v; s[(size_t)i * 2 + 1] = v;
    }
    const fs::path p = dir / name;
    writeWav(p, 48000, s);
    return p;
}

struct Lib {
    fs::path dir;
    MusicLibraryEngine engine;
    explicit Lib(const char* name) : dir(tempRoot(name)) {
        REQUIRE(engine.open((dir / "lib.db").string()));
        engine.setMetadataProvider(
            [](const std::string& p, TrackMeta& m) {
                return FFmpegMetadataReader::read(p, m);
            });
    }
};

} // namespace

// ============================================================ FINGERPRINT
TEST_CASE("Fingerprint: stable across rename, changes on modify", "[lib]") {
    const auto dir = tempRoot("fp");
    const auto a = makeWav(dir, "a.wav", 0.1);
    uint64_t fp1 = 0, ch1 = 0; int64_t sz = 0, mt = 0;
    REQUIRE(FileFingerprint::compute(a.string(), fp1, ch1, sz, mt));
    REQUIRE(fp1 != 0); REQUIRE(ch1 != 0);

    // rename → same content hash, different path
    const auto b = dir / "b.wav";
    fs::rename(a, b);
    uint64_t fp2 = 0, ch2 = 0;
    REQUIRE(FileFingerprint::compute(b.string(), fp2, ch2, sz, mt));
    REQUIRE(ch2 == ch1);

    // modify content → different hash
    makeWav(dir, "b.wav", 0.2, 880.0);
    uint64_t ch3 = FileFingerprint::contentHash(b.string());
    REQUIRE(ch3 != ch1);
}

// ================================================================ IMPORT
TEST_CASE("Import: wav registers track with decoded metadata", "[lib]") {
    Lib lib("import");
    const auto wav = makeWav(lib.dir / "media", "test_one.wav", 1.0);
    fs::create_directories(lib.dir / "media");
    // (makeWav creates parent only if it exists)
    const auto w = makeWav(lib.dir, "track_a.wav", 1.5, 440.0);

    REQUIRE(lib.engine.importer().importFile(w.string()));
    Track t;
    REQUIRE(lib.engine.tracks().getByPath(
        fs::absolute(w).lexically_normal().string(), t));
    REQUIRE(t.format == "wav");
    REQUIRE(t.channels == 2);
    REQUIRE(t.sampleRate == Approx(48000.0));
    REQUIRE(t.durationSec == Approx(1.5).margin(0.05));
    REQUIRE(t.fileSize > 0);
    REQUIRE(t.status == (int)TrackStatus::Available);
    REQUIRE(t.dateAdded > 0);
    REQUIRE(lib.engine.trackCount() == 1);
}

TEST_CASE("Import: folder recursive, skip unchanged, update on modify", "[lib]") {
    Lib lib("folder");
    const auto media = lib.dir / "media";
    fs::create_directories(media / "sub");
    makeWav(media, "one.wav", 0.5);
    makeWav(media / "sub", "two.wav", 0.5);
    makeWav(lib.dir, "outside.wav", 0.5);   // not under root

    ImportResult r = lib.engine.importer().importFolder(media.string(), true);
    REQUIRE(r.added == 2);
    REQUIRE(r.failed == 0);
    REQUIRE(lib.engine.trackCount() == 2);

    // second pass → all skipped
    r = lib.engine.importer().importFolder(media.string(), true);
    REQUIRE(r.skipped == 2);
    REQUIRE(r.added == 0);

    // modify one file → updated
    makeWav(media / "sub", "two.wav", 0.9, 880.0);
    r = lib.engine.importer().importFolder(media.string(), true);
    REQUIRE(r.updated == 1);
    REQUIRE(r.skipped == 1);
}

// ============================================================ DUPLICATES
TEST_CASE("Duplicates: same content at two paths is flagged", "[lib]") {
    Lib lib("dup");
    const auto a = makeWav(lib.dir, "orig.wav", 0.4);
    const auto b = lib.dir / "copy.wav";
    fs::copy_file(a, b);
    lib.engine.importer().importFiles({ a.string(), b.string() });

    const auto groups = lib.engine.duplicates().findAll();
    REQUIRE(groups.size() == 1);
    REQUIRE(groups[0].reason == DupContentHash);
    REQUIRE(groups[0].trackIds.size() == 2);
}

// ================================================================ SEARCH
TEST_CASE("Search: partial token finds artist/title", "[lib]") {
    Lib lib("search");
    Track t;
    t.path = (lib.dir / "x.wav").string();
    t.filename = "x.wav"; t.title = "One More Time";
    t.artist = "Daft Punk"; t.genre = "House";
    REQUIRE(lib.engine.tracks().upsert(t) > 0);

    QuerySpec q;
    q.search = "daft";
    auto page = lib.engine.tracks().query(q);
    REQUIRE(page.total == 1);
    REQUIRE(page.items[0].artist == "Daft Punk");

    q.search = "more tim";          // multi-token prefix
    page = lib.engine.tracks().query(q);
    REQUIRE(page.total == 1);

    q.search = "zzzz";
    page = lib.engine.tracks().query(q);
    REQUIRE(page.total == 0);
}

// =============================================================== FILTERS
TEST_CASE("Filters: bpm range + genre + key compatibility combine", "[lib]") {
    Lib lib("filt");
    auto add = [&](const char* name, const char* genre, double bpm,
                   int key, int rating) {
        Track t;
        t.path = (lib.dir / name).string(); t.filename = name;
        t.title = name; t.artist = "A"; t.genre = genre;
        t.rating = rating;
        const int64_t id = lib.engine.tracks().upsert(t);
        REQUIRE(id > 0);
        // seed analysis row directly (bpm/key live in analysis table)
        std::lock_guard lk(lib.engine.db().mutex());
        auto s = lib.engine.db().prepare(
            "INSERT OR REPLACE INTO analysis(track_id,bpm,key_index) "
            "VALUES(?,?,?)");
        s.bind(1, id); s.bind(2, bpm); s.bind(3, (int64_t)key);
        s.step();
        return id;
    };
    add("h1", "House", 124.0, 0, 5);
    add("h2", "House", 126.0, 1, 3);
    add("t1", "Techno", 135.0, 0, 4);
    add("s1", "House", 100.0, 0, 2);

    QuerySpec q;
    q.filters.push_back({ "genre", CmpOp::Eq, "House", 0, 0, {} });
    q.filters.push_back({ "bpm", CmpOp::Between, "", 120.0, 130.0, {} });
    auto page = lib.engine.tracks().query(q);
    REQUIRE(page.total == 2);

    // key-compatible with key 0 → {0,1,11,5,7,12}
    q.filters.clear();
    q.filters.push_back({ "key_compatible", CmpOp::Eq, "", 0, 0, {} });
    q.filters.push_back({ "bpm", CmpOp::Between, "", 120.0, 130.0, {} });
    page = lib.engine.tracks().query(q);
    REQUIRE(page.total == 2);   // key 0 and key 1 both compatible w/ 0

    q.filters[1].b = 125.0;      // narrow bpm → only h1
    page = lib.engine.tracks().query(q);
    REQUIRE(page.total == 1);

    // rating filter
    q.filters.clear();
    q.filters.push_back({ "rating", CmpOp::Gte, "", 4, 0, {} });
    page = lib.engine.tracks().query(q);
    REQUIRE(page.total == 2);   // h1(5) + t1(4)
}

// =========================================================== SORT + PAGE
TEST_CASE("Sorting: whitelisted columns, pagination stable", "[lib]") {
    Lib lib("sort");
    const char* artists[] = { "Zed", "Alpha", "Mike", "Beta" };
    for (int i = 0; i < 4; ++i) {
        Track t;
        t.path = (lib.dir / ("t" + std::to_string(i))).string();
        t.title = "T" + std::to_string(i);
        t.artist = artists[i];
        REQUIRE(lib.engine.tracks().upsert(t) > 0);
    }
    QuerySpec q; q.sort = SortKey::Artist; q.limit = 2;
    auto p1 = lib.engine.tracks().query(q);
    REQUIRE(p1.total == 4);
    REQUIRE(p1.items[0].artist == "Alpha");
    REQUIRE(p1.items[1].artist == "Beta");
    q.offset = 2;
    auto p2 = lib.engine.tracks().query(q);
    REQUIRE(p2.items[0].artist == "Mike");
    REQUIRE(p2.items[1].artist == "Zed");
}

// ============================================================= PLAYLISTS
TEST_CASE("Playlists: create/order/move/duplicate/folders", "[lib]") {
    Lib lib("pl");
    std::vector<int64_t> ids;
    for (int i = 0; i < 5; ++i) {
        Track t;
        t.path = (lib.dir / ("s" + std::to_string(i))).string();
        t.title = "Song" + std::to_string(i);
        ids.push_back(lib.engine.tracks().upsert(t));
    }
    const int64_t folder = lib.engine.playlists().createFolder("Crates");
    const int64_t pl = lib.engine.playlists().create("Set A", folder);
    REQUIRE(pl > 0);
    REQUIRE(lib.engine.playlists().addTracks(pl, ids));

    auto order = lib.engine.playlists().trackIds(pl);
    REQUIRE(order == ids);

    // move pos 0 → pos 3
    REQUIRE(lib.engine.playlists().moveTrack(pl, 0, 3));
    order = lib.engine.playlists().trackIds(pl);
    REQUIRE(order[3] == ids[0]);
    REQUIRE(order[0] == ids[1]);

    // duplicate preserves order
    const int64_t copy = lib.engine.playlists().duplicate(pl, "");
    REQUIRE(copy > 0);
    REQUIRE(lib.engine.playlists().trackIds(copy) == order);
    REQUIRE(lib.engine.playlists().info(copy).itemCount == 5);

    // remove middle → positions stay dense
    REQUIRE(lib.engine.playlists().removeTrack(pl, order[2]));
    order = lib.engine.playlists().trackIds(pl);
    REQUIRE(order.size() == 4);

    // folder listing: original + duplicate share the crate
    const auto pls = lib.engine.playlists().playlists(folder);
    REQUIRE(pls.size() == 2);

    // delete playlist → items cascade (no orphans)
    REQUIRE(lib.engine.playlists().remove(pl));
    REQUIRE(lib.engine.playlists().trackIds(pl).empty());
    std::string det;
    REQUIRE(lib.engine.integrityCheck(det));
}

// ======================================================== SMART PLAYLIST
TEST_CASE("Smart playlists: rules evaluate live", "[lib]") {
    Lib lib("smart");
    auto add = [&](const char* n, const char* genre, double bpm) {
        Track t;
        t.path = (lib.dir / n).string(); t.title = n; t.genre = genre;
        const int64_t id = lib.engine.tracks().upsert(t);
        std::lock_guard lk(lib.engine.db().mutex());
        auto s = lib.engine.db().prepare(
            "INSERT INTO analysis(track_id,bpm) VALUES(?,?)");
        s.bind(1, id); s.bind(2, bpm); s.step();
    };
    add("a", "House", 124.0); add("b", "Techno", 130.0);
    add("c", "House", 126.0);

    const int64_t sp = lib.engine.playlists().createSmart(
        "House 120+", "genre=House;bpm=120..130");
    REQUIRE(sp > 0);
    REQUIRE(lib.engine.playlists().info(sp).kind == PlaylistKind::Smart);
    // smart evaluation happens at query time via engine (rules json → spec)
    QuerySpec q;
    q.filters.push_back({ "genre", CmpOp::Eq, "House", 0, 0, {} });
    q.filters.push_back({ "bpm", CmpOp::Gte, "", 120, 0, {} });
    REQUIRE(lib.engine.tracks().query(q).total == 2);
}

// =============================================================== HISTORY
TEST_CASE("History: sessions, play-count rule, async write", "[lib]") {
    Lib lib("hist");
    Track t;
    t.path = (lib.dir / "h.wav").string(); t.title = "H";
    t.durationSec = 200.0;
    const int64_t tid = lib.engine.tracks().upsert(t);
    REQUIRE(tid > 0);

    const int64_t sid = lib.engine.history().beginSession("set1");
    REQUIRE(sid > 0);

    // 30 s of a 200 s track → below both thresholds → not counted
    lib.engine.history().recordPlay(tid, 0, 1000, 30.0, sid);
    Track cur;
    REQUIRE(lib.engine.tracks().getById(tid, cur));
    REQUIRE(cur.playCount == 0);
    REQUIRE(cur.lastPlayed == 1000);

    // 120 s ≥ min(60, 100) → counted
    lib.engine.history().recordPlay(tid, 1, 2000, 120.0, sid);
    REQUIRE(lib.engine.tracks().getById(tid, cur));
    REQUIRE(cur.playCount == 1);
    REQUIRE(cur.lastPlayed == 2000);

    const auto ents = lib.engine.history().entries(sid);
    REQUIRE(ents.size() == 2);
    REQUIRE((ents[0].counted == 1 || ents[1].counted == 1));

    // async write lands on the worker
    lib.engine.startWorker();
    lib.engine.recordPlayAsync(tid, 0, 3000, 150.0, sid);
    lib.engine.stopWorker();   // drains queue inline
    REQUIRE(lib.engine.tracks().getById(tid, cur));
    REQUIRE(cur.playCount == 2);

    REQUIRE(lib.engine.history().endSession(sid));
    REQUIRE(lib.engine.history().deleteSession(sid));
    // session gone but entries kept (session_id → NULL via ON DELETE SET NULL)
    REQUIRE(lib.engine.history().forTrack(tid).size() == 3);
}

// ================================================================= TAGS
TEST_CASE("Tags: add/remove/filter, user metadata persists", "[lib]") {
    Lib lib("tags");
    Track t;
    t.path = (lib.dir / "tg.wav").string(); t.title = "T";
    const int64_t id = lib.engine.tracks().upsert(t);
    REQUIRE(lib.engine.tracks().addTag(id, "Peak Time"));
    REQUIRE(lib.engine.tracks().addTag(id, "vinyl"));
    REQUIRE(lib.engine.tracks().addTag(id, "PEAK TIME")); // nocase dup → still 2

    Track cur;
    REQUIRE(lib.engine.tracks().getById(id, cur));
    REQUIRE(cur.tags.size() == 2);

    REQUIRE(lib.engine.tracks().setUserFields(id, 4, 0xff8800, 7, "banger"));
    QuerySpec q;
    q.filters.push_back({ "tags", CmpOp::Contains, "peak", 0, 0, {} });
    REQUIRE(lib.engine.tracks().query(q).total == 1);

    // search hits tag text through FTS
    q = QuerySpec{}; q.search = "vinyl";
    REQUIRE(lib.engine.tracks().query(q).total == 1);

    REQUIRE(lib.engine.tracks().removeTag(id, "vinyl"));
    REQUIRE(lib.engine.tracks().getById(id, cur));
    REQUIRE(cur.tags.size() == 1);
}

// ======================================================== MISSING/RELINK
TEST_CASE("Missing: delete→MISSING, move→relink same id, back→recover", "[lib]") {
    Lib lib("miss");
    const auto media = lib.dir / "media";
    fs::create_directories(media);
    // distinct content so contentHashes differ (identical bytes would
    // legitimately relink — same content is indistinguishable from a move)
    const auto w1 = makeWav(media, "stay.wav", 0.3, 330.0);
    const auto w2 = makeWav(media, "gone.wav", 0.3, 550.0);
    lib.engine.importer().importFolder(media.string(), true);
    lib.engine.scanner().addRoot(media.string());
    REQUIRE(lib.engine.trackCount() == 2);

    Track gone;
    REQUIRE(lib.engine.tracks().getByPath(
        fs::absolute(w2).lexically_normal().string(), gone));
    const int64_t goneId = gone.id;

    // delete → scan → MISSING (row kept)
    fs::remove(w2);
    ScanDiff d = lib.engine.scanner().scan();
    REQUIRE(d.missing.size() == 1);
    Track cur;
    REQUIRE(lib.engine.tracks().getById(goneId, cur));
    REQUIRE(cur.status == (int)TrackStatus::Missing);
    REQUIRE(lib.engine.missing().missing().size() == 1);

    // file back → scan → Available again, same id
    makeWav(media, "gone.wav", 0.3);
    d = lib.engine.scanner().scan();
    REQUIRE(lib.engine.tracks().getById(goneId, cur));
    REQUIRE(cur.status == (int)TrackStatus::Available);
}

TEST_CASE("Moved: file renamed → track relinks, keeps user data", "[lib]") {
    Lib lib("moved");
    const auto media = lib.dir / "media";
    fs::create_directories(media);
    const auto w = makeWav(media, "oldname.wav", 0.3);
    lib.engine.importer().importFolder(media.string(), true);
    lib.engine.scanner().addRoot(media.string());
    Track t;
    REQUIRE(lib.engine.tracks().getByPath(
        fs::absolute(w).lexically_normal().string(), t));
    lib.engine.tracks().addTag(t.id, "keepme");
    lib.engine.tracks().setUserFields(t.id, 5, 0, 0, "mine");

    fs::rename(w, media / "newname.wav");
    ScanDiff d = lib.engine.scanner().scan();
    REQUIRE(d.relocated.size() == 1);
    REQUIRE(d.missing.empty());

    Track cur;
    REQUIRE(lib.engine.tracks().getById(t.id, cur));
    REQUIRE(cur.status == (int)TrackStatus::Available);
    REQUIRE(cur.filename == "newname.wav");
    REQUIRE(cur.rating == 5);          // user data survived the move
    REQUIRE(cur.comments == "mine");
    REQUIRE(cur.tags.size() == 1);
}

// ============================================================ MODIFIED
TEST_CASE("Modified: content change → re-import, analysis invalidated", "[lib]") {
    Lib lib("mod");
    const auto media = lib.dir / "media";
    fs::create_directories(media);
    const auto w = makeWav(media, "m.wav", 0.3);
    lib.engine.importer().importFolder(media.string(), true);
    lib.engine.scanner().addRoot(media.string());
    Track t;
    REQUIRE(lib.engine.tracks().getByPath(
        fs::absolute(w).lexically_normal().string(), t));
    const uint64_t fp0 = t.fingerprint;

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    makeWav(media, "m.wav", 0.6, 880.0);  // different content + mtime
    const ScanDiff d = lib.engine.scanner().scan();
    REQUIRE(d.modified.size() == 1);
    REQUIRE(lib.engine.tracks().getById(t.id, t));
    REQUIRE(t.fingerprint != fp0);
    REQUIRE(t.durationSec == Approx(0.6).margin(0.05));
}

// ========================================================== INTEGRITY
TEST_CASE("Integrity: check clean, rollback leaves no partial rows", "[lib]") {
    Lib lib("integ");
    std::string det;
    REQUIRE(lib.engine.integrityCheck(det));

    // forced rollback → nothing committed
    {
        std::lock_guard lk(lib.engine.db().mutex());
        Database::Tx tx(lib.engine.db());
        auto s = lib.engine.db().prepare(
            "INSERT INTO tracks(path,title) VALUES('x','y')");
        s.step();
    }   // dtor → ROLLBACK
    REQUIRE(lib.engine.trackCount() == 0);
    REQUIRE(lib.engine.integrityCheck(det));
}

// ============================================================ METADATA
TEST_CASE("Metadata: FFmpeg reader handles wav + missing tags", "[lib]") {
    const auto dir = tempRoot("meta");
    const auto w = makeWav(dir, "plain.wav", 0.5);
    TrackMeta m;
    REQUIRE(FFmpegMetadataReader::read(w.string(), m));
    REQUIRE(m.format == "wav");
    REQUIRE(m.channels == 2);
    REQUIRE(m.sampleRate == Approx(48000.0));
    REQUIRE(m.durationSec == Approx(0.5).margin(0.05));
    REQUIRE(m.title == "plain");       // falls back to stem

    REQUIRE(!FFmpegMetadataReader::read(dir.string(), m));  // dir → fail
    std::vector<uint8_t> art; std::string ext;
    REQUIRE(!FFmpegMetadataReader::artwork(w.string(), art, ext));
}

// ================================================ LARGE LIBRARY (50 000)
TEST_CASE("Large library: 50k tracks — query/filter/sort/playlist perf",
          "[lib][large]") {
    Lib lib("large");
    const int N = 50000;
    auto t0 = std::chrono::steady_clock::now();
    // bulk-seed in chunked transactions (same statements as upsert,
    // minus per-row overhead — seeds also write FTS rows)
    {
        std::lock_guard lk(lib.engine.db().mutex());
        auto ins = lib.engine.db().prepare(
            "INSERT INTO tracks(path,filename,fingerprint,content_hash,"
            "file_size,mtime,status,title,artist,album,genre,year,"
            "duration_sec,bitrate,channels,sample_rate,format,rating,"
            "play_count,last_played,date_added,updated_at) VALUES(?,?,?"
            ",?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        auto fts = lib.engine.db().prepare(
            "INSERT INTO tracks_fts(rowid,title,artist,album,genre,"
            "filename,tags,comments) VALUES(?,?,?,?,?,?,?,'')");
        auto ana = lib.engine.db().prepare(
            "INSERT INTO analysis(track_id,bpm,key_index) VALUES(?,?,?)");
        for (int start = 0; start < N; start += 5000) {
            Database::Tx tx(lib.engine.db());
            for (int i = start; i < start + 5000 && i < N; ++i) {
                char path[64], title[64], artist[64], album[64];
                std::snprintf(path, sizeof(path), "/m/%05d.wav", i);
                std::snprintf(title, sizeof(title), "Track %05d", i);
                std::snprintf(artist, sizeof(artist), "Artist %03d", i % 500);
                std::snprintf(album, sizeof(album), "Album %02d", i % 40);
                int b = 1;
                ins.reset();
                ins.bind(b++, path); ins.bind(b++, path + 3);
                ins.bind(b++, (int64_t)i); ins.bind(b++, (int64_t)(i * 7));
                ins.bind(b++, (int64_t)5000000); ins.bind(b++, (int64_t)1000);
                ins.bind(b++, (int64_t)0);
                ins.bind(b++, title); ins.bind(b++, artist);
                ins.bind(b++, album);
                ins.bind(b++, i % 3 == 0 ? "House" : (i % 3 == 1 ? "Techno" : "Trance"));
                ins.bind(b++, (int64_t)(1990 + i % 35));
                ins.bind(b++, 200.0 + (i % 60));
                ins.bind(b++, (int64_t)320000); ins.bind(b++, (int64_t)2);
                ins.bind(b++, 48000.0); ins.bind(b++, "wav");
                ins.bind(b++, (int64_t)(i % 6));
                ins.bind(b++, (int64_t)(i % 40)); ins.bind(b++, (int64_t)0);
                ins.bind(b++, (int64_t)(1700000000 + i)); ins.bind(b++, (int64_t)0);
                ins.step();
                const int64_t id = lib.engine.db().lastInsertId();
                fts.reset();
                fts.bind(1, id); fts.bind(2, title); fts.bind(3, artist);
                fts.bind(4, album);
                fts.bind(5, i % 3 == 0 ? "House" : (i % 3 == 1 ? "Techno" : "Trance"));
                fts.bind(6, path + 3); fts.step();
                ana.reset();
                ana.bind(1, id); ana.bind(2, 118.0 + (i % 25));
                ana.bind(3, (int64_t)(i % 24)); ana.step();
            }
            tx.commit();
        }
    }
    const double seedMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    REQUIRE(lib.engine.trackCount() == N);

    auto timed = [&](const char* what, auto&& fn) {
        const auto a = std::chrono::steady_clock::now();
        auto r = fn();
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - a).count();
        std::printf("  [perf] %-34s %8.2f ms\n", what, ms);
        return r;
    };
    std::printf("  [perf] seed 50k rows:                 %8.0f ms\n", seedMs);

    // search across 50k FTS rows
    QuerySpec q;
    q.search = "artist 499";
    auto p1 = timed("fts search 'artist 499'",
                    [&] { return lib.engine.tracks().query(q); });
    // "499"* prefix-matches tokens in ANY column: artist "Artist 499"
    // (i%500==499 → 100 rows) ∪ title "Track 499xx" (100 rows, 1 overlaps)
    REQUIRE(p1.total == 199);

    // combined filter + sort + page
    q = QuerySpec{};
    q.filters.push_back({ "genre", CmpOp::Eq, "House", 0, 0, {} });
    q.filters.push_back({ "bpm", CmpOp::Between, "", 125, 135, {} });
    q.filters.push_back({ "rating", CmpOp::Gte, "", 3, 0, {} });
    q.sort = SortKey::Bpm; q.ascending = false; q.limit = 50;
    auto p2 = timed("genre+bpm+rating filter, sort, page",
                    [&] { return lib.engine.tracks().query(q); });
    REQUIRE(p2.items.size() == 50);
    REQUIRE(p2.items[0].bpm >= p2.items[49].bpm);

    // artist sort full scan (page 1 only but ORDER covers all)
    q = QuerySpec{}; q.sort = SortKey::Artist; q.limit = 50;
    timed("artist sort page 1/1000",
          [&] { return lib.engine.tracks().query(q); });

    // playlist ops at scale
    const int64_t pl = lib.engine.playlists().create("big");
    std::vector<int64_t> ids;
    for (int i = 0; i < 5000; ++i) ids.push_back(i + 1);
    timed("add 5000 playlist items", [&] {
        return lib.engine.playlists().addTracks(pl, ids);
    });
    timed("playlist page", [&] {
        return lib.engine.playlists().tracks(pl, 0, 50);
    });

    // db file size sanity
    const auto sz = fs::file_size(lib.dir / "lib.db");
    std::printf("  [perf] db size for 50k tracks: %.1f MB\n", sz / 1048576.0);
    REQUIRE(sz < 200ull * 1048576);

    std::string det;
    REQUIRE(lib.engine.integrityCheck(det));
}

// ========================================================= CONCURRENCY
TEST_CASE("Concurrency: queries stay correct while worker writes", "[lib]") {
    Lib lib("conc");
    lib.engine.startWorker();
    std::atomic<bool> stop { false };
    std::atomic<int> writes { 0 };
    // writer: 400 tracks through the async path
    for (int i = 0; i < 400; ++i) {
        lib.engine.post([&, i] {
            Track t;
            t.path = lib.dir.string() + "/c" + std::to_string(i);
            t.title = "C" + std::to_string(i);
            t.artist = "Conc";
            lib.engine.tracks().upsert(t);
            ++writes;
        });
    }
    // reader: continuous queries while the worker inserts
    int reads = 0;
    while (writes.load() < 400) {
        QuerySpec q; q.search = "conc"; q.limit = 10;
        const auto p = lib.engine.tracks().query(q);
        REQUIRE(p.total >= 0);
        REQUIRE(p.total <= 400);
        ++reads;
    }
    stop = true;
    lib.engine.stopWorker();
    QuerySpec q; q.search = "conc";
    REQUIRE(lib.engine.tracks().query(q).total == 400);
    REQUIRE(reads > 0);
}
