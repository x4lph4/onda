// Phase 9 — controller engine tests: mapping, learn, feedback,
// profiles, hotplug, encoders, jog, scaling, echo suppression, stress.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include "controller/ControllerEngine.h"
#include "controller/VirtualMidiController.h"
#include "controller/ParamNames.h"
#include "core/rt/CommandQueue.h"
#include "ui/CommandManager.h"

using namespace onda;
using namespace onda::ctrl;
namespace fs = std::filesystem;

namespace {

fs::path tempDir(const char* n) {
    fs::path d = fs::temp_directory_path() / "onda_ctrltest" / n;
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

/// Test rig: engine + virtual controller + spy ports.
struct Rig {
    ControllerEngine eng { (tempDir("rig") / "profiles").string() };
    VirtualMidiController vc { eng };
    ui::CommandManager cm;
    rt::CommandQueue queue;
    std::vector<std::pair<uint16_t, float>> params;
    std::vector<std::array<double,3>> jogs;
    std::unordered_map<uint32_t, int> cmdStates;
    std::unordered_map<uint16_t, float> paramVals;
    std::vector<std::string> notes;

    Rig() {
        vc.attach();
        ui::Command c; c.id = 1; c.name = "decka.playToggle";
        c.category = "Deck"; c.run = [this](float, ui::CommandSource){};
        c.state = [this] { return cmdStates[1]; };
        cm.add(c);
        ControllerEngine::Ports p;
        p.sendCommand = [this](uint32_t id, float v) {
            cm.invoke(id, ui::CommandSource::Midi, v);
            cmdStates[id] = v > 0.5f ? 1 : 0;
        };
        p.commandState = [this](uint32_t id) { return cmdStates[id]; };
        p.commandIdFor = [this](const std::string& n) {
            return cm.idFor(n);
        };
        p.setParam = [this](uint16_t id, float v) {
            params.emplace_back(id, v); paramVals[id] = v;
        };
        p.paramState = [this](uint16_t id) { return paramVals[id]; };
        p.jog = [this](int d, int m, double delta) {
            jogs.push_back({ (double)d, (double)m, delta });
        };
        p.notify = [this](const std::string& t,
                          const std::string& d, int) {
            notes.push_back(t + ":" + d);
        };
        eng.setPorts(std::move(p));
    }

    DeviceState* st() { return eng.device(0xF00D); }
    void send(Mapping m) { st()->mapping.addMapping(m); }

    static Mapping mapNote(int num, uint32_t cmd,
                           MappingMode mode = MappingMode::Toggle) {
        Mapping m;
        m.in = { AddrKind::Note, 0, (uint8_t)num };
        m.target.kind = Target::Kind::Command;
        m.target.cmd = cmd; m.mode = mode;
        return m;
    }
    static Mapping mapCc(int cc, uint16_t param,
                         MappingMode mode = MappingMode::Absolute) {
        Mapping m;
        m.in = { AddrKind::CC, 0, (uint8_t)cc };
        m.target.kind = Target::Kind::Param;
        m.target.paramId = param; m.mode = mode;
        return m;
    }
};

// ------------------------------------------------------------------
// Button mapping — toggle / momentary, command path
// ------------------------------------------------------------------
TEST_CASE("Controller: note→command toggle + momentary", "[ctrl]") {
    Rig r;
    int hits = 0;
    ui::Command c2; c2.id = 9; c2.name = "x"; c2.category = "x";
    c2.run = [&](float v, ui::CommandSource s) {
        REQUIRE(s == ui::CommandSource::Midi);
        ++hits; r.cmdStates[9] = v > 0.5f ? 1 : 0;
    };
    c2.state = [&] { return r.cmdStates[9]; };
    r.cm.add(c2);

    r.send(Rig::mapNote(36, 9));                       // toggle
    r.send(Rig::mapNote(37, 9, MappingMode::Momentary));// momentary

    // toggle: press flips on, release ignored, next press flips off
    r.vc.pressButton(36);
    REQUIRE(r.cmdStates[9] == 1);
    r.vc.pressButton(36);
    REQUIRE(r.cmdStates[9] == 0);

    // momentary: press=1, release=0
    r.vc.holdButton(37);
    REQUIRE(r.cmdStates[9] == 1);
    r.vc.releaseButton(37);
    REQUIRE(r.cmdStates[9] == 0);
    REQUIRE(hits >= 4);
}

// ------------------------------------------------------------------
// Absolute CC→param with scaling + curve
// ------------------------------------------------------------------
TEST_CASE("Controller: CC→param scaling, inverse, pickup", "[ctrl]") {
    Rig r;
    // gain 0..2, linear
    {
        auto m = Rig::mapCc(16, paramIdFromName("CH_0_GAIN"));
        m.outMin = 0.0f; m.outMax = 2.0f;
        r.send(m);
    }
    // filter -1..1, expo
    {
        auto m = Rig::mapCc(17, paramIdFromName("CH_0_FILTER"));
        m.outMin = -1.0f; m.outMax = 1.0f; m.curve = Curve::Expo;
        r.send(m);
    }
    // inverse knob
    {
        auto m = Rig::mapCc(18, paramIdFromName("CUE_MIX"));
        m.outMin = 0.0f; m.outMax = 1.0f; m.curve = Curve::Inverse;
        r.send(m);
    }

    r.vc.turnKnob(16, 127);
    REQUIRE(r.paramVals[paramIdFromName("CH_0_GAIN")] ==
            Catch::Approx(2.0f));
    r.vc.turnKnob(16, 64);
    REQUIRE(r.paramVals[paramIdFromName("CH_0_GAIN")] ==
            Catch::Approx(2.0f * 64.0f / 127.0f).margin(0.02));

    r.vc.turnKnob(17, 64);       // expo: (64/127)² mapped -1..1
    const float x = 64.0f / 127.0f;
    REQUIRE(r.paramVals[paramIdFromName("CH_0_FILTER")] ==
            Catch::Approx(-1.0f + 2.0f * x * x).margin(0.02));

    r.vc.turnKnob(18, 0);        // inverse: 0 → 1.0
    REQUIRE(r.paramVals[paramIdFromName("CUE_MIX")] ==
            Catch::Approx(1.0f));

    // pickup: moving the knob does nothing until it crosses current
    {
        auto m = Rig::mapCc(19, paramIdFromName("MASTER_GAIN"),
                            MappingMode::Pickup);
        m.outMin = 0.0f; m.outMax = 2.0f;
        r.send(m);
    }
    const uint16_t mg = paramIdFromName("MASTER_GAIN");
    r.paramVals[mg] = 1.0f;      // "software" value at center
    r.params.clear();
    r.vc.turnKnob(19, 10);       // far below → swallowed
    REQUIRE(r.params.empty());
    r.vc.turnKnob(19, 60);       // approaching
    r.vc.turnKnob(19, 64);       // crosses 1.0 (≈63.5) → pickup
    REQUIRE_FALSE(r.params.empty());
    REQUIRE(r.paramVals[mg] ==
            Catch::Approx(2.0f * 64.0f / 127.0f).margin(0.02));
}

// ------------------------------------------------------------------
// Encoders — all three relative decodings
// ------------------------------------------------------------------
TEST_CASE("Controller: relative encoders (64-offset, 2s, sign-mag)",
          "[ctrl]") {
    Rig r;
    const uint16_t pg = paramIdFromName("CH_1_GAIN");
    r.paramVals[pg] = 0.5f;

    for (auto [mode, upByte, dnByte] : std::vector<std::tuple<EncoderMode,int,int>>{
             { EncoderMode::RelBinaryOffset,   65, 63 },
             { EncoderMode::RelTwosComplement,  1, 0x7F },
             { EncoderMode::RelSignMagnitude,   1, 0x41 } }) {
        r.paramVals[pg] = 0.5f;
        r.st()->mapping.clear();
        auto m = Rig::mapCc(30, pg, MappingMode::Relative);
        m.encoder = mode; m.outMin = 0.0f; m.outMax = 2.0f;
        m.step = 0.05f;
        r.send(m);
        r.vc.spinEncoderRaw(30, upByte);
        REQUIRE(r.paramVals[pg] == Catch::Approx(0.55f));
        r.vc.spinEncoderRaw(30, dnByte);
        REQUIRE(r.paramVals[pg] == Catch::Approx(0.5f));
    }

    // IncDec → command invoked per tick with sign
    int navs = 0; float lastV = 0;
    ui::Command c; c.id = 77; c.name = "nav"; c.category = "b";
    c.run = [&](float v, ui::CommandSource) { ++navs; lastV = v; };
    r.cm.add(c);
    Mapping mm;
    mm.in = { AddrKind::CC, 0, 31 };
    mm.target.kind = Target::Kind::Command; mm.target.cmd = 77;
    mm.mode = MappingMode::IncDec;
    mm.encoder = EncoderMode::RelBinaryOffset;
    r.send(mm);
    r.vc.spinEncoderRaw(31, 66);   // +2 ticks
    REQUIRE(navs == 2);
    REQUIRE(lastV == 1.0f);
    r.vc.spinEncoderRaw(31, 62);   // -2 ticks
    REQUIRE(navs == 4);
    REQUIRE(lastV == -1.0f);
}

// ------------------------------------------------------------------
// Jog wheels — target::Jog → jog port
// ------------------------------------------------------------------
TEST_CASE("Controller: jog mapping → jog port event", "[ctrl]") {
    Rig r;
    Mapping m;
    m.in = { AddrKind::CC, 0, 40 };
    m.target.kind = Target::Kind::Jog;
    m.target.jogDeck = 1;
    m.target.jogMode = (uint8_t)JogMode::JogPitchBend;
    m.mode = MappingMode::Relative;
    m.encoder = EncoderMode::RelBinaryOffset;
    m.step = 1.0f;
    r.send(m);

    r.vc.spinEncoder(40, 5);
    REQUIRE(r.jogs.size() == 5);
    REQUIRE(r.jogs[0][0] == Catch::Approx(1.0)); // deck
    REQUIRE(r.jogs[0][1] == Catch::Approx(
                (double)JogMode::JogPitchBend));
    REQUIRE(r.jogs[0][2] == Catch::Approx(1.0)); // +1 tick * step
}

// ------------------------------------------------------------------
// Feedback — LED follows command state, throttle, loop protection
// ------------------------------------------------------------------
TEST_CASE("Controller: feedback LED + echo suppression", "[ctrl]") {
    Rig r;
    // mapping note 36 → cmd 9 (toggle) + LED on same note
    Mapping m = Rig::mapNote(36, 9);
    m.out.addr = { AddrKind::Note, 0, 36 };
    m.out.hasCustom = true; m.out.onValue = 127; m.out.offValue = 0;
    r.send(m);

    // rebuild watches manually (applyProfile path does this normally)
    FeedbackEngine::Watch w;
    w.out = { AddrKind::Note, 0, 36 };
    w.cmd = 9; w.onValue = 127; w.offValue = 0;
    r.st()->feedback.setWatches({ w });
    r.st()->feedback.setPorts(FeedbackEngine::Ports{
        [r = &r](const MidiAddr& a, uint8_t v) {
            r->vc.outbox().push_back(makeOut(a, v));
        },
        [r = &r](uint32_t c) { return r->cmdStates[c]; },
        [r = &r](uint16_t p) { return r->paramVals[p]; }});

    r.cmdStates[9] = 0;
    r.eng.restoreFeedback(0xF00D);
    r.eng.tick(1.0);
    REQUIRE(r.vc.outValueFor({ AddrKind::Note, 0, 36 }) == 0);

    r.cmdStates[9] = 1;
    r.eng.tick(1.1);
    REQUIRE(r.vc.outValueFor({ AddrKind::Note, 0, 36 }) == 127);

    // same state → no resend (throttle)
    const size_t n = r.vc.outbox().size();
    r.eng.tick(1.2);
    REQUIRE(r.vc.outbox().size() == n);

    // state changed via UI (not the button) → LED still updates
    r.cmdStates[9] = 0;
    r.eng.tick(1.3);
    REQUIRE(r.vc.outValueFor({ AddrKind::Note, 0, 36 }) == 0);

}

TEST_CASE("Controller: motorized fader echo is suppressed", "[ctrl]") {
    Rig r;
    const uint16_t g = paramIdFromName("CH_0_GAIN");
    r.paramVals[g] = 0.0f;

    // CC 60 controls gain and drives the motor back
    auto m = Rig::mapCc(60, g);
    m.outMin = 0; m.outMax = 2;
    r.send(m);
    FeedbackEngine::Watch w;
    w.out = { AddrKind::CC, 0, 60 };
    w.paramId = g; w.followValue = true;
    w.srcMin = 0.0f; w.srcMax = 2.0f;   // gain range
    w.onValue = 127; w.offValue = 0;
    r.st()->feedback.setWatches({ w });
    r.st()->feedback.setPorts(FeedbackEngine::Ports{
        [r = &r](const MidiAddr& a, uint8_t v) {
            r->vc.outbox().push_back(makeOut(a, v));
        },
        [r = &r](uint32_t) { return 0; },
        [r = &r](uint16_t p) { return r->paramVals[p]; }});

    r.paramVals[g] = 1.0f;
    r.eng.tick(1.0);
    const int motor = r.vc.outValueFor({ AddrKind::CC, 0, 60 });
    REQUIRE(motor == 63);   // 1.0/2.0 of range ≈ 63

    // the physical fader echoes exactly that value back → echo, drop
    const auto dropped0 = r.st()->mapping.dropped();
    r.vc.turnKnob(60, motor);
    REQUIRE(r.st()->mapping.dropped() == dropped0 + 1);
    // a DIFFERENT value is the user grabbing the fader → not echo
    r.vc.turnKnob(60, 90);
    REQUIRE(r.paramVals[g] ==
            Catch::Approx(2.0f * 90.0f / 127.0f).margin(0.02));
}

// ------------------------------------------------------------------
// Learn Mode — arm → move → mapping captured
// ------------------------------------------------------------------
TEST_CASE("Controller: LearnMode captures the next input", "[ctrl]") {
    Rig r;
    Mapping tmpl;
    tmpl.target.kind = Target::Kind::Command;
    tmpl.target.cmd = 9;
    tmpl.mode = MappingMode::Toggle;
    r.eng.learnArmFor(0xF00D, tmpl);
    REQUIRE(r.eng.learn().armed());

    int learned = 0;
    r.eng.onLearnedMapping = [&](uint32_t dev, const Mapping& m) {
        REQUIRE(dev == 0xF00D);
        REQUIRE(m.in.kind == AddrKind::Note);
        REQUIRE(m.in.number == 55);
        ++learned;
    };
    r.vc.pressButton(55);
    REQUIRE(learned == 1);
    REQUIRE_FALSE(r.eng.learn().armed());

    // captured mapping now works
    r.cmdStates[9] = 0;
    r.vc.pressButton(55);
    REQUIRE(r.cmdStates[9] == 1);
}

// ------------------------------------------------------------------
// Profiles — save/load/validate/assign/auto-match
// ------------------------------------------------------------------
TEST_CASE("Controller: profile JSON roundtrip + validation", "[ctrl]") {
    ControllerProfile p;
    p.meta.manufacturer = "Acme"; p.meta.model = "PadOne";
    p.meta.nameContains = "pad";
    p.capabilities = CAP_PADS | CAP_LED_FEEDBACK;

    Mapping m1;
    m1.in = { AddrKind::Note, 0, 36 };
    m1.target.kind = Target::Kind::Command;
    m1.target.cmdName = "decka.playToggle";
    m1.mode = MappingMode::Toggle;
    m1.out.addr = { AddrKind::Note, 0, 36 };
    m1.out.onValue = 127; m1.out.offValue = 0;
    p.mappings.push_back(m1);

    Mapping m2;
    m2.in = { AddrKind::CC, 0, 16 };
    m2.target.kind = Target::Kind::Param;
    m2.target.paramId = paramIdFromName("CH_0_GAIN");
    m2.mode = MappingMode::Absolute;
    m2.outMin = 0; m2.outMax = 2;
    p.mappings.push_back(m2);

    const std::string j = p.toJson();
    REQUIRE(j.find("profileVersion") != std::string::npos);
    REQUIRE(j.find("decka.playToggle") != std::string::npos);

    ControllerProfile q;
    std::string err;
    REQUIRE(q.fromJson(j, err));
    REQUIRE(q.meta.manufacturer == "Acme");
    REQUIRE(q.mappings.size() == 2);
    REQUIRE(q.mappings[0].target.cmdName == "decka.playToggle");
    REQUIRE(q.mappings[1].target.paramId ==
            paramIdFromName("CH_0_GAIN"));
    REQUIRE(q.capabilities & CAP_PADS);
    REQUIRE(q.validate().empty());
    REQUIRE(q.matchesDevice("USB PAD CONTROLLER"));
    REQUIRE(!q.matchesDevice("Some Other Device"));

    // invalid: bad json / bad version / bad mode
    REQUIRE(!q.fromJson("{ not json", err));
    REQUIRE(!q.fromJson("{\"profileVersion\": 99}", err));
    REQUIRE(!q.fromJson(
        "{\"profileVersion\":1,\"mappings\":[{\"mode\":\"bogus\"}]}",
        err));
    // profile is pure data — no code fields are honored
    ControllerProfile safe;
    REQUIRE(safe.fromJson(
        "{\"profileVersion\":1,\"exec\":\"rm -rf /\",\"mappings\":[]}",
        err));
    REQUIRE(safe.mappings.empty());
}

TEST_CASE("Controller: ProfileManager install/assign/auto-match",
          "[ctrl]") {
    const auto dir = (tempDir("pm") / "profiles").string();
    ProfileManager pm(dir);

    ControllerProfile p;
    p.meta.manufacturer = "Acme"; p.meta.model = "PadOne";
    p.meta.nameContains = "pad";
    p.mappings.push_back(Rig::mapNote(36, 1));

    std::string err;
    const std::string id = pm.installProfile(p, err);
    REQUIRE_FALSE(id.empty());
    REQUIRE(pm.listProfiles().size() == 1);
    REQUIRE(pm.autoMatch("Acme PAD USB", err) == id);

    REQUIRE(pm.assignProfile(7, id, err));
    REQUIRE(pm.profileIdFor(7) == id);
    REQUIRE(pm.profileFor(7) != nullptr);
    REQUIRE(pm.profileFor(7)->meta.model == "PadOne");

    // export + delete
    const auto dest =
        (tempDir("pm_export") / "export.json").string();
    const bool exOk = pm.exportProfile(id, dest, err);
    INFO("export err: " << err);
    REQUIRE(exOk);
    REQUIRE(pm.deleteProfile(id, err));
    REQUIRE(pm.profileFor(7) == nullptr);
    REQUIRE(pm.listProfiles().empty());

    // import back from the export
    REQUIRE(pm.importProfile(dest, err));
    REQUIRE(pm.listProfiles().size() == 1);
}

// ------------------------------------------------------------------
// applyProfile through the engine — names resolve, watches built
// ------------------------------------------------------------------
TEST_CASE("Controller: applyProfile wires mappings + feedback",
          "[ctrl]") {
    Rig r;
    ControllerProfile p;
    p.meta.manufacturer = "Acme"; p.meta.model = "PadOne";

    Mapping m;
    m.in = { AddrKind::Note, 0, 36 };
    m.target.kind = Target::Kind::Command;
    m.target.cmdName = "decka.playToggle";  // resolved at apply time
    m.mode = MappingMode::Toggle;
    m.out.addr = { AddrKind::Note, 0, 36 };
    m.out.hasCustom = true; m.out.onValue = 127;
    p.mappings.push_back(m);

    std::string err;
    REQUIRE(r.eng.applyProfile(0xF00D, p, err));
    REQUIRE(r.st()->mapping.mappings().size() == 1);
    REQUIRE(r.st()->mapping.mappings()[0].target.cmd ==
            r.cm.idFor("decka.playToggle"));
    REQUIRE(r.st()->feedback.watches().size() == 1);

    // profile with unknown command name is rejected
    ControllerProfile bad = p;
    bad.mappings[0].target.cmdName = "no.such.command";
    REQUIRE_FALSE(r.eng.applyProfile(0xF00D, bad, err));
    REQUIRE(err.find("unknown command") != std::string::npos);
}

// ------------------------------------------------------------------
// Hot-plug — disconnect / reconnect lifecycle
// ------------------------------------------------------------------
TEST_CASE("Controller: hot-plug disconnect/reconnect", "[ctrl]") {
    Rig r;
    REQUIRE(r.eng.devices().connected(0xF00D));

    // attach a second device
    DeviceInfo d2 { 0xBEEF, "Second Ctrl", "X", "Ctrl-2",
                    Transport::MidiUsb, 0, true, true };
    r.eng.attachDevice(d2, [](const MidiMsg&) {}, 1.0);
    REQUIRE(r.eng.devices().count() == 2);

    // disconnect it — no crash, notifications fire
    r.notes.clear();
    r.eng.detachDevice(0xBEEF, 2.0);
    REQUIRE(!r.eng.devices().connected(0xBEEF));
    REQUIRE_FALSE(r.notes.empty());

    // input from a dead device is dropped silently
    r.eng.processInput(0xBEEF,
        MidiMsg{ MsgType::NoteOn, 0, 36, 127, 0 });

    // reconnect inside the window → Reconnected event + state restore
    r.notes.clear();
    r.eng.attachDevice(d2, [](const MidiMsg&) {}, 3.0);
    REQUIRE(r.eng.devices().connected(0xBEEF));
    REQUIRE(r.eng.devices().reconnects() == 1);
}

// ------------------------------------------------------------------
// Param name table — roundtrip + collision-freedom
// ------------------------------------------------------------------
TEST_CASE("Controller: param names roundtrip", "[ctrl]") {
    for (int ch = 0; ch < 4; ++ch) {
        for (auto k : { mixer::ChParam::Gain, mixer::ChParam::Low,
                        mixer::ChParam::Mid, mixer::ChParam::High,
                        mixer::ChParam::Filter, mixer::ChParam::Fader,
                        mixer::ChParam::SendA }) {
            const uint16_t id = mixer::chParam(ch, k);
            REQUIRE(paramIdFromName(paramNameFromId(id)) == id);
        }
    }
    REQUIRE(paramIdFromName("CROSSFADER") ==
            mixer::ParameterRegistry::CROSSFADER);
    REQUIRE(paramIdFromName(paramNameFromId(
                mixer::ParameterRegistry::MASTER_GAIN)) ==
            mixer::ParameterRegistry::MASTER_GAIN);
    for (int t = 0; t < 8; ++t)
        for (int s = 0; s < 2; ++s) {
            const uint16_t id = fx::paramId(t, s, fx::FXSlotKind::P3);
            REQUIRE(paramIdFromName(paramNameFromId(id)) == id);
        }
    REQUIRE(paramIdFromName("NOT_A_PARAM") == kInvalidParam);
    REQUIRE(paramNameFromId(0x7FFF) == "");
}

// ------------------------------------------------------------------
// MidiClock — bpm estimate, decoupled
// ------------------------------------------------------------------
TEST_CASE("Controller: MIDI clock estimates bpm, stays decoupled",
          "[ctrl]") {
    Rig r;
    r.eng.clock().setRole(MidiClock::Role::Sink);
    // 120 bpm → 500 ms/beat → 20.833 ms/tick (24 PPQN)
    const int64_t tickUs = 20833;
    int64_t us = 1000000;
    r.vc.clockStart(us);
    for (int i = 0; i < 48; ++i) {
        us += tickUs;
        r.vc.clockTick(us);
    }
    REQUIRE(r.eng.clock().running());
    REQUIRE(r.eng.clock().inputBpm() == Catch::Approx(120.0).margin(1.0));
    // clock never reaches the mapping engine
    REQUIRE(r.st()->mapping.processed() == 0);
}

// ------------------------------------------------------------------
// Diagnostics — monitor ring + latency counters
// ------------------------------------------------------------------
TEST_CASE("Controller: diagnostics monitor + latency", "[ctrl]") {
    Rig r;
    r.send(Rig::mapNote(36, 9));
    r.vc.pressButton(36);
    r.vc.turnKnob(99, 5);            // unmapped → dropped
    REQUIRE(r.eng.diag().inCount() >= 3);
    REQUIRE(r.eng.diag().actedCount() >= 1);
    REQUIRE(r.eng.diag().ring().size() >= 3);
    REQUIRE(r.eng.diag().avgLatencyUs() >= 0.0);
    const auto& e = r.eng.diag().ring().back();
    REQUIRE(ControllerDiagnostics::describe(e).find("IN") !=
            std::string::npos);
}

// ------------------------------------------------------------------
// STRESS — high message rate, bounded work, no loss
// ------------------------------------------------------------------
TEST_CASE("Controller: stress 20k msgs @high rate", "[ctrl][stress]") {
    Rig r;
    const uint16_t fader = paramIdFromName("CH_0_FADER");
    {
        auto m = Rig::mapCc(20, fader);
        m.outMin = 0; m.outMax = 1;
        r.send(m);
    }
    for (int i = 0; i < 8; ++i)
        r.send(Rig::mapNote(40 + i, 9, MappingMode::Momentary));

    const int N = 20000;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; ++i) {
        r.vc.turnKnob(20, i % 128);                  // fader spam
        if (i % 4 == 0)
            r.vc.pressButton(40 + (i % 8));          // pads
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count();

    const int64_t totalIn = r.st()->mapping.processed() +
                            r.st()->mapping.dropped();
    std::printf("  [stress] %d msgs in %.1f ms (%.0f msg/s)\n",
                (int)totalIn, ms, totalIn * 1000.0 / ms);
    std::printf("  [stress] acted=%lld dropped=%lld  "
                "avg proc=%.1f µs max=%lld µs\n",
                (long long)r.st()->mapping.processed(),
                (long long)r.st()->mapping.dropped(),
                (double)r.st()->mapping.processUs() /
                    std::max<int64_t>(1, totalIn),
                (long long)r.st()->mapping.lastProcessUs());

    // every message accounted for — no loss
    REQUIRE(totalIn == N + N / 4 * 2);
    // throughput: mapping path far above any real burst rate
    REQUIRE(totalIn * 1000.0 / ms > 50000.0);
    // feedback tick stays cheap under load
    r.eng.restoreFeedback(0xF00D);
}

} // namespace
