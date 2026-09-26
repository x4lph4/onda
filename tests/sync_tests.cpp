// Phase 15 — Professional Sync Engine tests: TempoMaster policies,
// pause behaviour, sync lock, pitch interaction, confidence,
// diagnostics, extended quantize, beatgrid/tempo-map following and
// a four-deck stress pass. Deterministic — no audio device.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <chrono>
#include <cmath>
#include <vector>

#include "core/audio/TrackAudio.h"
#include "core/engine/DeckCommand.h"
#include "core/engine/DeckEngine.h"
#include "core/performance/PerformanceEngine.h"
#include "core/performance/SyncRt.h"
#include "core/sync/BeatPhaseTracker.h"
#include "core/sync/MasterClock.h"
#include "core/sync/SyncCorrection.h"
#include "core/sync/TempoFollower.h"
#include "core/sync/TempoMaster.h"
#include "core/dsp/WsolaStretcher.h"
#include "ui/CommandManager.h"
#include "ui/EngineCommands.h"

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

/// Grid with a tempo change at `changeBeat` — Track C: 128 → 130 BPM.
std::shared_ptr<BeatGrid> makeTempoMapGrid(double bpm1, double bpm2,
                                           int changeBeat) {
    auto g = makeGrid(bpm1);
    const int64_t f0 = g->beatToFrame((double)changeBeat);
    g->segments.push_back({ f0, (int64_t)changeBeat, bpm2 });
    return g;
}

/// Rig like performance_tests — command sink applies instantly and
/// also *records* every rt::Command so policy routing is assertable.
struct Rig {
    static constexpr int kDecks = 4;
    static constexpr int kBlock = 512;

    std::array<DeckEngine, kDecks> decks;
    std::array<DeckStats, kDecks>  stats;
    std::array<std::unique_ptr<TrackAudio>, kDecks> tracks;
    SyncRtCore sync;
    PerformanceEngine perf;
    std::vector<PerfEvent>   events;
    std::vector<rt::Command> sent;
    std::vector<float> l = std::vector<float>(kBlock),
                       r = std::vector<float>(kBlock);

    Rig() {
        perf.setCommandSink([this](const rt::Command& c) {
            sent.push_back(c);
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
        block(1);
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

    template <typename T>
    int countCmd() const {
        int n = 0;
        for (auto& c : sent)
            if (std::get_if<T>(&c)) ++n;
        return n;
    }
};

} // namespace

// ==================== TempoMaster policies (§4-5, §22-23) ====================

TEST_CASE("tempo master: AUTO never re-picks a still-valid master", "[sync]") {
    sync::TempoMaster tm;
    const std::array<bool, 4> synced  { true, true, true, false };
    const std::array<bool, 4> playing { true, true, false, false };
    // current master 1 stays even though deck 0 is "lower"
    REQUIRE(tm.resolveAuto(synced, playing, 1) == 1);
    // invalid current → lowest PLAYING synced deck wins
    REQUIRE(tm.resolveAuto(synced, playing, -1) == 0);
    // none playing → first synced deck (clock can freeze, role stays)
    const std::array<bool, 4> none {};
    REQUIRE(tm.resolveAuto(synced, none, -1) == 0);
    // nobody synced → none
    REQUIRE(tm.resolveAuto(none, playing, -1) == -1);
}

TEST_CASE("tempo master: pinned source / None / External resolve", "[sync]") {
    sync::TempoMaster tm;
    const std::array<bool, 4> synced  { true, false, true, false };
    const std::array<bool, 4> playing { true, true,  false, false };
    tm.source = sync::MasterSource::DeckB;
    // pinned B is the role even though B is not playing or synced
    REQUIRE(tm.resolve(synced, playing, -1) == 1);
    tm.source = sync::MasterSource::None;
    REQUIRE(tm.resolve(synced, playing, -1) == -1);
    tm.source = sync::MasterSource::External;
    REQUIRE(tm.resolve(synced, playing, -1) == -1);
}

TEST_CASE("tempo master: MasterSource::None refuses sync deterministically", "[sync]") {
    Rig rig;
    rig.load(0, 128.0);
    rig.decks[0].setPlaying(true);
    rig.perf.setMasterSource(sync::MasterSource::None);
    rig.perf.enableSync(0, SyncMode::Beat);
    REQUIRE(rig.perf.syncStatus(0) == SyncStatus::NoMaster);
    REQUIRE(rig.perf.masterDeck() == -1);
}

TEST_CASE("tempo master: pinned source makes that deck master", "[sync]") {
    Rig rig;
    rig.load(0, 128.0); rig.load(1, 126.0);
    rig.decks[0].setPlaying(true); rig.decks[1].setPlaying(true);
    rig.perf.setMasterSource(sync::MasterSource::DeckB);
    rig.perf.enableSync(0, SyncMode::Beat);   // syncs to pinned B
    REQUIRE(rig.perf.masterDeck() == 1);
    rig.secs(2.5);
    REQUIRE(rig.stats[0].effBpm.load() == Approx(126.0).margin(0.2));
}

TEST_CASE("tempo master: cycleMaster steps Auto→A→B→C→D→Auto", "[sync]") {
    Rig rig;
    REQUIRE(rig.perf.masterSource() == sync::MasterSource::Auto);
    rig.perf.cycleMaster();
    REQUIRE(rig.perf.masterSource() == sync::MasterSource::DeckA);
    rig.perf.cycleMaster(); rig.perf.cycleMaster(); rig.perf.cycleMaster();
    REQUIRE(rig.perf.masterSource() == sync::MasterSource::DeckD);
    rig.perf.cycleMaster();
    REQUIRE(rig.perf.masterSource() == sync::MasterSource::Auto);
}

TEST_CASE("master pause: ClockHolds keeps role + converged tempo", "[sync]") {
    Rig rig;
    rig.load(0, 128.0); rig.load(1, 124.0);
    rig.decks[0].setPlaying(true); rig.decks[1].setPlaying(true);
    rig.perf.enableSync(0, SyncMode::Beat);
    rig.perf.enableSync(1, SyncMode::Beat);
    rig.secs(2.0);
    rig.decks[0].setPlaying(false);   // master pauses
    rig.perf.poll(); rig.secs(0.5);
    REQUIRE(rig.perf.masterDeck() == 0);           // role kept
    REQUIRE(rig.sync.masterDeck == 0);
    // follower holds its last converged tempo — no spike, no re-master
    REQUIRE(rig.stats[1].effBpm.load() == Approx(128.0).margin(0.2));
}

TEST_CASE("master pause: ClockStops releases the RT clock", "[sync]") {
    Rig rig;
    rig.load(0, 128.0); rig.load(1, 124.0);
    rig.decks[0].setPlaying(true); rig.decks[1].setPlaying(true);
    rig.perf.setMasterPausePolicy(sync::MasterPausePolicy::ClockStops);
    rig.perf.enableSync(0, SyncMode::Beat);
    rig.perf.enableSync(1, SyncMode::Beat);
    rig.secs(0.5);
    rig.decks[0].setPlaying(false);
    rig.perf.poll();
    REQUIRE(rig.sync.masterDeck == -1);            // clock invalid
    rig.decks[0].setPlaying(true);
    rig.perf.poll();
    REQUIRE(rig.sync.masterDeck == 0);             // clock resumes
}

TEST_CASE("master pause: FollowOtherDeck re-resolves on poll", "[sync]") {
    Rig rig;
    rig.load(0, 128.0); rig.load(1, 124.0);
    rig.decks[0].setPlaying(true); rig.decks[1].setPlaying(true);
    rig.perf.setMasterPausePolicy(sync::MasterPausePolicy::FollowOtherDeck);
    rig.perf.enableSync(0, SyncMode::Beat);
    rig.perf.enableSync(1, SyncMode::Beat);
    rig.secs(0.5);
    rig.decks[0].setPlaying(false);
    rig.perf.poll();
    REQUIRE(rig.perf.masterDeck() == 1);           // promoted deck 1
    REQUIRE(rig.sync.masterDeck == 1);
    REQUIRE(rig.hasEvent(PerfEventType::MasterDeckChanged, 1));
}

// ==================== phase tracking / follower / correction ====================

TEST_CASE("beat phase tracker: beat/fraction/bar/phase + tempo map", "[sync]") {
    auto g = makeGrid(120.0);            // fpb = 24000
    auto p = sync::BeatPhaseTracker::track(*g, 24000 * 32 + 10080);
    REQUIRE(p.beat  == Approx(32.42).margin(0.001));
    REQUIRE(p.phase == Approx(0.42).margin(0.001));
    REQUIRE(p.bar   == 9);               // beats 32..35 → bar 9 (1-based)
    REQUIRE(p.bpm   == Approx(120.0));

    // tempo-map following: second segment at beat 16 → 150 BPM
    auto tm = makeTempoMapGrid(120.0, 150.0, 16);
    const int64_t fB16 = tm->beatToFrame(16.0);
    auto p2 = sync::BeatPhaseTracker::track(*tm, fB16 + 100);
    REQUIRE(p2.bpm == Approx(150.0).margin(0.01));

    // predictor: one block ahead advances phase deterministically
    auto pr = sync::BeatPhaseTracker::predict(*g, 24000 * 4, 1.0, 512);
    REQUIRE(pr.beat == Approx(4.0 + 512.0 / 24000.0).margin(1e-6));
}

TEST_CASE("tempo follower: target multiplier + SYNC RANGE LIMIT", "[sync]") {
    auto ok = sync::TempoFollower::resolve(126.0, 128.0, 8.0f);
    REQUIRE(ok.mul == Approx(128.0 / 126.0).margin(1e-9));
    REQUIRE_FALSE(ok.limited);
    auto far = sync::TempoFollower::resolve(90.0, 128.0, 8.0f);
    REQUIRE(far.limited);                       // impossible tempo → flag
    REQUIRE(far.mul == Approx(1.08));           // clamped to range edge
    auto zero = sync::TempoFollower::resolve(0.0, 128.0, 8.0f);
    REQUIRE(zero.mul == Approx(1.0));           // degenerate → unity
}

TEST_CASE("sync correction: deadband/chase/reposition + wrap", "[sync]") {
    const double fpb = 22500.0;  // 128 BPM @48k
    REQUIRE(sync::SyncCorrection::classify(4.0, fpb)
            == sync::CorrectionKind::Deadband);
    REQUIRE(sync::SyncCorrection::classify(500.0, fpb)
            == sync::CorrectionKind::Chase);
    REQUIRE(sync::SyncCorrection::classify(10000.0, fpb)
            == sync::CorrectionKind::Reposition);  // >35% of a beat
    // chase rate is bounded to ~5% of the block — inaudible slewing
    REQUIRE(std::fabs(sync::SyncCorrection::chaseRate(1000.0, 512, 1.0))
            <= 0.05 + 1e-9);
    // wrap shortest direction: +0.7 beats == −0.3
    REQUIRE(sync::SyncCorrection::wrapBeats(0.7)  == Approx(-0.3));
    REQUIRE(sync::SyncCorrection::wrapBeats(-0.7) == Approx(0.3));
    REQUIRE(sync::SyncCorrection::wrapBeats(0.25) == Approx(0.25));
}

// ==================== deck sync state / confidence / diagnostics ====================

TEST_CASE("deck sync state: enabled/master/target/phase populated", "[sync]") {
    Rig rig;
    rig.load(0, 128.0); rig.load(1, 124.0);
    rig.decks[0].setPlaying(true); rig.decks[1].setPlaying(true);
    rig.perf.enableSync(0, SyncMode::Beat);
    rig.perf.enableSync(1, SyncMode::Beat);
    rig.secs(2.5);

    const auto s0 = rig.perf.deckSyncState(0);
    REQUIRE(s0.enabled); REQUIRE(s0.isMaster);
    REQUIRE(s0.mode == SyncMode::Beat);
    REQUIRE(s0.bpm == Approx(128.0).margin(0.1));

    const auto s1 = rig.perf.deckSyncState(1);
    REQUIRE(s1.enabled); REQUIRE_FALSE(s1.isMaster);
    REQUIRE(s1.targetBpm == Approx(128.0).margin(0.1));
    REQUIRE(s1.bpm == Approx(128.0).margin(0.2));
    REQUIRE(s1.beatPos >= 0.0);
    REQUIRE(s1.phase >= 0.0); REQUIRE(s1.phase < 1.0);
    REQUIRE(s1.status == SyncStatus::Synced);
    REQUIRE_FALSE(s1.tempoLock);
}

TEST_CASE("sync confidence: data quality + phase stability", "[sync]") {
    Rig rig;
    rig.load(0, 128.0); rig.load(1, 124.0);
    rig.decks[0].setPlaying(true); rig.decks[1].setPlaying(true);
    REQUIRE(rig.perf.syncConfidence(1) == Approx(0.0f)); // off → 0
    rig.perf.enableSync(0, SyncMode::Beat);
    rig.perf.enableSync(1, SyncMode::Beat);
    rig.secs(3.0);   // converged
    const float c1 = rig.perf.syncConfidence(1);
    REQUIRE(c1 > 0.5f);
    REQUIRE(rig.perf.syncConfidence(0) > 0.5f);   // master = grid conf
    // out-of-range follower: limited → low confidence (SYNC UNCERTAIN)
    rig.load(2, 200.0);
    rig.decks[2].setPitchRange(8.0f);
    rig.decks[2].setPlaying(true);
    rig.perf.enableSync(2, SyncMode::Beat);
    rig.secs(1.0);
    REQUIRE(rig.perf.syncConfidence(2) < 0.25f);
}

TEST_CASE("sync diagnostics: complete + copyable", "[sync]") {
    Rig rig;
    rig.load(0, 128.0); rig.load(1, 124.0);
    rig.decks[0].setPlaying(true); rig.decks[1].setPlaying(true);
    rig.perf.enableSync(0, SyncMode::Beat);
    rig.perf.enableSync(1, SyncMode::Beat);
    rig.secs(2.0);

    const auto d = rig.perf.syncDiagnostics(1);
    REQUIRE(d.masterDeck == 0);
    REQUIRE(d.masterBpm == Approx(128.0).margin(0.1));
    REQUIRE(d.deck == 1);
    REQUIRE(d.deckBpm == Approx(128.0).margin(0.2));
    REQUIRE(d.targetBpm == Approx(128.0).margin(0.1));
    REQUIRE(d.beat >= 0.0);
    REQUIRE(d.tempoErrPct >= 0.0);
    REQUIRE(std::fabs(d.phaseErrMs) < 20.0);
    REQUIRE(d.confidence > 0.3f);
    const std::string txt = rig.perf.syncDiagText(1);
    REQUIRE(txt.find("SYNCED") != std::string::npos);
    REQUIRE(txt.find("BPM") != std::string::npos);
}

// ==================== sync lock + pitch interaction ====================

TEST_CASE("sync lock ON: new track grid keeps the deck synced", "[sync]") {
    Rig rig;
    rig.load(0, 128.0); rig.load(1, 124.0);
    rig.decks[0].setPlaying(true); rig.decks[1].setPlaying(true);
    rig.perf.enableSync(0, SyncMode::Beat);
    rig.perf.enableSync(1, SyncMode::Beat);
    rig.secs(0.5);
    // a different track's grid arrives on deck 1 — lock preserves sync
    rig.perf.attachGrid(1, makeGrid(130.0), /*trackId*/ 77);
    rig.block(1);
    REQUIRE(rig.perf.syncMode(1) == SyncMode::Beat);
    REQUIRE(rig.perf.syncStatus(1) != SyncStatus::NoGrid);
}

TEST_CASE("sync lock OFF: track change drops the deck to manual", "[sync]") {
    Rig rig;
    rig.load(0, 128.0); rig.load(1, 124.0);
    rig.decks[0].setPlaying(true); rig.decks[1].setPlaying(true);
    rig.perf.enableSync(0, SyncMode::Beat);
    rig.perf.enableSync(1, SyncMode::Beat);
    rig.perf.setSyncLock(1, false);
    rig.perf.attachGrid(1, makeGrid(130.0), 77);   // track swap
    REQUIRE(rig.perf.syncMode(1) == SyncMode::Off);
}

TEST_CASE("pitch policy: Override ignores the fader while synced", "[sync]") {
    Rig rig;
    rig.load(0, 128.0); rig.load(1, 124.0);
    rig.decks[0].setPlaying(true); rig.decks[1].setPlaying(true);
    rig.perf.enableSync(0, SyncMode::Beat);
    rig.perf.enableSync(1, SyncMode::Beat);
    rig.sent.clear();
    rig.perf.onUserPitch(1, 2.0f);
    REQUIRE(rig.countCmd<rt::SetPitch>() == 0);     // fader swallowed
    REQUIRE(rig.perf.syncMode(1) == SyncMode::Beat);
    // unsynced deck → passes straight through
    rig.perf.onUserPitch(0, 1.5f);                  // master is not synced
    REQUIRE(rig.countCmd<rt::SetPitch>() == 1);
}

TEST_CASE("pitch policy: Disable hands control back to the fader", "[sync]") {
    Rig rig;
    rig.load(0, 128.0); rig.load(1, 124.0);
    rig.decks[0].setPlaying(true); rig.decks[1].setPlaying(true);
    rig.perf.enableSync(0, SyncMode::Beat);
    rig.perf.enableSync(1, SyncMode::Beat);
    rig.perf.setPitchPolicy(sync::PitchPolicy::Disable);
    rig.perf.onUserPitch(1, 3.0f);
    REQUIRE(rig.perf.syncMode(1) == SyncMode::Off); // manual control back
    REQUIRE(rig.countCmd<rt::SetPitch>() == 1);     // fader applied
}

TEST_CASE("pitch policy: ManualAsBend routes fader to tempo bend", "[sync]") {
    Rig rig;
    rig.load(0, 128.0); rig.load(1, 124.0);
    rig.decks[0].setPlaying(true); rig.decks[1].setPlaying(true);
    rig.perf.enableSync(0, SyncMode::Beat);
    rig.perf.enableSync(1, SyncMode::Beat);
    rig.perf.setPitchPolicy(sync::PitchPolicy::ManualAsBend);
    rig.perf.onUserPitch(1, 4.0f);
    REQUIRE(rig.perf.syncMode(1) == SyncMode::Beat); // still synced
    REQUIRE(rig.countCmd<rt::SetPitch>() == 0);
    REQUIRE(rig.countCmd<rt::SetPitchBend>() == 1);
}

// ==================== quantize (§17-21) ====================

TEST_CASE("quantize: ThirtySecond + Bar resolutions honour beatsPerBar", "[sync]") {
    REQUIRE(quantizeBeats(Quantize::ThirtySecond) == Approx(0.03125));
    REQUIRE(quantizeBeats(Quantize::Bar, 4) == Approx(4.0));
    REQUIRE(quantizeBeats(Quantize::Bar, 3) == Approx(3.0)); // 3/4 grid

    Rig rig;
    rig.load(0, 120.0);
    rig.perf.setQuantizeResolution(0, Quantize::Bar);
    REQUIRE(rig.perf.quantizeBeatsFor(0) == Approx(4.0));
    // 3/4 grid: Bar quantum = 3 beats, read from the deck's grid
    auto g3 = makeGrid(120.0); g3->beatsPerBar = 3;
    rig.perf.attachGrid(0, g3, 0);
    REQUIRE(rig.perf.quantizeBeatsFor(0) == Approx(3.0));
    // cycle wraps through all seven resolutions
    rig.perf.setQuantizeResolution(0, Quantize::Bar);
    rig.perf.cycleQuantize(0);
    REQUIRE(rig.perf.quantizeResolution(0) == (int)Quantize::Beat1);
    rig.perf.cycleQuantize(0);
    REQUIRE(rig.perf.quantizeResolution(0) == (int)Quantize::Half);
}

TEST_CASE("quantize: bar-quantized cue lands on a bar boundary", "[sync]") {
    Rig rig;
    rig.load(0, 120.0);                        // fpb=24000, bar=96000
    rig.decks[0].setPlaying(true);
    rig.secs(0.3);
    rig.perf.setQuantize(0, true);
    rig.perf.setQuantizeResolution(0, Quantize::Bar);
    const int64_t off = 2 * 96000 + 20000;     // 20k past bar 2
    rig.perf.setCue(0, 0, CueType::Hot, off);
    rig.perf.triggerCue(0, 0);
    rig.block(1);
    const int64_t p = rig.pos(0);
    const int64_t bar = (p / 96000) * 96000;
    REQUIRE(std::min(std::llabs(p - bar),
                     std::llabs(p - (bar + 96000))) < 1100);
}

// ==================== clock source / clock view ====================

TEST_CASE("external clock source feeds MasterClock", "[sync]") {
    Rig rig;
    rig.load(0, 128.0);
    rig.decks[0].setPlaying(true);
    rig.perf.setExternalClock([] {
        sync::ClockSample c;
        c.valid = true; c.bpm = 140.0; c.beatPos = 8.5;
        c.bar = 3; c.phase = 0.5;
        return c;
    });
    rig.perf.setMasterSource(sync::MasterSource::External);
    REQUIRE(rig.perf.masterDeck() == -1);       // no deck master
    const auto clk = rig.perf.masterClock();
    REQUIRE(clk.valid);
    REQUIRE(clk.bpm == Approx(140.0));
    REQUIRE(clk.external);
    // deck sync state stays manual — RT sync does not chase externals
    REQUIRE(rig.perf.syncMode(0) == SyncMode::Off);
}

TEST_CASE("masterClockView: durations, bpb, timestamp, stopped", "[sync]") {
    Rig rig;
    rig.load(0, 128.0);
    rig.decks[0].setPlaying(true);
    rig.perf.enableSync(0, SyncMode::Beat);
    rig.secs(1.0);
    auto v = rig.perf.masterClockView();
    REQUIRE(v.sample.valid);
    REQUIRE(v.deck == 0);
    REQUIRE_FALSE(v.stopped);
    REQUIRE(v.sample.bpm == Approx(128.0).margin(0.1));
    REQUIRE(v.sample.beatDurSec == Approx(60.0 / 128.0).margin(0.01));
    REQUIRE(v.sample.barDurSec ==
            Approx(4.0 * 60.0 / 128.0).margin(0.04));
    REQUIRE(v.sample.beatsPerBar == 4);
    REQUIRE(v.sample.timestampSec > 0.5);
    rig.decks[0].setPlaying(false);
    v = rig.perf.masterClockView();
    REQUIRE(v.stopped);                          // §22: clock frozen
}

// ==================== beatgrid following (§11-12) ====================

TEST_CASE("beatgrid following: follower tracks master tempo change", "[sync]") {
    Rig rig;
    // Track C: 128 BPM for 8 beats, then 130 BPM — master follows its map
    rig.tracks[0] = makeClick(30.0);
    rig.decks[0].pendingTrack_.store(rig.tracks[0].get());
    rig.perf.attachGrid(0, makeTempoMapGrid(128.0, 130.0, 8), 0);
    rig.load(1, 124.0);
    rig.decks[0].setPlaying(true); rig.decks[1].setPlaying(true);
    rig.perf.enableSync(0, SyncMode::Beat);
    rig.perf.enableSync(1, SyncMode::Beat);
    rig.secs(2.0);   // before the change — converged at 128
    REQUIRE(rig.stats[1].effBpm.load() == Approx(128.0).margin(0.3));
    rig.secs(4.0);   // crosses beat 8 → master's grid says 130
    REQUIRE(rig.stats[0].effBpm.load() == Approx(130.0).margin(0.3));
    REQUIRE(rig.stats[1].effBpm.load() == Approx(130.0).margin(0.5));
}

TEST_CASE("same-BPM decks stay phase-locked without drift", "[sync]") {
    Rig rig;
    rig.load(0, 128.0); rig.load(1, 128.0);
    rig.decks[0].setPlaying(true);
    rig.secs(0.4);
    rig.decks[1].setPlaying(true);            // offset start
    rig.secs(0.3);
    rig.perf.enableSync(0, SyncMode::Beat);
    rig.perf.enableSync(1, SyncMode::Beat);
    rig.secs(3.0);
    REQUIRE(rig.stats[1].effBpm.load() == Approx(128.0).margin(0.05));
    REQUIRE(std::fabs(rig.stats[1].syncErrMs.load()) < 5.0f);
    REQUIRE(rig.perf.syncStatus(1) == SyncStatus::Synced);
}

// ==================== command surface (§27: hardware path) ====================

TEST_CASE("sync commands: master cycle, lock, quantum, bpm mode", "[sync]") {
    Rig rig;
    rig.load(0, 128.0); rig.load(1, 124.0);
    rig.decks[0].setPlaying(true); rig.decks[1].setPlaying(true);

    ui::CommandManager cm;
    ui::EnginePorts ports;
    ports.perf = &rig.perf;
    ports.deckStats = [&rig](int d) -> DeckStats* {
        return &rig.stats[d];
    };
    ui::registerEngineCommands(cm, ports);

    // the ids hardware maps to — same dispatch path MIDI/HID use
    namespace C = ui::cmd;
    REQUIRE(cm.invoke(C::SyncMasterCycle, ui::CommandSource::Hid));
    REQUIRE(rig.perf.masterSource() == sync::MasterSource::DeckA);
    REQUIRE(cm.invoke(C::SyncMasterCycle, ui::CommandSource::Midi));

    REQUIRE(cm.invoke(C::forDeck(C::SyncLockTgl, 1),
                      ui::CommandSource::Hid));
    REQUIRE_FALSE(rig.perf.syncLockOn(1));
    REQUIRE(cm.state(C::forDeck(C::SyncLockTgl, 1)) == 0);

    rig.perf.setQuantizeResolution(1, Quantize::Bar);
    REQUIRE(cm.invoke(C::forDeck(C::SyncQuantRes, 1),
                      ui::CommandSource::Hid));
    REQUIRE(rig.perf.quantizeResolution(1) == (int)Quantize::Beat1);

    REQUIRE(cm.invoke(C::forDeck(C::SyncBpmMode, 1),
                      ui::CommandSource::Hid));
    REQUIRE(rig.perf.syncMode(1) == SyncMode::Bpm);
    REQUIRE(cm.state(C::forDeck(C::SyncBpmMode, 1)) == 1);
}

// ==================== stress (§38) ====================

TEST_CASE("stress: 4 decks synced + loops + jumps, drift bounded", "[sync][stress]") {
    Rig rig;
    const double bpms[4] = { 128.0, 124.0, 126.0, 130.0 };
    for (int d = 0; d < 4; ++d) {
        rig.load(d, bpms[d], 60.0);
        rig.decks[d].setPlaying(true);
    }
    rig.perf.enableSync(0, SyncMode::Beat);
    for (int d = 1; d < 4; ++d) rig.perf.enableSync(d, SyncMode::Beat);
    rig.perf.setQuantize(1, true);

    const auto t0 = std::chrono::steady_clock::now();
    for (int s = 0; s < 25; ++s) {
        switch (s % 5) {
            case 0: rig.perf.loopBeats(1, 2.0); break;
            case 1: rig.perf.exitLoop(1);       break;
            case 2: rig.perf.beatJump(2, 4.0);  break;
            case 3: rig.perf.beatJump(3, -8.0); break;
            case 4: rig.perf.setCue(2, 0, CueType::Hot);
                    rig.perf.triggerCue(2, 0);  break;
        }
        rig.secs(0.6);
        for (int d = 0; d < 4; ++d) {
            const double p = rig.stats[d].positionSec.load();
            REQUIRE(std::isfinite(p));
            REQUIRE(p >= 0.0);
        }
    }
    const double wallSec =
        std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count();

    rig.secs(2.0);  // settle
    for (int d = 1; d < 4; ++d) {
        INFO("deck " << d);
        REQUIRE(rig.stats[d].effBpm.load() == Approx(128.0).margin(1.0));
        REQUIRE(std::fabs(rig.stats[d].syncErrMs.load()) < 30.0f);
    }
    // rendered ~17 s of 4-deck audio — must stay well under realtime
    // even in a Debug build (RT thread budget is ~10.7 ms/block)
    REQUIRE(wallSec < 15.0);
    WARN("4-deck sync stress: " << wallSec << " s wall for ~17 s audio");
}
