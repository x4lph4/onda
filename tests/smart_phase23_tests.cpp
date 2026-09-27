// Phase 23 — smart library: queries, crates, auto tags, energy,
// discovery, presets, stats, batch, resources, performance, failure.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "core/analysis/WaveformData.h"
#include "core/data/Database.h"
#include "core/library/FilterEngine.h"
#include "core/library/MusicLibraryEngine.h"
#include "core/library/SearchQuery.h"
#include "core/library/SmartPlaylistEngine.h"
#include "core/smart/AutoTagEngine.h"
#include "core/smart/EnergyAnalysisEngine.h"
#include "core/smart/LibraryBatchEngine.h"
#include "core/smart/LibraryPresets.h"
#include "core/smart/LibraryResourceManager.h"
#include "core/smart/LibraryStatistics.h"
#include "core/smart/SmartCrateEngine.h"
#include "core/smart/SmartLibraryEngine.h"
#include "core/smart/SmartQueryValidator.h"
#include "core/smart/TrackDiscoveryEngine.h"

using namespace onda;
namespace L = onda::library;
namespace S = onda::smart;
using Catch::Approx;
namespace fs = std::filesystem;

namespace {

fs::path tempRoot(const char* name) {
    // unique per process — concurrent test runs must not share DBs
    static const auto stamp = std::to_string(
        (long long)std::chrono::steady_clock::now()
            .time_since_epoch().count());
    fs::path d = fs::temp_directory_path()
        / ("onda_smarttest_" + stamp) / name;
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

/// Seed `count` tracks + analysis rows in one transaction.
/// key: i%24, bpm: 118+(i%31), energy: (i%10)/10-0.05 spread,
/// genre cycles House/Techno/Ambient, play_count: i%6,
/// date_added: now - (i%40)*86400 (last 40 days spread).
void seedTracks(L::MusicLibraryEngine& eng, int count,
                bool withAnalysis = true, int offset = 0) {
    std::lock_guard lk(eng.db().mutex());
    Database::Tx tx(eng.db());
    auto t = eng.db().prepare(
        "INSERT INTO tracks(path,filename,title,artist,genre,"
        "rating,play_count,last_played,date_added,updated_at,"
        "sample_rate,duration_sec,analysis_state)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)");
    auto a = eng.db().prepare(
        "INSERT INTO analysis(track_id,bpm,key_index,key_confidence,"
        "energy,lufs_i,rms,first_sound_frame)"
        " VALUES(?,?,?,?,?,?,?,?)");
    const int64_t now = (int64_t)std::time(nullptr);
    static const char* genres[] = { "House", "Techno", "Ambient" };
    for (int i = 0; i < count; ++i) {
        t.bind(1, "/t/" + std::to_string(offset + i));
        t.bind(2, "t" + std::to_string(i) + ".wav");
        t.bind(3, "T" + std::to_string(i));
        t.bind(4, "A" + std::to_string(i % 50));
        t.bind(5, genres[i % 3]);
        t.bind(6, (int64_t)(i % 6));
        t.bind(7, (int64_t)(i % 6));
        // -1800s margin so day-boundary presets don't race the clock
        t.bind(8, i % 4 ? now - (i % 40) * 86400 - 1800 : 0);
        t.bind(9, now - (int64_t)(i % 40) * 86400 - 1800);
        t.bind(10, now);
        t.bind(11, 48000.0);
        t.bind(12, 300.0 + (i % 120));
        t.bind(13, (int64_t)3);
        t.step(); t.reset();
        const int64_t id = eng.db().lastInsertId();
        if (withAnalysis) {
            a.bind(1, id);
            a.bind(2, 118.0 + (i % 31));
            a.bind(3, (int64_t)(i % 24));
            a.bind(4, 0.5 + (i % 5) * 0.1);
            a.bind(5, (double)((i % 10) / 10.0));
            a.bind(6, -30.0 + (i % 25));
            a.bind(7, 0.05 + (i % 20) * 0.02);
            a.bind(8, (int64_t)((i % 3) * 96000));  // 0s/2s/4s intro
            a.step(); a.reset();
        }
    }
    t.finalize(); a.finalize();
    tx.commit();
}

} // namespace

// ================================================== ENERGY (§13-18)
TEST_CASE("EnergyLevels: configurable bounds", "[smart23]") {
    S::EnergyLevels lv;
    using L2 = S::EnergyLevels::Level;
    REQUIRE(lv.levelFor(0.1f)  == L2::VeryLow);
    REQUIRE(lv.levelFor(0.3f)  == L2::Low);
    REQUIRE(lv.levelFor(0.5f)  == L2::Medium);
    REQUIRE(lv.levelFor(0.7f)  == L2::High);
    REQUIRE(lv.levelFor(0.9f)  == L2::VeryHigh);
    REQUIRE(lv.levelFor(-1.0f) == L2::Unknown);
    // configurable
    lv.high = 0.9f;
    REQUIRE(lv.levelFor(0.85f) == L2::High);
}

TEST_CASE("EnergyAnalysisEngine: scalar + curve + transitions", "[smart23]") {
    // scalar: loud loudness → high energy; quiet → low
    const float hi = S::EnergyAnalysisEngine::energyOf(-8.0f, 0.3f, 8.0f);
    const float lo = S::EnergyAnalysisEngine::energyOf(-35.0f, 0.02f, 2.0f);
    REQUIRE(hi > 0.65f);
    REQUIRE(lo < 0.35f);
    REQUIRE(S::EnergyAnalysisEngine::energyOf(-70.0f, 0.0f, 0.0f)
            == -1.0f);                                    // unknown

    // curve from a synthetic overview pyramid (quiet→loud)
    WaveformData wf;
    wf.channels = 2; wf.numFrames = 48000 * 200; wf.sampleRate = 48000.0;
    wf.overview.resize(4096 * 2);
    for (int b = 0; b < 4096; ++b) {
        const float v = (float)b / 4096.0f;
        for (int c = 0; c < 2; ++c)
            wf.overview[(size_t)b * 2 + c] = { -v, v, v };
    }
    const auto curve = S::EnergyAnalysisEngine::curveOf(wf, 96);
    REQUIRE(!curve.empty());
    REQUIRE(curve.size() <= 96);
    REQUIRE(curve.front().value < curve.back().value);   // rising
    REQUIRE(curve.back().value == Approx(1.0f).margin(0.01));

    // step curve → one transition detected
    for (int b = 0; b < 4096; ++b) {
        const float v = b < 2048 ? 0.2f : 0.9f;
        for (int c = 0; c < 2; ++c)
            wf.overview[(size_t)b * 2 + c] = { -v, v, v };
    }
    const auto step = S::EnergyAnalysisEngine::curveOf(wf, 96);
    const auto tr = S::EnergyAnalysisEngine::transitionsOf(step, 0.25f);
    REQUIRE(!tr.empty());
    REQUIRE(tr.front().delta > 0.5f);
    REQUIRE(tr.front().confidence > 0.5f);

    // structure: quiet tail → outro claim; loud track → none
    auto flat = step;
    for (size_t i = flat.size() - flat.size() / 8; i < flat.size(); ++i)
        flat[i].value = 0.05f;
    const auto st = S::EnergyAnalysisEngine::structureOf(flat, 200.0);
    bool hasOutro = false;
    for (auto& s : st) {
        if (s.kind == S::StructureSegment::Kind::Outro) {
            hasOutro = true;
            REQUIRE(s.confidence > 0.0f);
            REQUIRE(!s.userDefined);
        }
    }
    REQUIRE(hasOutro);

    // JSON round-trips
    const auto cj = S::EnergyAnalysisEngine::curveJson(curve);
    const auto parsed = S::EnergyAnalysisEngine::parseCurve(cj);
    REQUIRE(parsed.size() == curve.size());
    REQUIRE(parsed[0].value == Approx(curve[0].value));
    const auto sj = S::EnergyAnalysisEngine::structureJson(st);
    REQUIRE(S::EnergyAnalysisEngine::parseStructure(sj).size()
            == st.size());
}

TEST_CASE("Energy persists via AnalysisStore columns", "[smart23]") {
    const auto dir = tempRoot("energydb");
    Database db;
    REQUIRE(db.open((dir / "e.db").string()));
    db.exec("INSERT INTO tracks(path,updated_at) VALUES('/x',0)");
    const int64_t id = db.lastInsertId();
    db.exec(("INSERT INTO analysis(track_id,energy,energy_curve,"
             "energy_transitions,structure_json) VALUES("
             + std::to_string(id) + ",0.85,'[[0,1,0.5]]','[]','[]')")
            .c_str());
    auto s = db.prepare("SELECT energy,energy_curve FROM analysis"
                        " WHERE track_id=?");
    s.bind(1, id);
    REQUIRE(s.step());
    REQUIRE(s.colDouble(0) == Approx(0.85));
    REQUIRE(S::EnergyAnalysisEngine::parseCurve(s.colText(1)).size()
            == 1);
}

// ============================================ VALIDATOR (§8)
TEST_CASE("SmartQueryValidator: fields, ops, ranges", "[smart23]") {
    using V = S::SmartQueryValidator;
    L::SmartRules sr;
    REQUIRE(V::validate(sr).size() == 1);      // empty → warning only

    L::SmartRule r;
    r.field = "bpm"; r.op = "between"; r.a = 120; r.b = 128;
    sr.rules.push_back(r);
    REQUIRE(V::validate(sr).empty());

    r.field = "tempo";                         // invalid field
    sr.rules.back() = r;
    REQUIRE(V::validate(sr).size() == 1);

    r.field = "bpm"; r.op = "between"; r.a = 140; r.b = 120;
    sr.rules.back() = r;                       // inverted range
    REQUIRE(V::validate(sr).size() == 1);

    r.field = "genre"; r.op = "between"; r.a = 1; r.b = 2;
    sr.rules.back() = r;                       // numeric op on text
    REQUIRE(V::validate(sr).size() == 1);

    r.field = "bpm"; r.op = "contains"; r.text = "x";
    sr.rules.back() = r;                       // text op on numeric
    REQUIRE(V::validate(sr).size() == 1);

    r.field = "genre"; r.op = "contains"; r.text = "";
    sr.rules.back() = r;                       // empty text value
    REQUIRE(V::validate(sr).size() == 1);
}

// ============================================ QUERY BUILDER (§47/3)
TEST_CASE("QueryBuilder: rules, groups, AND/OR/NOT", "[smart23]") {
    S::QueryBuilder qb;
    qb.addRule("genre", "eq", "House");
    const int g = qb.addGroup(true);           // OR group
    qb.addRuleToGroup(g, { "bpm", "between", "", 120, 128 });
    qb.addRuleToGroup(g, { "bpm", "between", "", 128, 132 });
    qb.negateRule(qb.addRule("rating", "gte", "", 4.0));

    const auto& sr = qb.rules();
    REQUIRE(sr.rules.size() == 2);
    REQUIRE(sr.groups.size() == 1);
    REQUIRE(sr.groups[0].rules.size() == 2);
    REQUIRE(sr.groups[0].any);
    REQUIRE(sr.rules[1].negate);

    // round-trips through JSON
    const auto parsed =
        L::SmartPlaylistEngine::fromJson(qb.toJson());
    REQUIRE(parsed.rules.size() == 2);
    REQUIRE(parsed.groups.size() == 1);
    REQUIRE(parsed.rules[1].negate);
}

// ============================== SMART PLAYLIST + CRATE (§5/6/7)
TEST_CASE("Smart playlist + crate: same engine, live eval", "[smart23]") {
    const auto dir = tempRoot("smartpl");
    L::MusicLibraryEngine eng;
    REQUIRE(eng.open((dir / "lib.db").string()));
    seedTracks(eng, 500);
    S::SmartLibraryEngine smart(eng);

    S::QueryBuilder qb;
    qb.addRule("genre", "eq", "House");
    const auto& rules = qb.rules();
    REQUIRE(smart.validate(rules).empty());

    // §7 preview — no writes
    const int previews = smart.previewCount(rules);
    REQUIRE(previews == 167);                  // 500/3 ≈ 166.6 → 167

    // §5 save as playlist — evaluates live
    const int64_t pl = smart.saveAsPlaylist("House", rules);
    REQUIRE(pl > 0);
    REQUIRE(eng.playlists().tracks(pl, 0, 10).total == previews);

    // §6 save as crate — identical engine
    const int64_t cr = smart.saveAsCrate("HouseCrate", rules);
    REQUIRE(cr > 0);
    REQUIRE(smart.crates().tracks(cr, 0, 10).total == previews);

    // rules mutate → live eval follows, nothing materialised
    S::QueryBuilder qb2;
    qb2.addRule("genre", "eq", "Techno");
    REQUIRE(eng.playlists().setRules(
        pl, L::SmartPlaylistEngine::toJson(qb2.rules())));
    REQUIRE(eng.playlists().tracks(pl, 0, 10).total == 167);
}

// ============================================ AUTO TAGS (§9-12)
TEST_CASE("AutoTagEngine: rules, status, user-tag isolation", "[smart23]") {
    // rule evaluation (pure)
    auto sg = S::AutoTagEngine::suggest(0.9f, 128.0, 2.0);
    std::set<std::string> names;
    for (auto& s : sg) { names.insert(s.name);
                         REQUIRE(s.confidence > 0.4f); }
    REQUIRE(names.count("High Energy"));
    REQUIRE(names.count("Peak Time"));
    REQUIRE(names.count("Short Intro"));
    REQUIRE(!names.count("Warmup"));

    sg = S::AutoTagEngine::suggest(0.2f, 110.0, 20.0);
    names.clear();
    for (auto& s : sg) names.insert(s.name);
    REQUIRE(names.count("Low Energy"));
    REQUIRE(names.count("Warmup"));
    REQUIRE(names.count("Long Intro"));

    sg = S::AutoTagEngine::suggest(-1.0f, 128.0, 2.0);   // unknown
    REQUIRE(sg.empty());

    // persistence + status + no-reject-resurrect
    const auto dir = tempRoot("autotags");
    L::MusicLibraryEngine eng;
    REQUIRE(eng.open((dir / "lib.db").string()));
    seedTracks(eng, 60);
    S::SmartLibraryEngine smart(eng);
    const int wrote = smart.autoTags().runAll();
    REQUIRE(wrote > 0);

    auto sugg = smart.autoTags().of(1, S::AutoTagStatus::Suggested);
    REQUIRE(!sugg.empty());
    const auto first = sugg.front();
    REQUIRE(smart.autoTags().reject(1, first.name));
    // re-run: rejected must NOT come back (§12)
    const int before =
        smart.autoTags().of(1, S::AutoTagStatus::Suggested).size();
    smart.autoTags().run({ 1 });
    const auto all = smart.autoTags().of(
        1, S::AutoTagStatus::Suggested, true);
    bool rejectedStill = false;
    for (auto& t : all)
        if (t.name == first.name
            && t.status == S::AutoTagStatus::Rejected)
            rejectedStill = true;
    REQUIRE(rejectedStill);
    REQUIRE((int)smart.autoTags().of(
        1, S::AutoTagStatus::Suggested).size() <= before);

    // accept survives re-run too
    if (sugg.size() > 1) {
        REQUIRE(smart.autoTags().accept(1, sugg[1].name));
        smart.autoTags().run({ 1 });
        const auto acc =
            smart.autoTags().of(1, S::AutoTagStatus::Accepted);
        REQUIRE(!acc.empty());
    }

    // §10 user tags are a different table — auto-tag pass can't
    // delete them
    eng.tagManager().add(1, "my-user-tag");
    smart.autoTags().run({ 1 });
    const auto userTags = eng.tagManager().of(1);
    REQUIRE(std::find(userTags.begin(), userTags.end(),
                      "my-user-tag") != userTags.end());
}

// ============================================ DISCOVERY (§19-22)
TEST_CASE("TrackDiscoveryEngine: criteria + explainable reasons", "[smart23]") {
    const auto dir = tempRoot("discover");
    L::MusicLibraryEngine eng;
    REQUIRE(eng.open((dir / "lib.db").string()));
    seedTracks(eng, 200);
    S::SmartLibraryEngine smart(eng);

    S::DiscoveryCriteria c;
    c.excludeTrackId = 1;
    c.useBpm = true; c.bpm = 128.0; c.bpmTolerance = 1.0;
    c.keyRelation = 2; c.keyIndex = 21;        // strict Camelot 8A
    c.genre = "House";
    c.useEnergy = true; c.energyMode = 0;
    c.energyRef = 0.5f; c.energyRange = 0.2f;
    c.minRating = 1;
    c.limit = 32;

    const auto out = smart.discover(c);
    REQUIRE(!out.empty());
    for (const auto& m : out) {
        REQUIRE(m.track.id != 1);
        REQUIRE(!m.reasons.empty());           // §21/52 explainable
        // every reason names a field
        std::set<std::string> fields;
        for (auto& r : m.reasons) fields.insert(r.field);
        REQUIRE(fields.count("bpm"));
        REQUIRE(fields.count("key"));
        REQUIRE(fields.count("genre"));
        REQUIRE(fields.count("energy"));
        // hard criteria actually applied
        REQUIRE(std::fabs(m.track.bpm - 128.0) <= 1.0 + 1e-6);
        REQUIRE(m.track.genre.find("House") != std::string::npos);
        REQUIRE(m.track.rating >= 1);
    }
    // unplayed filter
    c = {};
    c.historyMode = 2; c.limit = 64;
    const auto un = smart.discover(c);
    for (const auto& m : un) REQUIRE(m.track.playCount == 0);
}

// ============================================ PRESETS (§28-33)
TEST_CASE("LibraryPresets: history + date + playcount", "[smart23]") {
    const auto dir = tempRoot("presets");
    L::MusicLibraryEngine eng;
    REQUIRE(eng.open((dir / "lib.db").string()));
    seedTracks(eng, 240);
    S::SmartLibraryEngine smart(eng);

    // recently added — desc by date_added
    auto p = smart.preset(S::LibraryPresets::Id::RecentlyAdded);
    REQUIRE(p.total == 240);
    REQUIRE(p.items.front().dateAdded
            >= p.items.back().dateAdded);

    // unplayed — play_count based (i%6 → 1/6th are 0)
    p = smart.preset(S::LibraryPresets::Id::Unplayed);
    REQUIRE(p.total == 40);
    for (auto& t : p.items) REQUIRE(t.playCount == 0);

    // most played
    p = smart.preset(S::LibraryPresets::Id::MostPlayed);
    REQUIRE(p.total == 200);
    REQUIRE(p.items.front().playCount
            >= p.items.back().playCount);

    // not played recently — played once but not in 30d
    p = smart.preset(S::LibraryPresets::Id::NotPlayedRecently);
    for (auto& t : p.items) {
        REQUIRE(t.lastPlayed > 0);
        REQUIRE(t.lastPlayed
                <= (int64_t)std::time(nullptr) - 30 * 86400);
    }

    // date presets — added in last 7d (i%40 ≤ 6 → 42 tracks)
    auto q = S::LibraryPresets::dateFiltered(
        "added_after", S::LibraryPresets::DateRange::Last7Days);
    p = eng.tracks().query(q);
    REQUIRE(p.total == 42);

    // play count buckets
    q = S::LibraryPresets::playCountBucket(0);
    REQUIRE(eng.tracks().query(q).total == 40);
    q = S::LibraryPresets::playCountRange(1, 5);
    REQUIRE(eng.tracks().query(q).total == 200);

    // sessions — record two plays in a session, query them back
    const int64_t sess = eng.history().beginSession("gig");
    eng.history().recordPlay(1, 0, 1000, 120.0, sess);
    eng.history().recordPlay(2, 1, 1100, 120.0, sess);
    auto page = smart.sessions().sessionTracks(sess);
    REQUIRE(page.items.size() == 2);
    const auto sl = smart.sessions().sessions();
    REQUIRE(!sl.empty());
    REQUIRE(sl.front().trackCount == 2);

    // §37/38 — missing-file + analysis-status + duplicates views
    eng.tracks().setStatus(1, L::TrackStatus::Missing);
    p = smart.preset(S::LibraryPresets::Id::MissingFiles);
    REQUIRE(p.total == 1);
    REQUIRE(p.items.front().id == 1);

    seedTracks(eng, 5, false, 240);          // distinct paths
    p = smart.preset(S::LibraryPresets::Id::Unanalyzed);
    REQUIRE(p.total == 5);

    eng.db().exec(
        "UPDATE tracks SET content_hash=42 WHERE id IN (2,3)");
    const auto groups = smart.duplicatesView();
    REQUIRE(!groups.empty());
    bool pairFound = false;
    for (auto& g : groups)
        if (std::find(g.trackIds.begin(), g.trackIds.end(), 2)
                != g.trackIds.end()
            && std::find(g.trackIds.begin(), g.trackIds.end(), 3)
                != g.trackIds.end())
            pairFound = true;
    REQUIRE(pairFound);                      // view, never auto-delete
    L::Track stillThere;
    REQUIRE(eng.tracks().getById(2, stillThere));
}

// ============================================ STATISTICS (§34-36)
TEST_CASE("LibraryStatistics: totals + distributions", "[smart23]") {
    const auto dir = tempRoot("stats");
    L::MusicLibraryEngine eng;
    REQUIRE(eng.open((dir / "lib.db").string()));
    seedTracks(eng, 300);
    S::SmartLibraryEngine smart(eng);
    smart.autoTags().runAll();

    const auto s = smart.statistics().compute();
    REQUIRE(s.totalTracks == 300);
    REQUIRE(s.analyzed == 300);
    REQUIRE(s.artists == 50);
    REQUIRE(s.genres == 3);
    REQUIRE(s.avgBpm > 118.0);
    int64_t keySum = 0;
    for (int i = 0; i < 24; ++i) keySum += s.keyCounts[i];
    REQUIRE(keySum == 300);
    int64_t enSum = 0;
    for (int i = 1; i < 6; ++i) enSum += s.energyCounts[i];
    REQUIRE(enSum == 300);
    REQUIRE(s.avgEnergy >= 0.0);
    REQUIRE(s.autoTagsSuggested > 0);

    // tag stats
    eng.tagManager().add(1, "peak");
    eng.tagManager().add(2, "peak");
    eng.tagManager().add(3, "warmup");
    auto top = smart.statistics().topUserTags();
    REQUIRE(!top.empty());
    REQUIRE(top.front().first == "peak");
    REQUIRE(top.front().second == 2);
    eng.tagManager().add(4, "unused-candidate");
    eng.tagManager().remove(4, "unused-candidate");
    REQUIRE(!smart.statistics().unusedUserTags().empty());
}

// ============================================ BATCH (§39-40)
TEST_CASE("LibraryBatchEngine: ops + cancel + single-op undo", "[smart23]") {
    const auto dir = tempRoot("batch");
    L::MusicLibraryEngine eng;
    REQUIRE(eng.open((dir / "lib.db").string()));
    seedTracks(eng, 50);
    S::SmartLibraryEngine smart(eng);
    std::vector<int64_t> ids = { 1, 2, 3, 4, 5 };

    // add tag — undo restores prior membership in one op
    REQUIRE(smart.batch().apply(ids, S::LibraryBatchEngine::Op::AddTag,
                                "setA") == 5);
    REQUIRE(eng.tagManager().of(1).size() == 1);
    REQUIRE(smart.batch().canUndo());
    REQUIRE(smart.batch().undoLast() == 5);
    REQUIRE(eng.tagManager().of(1).empty());

    // remove tag — undo puts it back
    eng.tagManager().add(1, "keep");
    REQUIRE(smart.batch().apply({ 1, 2 },
                                S::LibraryBatchEngine::Op::RemoveTag,
                                "keep") >= 1);
    smart.batch().undoLast();
    const auto t1tags = eng.tagManager().of(1);
    REQUIRE(std::find(t1tags.begin(), t1tags.end(), "keep")
            != t1tags.end());

    // rating — undo restores prior value
    REQUIRE(smart.batch().apply(ids, S::LibraryBatchEngine::Op::SetRating,
                                "", 5) == 5);
    L::Track t; REQUIRE(eng.tracks().getById(1, t));
    REQUIRE(t.rating == 5);
    smart.batch().undoLast();
    REQUIRE(eng.tracks().getById(1, t));
    REQUIRE(t.rating == 0);                    // seeded i%6

    // cancel mid-flight
    std::vector<int64_t> big(100);
    for (int i = 0; i < 100; ++i) big[(size_t)i] = i + 1;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    // cancel before start → zero processed
    cancel->store(true);
    const int n = smart.batch().apply(big,
        S::LibraryBatchEngine::Op::AddTag, "never", 0, cancel);
    REQUIRE(n == 0);
    REQUIRE(smart.batch().progress().cancelled);

    // async facade runs through post() — the library worker contract
    bool done = false;
    smart.batch().applyAsync(
        [&eng](std::function<void()> fn) { eng.post(std::move(fn)); },
        { 6, 7 }, S::LibraryBatchEngine::Op::AddTag, "async", 0,
        [&done](int) { done = true; });
    eng.runPending();
    REQUIRE(done);
    REQUIRE(eng.tagManager().of(6).size() == 1);

    // analyze op goes through the analyzeFn seam
    std::vector<int64_t> analyzed;
    smart.batch().setAnalyzeFn(
        [&analyzed](int64_t id, bool force) {
            analyzed.push_back(id); (void)force;
        });
    smart.batch().apply({ 9, 10 },
                        S::LibraryBatchEngine::Op::Reanalyze, "", 0);
    REQUIRE(analyzed.size() == 2);
}

// ============================================ RESOURCES (§43-44)
TEST_CASE("LibraryResourceManager: limits + safe cleanup", "[smart23]") {
    const auto dir = tempRoot("resources");
    L::MusicLibraryEngine eng;
    REQUIRE(eng.open((dir / "lib.db").string()));
    S::SmartLibraryEngine smart(eng);

    auto l = smart.resources().limits();
    REQUIRE(l.analysisWorkers >= 1);
    l.analysisWorkers = 4;
    l.analyzeOnImport = 2;
    smart.resources().setLimits(l);
    const auto l2 = smart.resources().limits();
    REQUIRE(l2.analysisWorkers == 4);
    REQUIRE(l2.analyzeOnImport == 2);

    // cleanup never touches user or real analysis data (§44). FK
    // enforcement makes true orphans impossible, so the call must be
    // a no-op on a clean DB and leave every row intact.
    seedTracks(eng, 20);
    eng.tagManager().add(1, "userdata");
    eng.playlists().create("pl");
    const int cleaned = smart.resources().cleanOrphanedAnalysis();
    REQUIRE(cleaned == 0);                     // nothing orphaned
    REQUIRE(!eng.tagManager().of(1).empty());
    L::Track t;
    REQUIRE(eng.tracks().getById(1, t));
    auto st = eng.db().prepare(
        "SELECT COUNT(*) FROM analysis WHERE track_id=?");
    st.bind(1, (int64_t)1);
    REQUIRE(st.step());
    REQUIRE(st.colInt(0) == 1);                // real analysis kept
    const auto cs = smart.resources().cacheStats();
    REQUIRE(cs.analysis == 20);
}

// ============================================ SEARCH TOKENS (§45)
TEST_CASE("SearchQuery: energy/added/unplayed/session/autotag", "[smart23]") {
    L::QuerySpec q;
    L::SearchQuery::parse("energy:high", q);
    REQUIRE(q.filters.size() == 1);
    REQUIRE(q.filters[0].field == "energy_score");

    q = {};
    L::SearchQuery::parse("energy:0.6-0.9", q);
    REQUIRE(q.filters[0].op == L::CmpOp::Between);

    q = {};
    L::SearchQuery::parse("added:today", q);
    REQUIRE(q.filters.size() == 2);
    REQUIRE(q.filters[0].field == "added_after");

    q = {};
    L::SearchQuery::parse("unplayed:1", q);
    REQUIRE(q.filters[0].field == "unplayed");

    q = {};
    L::SearchQuery::parse("session:3", q);
    REQUIRE(q.filters[0].field == "session");

    q = {};
    L::SearchQuery::parse("autotag:warmup", q);
    REQUIRE(q.filters[0].field == "autotag");

    // garbage falls through to FTS as before
    q = {};
    L::SearchQuery::parse("added:nonsense", q);
    REQUIRE(q.filters.empty());
    REQUIRE(q.search == "added:nonsense");
}

// ============================================ FILTER OPS (§4)
TEST_CASE("FilterEngine: not_contains/ends_with/autotag clauses", "[smart23]") {
    std::vector<std::string> binds;
    L::FilterCond c;
    c.field = "genre"; c.op = L::CmpOp::NotContains; c.text = "Rock";
    const std::string w = L::FilterEngine::where({ c }, binds);
    REQUIRE(w.find("NOT LIKE") != std::string::npos);

    c = {}; c.field = "artist"; c.op = L::CmpOp::EndsWith;
    c.text = "DJ";
    binds.clear();
    const std::string w2 = L::FilterEngine::where({ c }, binds);
    REQUIRE(w2.find("LIKE ?") != std::string::npos);
    REQUIRE(binds.front() == "%DJ");

    c = {}; c.field = "unplayed"; c.op = L::CmpOp::Eq; c.a = 1.0;
    binds.clear();
    REQUIRE(L::FilterEngine::where({ c }, binds).find("play_count = 0")
            != std::string::npos);

    c = {}; c.field = "autotag"; c.op = L::CmpOp::Contains;
    c.text = "Warmup"; c.a = -1.0;
    binds.clear();
    const std::string w3 = L::FilterEngine::where({ c }, binds);
    REQUIRE(w3.find("auto_tags") != std::string::npos);

    // isnotnull round-trips to IS NOT NULL via negate
    L::SmartRule r; r.field = "comments"; r.op = "isnotnull";
    const auto cond = r.toCond();
    REQUIRE(cond.negate);
}

// ==================== LIBRARY SCALE + EXPLAINABILITY (§51/52)
TEST_CASE("Smart library at 100k: query latency", "[smart23]") {
    const auto dir = tempRoot("scale");
    L::MusicLibraryEngine eng;
    REQUIRE(eng.open((dir / "lib.db").string()));
    const auto t0 = std::chrono::steady_clock::now();
    seedTracks(eng, 100000);
    const double seedMs =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
    INFO("seed 100k: " << seedMs << " ms");
    S::SmartLibraryEngine smart(eng);

    auto timed = [&](const char* what, L::QuerySpec& q) {
        const auto a = std::chrono::steady_clock::now();
        auto page = eng.tracks().query(q);
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - a).count();
        INFO(what << ": " << page.total << " in " << ms << " ms");
        return page;
    };

    L::QuerySpec q;
    L::FilterCond f;
    f.field = "energy_score"; f.op = L::CmpOp::Between;
    f.a = 0.6; f.b = 0.9;
    q.filters.push_back(f);
    auto p = timed("energy 0.6-0.9", q);
    REQUIRE(p.total > 0);

    // smart playlist preview at scale
    S::QueryBuilder qb;
    qb.addRule("genre", "eq", "House");
    qb.addRule("energy_score", "gte", "", 0.5);
    const auto a = std::chrono::steady_clock::now();
    const int n = smart.previewCount(qb.rules());
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - a).count();
    INFO("smart preview 100k: " << n << " in " << ms << " ms");
    REQUIRE(n > 0);
    REQUIRE(ms < 1500.0);

    // discovery at scale stays explainable
    S::DiscoveryCriteria dc;
    dc.useBpm = true; dc.bpm = 128; dc.bpmTolerance = 2.0;
    dc.keyRelation = 2; dc.keyIndex = 21;
    dc.limit = 32;
    const auto b = std::chrono::steady_clock::now();
    const auto out = smart.discover(dc);
    const double dms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - b).count();
    INFO("discover 100k: " << out.size() << " in " << dms << " ms");
    for (const auto& m : out) REQUIRE(!m.reasons.empty());

    // presets at scale
    q = S::LibraryPresets::unplayed();
    p = timed("unplayed 100k", q);
    REQUIRE(p.total > 0);
    q = S::LibraryPresets::mostPlayed(3);
    p = timed("mostPlayed>=3 100k", q);
    REQUIRE(p.total > 0);
}

// ==================== FAILURE / INTEGRITY (§53)
TEST_CASE("Smart library failure modes stay safe", "[smart23]") {
    const auto dir = tempRoot("failure");
    L::MusicLibraryEngine eng;
    REQUIRE(eng.open((dir / "lib.db").string()));
    seedTracks(eng, 100);
    S::SmartLibraryEngine smart(eng);

    // corrupt rules JSON → parse fails gracefully, no crash
    bool ok = true;
    auto sr = L::SmartPlaylistEngine::fromJson("{garbage[[[", &ok);
    REQUIRE(!ok);
    // preview with corrupt JSON → empty rules → match-all (never throws)
    const auto pg = L::SmartPlaylistEngine::evaluate(
        eng.tracks(), "{garbage[[[", {});
    REQUIRE(pg.total == 100);

    // smart playlist row with corrupt stored rules → same path
    const int64_t pl = eng.playlists().createSmart(
        "broken", "{not json");
    REQUIRE(pl > 0);
    REQUIRE(eng.playlists().tracks(pl, 0, 10).total == 100);

    // missing metadata rows: a track with no analysis row must not
    // crash queries and must report energy as unknown (-1)
    {
        std::lock_guard lk(eng.db().mutex());
        Database::Tx tx(eng.db());
        auto s = eng.db().prepare(
            "INSERT INTO tracks(path,filename,title) VALUES"
            "('/bare','bare.wav','Bare')");
        s.step();
        tx.commit();
    }
    L::QuerySpec q;
    const auto p = eng.tracks().query(q);
    bool sawBare = false;
    for (const auto& t : p.items)
        if (t.title == "Bare") {
            sawBare = true;
            REQUIRE(t.energyScore < 0.0f);     // unknown, not 0
        }
    REQUIRE(sawBare);

    // batch on a deleted/nonexistent track — fails that item only
    const int n = smart.batch().apply(
        { 99998, 1, 99999 }, S::LibraryBatchEngine::Op::AddTag, "x");
    REQUIRE(n >= 1);                           // real track tagged
    std::string det;
    REQUIRE(eng.integrityCheck(det));          // db consistent
}

TEST_CASE("SmartLibraryEngine: facade completeness", "[smart23]") {
    const auto dir = tempRoot("facade");
    L::MusicLibraryEngine eng;
    REQUIRE(eng.open((dir / "lib.db").string()));
    S::SmartLibraryEngine smart(eng);
    REQUIRE(&smart.library() == &eng);
    REQUIRE(&smart.crates() != nullptr);
    REQUIRE(&smart.autoTags() != nullptr);
    REQUIRE(&smart.sessions() != nullptr);
    REQUIRE(&smart.statistics() != nullptr);
    REQUIRE(&smart.batch() != nullptr);
    REQUIRE(&smart.resources() != nullptr);
    const auto d = smart.diagnostics();
    REQUIRE(d.stats.totalTracks == 0);
}
