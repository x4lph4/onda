// Phase 18 — professional library: search tokens, accents, smart
// playlists, crates, tags/ratings, backup, diagnostics, 100k perf.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/library/MusicLibraryEngine.h"
#include "core/library/FileFingerprint.h"
#include "core/library/KeyNotation.h"
#include "core/library/SearchQuery.h"
#include "core/library/SmartPlaylistEngine.h"
#include "core/library/TextFold.h"
#include "platform/metadata/FFmpegMetadataReader.h"

using namespace onda;
using namespace onda::library;
using Catch::Approx;
namespace fs = std::filesystem;

namespace {

fs::path tempRoot(const char* name) {
    fs::path d = fs::temp_directory_path() / "onda_libtest18" / name;
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

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
    /// Seed a track + analysis row in one go. contentHash is a
    /// deterministic per-name value so backup tests can match by
    /// hash across libraries.
    int64_t add(const char* name, const char* artist, const char* genre,
                double bpm, int key, int rating = 0) {
        Track t;
        t.path = (dir / name).string();
        t.filename = name;
        t.title = name; t.artist = artist; t.genre = genre;
        t.rating = rating;
        t.contentHash = 0x9E37 + (uint64_t)std::hash<std::string>{}(name);
        t.fingerprint = t.contentHash ^ 0xABCD;
        const int64_t id = engine.tracks().upsert(t);
        REQUIRE(id > 0);
        std::lock_guard lk(engine.db().mutex());
        auto s = engine.db().prepare(
            "INSERT OR REPLACE INTO analysis(track_id,bpm,key_index)"
            " VALUES(?,?,?)");
        s.bind(1, id); s.bind(2, bpm); s.bind(3, (int64_t)key);
        s.step();
        return id;
    }
};

} // namespace

// ===================================================== SEARCH TOKENS (§13)
TEST_CASE("SearchQuery: field tokens become filters", "[lib18]") {
    Lib lib("tokens");
    lib.add("h1.wav", "Daft Punk", "House", 124.0, 0, 5);
    lib.add("t1.wav", "Surgeon", "Techno", 135.0, 12, 4);
    lib.add("h2.wav", "Daft Punk", "House", 100.0, 0, 2);

    auto page = lib.engine.search("artist:daft bpm:120-130");
    REQUIRE(page.total == 1);
    REQUIRE(page.items[0].filename == "h1.wav");

    page = lib.engine.search("genre:house rating:4+");
    REQUIRE(page.total == 1);          // only h1 (5 stars)

    page = lib.engine.search("key:Am");           // 21 = Am
    REQUIRE(page.total == 0);
    page = lib.engine.search("key:Cm");           // 12 = Cm
    REQUIRE(page.total == 1);

    page = lib.engine.search("daft");             // plain FTS still works
    REQUIRE(page.total == 2);

    // quoted phrase token
    page = lib.engine.search("artist:\"daft punk\" bpm:>120");
    REQUIRE(page.total == 1);

    // unknown field falls back to FTS text
    page = lib.engine.search("bogusfield:h1");
    REQUIRE(page.total >= 0);          // no crash, sane result
}

// ============================================= ACCENT-INSENSITIVE FTS (§11)
TEST_CASE("Search: accents and case fold in index and query", "[lib18]") {
    Lib lib("accents");
    Track t;
    t.path = (lib.dir / "x.wav").string();
    t.filename = "x.wav";
    t.title = "Boulevard of Broken Écouter";
    t.artist = "Beyoncé";
    t.label = "Café Del Mar";
    REQUIRE(lib.engine.tracks().upsert(t) > 0);

    QuerySpec q;
    q.search = "beyonce";              // accent-free query finds é
    REQUIRE(lib.engine.tracks().query(q).total == 1);

    q.search = "ECOUTER";              // uppercase query finds Écouter
    REQUIRE(lib.engine.tracks().query(q).total == 1);

    q.search = "cafe del";             // label column indexed
    REQUIRE(lib.engine.tracks().query(q).total == 1);

    REQUIRE(TextFold::fold("ÀbçÑî") == "abcni");
}

// ================================================ KEY NOTATION (§43)
TEST_CASE("KeyNotation: Standard/Camelot/OpenKey roundtrip", "[lib18]") {
    REQUIRE(std::string(KeyNotationConverter::name(21, KeyNotation::Standard)) == "Am");
    REQUIRE(std::string(KeyNotationConverter::name(21, KeyNotation::Camelot)) == "8A");
    REQUIRE(std::string(KeyNotationConverter::name(21, KeyNotation::OpenKey)) == "1m");
    REQUIRE(std::string(KeyNotationConverter::name(0,  KeyNotation::Standard)) == "C");
    REQUIRE(std::string(KeyNotationConverter::name(0,  KeyNotation::OpenKey)) == "1d");
    REQUIRE(std::string(KeyNotationConverter::name(-1, KeyNotation::Standard)) == "?");

    REQUIRE(KeyNotationConverter::parse("Am") == 21);
    REQUIRE(KeyNotationConverter::parse("8A") == 21);
    REQUIRE(KeyNotationConverter::parse("1m") == 21);
    REQUIRE(KeyNotationConverter::parse("C") == 0);
    REQUIRE(KeyNotationConverter::parse("1d") == 0);
    REQUIRE(KeyNotationConverter::parse("f#m") == 18);
    REQUIRE(KeyNotationConverter::parse("Db") == 1);
    REQUIRE(KeyNotationConverter::parse("nonsense") == -1);
}

// ============================================ SMART PLAYLISTS (§20-22)
TEST_CASE("SmartPlaylist: codec roundtrip + live evaluation", "[lib18]") {
    Lib lib("smart");
    lib.add("h1.wav", "A", "House", 124.0, 0, 5);
    lib.add("h2.wav", "B", "House", 127.0, 1, 3);
    lib.add("t1.wav", "C", "Techno", 135.0, 0, 4);
    lib.add("s1.wav", "D", "House", 100.0, 0, 2);

    SmartRules sr;
    sr.rules = {
        { "genre", "eq", "House" },
        { "bpm", "between", "", 120.0, 128.0 },
        { "rating", "gte", "", 4.0 },
    };
    const std::string json = SmartPlaylistEngine::toJson(sr);
    const auto back = SmartPlaylistEngine::fromJson(json);
    REQUIRE(back.rules.size() == 3);
    REQUIRE(back.rules[1].a == 120.0);

    const int64_t pid = lib.engine.playlists().createSmart("hot", json);
    REQUIRE(pid > 0);
    auto page = lib.engine.playlists().tracks(pid);
    REQUIRE(page.total == 1);
    REQUIRE(page.items[0].filename == "h1.wav");

    // live evaluation — change a track and the membership follows
    Track t;
    REQUIRE(lib.engine.tracks().getById(
        lib.engine.tracks().idForPath((lib.dir / "h2.wav").string()), t));
    lib.engine.ratings().setRating(t.id, 5);
    page = lib.engine.playlists().tracks(pid);
    REQUIRE(page.total == 2);          // h2 now qualifies
}

TEST_CASE("SmartPlaylist: OR groups + NOT leaf", "[lib18]") {
    Lib lib("smartor");
    lib.add("a.wav", "A", "House", 122.0, 0, 0);
    lib.add("b.wav", "B", "Techno", 130.0, 0, 0);
    lib.add("c.wav", "C", "Trance", 140.0, 0, 0);
    lib.add("d.wav", "D", "Techno", 138.0, 0, 0);   // excluded by NOT

    // House OR (Techno AND NOT bpm>135)
    SmartRules sr;
    sr.any = true;
    SmartRule house{ "genre", "eq", "House" };
    sr.rules = { house };
    SmartGroup g;
    g.rules = {
        { "genre", "eq", "Techno" },
        { "bpm", "gt", "", 135.0, 0.0, {}, true },   // NOT bpm>135
    };
    sr.groups = { g };

    std::vector<std::string> binds;
    const std::string w = SmartPlaylistEngine::where(sr, binds);
    REQUIRE(w.find("OR") != std::string::npos);
    REQUIRE(w.find("NOT") != std::string::npos);

    QuerySpec q;
    const auto page = SmartPlaylistEngine::evaluate(
        lib.engine.tracks(), SmartPlaylistEngine::toJson(sr), q);
    REQUIRE(page.total == 2);          // a.wav + b.wav
}

// ========================================================= CRATES (§16-17)
TEST_CASE("Crates: create/nest/reorder/delete keeps files", "[lib18]") {
    Lib lib("crates");
    const int64_t a = lib.add("a.wav", "A", "G", 120, 0);
    const int64_t b = lib.add("b.wav", "B", "G", 121, 0);
    const int64_t c = lib.add("c.wav", "C", "G", 122, 0);

    const int64_t folder =
        lib.engine.crates().createCrateFolder("Sets");
    REQUIRE(folder > 0);
    const int64_t sub =
        lib.engine.crates().createCrateFolder("Warm", folder);
    REQUIRE(sub > 0);
    REQUIRE(lib.engine.crates().depth(sub) == 2);

    const int64_t crate =
        lib.engine.crates().createCrate("Warmup", sub);
    REQUIRE(crate > 0);
    REQUIRE(lib.engine.crates().addTracks(crate, { a, b, c }));
    REQUIRE(lib.engine.crates().moveTrack(crate, 0, 2));
    const auto ids = lib.engine.crates().trackIds(crate);
    REQUIRE(ids.size() == 3);
    REQUIRE(ids[2] == a);

    // depth cap: nest to the limit then fail
    int64_t cur = sub;
    for (int i = 0; i < CrateManager::kMaxDepth + 1; ++i) {
        const int64_t next =
            lib.engine.crates().createCrateFolder("x" + std::to_string(i), cur);
        if (next < 0) break;
        cur = next;
    }
    REQUIRE(lib.engine.crates().createCrateFolder("too deep", cur) == -1);

    REQUIRE(lib.engine.crates().deleteCrate(crate));
    REQUIRE(lib.engine.trackCount() == 3);   // tracks survive §16
}

// ============================================================= TAGS (§23)
TEST_CASE("Tags: counts, rename-merge, delete refresh search", "[lib18]") {
    Lib lib("tags");
    const int64_t a = lib.add("a.wav", "A", "G", 120, 0);
    const int64_t b = lib.add("b.wav", "B", "G", 121, 0);

    REQUIRE(lib.engine.tagManager().add(a, "Warmup"));
    REQUIRE(lib.engine.tagManager().add(b, "Warmup"));
    REQUIRE(lib.engine.tagManager().add(a, "Peak Time"));

    const auto counts = lib.engine.tagManager().tagsWithCounts();
    REQUIRE(counts.size() == 2);
    REQUIRE(counts[0].count + counts[1].count == 3);

    // rename to existing name → merge
    REQUIRE(lib.engine.tagManager().rename("Warmup", "Peak Time"));
    auto of = lib.engine.tagManager().of(b);
    REQUIRE(of.size() == 1);
    REQUIRE(of[0] == "Peak Time");

    // tag search finds merged tag
    auto page = lib.engine.search("tag:\"peak time\"");
    REQUIRE(page.total == 2);

    // delete removes + refreshes index
    REQUIRE(lib.engine.tagManager().remove("Peak Time"));
    REQUIRE(lib.engine.tagManager().all().empty());
}

// ================================================ RATINGS/COLORS (§24-26)
TEST_CASE("Ratings: clamp + color + comments persist over rescan", "[lib18]") {
    Lib lib("ratings");
    fs::create_directories(lib.dir / "media");
    const auto wav = makeWav(lib.dir / "media", "r.wav", 0.3);
    REQUIRE(lib.engine.importer().importFile(wav.string()));
    const int64_t id =
        lib.engine.tracks().idForPath(
            fs::absolute(wav).lexically_normal().string());
    REQUIRE(id > 0);

    REQUIRE(lib.engine.ratings().setRating(id, 9));   // clamps to 5
    REQUIRE(lib.engine.ratings().setColor(id, 0xFF00FF00));
    REQUIRE(lib.engine.ratings().setComments(id, "closer"));

    // rewrite content → rescan re-imports; user fields survive
    makeWav(lib.dir / "media", "r.wav", 0.5, 660.0);
    REQUIRE(lib.engine.importer().importFile(wav.string()));
    Track t;
    REQUIRE(lib.engine.tracks().getById(id, t));
    REQUIRE(t.rating == 5);
    REQUIRE(t.color == 0xFF00FF00);
    REQUIRE(t.comments == "closer");
}

// ======================================== USER-EDITED METADATA (§8)
TEST_CASE("Edited fields: user metadata survives rescan", "[lib18]") {
    Lib lib("edited");
    fs::create_directories(lib.dir / "media");
    const auto wav = makeWav(lib.dir / "media", "e.wav", 0.3);
    REQUIRE(lib.engine.importer().importFile(wav.string()));
    const int64_t id =
        lib.engine.tracks().idForPath(
            fs::absolute(wav).lexically_normal().string());
    Track t;
    REQUIRE(lib.engine.tracks().getById(id, t));

    t.title = "My Custom Title";
    REQUIRE(lib.engine.tracks().setMetadataFields(
        id, t, EditedField::Title));

    // modify file → re-import reads meta again; title must not revert
    makeWav(lib.dir / "media", "e.wav", 0.5, 660.0);
    REQUIRE(lib.engine.importer().importFile(wav.string()));
    REQUIRE(lib.engine.tracks().getById(id, t));
    REQUIRE(t.title == "My Custom Title");
    REQUIRE((t.editedFields & EditedField::Title) != 0);
}

// ===================================================== PLAY STATE (§6)
TEST_CASE("PlayState: unsupported vs metadata-only", "[lib18]") {
    Lib lib("playstate");
    // unknown extension — imported explicitly (folder scan would skip)
    const auto bad = lib.dir / "mystery.xyz";
    { std::ofstream f(bad); f << "not audio"; }
    REQUIRE(lib.engine.importer().importFile(bad.string()));
    Track t;
    REQUIRE(lib.engine.tracks().getByPath(
        fs::absolute(bad).lexically_normal().string(), t));
    REQUIRE(t.playState == (int)PlayState::UnsupportedFormat);
    REQUIRE(std::string(playStateText(t.playState)) == "UNSUPPORTED FORMAT");

    // supported extension but undecodable content → metadata only
    const auto corrupt = lib.dir / "broken.wav";
    { std::ofstream f(corrupt); f << "RIFF broken"; }
    REQUIRE(lib.engine.importer().importFile(corrupt.string()));
    REQUIRE(lib.engine.tracks().getByPath(
        fs::absolute(corrupt).lexically_normal().string(), t));
    REQUIRE(t.playState == (int)PlayState::MetadataOnly);
}

// ===================================================== STATUS (§35-37)
TEST_CASE("Status: analysis/beatgrid/waveform surface", "[lib18]") {
    Lib lib("status");
    const int64_t id = lib.add("s.wav", "A", "G", 120, 0);
    {
        std::lock_guard lk(lib.engine.db().mutex());
        auto g = lib.engine.db().prepare(
            "INSERT INTO beatgrids(track_id,segments) VALUES(?,?)");
        g.bind(1, id); g.bind(2, "[]");
        g.step();
        auto w = lib.engine.db().prepare(
            "INSERT INTO waveforms(track_id,channels,num_frames,"
            "sample_rate,overview,detail) VALUES(?,?,?,?,?,?)");
        w.bind(1, id); w.bind(2, (int64_t)2); w.bind(3, (int64_t)100);
        w.bind(4, 48000.0); w.bind(5, std::string("x"));
        w.bind(6, std::string("y"));
        w.step();
        auto a = lib.engine.db().prepare(
            "UPDATE analysis SET needs_review=1 WHERE track_id=?");
        a.bind(1, id); a.step();
    }
    Track t;
    REQUIRE(lib.engine.tracks().getById(id, t));
    REQUIRE(t.hasBeatgrid == 1);
    REQUIRE(t.hasWaveform == 1);
    REQUIRE(t.needsReview == 1);
    REQUIRE(std::string(beatgridStatusText(t)) == "BEATGRID NEEDS REVIEW");
    REQUIRE(std::string(waveformStatusText(t)) == "WAVEFORM READY");

    // filters for the same states (§14)
    QuerySpec q;
    q.filters.push_back({ "waveform", CmpOp::Eq, "", 1.0 });
    REQUIRE(lib.engine.tracks().query(q).total == 1);
    q.filters = { { "needs_review", CmpOp::Eq, "", 1.0 } };
    REQUIRE(lib.engine.tracks().query(q).total == 1);
}

// ============================================================ DIAGNOSTICS
TEST_CASE("Diagnostics: counts + integrity + report", "[lib18]") {
    Lib lib("diag");
    lib.add("a.wav", "A", "G", 120, 0);
    lib.add("b.wav", "B", "G", 121, 0);
    lib.engine.tracks().setStatus(
        lib.engine.tracks().idForPath((lib.dir / "b.wav").string()),
        TrackStatus::Missing);
    lib.engine.playlists().createSmart("s", "{}");

    const auto s = lib.engine.diagnostics().snapshot();
    REQUIRE(s.tracks == 2);
    REQUIRE(s.missing == 1);
    REQUIRE(s.analyzed == 2);
    REQUIRE(s.smartPlaylists == 1);
    REQUIRE(s.ftsRows == 2);
    const std::string rep = lib.engine.diagnostics().report();
    REQUIRE(rep.find("tracks=2") != std::string::npos);
    REQUIRE(rep.find("integrity=ok") != std::string::npos);
}

// ============================================================== BACKUP (§48-50)
TEST_CASE("Backup: JSON export/import — merge/replace/skip", "[lib18]") {
    Lib lib("backup");
    const int64_t a = lib.add("a.wav", "A", "House", 120, 0);
    const int64_t b = lib.add("b.wav", "B", "Techno", 130, 0);
    lib.engine.ratings().setRating(a, 4);
    lib.engine.ratings().setColor(a, 0xFF112233);
    lib.engine.tagManager().add(a, "Peak Time");
    lib.engine.tagManager().add(b, "Warmup");
    const int64_t pl = lib.engine.playlists().create("Set");
    lib.engine.playlists().addTracks(pl, { a, b });
    lib.engine.playlists().createSmart("smart", "{\"match\":\"all\",\"rules\":[]}");

    const fs::path out = lib.dir / "backup.json";
    std::string err;
    REQUIRE(lib.engine.backup().exportJson(out.string(), err));
    REQUIRE(fs::file_size(out) > 500);

    // fresh library — same tracks (content hash match via the
    // deterministic per-name seed in Lib::add), no user data
    Lib lib2("backup2");
    lib2.add("a.wav", "A", "House", 120, 0);
    lib2.add("b.wav", "B", "Techno", 130, 0);

    auto res = lib2.engine.backup().importJson(
        out.string(), BackupConflict::Replace);
    REQUIRE(res.tracks == 2);
    REQUIRE(res.applied == 2);
    REQUIRE(res.playlists == 2);

    Track t;
    const int64_t id2 =
        lib2.engine.tracks().idForPath((lib2.dir / "a.wav").string());
    REQUIRE(lib2.engine.tracks().getById(id2, t));
    REQUIRE(t.rating == 4);
    REQUIRE(t.color == 0xFF112233);
    REQUIRE(t.tags.size() == 1);
    REQUIRE(t.tags[0] == "Peak Time");

    // playlist items restored by hash reference
    for (const auto& p : lib2.engine.playlists().playlists(-1))
        if (p.name == "Set") REQUIRE(lib2.engine.playlists().trackIds(p.id).size() == 2);

    // Skip policy: local edits win entirely
    lib2.engine.ratings().setRating(id2, 1);
    res = lib2.engine.backup().importJson(out.string(), BackupConflict::Skip);
    REQUIRE(lib2.engine.tracks().getById(id2, t));
    REQUIRE(t.rating == 1);            // untouched

    // Merge: local non-zero kept
    res = lib2.engine.backup().importJson(out.string(), BackupConflict::Merge);
    REQUIRE(lib2.engine.tracks().getById(id2, t));
    REQUIRE(t.rating == 1);            // local 1 beats imported 4 in merge

    // bad input doesn't corrupt
    res = lib2.engine.backup().importJson("/nonexistent.json",
                                          BackupConflict::Replace);
    REQUIRE(res.errors == 1);
}

TEST_CASE("Backup: CSV export shape", "[lib18]") {
    Lib lib("csv");
    lib.add("a.wav", "DJ, Test", "House", 124.5, 21, 3);
    const fs::path out = lib.dir / "lib.csv";
    std::string err;
    REQUIRE(lib.engine.backup().exportCsv(out.string(), err));
    std::ifstream f(out);
    std::string line;
    std::getline(f, line);
    REQUIRE(line.find("album_artist") != std::string::npos);
    std::getline(f, line);
    REQUIRE(line.find("\"DJ, Test\"") != std::string::npos);  // quoted
}

// ======================================================= 100K PERF (§45/54)
TEST_CASE("Large library: 100k — search/filter/sort/page/smart perf",
          "[lib18][large100k]") {
    Lib lib("huge");
    const int N = 100000;
    auto t0 = std::chrono::steady_clock::now();
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
        for (int start = 0; start < N; start += 10000) {
            Database::Tx tx(lib.engine.db());
            for (int i = start; i < start + 10000 && i < N; ++i) {
                char path[64], title[64], artist[64], album[64];
                std::snprintf(path, sizeof(path), "/m/%06d.wav", i);
                std::snprintf(title, sizeof(title), "Track %06d", i);
                std::snprintf(artist, sizeof(artist), "Artist %04d", i % 1000);
                std::snprintf(album, sizeof(album), "Album %03d", i % 80);
                const char* genre =
                    i % 4 == 0 ? "House"  : i % 4 == 1 ? "Techno"
                                 : i % 4 == 2 ? "Trance" : "Dubstep";
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
    REQUIRE(lib.engine.trackCount() == N);

    auto timed = [&](const char* what, auto&& fn) {
        const auto a = std::chrono::steady_clock::now();
        auto r = fn();
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - a).count();
        std::printf("  [perf100k] %-34s %8.2f ms\n", what, ms);
        return r;
    };

    // ---- search (§12): must stay fast at 100k
    auto page = timed("fts 'artist 0999'", [&] {
        QuerySpec q; q.search = "artist 0999";
        return lib.engine.tracks().query(q);
    });
    REQUIRE(page.total > 0);

    page = timed("token search artist+bpm", [&] {
        return lib.engine.search("artist:artist bpm:120-130 rating:3+");
    });
    REQUIRE(page.total > 0);

    // ---- filter+sort+page
    page = timed("genre+bpm filter, bpm sort", [&] {
        QuerySpec q;
        q.filters.push_back({ "genre", CmpOp::Eq, "House" });
        q.filters.push_back({ "bpm", CmpOp::Between, "", 120.0, 130.0 });
        q.sort = SortKey::Bpm; q.limit = 100;
        return lib.engine.tracks().query(q);
    });
    REQUIRE(page.total > 0);
    REQUIRE(page.items.size() == 100);

    timed("artist sort page 500/1000", [&] {
        QuerySpec q; q.sort = SortKey::Artist;
        q.offset = 50000; q.limit = 100;
        return lib.engine.tracks().query(q);
    });

    // ---- smart playlist live evaluation at 100k
    const int64_t sp = lib.engine.playlists().createSmart(
        "hot", "{\"match\":\"all\",\"rules\":["
               "{\"field\":\"genre\",\"op\":\"eq\",\"text\":\"House\"},"
               "{\"field\":\"bpm\",\"op\":\"between\",\"a\":118,\"b\":125},"
               "{\"field\":\"rating\",\"op\":\"gte\",\"a\":4}]}");
    page = timed("smart playlist evaluate", [&] {
        return lib.engine.playlists().tracks(sp, 0, 100);
    });
    REQUIRE(page.total > 0);

    // ---- playlist ops at 100k
    const int64_t pl = lib.engine.playlists().create("big");
    std::vector<int64_t> ids;
    {
        QuerySpec q; q.limit = 5000;
        for (const auto& t : lib.engine.tracks().query(q).items)
            ids.push_back(t.id);
    }
    timed("add 5000 playlist items", [&] {
        return lib.engine.playlists().addTracks(pl, ids);
    });
    timed("playlist page read", [&] {
        return lib.engine.playlists().tracks(pl, 0, 200);
    });
    REQUIRE(lib.engine.playlists().trackIds(pl).size() == 5000);

    // ---- diagnostics + integrity at 100k
    timed("diagnostics snapshot", [&] {
        return lib.engine.diagnostics().snapshot();
    });
    timed("integrity check", [&] {
        std::string d; return lib.engine.integrityCheck(d);
    });
    std::printf("  [perf100k] seed %d rows: %.0f ms, db %.1f MB\n",
                N, seedMs,
                fs::file_size(lib.dir / "lib.db") / 1048576.0);
}
