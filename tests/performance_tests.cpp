// Performance engine tests: sync, quantize, cues, loops, beat jump,
// slip — driven headless through the same command path as the RT
/// callback (applyDeckCommand + SyncRtCore), no audio device needed.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <cmath>
#include <filesystem>
#include <vector>

#include "core/audio/TrackAudio.h"
#include "core/engine/DeckCommand.h"
#include "core/engine/DeckEngine.h"
#include "core/performance/PerformanceEngine.h"
#include "core/performance/SyncRt.h"
#include "core/data/AnalysisStore.h"
#include "core/data/Database.h"
#include "core/data/PerformanceStore.h"
#include "core/dsp/WsolaStretcher.h"

using namespace onda;
using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;

std::unique_ptr<TrackAudio> makeClick(double secs) {
    auto t = std::make_unique<TrackAudio>();
    t->fileSampleRate = kRate;
    t->deviceRate     = kRate;
    t->numFrames      = (int64_t)(kRate * secs);
    t->samples.resize((size_t)t->numFrames * 2);
    for (int64_t i = 0; i < t->numFrames; ++i) {
        const float v = 0.3f * (float)std::sin(2.0 * 3.14159265 * 220.0 * i / kRate);
        t->samples[(size_t)i * 2]     = v;
        t->samples[(size_t)i * 2 + 1] = v;
    }
    return t;
}

std::shared_ptr<BeatGrid> makeGrid(double bpm, int64_t firstBeat = 0) {
    auto g = std::make_shared<BeatGrid>();
    g->firstBeatFrame = firstBeat;
    g->setRate(kRate);
    g->segments.push_back({ firstBeat, 0, bpm });
    return g;
}

/// Two decks driven like the RT callback: command sink applies commands
/// instantly (deterministic), SyncRtCore runs the pre-pass each block.
struct Rig {
    static constexpr int kDecks = 4;
    static constexpr int kBlock = 512;

    std::array<DeckEngine, kDecks> decks;
    std::array<DeckStats, kDecks>  stats;
    std::array<std::unique_ptr<TrackAudio>, kDecks> tracks;
    SyncRtCore sync;
    PerformanceEngine perf;
    std::vector<PerfEvent> events;
    std::vector<float> l = std::vector<float>(kBlock),
                       r = std::vector<float>(kBlock);

    Rig() {
        perf.setCommandSink([this](const rt::Command& c) {
            if (auto* m = std::get_if<rt::SyncSetMaster>(&c)) {
                sync.masterDeck = m->deck >= kDecks ? -1 : (int)m->deck;
                return;
            }
            for (int i = 0; i < kDecks; ++i)
                applyDeckCommand(decks[i], i, c);
        });
        perf.addListener([this](const PerfEvent& e) { events.push_back(e); });
        for (int i = 0; i < kDecks; ++i) {
            decks[i].bind(&stats[i]);
            decks[i].setDeviceRate(kRate);
            decks[i].setStretcher(std::make_unique<WsolaStretcher>());
            decks[i].prepareDsp(kRate);
            perf.setGridSlot(i, &decks[i].pendingGrid_,
                             &decks[i].retiredGrid_, &stats[i]);
        }
    }

    void load(int deck, double bpm, double secs = 30.0) {
        tracks[deck] = makeClick(secs);
        decks[deck].pendingTrack_.store(tracks[deck].get());
        perf.attachGrid(deck, makeGrid(bpm), deck);
        block(1); // adopts track + grid
    }

    void block(int n = 1) {
        for (int i = 0; i < n; ++i) {
            sync.apply(decks.data(), kDecks, kBlock);
            for (auto& d : decks)
                d.renderPcm(l.data(), r.data(), kBlock);
        }
    }
    void secs(double s) { block((int)(s * kRate / kBlock)); }

    int64_t pos(int d) const { return stats[d].framesRead.load(); }
    int64_t fpb(double bpm) const { return (int64_t)(kRate * 60.0 / bpm); }

    bool hasEvent(PerfEventType t, int deck = -1) const {
        for (auto& e : events)
            if (e.type == t && (deck < 0 || e.deck == deck)) return true;
        return false;
    }
};

} // namespace

TEST_CASE("rig teardown is clean", "[perf]") {
    Rig rig;
    rig.load(0, 128.0);
    rig.decks[0].setPlaying(true);
    rig.secs(0.2);
}

// ================================ SYNC ================================

TEST_CASE("sync: bpm mode converges follower tempo to master", "[perf]") {
    Rig rig;
    rig.load(0, 128.0); rig.load(1, 124.0);
    rig.decks[0].setPlaying(true); rig.decks[1].setPlaying(true);
    rig.secs(0.5);

    rig.perf.enableSync(0, SyncMode::Beat);   // deck 0 becomes master
    rig.perf.enableSync(1, SyncMode::Bpm);
    rig.secs(3.0);

    REQUIRE(rig.stats[1].effBpm.load() == Approx(128.0).margin(0.15));
    REQUIRE(rig.perf.syncStatus(1) == SyncStatus::Synced);
}

TEST_CASE("sync: beat mode aligns beat phase", "[perf]") {
    Rig rig;
    rig.load(0, 128.0); rig.load(1, 124.0);
    rig.decks[0].setPlaying(true);
    rig.secs(0.6);
    rig.decks[1].setPlaying(true);
    rig.secs(0.4); // deliberately offset phase

    rig.perf.enableSync(0, SyncMode::Beat);
    rig.perf.enableSync(1, SyncMode::Beat);
    rig.secs(3.0);

    const double phaseA = rig.stats[0].beatPos.load()
                        - std::floor(rig.stats[0].beatPos.load());
    const double phaseB = rig.stats[1].beatPos.load()
                        - std::floor(rig.stats[1].beatPos.load());
    double d = std::fabs(phaseA - phaseB);
    d = std::min(d, 1.0 - d);
    REQUIRE(d < 0.02); // within 2% of a beat
    REQUIRE(std::fabs(rig.stats[1].syncErrMs.load()) < 5.0f);
}

TEST_CASE("sync: disable restores user pitch", "[perf]") {
    Rig rig;
    rig.load(0, 128.0); rig.load(1, 124.0);
    rig.decks[0].setPlaying(true); rig.decks[1].setPlaying(true);
    rig.perf.enableSync(0, SyncMode::Beat);
    rig.perf.enableSync(1, SyncMode::Beat);
    rig.secs(2.0);
    rig.perf.disableSync(1);
    rig.secs(0.5);
    REQUIRE(rig.stats[1].effBpm.load() == Approx(124.0).margin(0.5));
    REQUIRE(rig.perf.syncStatus(1) == SyncStatus::Off);
}

TEST_CASE("sync: missing grid reports NoGrid instead of engaging", "[perf]") {
    Rig rig;
    rig.load(0, 128.0);
    rig.tracks[1] = makeClick(10.0);
    rig.decks[1].pendingTrack_.store(rig.tracks[1].get());
    rig.block(1); // deck 1 loaded WITHOUT grid
    rig.decks[0].setPlaying(true); rig.decks[1].setPlaying(true);

    rig.perf.enableSync(0, SyncMode::Beat);
    rig.perf.enableSync(1, SyncMode::Beat);
    REQUIRE(rig.perf.syncStatus(1) == SyncStatus::NoGrid);
    REQUIRE(rig.hasEvent(PerfEventType::SyncStateChanged, 1));
}

TEST_CASE("sync: out-of-range tempo reports OutOfRange", "[perf]") {
    Rig rig;
    rig.load(0, 128.0); rig.load(1, 200.0);
    rig.decks[1].setPitchRange(8.0f);
    rig.decks[0].setPlaying(true); rig.decks[1].setPlaying(true);
    rig.perf.enableSync(0, SyncMode::Beat);
    rig.perf.enableSync(1, SyncMode::Bpm);
    rig.secs(1.5);
    REQUIRE(rig.stats[1].syncLimited.load());
    REQUIRE(rig.perf.syncStatus(1) == SyncStatus::OutOfRange);
}

TEST_CASE("sync: master change re-locks to new master", "[perf]") {
    Rig rig;
    rig.load(0, 128.0); rig.load(1, 124.0); rig.load(2, 130.0);
    for (int i = 0; i < 3; ++i) rig.decks[i].setPlaying(true);
    rig.perf.enableSync(0, SyncMode::Beat);
    rig.perf.enableSync(1, SyncMode::Bpm);
    rig.secs(1.0);
    REQUIRE(rig.stats[1].effBpm.load() == Approx(128.0).margin(0.3));

    rig.perf.setMasterDeck(2); // switch master to 130 bpm deck
    rig.secs(2.5);
    REQUIRE(rig.stats[1].effBpm.load() == Approx(130.0).margin(0.3));
    REQUIRE(rig.hasEvent(PerfEventType::MasterDeckChanged, 2));
}

TEST_CASE("sync: master clock reports bpm/beat/bar/phase", "[perf]") {
    Rig rig;
    rig.load(0, 120.0);
    rig.decks[0].setPlaying(true);
    rig.perf.enableSync(0, SyncMode::Beat);
    rig.secs(2.05);
    const auto c = rig.perf.masterClock();
    REQUIRE(c.valid);
    REQUIRE(c.bpm == Approx(120.0).margin(0.5));
    REQUIRE(c.beatPos >= 0.0);
    REQUIRE(c.phase >= 0.0); REQUIRE(c.phase < 1.0);
    REQUIRE(c.bar >= 1);
}

// ============================== QUANTIZE ==============================

TEST_CASE("quantize: cue trigger lands on beat boundary", "[perf]") {
    Rig rig;
    rig.load(0, 120.0); // fpb = 24000
    rig.decks[0].setPlaying(true);
    rig.secs(0.3);
    rig.perf.setQuantize(0, true);
    // place cue 30% into a beat
    const int64_t off = 5 * 24000 + (int64_t)(24000 * 0.3);
    rig.perf.setCue(0, 0, CueType::Hot, off);
    rig.perf.triggerCue(0, 0);
    rig.block(1); // the deck keeps playing ~1 block after the seek
    const int64_t p = rig.pos(0);
    const int64_t nearest = (p / 24000) * 24000;
    REQUIRE(std::min(std::llabs(p - nearest),
                     std::llabs(p - (nearest + 24000))) < 1100);
}

TEST_CASE("quantize off: cue lands exactly on authored frame", "[perf]") {
    Rig rig;
    rig.load(0, 120.0);
    rig.decks[0].setPlaying(true);
    rig.secs(0.2);
    const int64_t off = 5 * 24000 + 7000;
    rig.perf.setCue(0, 0, CueType::Hot, off);
    rig.perf.triggerCue(0, 0);
    rig.block(1); // ~1 block of playback after the seek
    REQUIRE(rig.pos(0) == Approx(off).margin(1100));
}

// ================================ CUES ================================

TEST_CASE("cues: hot cue triggers play; memory cue only seeks", "[perf]") {
    Rig rig;
    rig.load(0, 120.0);
    rig.perf.setCue(0, 0, CueType::Hot,    48000 * 4);
    rig.perf.setCue(0, 1, CueType::Memory, 48000 * 8);

    rig.perf.triggerCue(0, 0); // hot → starts playing
    rig.block(2);
    REQUIRE(rig.decks[0].isPlaying());

    rig.decks[0].setPlaying(false);
    rig.block(1);
    rig.perf.triggerCue(0, 1); // memory → stays paused
    rig.block(1);
    REQUIRE_FALSE(rig.decks[0].isPlaying());
    REQUIRE(rig.pos(0) == Approx(48000 * 8).margin(600));
    REQUIRE(rig.hasEvent(PerfEventType::CueTriggered, 0));
}

TEST_CASE("cues: create/update/delete + persistence roundtrip", "[perf]") {
    const auto dir = std::filesystem::temp_directory_path() / "onda_test";
    std::filesystem::create_directories(dir);
    const auto dbPath = (dir / "perf_test.db").string();
    std::filesystem::remove(dbPath);

    Database db;
    REQUIRE(db.open(dbPath));
    AnalysisStore aStore(db);
    PerformanceStore store(db);
    const int64_t trackId = aStore.trackIdFor("/tmp/fake_track.wav");
    REQUIRE(trackId >= 0);
    PerformanceEngine perf;
    perf.setStore(&store);
    auto g = makeGrid(120.0);

    perf.setGridSlot(0, nullptr, nullptr, nullptr);
    perf.attachGrid(0, g, trackId);
    perf.setCue(0, 0, CueType::Hot, 1000, -1, 0xFF00FF00, "drop");
    perf.setCue(0, 2, CueType::Memory, 2000);
    perf.updateCue(0, 0, 0xFF0000FF, "break");

    // reload from DB
    auto loaded = store.loadCues(trackId);
    REQUIRE(loaded.size() == 2);
    REQUIRE(loaded[0].slot == 0);
    REQUIRE(loaded[0].frame == 1000);
    REQUIRE(loaded[0].argb == 0xFF0000FF);
    REQUIRE(loaded[0].label == "break");
    REQUIRE(loaded[1].type == CueType::Memory);

    perf.deleteCue(0, 0);
    REQUIRE(store.loadCues(trackId).size() == 1);
    db.close();
}

// ================================ LOOPS ===============================

TEST_CASE("loops: beat loop spans the requested beats", "[perf]") {
    Rig rig;
    rig.load(0, 120.0); // fpb 24000
    rig.decks[0].setPlaying(true);
    rig.secs(1.0);
    rig.perf.loopBeats(0, 4.0);
    rig.block(2);
    const double in  = rig.stats[0].loopInSec.load();
    const double out = rig.stats[0].loopOutSec.load();
    REQUIRE((out - in) == Approx(2.0).margin(0.02)); // 4 beats @120 = 2s
    REQUIRE(rig.stats[0].loopOn.load());
    REQUIRE(rig.hasEvent(PerfEventType::LoopEntered, 0));
}

TEST_CASE("loops: halve / double / move", "[perf]") {
    Rig rig;
    rig.load(0, 120.0);
    rig.decks[0].setPlaying(true);
    rig.secs(1.0);
    rig.perf.loopBeats(0, 4.0);
    rig.block(1);
    const double in0 = rig.stats[0].loopInSec.load();
    const double len0 = rig.stats[0].loopOutSec.load() - in0;

    rig.perf.loopHalve(0); rig.block(1);
    REQUIRE(rig.stats[0].loopOutSec.load()
            - rig.stats[0].loopInSec.load() == Approx(len0 / 2).margin(0.01));

    rig.perf.loopDouble(0); rig.block(1);
    REQUIRE(rig.stats[0].loopOutSec.load()
            - rig.stats[0].loopInSec.load() == Approx(len0).margin(0.01));

    rig.perf.loopMove(0, 2.0); rig.block(1);
    REQUIRE(rig.stats[0].loopInSec.load() == Approx(in0 + 1.0).margin(0.02));
}

TEST_CASE("loops: playback actually wraps inside the loop", "[perf]") {
    Rig rig;
    rig.load(0, 120.0);
    rig.decks[0].setPlaying(true);
    rig.secs(1.0);
    rig.perf.loopBeats(0, 1.0); // 0.5 s loop
    rig.secs(2.0);              // several wraps
    const double in = rig.stats[0].loopInSec.load();
    const double out = rig.stats[0].loopOutSec.load();
    const double pos = rig.stats[0].positionSec.load();
    REQUIRE(pos >= in - 0.001);
    REQUIRE(pos <= out + 0.02);
}

// ============================= BEAT JUMP ==============================

TEST_CASE("beat jump: ±N beats preserving phase / quantize to grid", "[perf]") {
    Rig rig;
    rig.load(0, 120.0); // fpb 24000
    rig.decks[0].setPlaying(true);
    rig.secs(2.37); // arbitrary phase
    const int64_t p0 = rig.pos(0);

    rig.perf.beatJump(0, 4.0);
    rig.block(1);
    REQUIRE(rig.pos(0) == Approx(p0 + 4 * 24000).margin(1100));

    rig.perf.beatJump(0, -2.0);
    rig.block(1);
    REQUIRE(rig.pos(0) == Approx(p0 + 2 * 24000).margin(1100));
    REQUIRE(rig.hasEvent(PerfEventType::BeatJumped, 0));
}

// ================================ SLIP ================================

TEST_CASE("slip: loop roll returns to shadow position", "[perf]") {
    Rig rig;
    rig.load(0, 120.0);
    rig.decks[0].setPlaying(true);
    rig.secs(2.0);
    const int64_t before = rig.pos(0);

    rig.perf.loopRoll(0, 0.5);      // 1/2-beat roll
    rig.secs(1.0);                  // audible: rolling ~1s
    const int64_t rollPos = rig.pos(0); // inside the roll loop
    rig.perf.loopRollEnd(0);
    rig.block(2);

    const int64_t expected = before + (int64_t)(1.0 * kRate);
    REQUIRE(rig.pos(0) == Approx(expected).margin(1100));
    // and it must differ from where the roll left the playhead
    REQUIRE(std::llabs(rig.pos(0) - rollPos) > 1000);
    REQUIRE_FALSE(rig.stats[0].slip.load());
}

TEST_CASE("slip: hot cue during slip re-anchors the shadow (Phase 20)",
          "[perf]") {
    Rig rig;
    rig.load(0, 120.0);
    rig.decks[0].setPlaying(true);
    rig.secs(2.0);
    rig.perf.setSlip(0, true);
    rig.perf.setCue(0, 0, CueType::Hot, 24000 * 20);
    rig.perf.triggerCue(0, 0);   // permanent move → shadow re-anchors
    rig.secs(1.0);
    rig.perf.setSlip(0, false);  // release → continue from the CUE
    rig.block(2);                // timeline, no snap-back
    const int64_t expected = 24000 * 20 + (int64_t)(1.0 * kRate);
    REQUIRE(rig.pos(0) == Approx(expected).margin(3000));
}

// ============================== STRESS ================================

TEST_CASE("stress: two decks, sync + loops + cues + jump + slip", "[perf][stress]") {
    Rig rig;
    rig.load(0, 128.0, 60.0); rig.load(1, 124.0, 60.0);
    rig.decks[0].setPlaying(true); rig.decks[1].setPlaying(true);
    rig.perf.enableSync(0, SyncMode::Beat);
    rig.perf.enableSync(1, SyncMode::Beat);
    rig.perf.setQuantize(1, true);

    for (int s = 0; s < 30; ++s) {
        switch (s % 6) {
            case 0: rig.perf.loopBeats(1, 2.0); break;
            case 1: rig.perf.exitLoop(1);       break;
            case 2: rig.perf.beatJump(1, 4.0);  break;
            case 3: rig.perf.setCue(1, 0, CueType::Hot);
                    rig.perf.triggerCue(1, 0);  break;
            case 4: rig.perf.loopRoll(1, 0.5);  break;
            case 5: rig.perf.loopRollEnd(1);    break;
        }
        rig.secs(0.8);
        const double p = rig.stats[1].positionSec.load();
        REQUIRE(std::isfinite(p));
        REQUIRE(p >= 0.0); REQUIRE(p <= 60.0);
    }

    // after all that, sync still converged
    rig.secs(2.0);
    REQUIRE(rig.stats[1].effBpm.load() == Approx(128.0).margin(1.0));
    REQUIRE(std::fabs(rig.stats[1].syncErrMs.load()) < 30.0f);
}
