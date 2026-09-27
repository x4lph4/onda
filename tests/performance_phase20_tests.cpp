// Phase 20 — professional performance engine tests.
// Hot cues / memory cues / loops / loop roll / beat jump / quantize /
// slip / reverse / cue button / instant restart — driven through the
// same rt::Command path as the audio callback (applyDeckCommand), plus
// pure-component tests for the scheduler, slip, reverse, size tables
// and pad layers. Timing sweep at 90/120/128/140 BPM per §45.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include "core/audio/TrackAudio.h"
#include "core/data/AnalysisStore.h"
#include "core/data/Database.h"
#include "core/data/PerformanceStore.h"
#include "core/engine/DeckCommand.h"
#include "core/engine/DeckEngine.h"
#include "core/dsp/WsolaStretcher.h"
#include "core/library/FileFingerprint.h"
#include "core/library/MusicLibraryEngine.h"
#include "core/performance/BeatJumpEngine.h"
#include "core/performance/LoopSizeController.h"
#include "core/performance/PerformanceEngine.h"
#include "core/performance/PerformanceScheduler.h"
#include "core/performance/ReverseEngine.h"
#include "core/performance/SlipEngine.h"
#include "core/performance/SyncRt.h"
#include "platform/metadata/FFmpegMetadataReader.h"
#include "ui/CommandIds.h"
#include "ui/UndoManager.h"
#include "ui/pads/PadFeedbackEngine.h"
#include "ui/pads/PadModeManager.h"

using namespace onda;
using namespace onda::ui;
using namespace onda::library;
using Catch::Approx;
namespace fs = std::filesystem;

namespace {

constexpr double kRate = 48000.0;

std::unique_ptr<TrackAudio> makeClick(double secs) {
    auto t = std::make_unique<TrackAudio>();
    t->fileSampleRate = kRate;
    t->deviceRate     = kRate;
    t->numFrames      = (int64_t)(kRate * secs);
    t->samples.resize((size_t)t->numFrames * 2);
    for (int64_t i = 0; i < t->numFrames; ++i) {
        const float v = 0.3f * (float)std::sin(
            2.0 * 3.14159265 * 220.0 * i / kRate);
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

/// Same rig shape as performance_tests: commands apply instantly to
/// real DeckEngines — seeks/loops/slip actually happen, so positions
/// and echoes in DeckStats are honest.
struct Rig {
    static constexpr int kDecks = 4;
    static constexpr int kBlock = 512;

    std::array<DeckEngine, kDecks> decks;
    std::array<DeckStats, kDecks>  stats;
    std::array<std::unique_ptr<TrackAudio>, kDecks> tracks;
    SyncRtCore sync;
    PerformanceEngine perf;
    std::vector<PerfEvent> events;
    std::vector<std::pair<int,int64_t>> seeks; // (deck,frame) capture
    std::vector<float> l = std::vector<float>(kBlock),
                       r = std::vector<float>(kBlock);

    Rig() {
        perf.setCommandSink([this](const rt::Command& c) {
            if (auto* m = std::get_if<rt::SyncSetMaster>(&c)) {
                sync.masterDeck = m->deck >= kDecks ? -1 : (int)m->deck;
                return;
            }
            if (auto* s = std::get_if<rt::DeckSeekFrame>(&c))
                seeks.push_back({ (int)s->deck, s->frame });
            for (int i = 0; i < kDecks; ++i)
                applyDeckCommand(decks[i], i, c);
        });
        perf.addListener([this](const PerfEvent& e) {
            events.push_back(e); });
        for (int i = 0; i < kDecks; ++i) {
            decks[i].bind(&stats[i]);
            decks[i].setDeviceRate(kRate);
            decks[i].setStretcher(std::make_unique<WsolaStretcher>());
            decks[i].prepareDsp(kRate);
            perf.setGridSlot(i, &decks[i].pendingGrid_,
                             &decks[i].retiredGrid_, &stats[i]);
        }
    }

    void load(int deck, double bpm, double secs = 60.0) {
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
    static int64_t fpb(double bpm) { return (int64_t)(kRate * 60.0 / bpm); }

    bool hasEvent(PerfEventType t, int deck = -1) const {
        for (auto& e : events)
            if (e.type == t && (deck < 0 || e.deck == deck)) return true;
        return false;
    }
};

fs::path tempRoot(const char* name) {
    fs::path d = fs::temp_directory_path() / "onda_p20test" / name;
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

} // namespace

// ===================== pure components (§11/14/16/19) =================

TEST_CASE("LoopSizeController: tables, clamps, format", "[p20]") {
    using L = LoopSizeController;
    REQUIRE(L::loopCount() == 11);
    REQUIRE(L::loopSize(0)  == Approx(1.0 / 32));
    REQUIRE(L::loopSize(5)  == Approx(1.0));
    REQUIRE(L::loopSize(10) == Approx(32.0));
    REQUIRE(L::loopSize(-5) == Approx(1.0 / 32));   // clamped
    REQUIRE(L::loopSize(99) == Approx(32.0));
    // roll: 1/32…1 in-table, extended beyond (§16)
    REQUIRE(L::rollSize(0) == Approx(1.0 / 32));
    REQUIRE(L::rollSize(5) == Approx(1.0));
    REQUIRE(L::rollSize(6) == Approx(2.0));
    REQUIRE(L::rollSize(7) == Approx(4.0));
    REQUIRE(L::jumpCount() == 7);
    REQUIRE(L::jumpSize(0) == Approx(1.0));
    REQUIRE(L::jumpSize(6) == Approx(64.0));
    // nearest + step helpers
    REQUIRE(L::nearestLoopIndex(1.0) == 5);
    REQUIRE(L::nearestLoopIndex(0.9) == 5);
    REQUIRE(L::nearestJumpIndex(-8.0) == 3); // sign-free
    REQUIRE(L::stepIndex(10, 1, 11) == 10);
    REQUIRE(L::stepIndex(0, -3, 11) == 0);
    char buf[16];
    L::formatBeats(0.25, buf, 16);  REQUIRE(std::string(buf) == "1/4");
    L::formatBeats(32.0, buf, 16);  REQUIRE(std::string(buf) == "32");
}

TEST_CASE("SlipEngine: reference-counted sources, edge-only", "[p20]") {
    SlipEngine s;
    REQUIRE_FALSE(s.active());
    REQUIRE(s.set(SlipEngine::UserMode, true));  // edge → SlipBegin
    REQUIRE(s.active());
    REQUIRE(s.userOn());
    // roll ref on top — NOT an edge (already shadowing)
    REQUIRE_FALSE(s.set(SlipEngine::Roll, true));
    REQUIRE(s.rolling());
    // releasing the user ref while rolling keeps the shadow alive
    REQUIRE_FALSE(s.set(SlipEngine::UserMode, false));
    REQUIRE(s.active());
    REQUIRE_FALSE(s.userOn());
    // last ref release is the only end-edge
    REQUIRE(s.set(SlipEngine::Roll, false));
    REQUIRE_FALSE(s.active());
    // re-arming produces a fresh edge
    REQUIRE(s.set(SlipEngine::Momentary, true));
    s.clear();
    REQUIRE_FALSE(s.active());
}

TEST_CASE("ReverseEngine: latched XOR momentary", "[p20]") {
    ReverseEngine r;
    REQUIRE(r.setLatched(true));
    REQUIRE(r.effective());
    // momentary press while latched → temporary FORWARD (hardware
    // semantic): effective flips back to normal
    REQUIRE(r.setMomentary(true));
    REQUIRE_FALSE(r.effective());
    // release restores the latch
    REQUIRE(r.setMomentary(false));
    REQUIRE(r.effective());
    // no-op writes report no edge
    REQUIRE_FALSE(r.setLatched(true));
    r.clear();
    REQUIRE_FALSE(r.effective());
}

TEST_CASE("PerformanceScheduler: priority + coalescing (§29-31)", "[p20]") {
    PerformanceScheduler sched;
    std::vector<rt::Command> out;
    sched.setSink([&](const rt::Command& c) { out.push_back(c); });

    // post in *arrival* order: transport first, then slip-end, seek,
    // loop-arm — flush must still emit in policy order.
    sched.post(0, rt::DeckTransport{ 0, 1 });
    sched.post(0, rt::SlipEnd{ 0 });
    sched.post(0, rt::DeckSeekFrame{ 0, 5000 });
    sched.post(0, rt::SetLoopFrames{ 0, 100, 200 });
    sched.post(0, rt::SetLoopOn{ 0, false });   // exit = prio 2
    sched.flush();

    REQUIRE(out.size() == 5);
    REQUIRE(std::holds_alternative<rt::SlipEnd>(out[0]));
    REQUIRE(std::holds_alternative<rt::SetLoopOn>(out[1]));
    REQUIRE(std::holds_alternative<rt::DeckSeekFrame>(out[2]));
    REQUIRE(std::holds_alternative<rt::SetLoopFrames>(out[3]));
    REQUIRE(std::holds_alternative<rt::DeckTransport>(out[4]));

    // coalescing: two seeks in one action → newest wins, once.
    out.clear();
    sched.post(0, rt::DeckSeekFrame{ 0, 111 });
    sched.post(0, rt::DeckSeekFrame{ 0, 222 });
    REQUIRE(sched.pending() == 1);
    REQUIRE(sched.coalesced() == 1);
    sched.flush();
    REQUIRE(out.size() == 1);
    REQUIRE(std::get<rt::DeckSeekFrame>(out[0]).frame == 222);

    // SlipReanchor rides with seeks but never coalesces — both emit,
    // seek first (submission order).
    out.clear();
    sched.post(0, rt::DeckSeekFrame{ 0, 900 });
    sched.post(0, rt::SlipReanchor{ 0 });
    sched.flush();
    REQUIRE(out.size() == 2);
    REQUIRE(std::holds_alternative<rt::DeckSeekFrame>(out[0]));
    REQUIRE(std::holds_alternative<rt::SlipReanchor>(out[1]));

    // decks are independent coalescing namespaces
    out.clear();
    sched.post(0, rt::DeckSeekFrame{ 0, 1 });
    sched.post(1, rt::DeckSeekFrame{ 1, 2 });
    sched.flush();
    REQUIRE(out.size() == 2);
}

TEST_CASE("PadModeManager: hot cue / loop / roll / jump semantics", "[p20]") {
    using namespace pads;
    PadModeManager pm;

    // hot cue: press → trigger cmd, shift+press → delete, release → none
    pm.setMode(0, PadMode::HotCue);
    auto a = pm.resolve(0, 3, false, true);
    REQUIRE(a.kind == PadAction::Command);
    REQUIRE(a.cmdId == cmd::forHotCue(0, 3, 0));
    a = pm.resolve(0, 3, true, true);
    REQUIRE(a.cmdId == cmd::forHotCue(0, 3, 2));
    REQUIRE(pm.resolve(0, 3, false, false).kind == PadAction::None);

    // loop: pads 0-3 ops, 4-7 sized loops (arg = 10 + table idx)
    pm.setMode(0, PadMode::Loop);
    REQUIRE(pm.resolve(0, 0, false, true).cmdId
            == cmd::forDeck(cmd::DeckLoopHalve, 0));
    REQUIRE(pm.resolve(0, 2, false, true).cmdId
            == cmd::forDeck(cmd::DeckLoopIn, 0));
    a = pm.resolve(0, 5, false, true);
    REQUIRE(a.cmdId == cmd::forDeck(cmd::DeckLoopSizeTrig, 0));
    REQUIRE(a.arg == Approx(15.0f));   // 10 + idx5 = 1-beat loop
    // releases consumed
    REQUIRE(pm.resolve(0, 5, false, false).kind == PadAction::None);

    // roll (§15-16): press = size idx+1, release = arg 0 gate end
    pm.setMode(0, PadMode::Roll);
    a = pm.resolve(0, 0, false, true);
    REQUIRE(a.cmdId == cmd::forDeck(cmd::DeckRollGate, 0));
    REQUIRE(a.arg == Approx(1.0f));
    a = pm.resolve(0, 5, false, true);
    REQUIRE(a.arg == Approx(6.0f));    // 1-beat roll
    a = pm.resolve(0, 5, false, false);
    REQUIRE(a.arg == Approx(0.0f));    // release = gate end

    // beat jump: pads 0-3 back, 4-7 fwd — arg carries the size hint
    pm.setMode(0, PadMode::BeatJump);
    a = pm.resolve(0, 2, false, true);
    REQUIRE(a.cmdId == cmd::forDeck(cmd::DeckJumpBack, 0));
    REQUIRE(a.arg == Approx(4.0f));
    a = pm.resolve(0, 7, false, true);
    REQUIRE(a.cmdId == cmd::forDeck(cmd::DeckJumpFwd, 0));
    REQUIRE(a.arg == Approx(8.0f));

    // §34/§45 — all 4 decks resolve independently
    pm.setMode(3, PadMode::Roll);
    REQUIRE(pm.mode(3) == PadMode::Roll);
    REQUIRE(pm.resolve(3, 0, false, true).cmdId
            == cmd::forDeck(cmd::DeckRollGate, 3));
    REQUIRE(padModeImplemented(PadMode::Roll));
}

TEST_CASE("PadFeedbackEngine: pad states per mode (§36)", "[p20]") {
    using namespace pads;
    PadFeedbackEngine fb;
    PadDeckState s;
    s.mode = PadMode::HotCue;
    Cue c0; c0.slot = 0; c0.argb = 0xFF00FF00; c0.enabled = true;
    Cue c1; c1.slot = 1; c1.enabled = false;
    s.hot[0] = &c0; s.hot[1] = &c1;

    REQUIRE(fb.light(s, 0).state == PadLightState::Set);
    REQUIRE(fb.light(s, 0).argb == 0xFF00FF00);
    REQUIRE(fb.light(s, 1).state == PadLightState::Dim); // disabled
    REQUIRE(fb.light(s, 7).state == PadLightState::Dim); // empty
    s.playing = true; s.activeCueSlot = 0;
    REQUIRE(fb.light(s, 0).state == PadLightState::Playing);

    s.mode = PadMode::Loop; s.loopSizeIdx = 5; s.loopOn = true;
    s.activeCueSlot = -1;
    REQUIRE(fb.light(s, 5).state == PadLightState::Playing); // sel+on
    REQUIRE(fb.light(s, 4).state == PadLightState::Set);
    s.loopOn = false;
    REQUIRE(fb.light(s, 5).state == PadLightState::Selected);
    s.pendingLoopIn = 0;
    REQUIRE(fb.light(s, 2).state == PadLightState::Selected); // in armed

    s.mode = PadMode::Roll; s.rolling = true;
    REQUIRE(fb.light(s, 0).state == PadLightState::Playing);
    s.rolling = false;
    REQUIRE(fb.light(s, 0).state == PadLightState::Set);

    // §36 — FLX4 LED levels strictly ordered by intensity
    REQUIRE(PadFeedbackEngine::flx4LedValue(PadLightState::Off) == 0x00);
    REQUIRE(PadFeedbackEngine::flx4LedValue(PadLightState::Dim)
          < PadFeedbackEngine::flx4LedValue(PadLightState::Set));
    REQUIRE(PadFeedbackEngine::flx4LedValue(PadLightState::Set)
          < PadFeedbackEngine::flx4LedValue(PadLightState::Selected));
    REQUIRE(PadFeedbackEngine::flx4LedValue(PadLightState::Selected)
          < PadFeedbackEngine::flx4LedValue(PadLightState::Playing));
}

TEST_CASE("command ids: hot-cue / mem-cue / pad encodings", "[p20]") {
    using namespace cmd;
    REQUIRE(deckOf(forDeck(DeckLoopToggle, 2)) == 2);
    REQUIRE(actionOf(forDeck(DeckLoopToggle, 2)) == DeckLoopToggle);
    REQUIRE(forHotCue(1, 5, 0) == forDeck(DeckHotCueBase, 1) + 20);
    REQUIRE(forHotCue(1, 5, 2) == forDeck(DeckHotCueBase, 1) + 22);
    REQUIRE(forMemCue(0, 3, 1) == forDeck(DeckMemCueBase, 0) + 13);
    REQUIRE(padDeckOf(forPad(2, 4, true)) == 2);
    REQUIRE(padIndexOf(forPad(2, 4, true)) == 4);
    REQUIRE(padShiftOf(forPad(2, 4, true)));
    REQUIRE_FALSE(padShiftOf(forPad(2, 4, false)));
    // phase-20 ids live in the per-deck space — deck bits preserved
    REQUIRE(deckOf(forDeck(DeckRollGate, 3)) == 3);
    REQUIRE(actionOf(forDeck(DeckInstantRestart, 1))
            == DeckInstantRestart);
}

// ========================== hot cues (§2-6) ===========================

TEST_CASE("hot cues: create → trigger while playing jumps + plays",
          "[p20]") {
    Rig rig;
    rig.load(0, 120.0);                      // fpb = 24000
    rig.decks[0].setPlaying(true);
    rig.secs(1.0);
    rig.perf.setCue(0, 0, CueType::Hot, Rig::fpb(120.0) * 8); // beat 8
    rig.secs(0.2);
    REQUIRE(rig.perf.triggerCue(0, 0));
    rig.block(2);
    REQUIRE(rig.decks[0].isPlaying());
    REQUIRE(rig.pos(0) == Approx(Rig::fpb(120.0) * 8).margin(1500));
    REQUIRE(rig.hasEvent(PerfEventType::CueTriggered, 0));
    REQUIRE(rig.perf.diag().deck(0).cuesTriggered == 1);
}

TEST_CASE("hot cues: stopped deck honours HotCueMode (§5)", "[p20]") {
    Rig rig;
    rig.load(0, 120.0);
    const int64_t f = Rig::fpb(120.0) * 4;
    rig.perf.setCue(0, 0, CueType::Hot, f);
    rig.perf.setCue(0, 1, CueType::Hot, f + 1000);
    rig.perf.setCueKind(0, 1, HotKind::Cue);

    // JumpPlay (default): seek + start playing
    rig.perf.setHotCueMode(0, HotCueMode::JumpPlay);
    REQUIRE(rig.perf.triggerCue(0, 0));
    rig.block(2);
    REQUIRE(rig.decks[0].isPlaying());
    REQUIRE(rig.pos(0) == Approx(f).margin(1500));

    // back to paused, then JumpCue: seek only
    rig.decks[0].setPlaying(false);
    rig.perf.setHotCueMode(0, HotCueMode::JumpCue);
    REQUIRE(rig.perf.triggerCue(0, 0));
    rig.block(2);
    REQUIRE_FALSE(rig.decks[0].isPlaying());
    REQUIRE(rig.pos(0) == Approx(f).margin(1500));

    // HotKind::Cue forces the cued path even under JumpPlay mode
    rig.perf.setHotCueMode(0, HotCueMode::JumpPlay);
    REQUIRE(rig.perf.triggerCue(0, 1));
    rig.block(2);
    REQUIRE_FALSE(rig.decks[0].isPlaying());
    REQUIRE(rig.pos(0) == Approx(f + 1000).margin(1500));
}

TEST_CASE("hot cues: disabled cue rejects deterministically (§43)",
          "[p20]") {
    Rig rig;
    rig.load(0, 120.0);
    rig.perf.setCue(0, 0, CueType::Hot, 4800);
    REQUIRE(rig.perf.setCueEnabled(0, 0, false));
    const int64_t before = rig.pos(0);
    REQUIRE_FALSE(rig.perf.triggerCue(0, 0));
    REQUIRE(rig.perf.lastError(0) == PerfError::Disabled);
    REQUIRE(rig.hasEvent(PerfEventType::ActionRejected, 0));
    REQUIRE(rig.pos(0) == before);            // state preserved
    REQUIRE(rig.perf.diag().deck(0).rejects == 1);

    REQUIRE_FALSE(rig.perf.triggerCue(0, 7)); // empty slot
    REQUIRE(rig.perf.lastError(0) == PerfError::EmptySlot);
}

TEST_CASE("hot cues: move/rename/recolor/kind/comment fields", "[p20]") {
    Rig rig;
    rig.load(0, 120.0);
    rig.perf.setCue(0, 0, CueType::Hot, 4800, -1, 0xFF112233, "A");
    REQUIRE(rig.perf.moveCue(0, 0, 9600));
    REQUIRE(rig.perf.renameCue(0, 0, "drop"));
    REQUIRE(rig.perf.recolorCue(0, 0, 0xFF0000FF));
    REQUIRE(rig.perf.setCueKind(0, 0, HotKind::Cue));
    REQUIRE(rig.perf.setCueQuantized(0, 0, false));
    REQUIRE(rig.perf.setCueComment(0, 0, "second drop"));

    const Cue* c = nullptr;
    for (auto& x : rig.perf.cues(0)) if (x.slot == 0) c = &x;
    REQUIRE(c != nullptr);
    REQUIRE(c->frame == 9600);
    REQUIRE(c->label == "drop");
    REQUIRE(c->argb == 0xFF0000FF);
    REQUIRE(c->kind == HotKind::Cue);
    REQUIRE_FALSE(c->quantized);
    REQUIRE(c->comment == "second drop");
    REQUIRE(c->beat == Approx(rig.perf.grid(0)->frameToBeat(9600)));

    // typed facades see the same storage (§2)
    REQUIRE(rig.perf.hot(0).count() == 1);
    REQUIRE(rig.perf.hot(0).firstFreePad() == 1);
    REQUIRE(rig.perf.hotCues(0).size() == 1);
    REQUIRE(rig.perf.memoryCues(0).empty());
}

// ======================== memory cues (§7-8) ==========================

TEST_CASE("memory cues: navigation-only trigger, comment, distinct set",
          "[p20]") {
    Rig rig;
    rig.load(0, 120.0);
    // set via the typed manager facade
    auto mem = rig.perf.memory(0);
    Cue c = mem.set(2, Rig::fpb(120.0) * 12, rig.perf.grid(0),
                    0xFF33B5E5, "breakdown", "second drop entry");
    REQUIRE(c.type == CueType::Memory);
    REQUIRE(rig.perf.memoryCues(0).size() == 1);
    REQUIRE(rig.perf.hotCues(0).empty());     // §8 — separated
    REQUIRE(rig.perf.memory(0).count() == 1);
    REQUIRE(rig.perf.memory(0).firstFree() == 0);

    // trigger while paused → seek only, stays paused (navigation)
    REQUIRE(rig.perf.triggerCue(0, 2));
    rig.block(2);
    REQUIRE_FALSE(rig.decks[0].isPlaying());
    REQUIRE(rig.pos(0) == Approx(Rig::fpb(120.0) * 12).margin(1500));

    // comment survives the facade
    REQUIRE(rig.perf.cues(0)[0].comment == "second drop entry");
}

// ===================== cue point + cue button (§24-26) ================

TEST_CASE("cue button: press while playing returns+pauses (§25)",
          "[p20]") {
    Rig rig;
    rig.load(0, 120.0);
    rig.decks[0].setPlaying(true);
    rig.secs(0.5);
    rig.perf.setCuePoint(0, Rig::fpb(120.0) * 2); // cue at beat 2
    rig.secs(0.5);
    rig.perf.cueButton(0, CuePhase::Press);
    rig.block(2);
    REQUIRE_FALSE(rig.decks[0].isPlaying());
    REQUIRE(rig.pos(0) == Approx(Rig::fpb(120.0) * 2).margin(1500));
    REQUIRE(rig.perf.diag().deck(0).cueButtonPress == 1);
}

TEST_CASE("cue button: press while paused previews, release returns",
          "[p20]") {
    Rig rig;
    rig.load(0, 120.0);
    rig.secs(0.3); // paused but positioned
    rig.perf.cueButton(0, CuePhase::Press);      // defines point + preview
    REQUIRE(rig.perf.cuePoint(0) >= 0);
    const int64_t cp = rig.perf.cuePoint(0);
    rig.block(2);
    REQUIRE(rig.decks[0].isPlaying());           // previewing
    rig.secs(0.2);
    rig.perf.cueButton(0, CuePhase::Release);    // stops + returns
    rig.block(2);
    REQUIRE_FALSE(rig.decks[0].isPlaying());
    REQUIRE(rig.pos(0) == Approx(cp).margin(1500));
}

TEST_CASE("instant restart: seeks to cue + plays, sync untouched (§26)",
          "[p20]") {
    Rig rig;
    rig.load(0, 128.0);
    rig.load(1, 128.0);
    rig.decks[0].setPlaying(true);
    rig.secs(0.3);
    rig.perf.enableSync(0);                      // deck 0 becomes master
    rig.perf.enableSync(1);                      // deck 1 follows
    REQUIRE(rig.perf.masterDeck() == 0);
    const auto clockBefore = rig.perf.masterClock();

    rig.perf.setCuePoint(0, Rig::fpb(128.0) * 4);
    rig.secs(0.2);
    rig.perf.instantRestart(0);
    rig.block(2);
    REQUIRE(rig.decks[0].isPlaying());
    REQUIRE(rig.pos(0) == Approx(Rig::fpb(128.0) * 4).margin(1500));
    // §26 — master role + clock survive the restart
    REQUIRE(rig.perf.masterDeck() == 0);
    REQUIRE(rig.perf.syncMode(1) != SyncMode::Off);
    REQUIRE(rig.perf.masterClock().bpm == Approx(clockBefore.bpm));
    REQUIRE(rig.perf.diag().deck(0).instantRestart == 1);
}

// =========================== loops (§9-14) ============================

TEST_CASE("loops: sized auto-loops land on beat boundaries (§11)",
          "[p20]") {
    for (double bpm : { 90.0, 120.0, 128.0, 140.0 }) {
        Rig rig;
        rig.load(0, bpm);
        rig.decks[0].setPlaying(true);
        rig.secs(1.0);
        const int sizeIdx = 6; // 2 beats
        rig.perf.loopBeatsIdx(0, sizeIdx);
        rig.block(2);
        REQUIRE(rig.stats[0].loopOn.load());
        const auto* g = rig.perf.grid(0);
        const double inB  = g->frameToBeat(
            (int64_t)(rig.stats[0].loopInSec.load() * kRate));
        const double outB = g->frameToBeat(
            (int64_t)(rig.stats[0].loopOutSec.load() * kRate));
        // loop from a quantized in-point spans exactly `size` beats
        REQUIRE(outB - inB == Approx(2.0).margin(0.05));
        REQUIRE(rig.perf.loopSizeIndex(0) == sizeIdx);
        REQUIRE(rig.perf.loopSizeBeats(0) == Approx(2.0));
    }
}

TEST_CASE("loops: manual in/out quantizes boundaries (§12-13)", "[p20]") {
    Rig rig;
    rig.load(0, 120.0);
    rig.decks[0].setPlaying(true);
    rig.perf.setQuantize(0, true);
    rig.secs(1.0);
    rig.perf.loopIn(0);
    REQUIRE(rig.perf.loopInArmed(0));
    rig.secs(1.0);                             // ~2 beats later
    rig.perf.loopOut(0);
    rig.block(2);
    REQUIRE_FALSE(rig.perf.loopInArmed(0));
    REQUIRE(rig.stats[0].loopOn.load());
    const auto* g = rig.perf.grid(0);
    const double inB = g->frameToBeat(
        (int64_t)(rig.stats[0].loopInSec.load() * kRate));
    const double outB = g->frameToBeat(
        (int64_t)(rig.stats[0].loopOutSec.load() * kRate));
    // quantized boundaries → integer beats
    REQUIRE(inB  == Approx(std::round(inB)).margin(0.02));
    REQUIRE(outB == Approx(std::round(outB)).margin(0.02));
    REQUIRE(outB - inB >= 1.0);
    REQUIRE(rig.perf.diag().deck(0).quantizeSnaps >= 2);
}

TEST_CASE("loops: out before in rejects BadRange, state kept (§43)",
          "[p20]") {
    Rig rig;
    rig.load(0, 120.0);
    rig.decks[0].setPlaying(true);
    rig.secs(0.5);
    rig.perf.loopIn(0);
    // simulate pathological: in armed then immediate out at same spot
    rig.stats[0].framesRead.store(rig.pos(0) - 1000); // move "back"
    rig.perf.loopOut(0);
    REQUIRE(rig.perf.lastError(0) == PerfError::BadRange);
    REQUIRE_FALSE(rig.stats[0].loopOn.load());
    REQUIRE(rig.hasEvent(PerfEventType::ActionRejected, 0));
}

TEST_CASE("loops: resize preserves in-anchor + phase (§14)", "[p20]") {
    Rig rig;
    rig.load(0, 128.0);
    rig.decks[0].setPlaying(true);
    rig.secs(1.0);
    rig.perf.loopBeatsIdx(0, 7);               // 4 beats
    rig.block(2);
    const double in0 = rig.stats[0].loopInSec.load();
    const double len0 = rig.stats[0].loopOutSec.load() - in0;

    rig.perf.loopHalve(0); rig.block(2);
    REQUIRE(rig.stats[0].loopInSec.load() == Approx(in0).margin(0.001));
    REQUIRE(rig.stats[0].loopOutSec.load() - in0
            == Approx(len0 / 2).margin(0.01));

    rig.perf.loopDouble(0); rig.block(2);
    REQUIRE(rig.stats[0].loopOutSec.load() - in0
            == Approx(len0).margin(0.01));

    // size-table re-fire re-sizes at the same anchor — musical phase
    // of the in-point never moves
    rig.perf.loopBeatsIdx(0, 5);               // 1 beat
    rig.block(2);
    REQUIRE(rig.stats[0].loopOutSec.load()
            - rig.stats[0].loopInSec.load()
            == Approx(60.0 / 128.0).margin(0.02));
}

// ========================= loop roll (§15-16) =========================

TEST_CASE("loop roll: slip shadow recovers the musical position",
          "[p20]") {
    Rig rig;
    rig.load(0, 120.0);
    rig.decks[0].setPlaying(true);
    rig.secs(1.0);
    const int64_t rollStart = rig.pos(0);

    rig.perf.loopRollIdx(0, 5);                // 1-beat roll
    REQUIRE(rig.perf.rollActive(0));
    rig.block(2);
    REQUIRE(rig.stats[0].loopOn.load());       // roll loop engaged
    REQUIRE(rig.stats[0].slip.load());         // shadow anchored
    rig.secs(1.0);                             // ride the roll
    rig.perf.loopRollEnd(0);
    rig.block(2);
    REQUIRE_FALSE(rig.stats[0].loopOn.load());
    REQUIRE_FALSE(rig.stats[0].slip.load());
    REQUIRE_FALSE(rig.perf.rollActive(0));
    // the recovered position is where uninterrupted playback would
    // have arrived — NOT rewound to the roll-start
    const int64_t expected = rollStart + (int64_t)(kRate * 1.0);
    REQUIRE(rig.pos(0) > rollStart + Rig::fpb(120.0)); // past roll-start
    REQUIRE(rig.pos(0) == Approx(expected).margin(2000));
    REQUIRE(rig.hasEvent(PerfEventType::LoopRollStarted, 0));
    REQUIRE(rig.hasEvent(PerfEventType::LoopRollEnded, 0));
    REQUIRE(rig.decks[0].isPlaying());
}

TEST_CASE("loop roll: layered with user slip — refs stack (§17-18)",
          "[p20]") {
    Rig rig;
    rig.load(0, 120.0);
    rig.decks[0].setPlaying(true);
    rig.secs(0.5);
    rig.perf.setSlip(0, true);                 // user slip on
    REQUIRE(rig.perf.slipOn(0));
    const auto slipBegins0 = rig.perf.diag().deck(0).slipBegins;

    rig.perf.loopRoll(0, 1.0);                 // roll inside user slip
    REQUIRE(rig.perf.diag().deck(0).slipBegins == slipBegins0);
    rig.secs(0.3);
    rig.perf.loopRollEnd(0);                   // roll ref released…
    REQUIRE(rig.stats[0].slip.load());         // …shadow still alive
    REQUIRE(rig.perf.slipActive(0));
    rig.perf.setSlip(0, false);                // last ref → real SlipEnd
    rig.block(2);
    REQUIRE_FALSE(rig.stats[0].slip.load());
}

TEST_CASE("hot cue under slip re-anchors the shadow (§17)", "[p20]") {
    Rig rig;
    rig.load(0, 120.0);
    rig.decks[0].setPlaying(true);
    rig.secs(0.5);
    rig.perf.setSlip(0, true);
    rig.secs(0.3);
    // permanent move while slipping → re-anchor, no snap-back later
    rig.perf.setCue(0, 0, CueType::Hot, Rig::fpb(120.0) * 16);
    rig.perf.triggerCue(0, 0);
    rig.block(2);
    REQUIRE_FALSE(rig.seeks.empty());
    REQUIRE(rig.seeks.back().second == Rig::fpb(120.0) * 16);
    rig.perf.setSlip(0, false);
    rig.block(2);
    // shadow was re-anchored at the cue — slip-off continues from the
    // cue timeline (the pre-cue ~38400 position is never restored)
    REQUIRE(rig.pos(0) > Rig::fpb(120.0) * 8);
    REQUIRE(rig.pos(0) == Approx(Rig::fpb(120.0) * 16).margin(4000));
}

// ======================== beat jump (§19-21) ==========================

TEST_CASE("beat jump: sized, grid-aware, sign via dir (§19-20)", "[p20]") {
    Rig rig;
    rig.load(0, 128.0);
    rig.decks[0].setPlaying(true);
    rig.secs(4.0);
    const int64_t p0 = rig.pos(0);
    const auto* g = rig.perf.grid(0);

    rig.perf.setJumpSizeIndex(0, 3);           // 8 beats
    REQUIRE(rig.perf.jumpSizeBeats(0) == Approx(8.0));
    rig.perf.beatJumpIdx(0, 3, +1);
    rig.block(2);
    const int64_t p1 = rig.pos(0);
    // forward 8 beats through the grid — frames match beatToFrame math
    const double beatsTravelled = g->frameToBeat(p1) - g->frameToBeat(p0);
    REQUIRE(beatsTravelled == Approx(8.0).margin(0.1));
    REQUIRE(rig.decks[0].isPlaying());         // §21 — keeps playing

    rig.perf.beatJumpIdx(0, 3, -1);            // back 8
    rig.block(2);
    REQUIRE(g->frameToBeat(rig.pos(0))
            == Approx(g->frameToBeat(p1) - 8.0).margin(0.1));
}

TEST_CASE("beat jump: quantize snaps to boundaries (§20,§23)", "[p20]") {
    Rig rig;
    rig.load(0, 120.0);
    rig.decks[0].setPlaying(true);
    rig.secs(2.0);
    rig.perf.setQuantize(0, true);
    const auto* g = rig.perf.grid(0);
    rig.perf.beatJump(0, 4.0);
    rig.block(2);
    // quantized → the emitted target is an integer beat
    REQUIRE_FALSE(rig.seeks.empty());
    const double b = g->frameToBeat(rig.seeks.back().second);
    REQUIRE(b == Approx(std::round(b)).margin(0.01));
    REQUIRE(rig.perf.diag().deck(0).quantizeSnaps >= 1);
}

// ========================= quantize (§22-23) ==========================

TEST_CASE("quantize: trigger lands on grid at 90/120/128/140 (§45)",
          "[p20]") {
    for (double bpm : { 90.0, 120.0, 128.0, 140.0 }) {
        Rig rig;
        rig.load(0, bpm);
        const int64_t fpb = Rig::fpb(bpm);
        // author a cue OFF the beat (half-beat offset)
        rig.perf.setCue(0, 0, CueType::Hot, fpb * 6 + fpb / 2);
        rig.perf.setQuantize(0, true);
        rig.decks[0].setPlaying(true);
        rig.secs(0.3);
        rig.perf.triggerCue(0, 0);
        rig.block(2);
        // the emitted seek target is the snapped position — assert on
        // it, not the still-playing playhead
        REQUIRE(rig.seeks.size() == 1);
        const auto* g = rig.perf.grid(0);
        const double tb = g->frameToBeat(rig.seeks[0].second);
        REQUIRE(tb == Approx(std::round(tb)).margin(0.01));
        REQUIRE(std::fabs(tb - 6.5) <= 1.0);   // near the authored spot
    }
}

TEST_CASE("quantize: resolution cycle + per-cue opt-out (§22)", "[p20]") {
    Rig rig;
    rig.load(0, 120.0);
    REQUIRE_FALSE(rig.perf.quantizeOn(0));
    rig.perf.setQuantize(0, true);
    rig.block(1);                              // echo publishes per block
    REQUIRE(rig.stats[0].quantize.load());
    REQUIRE(rig.perf.quantizeOn(0));

    const int r0 = rig.perf.quantizeResolution(0);
    rig.perf.cycleQuantize(0);
    REQUIRE(rig.perf.quantizeResolution(0) == (r0 + 1) % 7);
    rig.perf.setQuantizeResolution(0, Quantize::Bar);
    REQUIRE(rig.perf.quantizeBeatsFor(0) == Approx(4.0)); // bpb=4

    // Phase 28 §10 — create-time snap applies when quantize is ON at
    // write time; quantized=false opts out of TRIGGER-time snapping.
    const int64_t fpb = Rig::fpb(120.0);
    const int64_t off = fpb * 6 + fpb / 2;
    // authored off-grid while quantize is OFF → stored frame kept
    rig.perf.setQuantize(0, false);
    rig.perf.setCue(0, 1, CueType::Hot, off);
    rig.perf.setCue(0, 2, CueType::Hot, off);
    rig.perf.setQuantize(0, true);
    // quantized=false cue ignores trigger snapping entirely
    rig.perf.setCueQuantized(0, 1, false);
    rig.decks[0].setPlaying(true);
    rig.perf.triggerCue(0, 1);
    rig.block(2);
    REQUIRE(rig.pos(0) == Approx(off).margin(1500));
    // a quantized cue with the same authored frame snaps on trigger
    // (Bar resolution → nearest 4-beat boundary: beat 8)
    rig.perf.triggerCue(0, 2);
    rig.block(2);
    REQUIRE(rig.pos(0) == Approx(fpb * 8).margin(1500));
}

// ======================= slip + reverse (§27-28) ======================

TEST_CASE("reverse: latched + momentary through the engine (§27)",
          "[p20]") {
    Rig rig;
    rig.load(0, 120.0);
    rig.decks[0].setPlaying(true);
    rig.secs(0.5);
    REQUIRE_FALSE(rig.perf.reverseEffective(0));

    rig.perf.setReverse(0, true);
    rig.block(2);
    REQUIRE(rig.perf.reverseEffective(0));
    REQUIRE(rig.stats[0].reverse.load());
    const int64_t p0 = rig.pos(0);
    rig.secs(0.2);
    REQUIRE(rig.pos(0) < p0);                  // actually moving backward

    // momentary press while latched → temporary forward
    rig.perf.reverseMomentary(0, true);
    rig.block(2);
    REQUIRE_FALSE(rig.perf.reverseEffective(0));
    const int64_t p1 = rig.pos(0);
    rig.secs(0.2);
    REQUIRE(rig.pos(0) > p1);                  // forward while held
    rig.perf.reverseMomentary(0, false);       // release → latch back
    rig.block(2);
    REQUIRE(rig.perf.reverseEffective(0));
}

TEST_CASE("reverse under slip keeps the shadow advancing (§28)", "[p20]") {
    Rig rig;
    rig.load(0, 120.0);
    rig.decks[0].setPlaying(true);
    rig.secs(2.0);
    rig.perf.setSlip(0, true);
    const int64_t anchor = rig.pos(0);
    rig.perf.setReverse(0, true);
    rig.secs(0.5);                             // heard backwards ~0.5s
    rig.perf.setReverse(0, false);
    rig.perf.setSlip(0, false);
    rig.block(2);
    // shadow advanced for the full 0.5 s → slip-off lands past anchor,
    // not rewound to where the reversed playhead was
    REQUIRE(rig.pos(0) > anchor + (int64_t)(kRate * 0.3));
}

// ===================== diagnostics + scheduler live ===================

TEST_CASE("diagnostics: counters + text + rejection paths (§43,§50)",
          "[p20]") {
    Rig rig;
    rig.load(0, 120.0);
    rig.decks[0].setPlaying(true);
    rig.secs(0.5);
    rig.perf.setCue(0, 0, CueType::Hot, 4800);
    rig.perf.triggerCue(0, 0);
    rig.perf.loopBeatsIdx(0, 5);
    rig.perf.beatJump(0, 2.0);
    rig.perf.loopRollIdx(0, 4);
    rig.perf.loopRollEnd(0);
    rig.perf.setReverse(0, true);
    rig.perf.setReverse(0, false);
    rig.perf.deleteCue(0, 0);

    const auto& d = rig.perf.diag().deck(0);
    REQUIRE(d.cuesSet == 1);
    REQUIRE(d.cuesDeleted == 1);
    REQUIRE(d.cuesTriggered == 1);
    REQUIRE(d.loopsEntered >= 1);
    REQUIRE(d.loopRolls == 1);
    REQUIRE(d.beatJumps == 1);
    REQUIRE(d.slipBegins == 1);
    REQUIRE(d.slipEnds == 1);
    REQUIRE(d.reverses == 2);
    REQUIRE(d.actionsPosted > 0);
    REQUIRE(d.rtCmdsSent > 0);
    REQUIRE(d.rejects == 0);
    REQUIRE_FALSE(rig.perf.perfDiagText(0).empty());
    // invalid deck rejected with NoDeck, no crash
    REQUIRE(rig.perf.lastError(9) == PerfError::NoDeck);
}

TEST_CASE("undo support: restoreCue verbatim incl. new fields (§41)",
          "[p20]") {
    Rig rig;
    rig.load(0, 120.0);
    rig.perf.setCue(0, 0, CueType::Hot, 4800, -1, 0xFF112233, "x");
    rig.perf.setCueKind(0, 0, HotKind::Cue);
    rig.perf.setCueEnabled(0, 0, false);
    rig.perf.setCueComment(0, 0, "note");
    const Cue captured = rig.perf.cues(0)[0];

    rig.perf.deleteCue(0, 0);
    REQUIRE(rig.perf.cues(0).empty());
    rig.perf.restoreCue(0, captured);          // what UndoManager calls
    REQUIRE(rig.perf.cues(0).size() == 1);
    const Cue& r = rig.perf.cues(0)[0];
    REQUIRE(r.frame == 4800);
    REQUIRE(r.kind == HotKind::Cue);
    REQUIRE_FALSE(r.enabled);
    REQUIRE(r.comment == "note");
    REQUIRE(r.argb == 0xFF112233);

    // loop-range undo snapshot/restore
    rig.decks[0].setPlaying(true);
    rig.secs(0.5);
    rig.perf.loopBeatsIdx(0, 6);
    rig.block(2);
    int64_t in = -1, out = -1;
    rig.perf.loopRange(0, in, out);
    REQUIRE(out > in);
    rig.perf.exitLoop(0);
    rig.block(2);
    rig.perf.restoreLoopRange(0, in, out, true);
    rig.block(2);
    REQUIRE(rig.stats[0].loopOn.load());
    REQUIRE(rig.stats[0].loopInSec.load() * kRate
            == Approx((double)in).margin(100));
}

TEST_CASE("UndoManager: record → undo → redo round-trip", "[p20]") {
    UndoManager um;
    int v = 0;
    um.record("inc", [&]{ v = 0; }, [&]{ v = 1; });
    v = 1;                                     // the applied state
    REQUIRE(um.undo());
    REQUIRE(v == 0);
    REQUIRE(um.redo());
    REQUIRE(v == 1);
    // new record kills redo
    um.record("inc2", [&]{ v = 1; }, [&]{ v = 2; });
    v = 2;
    REQUIRE_FALSE(um.redo());
    REQUIRE(um.undo());
    REQUIRE(v == 1);
    REQUIRE(um.undo());
    REQUIRE(v == 0);
    REQUIRE_FALSE(um.undo());
}

// ====================== persistence + relocation ======================

TEST_CASE("persistence: v6 fields round-trip through the store (§39)",
          "[p20]") {
    const auto dir = tempRoot("persist");
    const auto dbPath = (dir / "perf.db").string();
    Database db;
    REQUIRE(db.open(dbPath));
    AnalysisStore aStore(db);
    PerformanceStore store(db);
    const int64_t trackId = aStore.trackIdFor("/tmp/p20_track.wav");
    REQUIRE(trackId >= 0);

    PerformanceEngine perf;
    perf.setStore(&store);
    perf.setGridSlot(0, nullptr, nullptr, nullptr);
    perf.attachGrid(0, makeGrid(128.0), trackId);
    perf.setCue(0, 0, CueType::Hot, 12345, -1, 0xFF00FF00, "hotA");
    perf.setCue(0, 3, CueType::Memory, 9999, -1, 0xFF33B5E5, "memB");
    perf.setCueKind(0, 0, HotKind::Cue);
    perf.setCueEnabled(0, 0, false);
    perf.setCueQuantized(0, 0, false);
    perf.setCueComment(0, 3, "intro end");
    perf.loopBeatsIdx(0, 7);                   // need a live loop…
    // no stats → curFrame=0; loop math still stores range via grid
    perf.saveLoop(0, 1);

    const auto cues = store.loadCues(trackId);
    REQUIRE(cues.size() == 2);
    const Cue* hot = nullptr; const Cue* mem = nullptr;
    for (auto& c : cues) {
        if (c.type == CueType::Hot) hot = &c;
        if (c.type == CueType::Memory) mem = &c;
    }
    REQUIRE(hot != nullptr); REQUIRE(mem != nullptr);
    REQUIRE(hot->kind == HotKind::Cue);
    REQUIRE_FALSE(hot->enabled);
    REQUIRE_FALSE(hot->quantized);
    REQUIRE(hot->argb == 0xFF00FF00);
    REQUIRE(mem->comment == "intro end");
    REQUIRE(mem->label == "memB");

    const auto loops = store.loadLoops(trackId);
    REQUIRE(loops.size() == 1);
    REQUIRE(loops[0].slot == 1);
    REQUIRE(loops[0].outFrame > loops[0].inFrame);
    REQUIRE(loops[0].outBeat > loops[0].inBeat);

    // reload path: a fresh engine restores cues for the track
    PerformanceEngine perf2;
    perf2.setStore(&store);
    perf2.setGridSlot(0, nullptr, nullptr, nullptr);
    perf2.attachGrid(0, makeGrid(128.0), trackId);
    REQUIRE(perf2.cues(0).size() == 2);
    REQUIRE(perf2.cues(0)[0].kind == HotKind::Cue);
    db.close();
}

TEST_CASE("relocation: same content → same trackId → cues survive (§40)",
          "[p20]") {
    const auto dir = tempRoot("relink");
    const auto wav = makeWav(dir, "before.wav", 0.5, 330.0);

    library::MusicLibraryEngine lib;
    REQUIRE(lib.open((dir / "lib.db").string()));
    lib.setMetadataProvider(
        [](const std::string& p, TrackMeta& m) {
            return FFmpegMetadataReader::read(p, m);
        });
    REQUIRE(lib.importer().importFile(wav.string()));
    Track t;
    REQUIRE(lib.tracks().getByPath(
        fs::absolute(wav).lexically_normal().string(), t));
    const int64_t trackId = t.id;

    PerformanceStore store(lib.db());
    Cue c; c.slot = 0; c.type = CueType::Hot; c.frame = 24000;
    c.label = "keepme"; c.argb = 0xFF00FF00;
    store.saveCue(trackId, c);

    // move the file — verified relink keeps the row + its data
    const auto moved = dir / "after.wav";
    fs::rename(wav, moved);
    REQUIRE(lib.missing().relinkVerified(trackId, moved.string()));
    REQUIRE(t.id == trackId);                  // identity unchanged
    const auto cues = store.loadCues(trackId);
    REQUIRE(cues.size() == 1);
    REQUIRE(cues[0].label == "keepme");
    REQUIRE(cues[0].frame == 24000);
    lib.close();
}

// ===================== failure recovery + stress (§43,§46) ============

TEST_CASE("recovery: actions on an unloaded deck reject cleanly", "[p20]") {
    Rig rig;
    // no load → no stats, no grid
    rig.perf.triggerCue(0, 0);
    REQUIRE(rig.perf.lastError(0) == PerfError::EmptySlot);
    rig.perf.beatJump(0, 4.0);
    REQUIRE(rig.perf.lastError(0) == PerfError::NoGrid);
    rig.perf.loopBeats(0, 4.0);
    REQUIRE(rig.perf.lastError(0) == PerfError::NoGrid);
    rig.perf.cueButton(0, CuePhase::Press);
    REQUIRE(rig.perf.lastError(0) == PerfError::NoTrack);
    rig.perf.instantRestart(0);
    REQUIRE(rig.perf.lastError(0) == PerfError::NoTrack);
    // audio never touched — decks still empty, nothing stuck
    REQUIRE_FALSE(rig.decks[0].isPlaying());
    REQUIRE(rig.perf.diag().deck(0).rejects >= 4);
    // engine stays usable afterwards
    rig.load(0, 120.0);
    REQUIRE(rig.perf.hasGrid(0));
    REQUIRE(rig.perf.lastError(0) != PerfError::NoDeck); // never fatal
}

TEST_CASE("4-deck stress: sync + loops + cues + jumps + rolls", "[p20]") {
    Rig rig;
    for (int d = 0; d < 4; ++d) {
        rig.load(d, 126.0 + d * 2.0);
        rig.decks[d].setPlaying(true);
        for (int s = 0; s < 4; ++s)
            rig.perf.setCue(d, s, CueType::Hot,
                            Rig::fpb(126.0) * (s + 2) * 4);
        rig.perf.setQuantize(d, true);
    }
    rig.secs(0.5);
    rig.perf.enableSync(0);                    // master
    for (int d = 1; d < 4; ++d) rig.perf.enableSync(d);

    const auto t0 = std::chrono::steady_clock::now();
    for (int iter = 0; iter < 40; ++iter) {
        for (int d = 0; d < 4; ++d) {
            rig.perf.triggerCue(d, iter % 4);
            rig.perf.loopBeatsIdx(d, 5 + (iter % 3));
            rig.perf.beatJumpIdx(d, iter % 7, (iter & 1) ? 1 : -1);
            rig.perf.loopRollIdx(d, iter % 6);
            rig.perf.loopRollEnd(d);
            rig.perf.setReverse(d, (iter + d) % 3 == 0);
        }
        rig.block(4);
    }
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    std::printf("[p20] 4-deck stress: 160 mixed actions + %.0f blocks"
                " in %.1f ms\n", 40.0 * 4, ms);

    for (int d = 0; d < 4; ++d) {
        REQUIRE(rig.decks[d].isPlaying());     // stability: still running
        REQUIRE(rig.pos(d) > 0);
        REQUIRE(rig.perf.lastError(d) == PerfError::Ok);
        REQUIRE(rig.perf.syncMode(d) != SyncMode::Off);
    }
    const auto tot = rig.perf.diag().totals();
    REQUIRE(tot.cuesTriggered == 160);
    REQUIRE(tot.loopRolls == 160);
    REQUIRE(tot.rejects == 0);
    REQUIRE(ms < 30000);                       // generous CI bound
}

// ==================== visual feedback state (§48) =====================

TEST_CASE("feedback model reflects engine state immediately (§48)",
          "[p20]") {
    using namespace pads;
    Rig rig;
    rig.load(0, 120.0);
    rig.decks[0].setPlaying(true);
    rig.secs(0.5);
    rig.perf.setCue(0, 0, CueType::Hot, 4800);
    rig.perf.setCue(0, 1, CueType::Memory, 9600);

    // assemble the same PadDeckState AppShell builds per tick
    PadFeedbackEngine fb;
    PadDeckState ps;
    ps.mode = PadMode::HotCue;
    ps.playing = true;
    for (auto& c : rig.perf.cues(0)) {
        if (c.type != CueType::Memory && c.slot < 8)
            ps.hot[(size_t)c.slot] = &c;
        else if (c.slot < 8)
            ps.mem[(size_t)c.slot] = &c;
    }
    auto ls = fb.lights(ps);
    REQUIRE(ls[0].state == PadLightState::Set);
    REQUIRE(ls[0].argb == rig.perf.cues(0)[0].argb);
    REQUIRE(ls[1].state == PadLightState::Dim);   // mem cue isn't a pad

    // loop on → Loop-mode pads reflect selected+playing
    rig.perf.loopBeatsIdx(0, 5);
    rig.block(2);
    ps.mode = PadMode::Loop;
    ps.loopOn = rig.stats[0].loopOn.load();
    ps.loopSizeIdx = rig.perf.loopSizeIndex(0);
    REQUIRE(fb.light(ps, 5).state == PadLightState::Playing);

    // quantize/slip/reverse read straight off stats — flags live
    rig.perf.setSlip(0, true); rig.block(1);
    REQUIRE(rig.stats[0].slip.load());
    rig.perf.setReverse(0, true); rig.block(1);
    REQUIRE(rig.stats[0].reverse.load());
    rig.perf.setSlip(0, false); rig.perf.setReverse(0, false);
}
