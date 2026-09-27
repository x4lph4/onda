// Phase 27 — controller engine completion tests: lifecycle, control
// registry, layers, long-press, priority, validation, sysex, LED
// blink, generic jog/pad engines, factory/user profiles, monitor
// decode, multi-controller, flood. Byte-level FLX4 data comes only
// from documented public sources (see controller/flx4/Flx4Ids.h).
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "controller/ControlRegistry.h"
#include "controller/ControllerEngine.h"
#include "controller/VirtualMidiController.h"
#include "controller/flx4/Flx4Ids.h"
#include "controller/flx4/Flx4Plugin.h"
#include "controller/flx4/Flx4Profile.h"
#include "controller/flx4/VirtualFlx4.h"
#include "core/rt/CommandQueue.h"
#include "ui/CommandManager.h"
#include "ui/CommandIds.h"

using namespace onda;
using namespace onda::ctrl;
namespace fs = std::filesystem;

namespace {

fs::path tempDir(const char* n) {
    fs::path d = fs::temp_directory_path() / "onda_ctrl27" / n;
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

struct Rig {
    ControllerEngine eng { (tempDir("rig") / "profiles").string() };
    VirtualMidiController vc { eng };
    ui::CommandManager cm;
    std::vector<std::pair<uint32_t, float>> cmds;
    std::vector<std::pair<uint16_t, float>> params;
    std::vector<std::array<double,3>>       jogs;
    std::unordered_map<uint32_t, int>       cmdStates;
    std::unordered_map<uint16_t, float>     paramVals;
    std::vector<std::string>                notes;

    Rig() {
        vc.attach();
        ui::Command c; c.id = 1; c.name = "test.cmd";
        c.category = "T"; c.run = [](float, ui::CommandSource) {};
        c.state = [this] { return cmdStates[1]; };
        cm.add(c);
        ControllerEngine::Ports p;
        p.sendCommand = [this](uint32_t id, float v) {
            cmds.emplace_back(id, v);
            cmdStates[id] = v > 0.5f ? 1 : 0;
        };
        p.commandState = [this](uint32_t id) { return cmdStates[id]; };
        p.commandIdFor = [this](const std::string& n) { return cm.idFor(n); };
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

    static Mapping note(int n, uint32_t cmd,
                        MappingMode md = MappingMode::Toggle) {
        Mapping m;
        m.in = { AddrKind::Note, 0, (uint8_t)n };
        m.target.kind = Target::Kind::Command;
        m.target.cmd = cmd; m.mode = md;
        return m;
    }
    static Mapping cc(int c, uint16_t par,
                      MappingMode md = MappingMode::Absolute) {
        Mapping m;
        m.in = { AddrKind::CC, 0, (uint8_t)c };
        m.target.kind = Target::Kind::Param;
        m.target.paramId = par; m.mode = md;
        return m;
    }
};

// ------------------------------------------------------------------
TEST_CASE("Lifecycle: detected→ready→disconnected transitions",
          "[ctrl27]") {
    Rig r;
    auto s = r.eng.state(0xF00D);
    // no profile matched → transport open but not READY
    REQUIRE(s.connected);
    REQUIRE(s.lifecycle == DeviceLifecycle::Connected);
    REQUIRE(!s.initialized);
    REQUIRE(lifecycleName(s.lifecycle) == std::string("CONNECTED"));

    // apply a profile → READY + initialized
    ControllerProfile p;
    p.meta.manufacturer = "T"; p.meta.model = "X";
    p.mappings.push_back(Rig::note(40, 1));
    std::string err;
    REQUIRE(r.eng.applyProfile(0xF00D, p, err));
    s = r.eng.state(0xF00D);
    REQUIRE(s.lifecycle == DeviceLifecycle::Ready);
    REQUIRE(s.initialized);
    REQUIRE(s.mappingCount == 1);

    // bad profile → ERROR + lastError
    ControllerProfile bad;
    bad.version = 99;
    REQUIRE(!r.eng.applyProfile(0xF00D, bad, err));
    s = r.eng.state(0xF00D);
    REQUIRE(s.lifecycle == DeviceLifecycle::Error);
    REQUIRE(!s.lastError.empty());

    // disconnect → Disconnected (transient Disconnecting inside)
    r.vc.detach(2.0);
    s = r.eng.state(0xF00D);
    REQUIRE(s.lifecycle == DeviceLifecycle::Disconnected);
    REQUIRE(!s.connected);

    // state() on an unknown id → disconnected snapshot, no crash
    REQUIRE(r.eng.state(0xDEAD).lifecycle ==
            DeviceLifecycle::Disconnected);
}

// ------------------------------------------------------------------
TEST_CASE("ControlRegistry: logical ids resolve to targets",
          "[ctrl27]") {
    const auto& reg = controlRegistry();
    REQUIRE(reg.list().size() > 60);   // 4 decks + mixer + browser…

    const LogicalControl* play = reg.find("decka.play");
    REQUIRE(play != nullptr);
    REQUIRE(play->target.kind == Target::Kind::Command);
    REQUIRE(play->target.cmd == ui::cmd::forDeck(ui::cmd::DeckPlayToggle, 0));
    REQUIRE(play->kind == ControlKind::Button);

    const LogicalControl* pad = reg.find("deckb.pad.3");
    REQUIRE(pad != nullptr);
    REQUIRE(pad->target.cmd == ui::cmd::forPad(1, 2, false));
    REQUIRE(reg.find("deckb.pad.3.shift")->target.cmd ==
            ui::cmd::forPad(1, 2, true));

    const LogicalControl* jog = reg.find("deckc.jog.scratch");
    REQUIRE(jog != nullptr);
    REQUIRE(jog->target.kind == Target::Kind::Jog);
    REQUIRE(jog->target.jogDeck == 2);
    REQUIRE(jog->target.jogMode == (uint8_t)JogMode::JogScratch);

    REQUIRE(reg.find("mixer.crossfader")->target.paramId ==
            mixer::ParameterRegistry::CROSSFADER);
    REQUIRE(reg.find("ch2.cue")->target.paramId ==
            mixer::ParameterRegistry::channelCue(1));
    REQUIRE(reg.find("browser.next")->target.cmd ==
            ui::cmd::BrowserNextTrack);
    REQUIRE(reg.find("nonexistent.control") == nullptr);

    // skeleton produces a ready-to-fill mapping
    Mapping sk;
    REQUIRE(reg.skeleton("decka.play", sk));
    REQUIRE(sk.mode == MappingMode::Momentary);
    REQUIRE(!reg.skeleton("bogus.id", sk));
}

// ------------------------------------------------------------------
TEST_CASE("ControlRegistry: applyProfile resolves 'control' targets",
          "[ctrl27]") {
    Rig r;
    {   // "decka.playToggle" is a REAL command whose id is 0 — apply
        // resolution must find it (cmd==0 is a legal id, not "unset")
        ui::Command c;
        c.id = ui::cmd::forDeck(ui::cmd::DeckPlayToggle, 0);
        c.name = "decka.playToggle"; c.category = "Deck";
        c.run = [](float, ui::CommandSource) {};
        r.cm.add(std::move(c));
    }
    ControllerProfile p;
    p.meta.manufacturer = "T"; p.meta.model = "Reg";
    Mapping m;
    m.in = { AddrKind::Note, 0, 50 };
    m.target.control = "decka.play";    // logical id, no cmd/cmdName
    m.mode = MappingMode::Momentary;
    p.mappings.push_back(m);
    std::string err;
    REQUIRE(r.eng.applyProfile(0xF00D, p, err));

    r.vc.pressButton(50);
    bool saw = false;
    for (auto& [id, v] : r.cmds)
        if (id == ui::cmd::forDeck(ui::cmd::DeckPlayToggle, 0))
            saw = true;
    REQUIRE(saw);

    // unknown control id → applyProfile refuses
    ControllerProfile bad;
    bad.meta.manufacturer = "T"; bad.meta.model = "Reg2";
    bad.mappings.push_back(m);
    bad.mappings[0].target.control = "deckz.nothing";
    REQUIRE(!r.eng.applyProfile(0xF00D, bad, err));
    REQUIRE(err.find("unknown control") != std::string::npos);
}

// ------------------------------------------------------------------
TEST_CASE("Layers: shift-layer binding overrides default on same input",
          "[ctrl27]") {
    Rig r;
    auto a = Rig::note(36, 100);        // default-layer action
    auto b = Rig::note(36, 200);        // shift-layer action
    b.layer = (uint8_t)MapLayer::Shift;
    r.st()->mapping.addMapping(a);
    r.st()->mapping.addMapping(b);

    r.vc.pressButton(36);               // default layer → cmd 100
    REQUIRE(r.cmdStates[100] == 1);
    REQUIRE(r.cmdStates[200] == 0);

    r.eng.setLayer(0xF00D, (uint8_t)MapLayer::Shift);
    r.vc.pressButton(36);               // shift → cmd 200, 100 stays
    REQUIRE(r.cmdStates[200] == 1);
    REQUIRE(r.cmdStates[100] == 1);     // unchanged since last press

    // shadow check: default binding must NOT re-fire while shifted
    const size_t before = r.cmds.size();
    r.vc.pressButton(36);
    int saw100 = 0, saw200 = 0;
    for (size_t i = before; i < r.cmds.size(); ++i) {
        if (r.cmds[i].first == 100) ++saw100;
        if (r.cmds[i].first == 200) ++saw200;
    }
    REQUIRE(saw100 == 0);
    REQUIRE(saw200 == 1);

    r.eng.setLayer(0xF00D, 0);
    REQUIRE(r.eng.layer(0xF00D) == 0);
}

// ------------------------------------------------------------------
TEST_CASE("Long-press: longMs emits longCmd once, release ends it",
          "[ctrl27]") {
    Rig r;
    auto m = Rig::note(37, 300, MappingMode::Momentary);
    m.longMs = 200; m.longCmd = 400;
    r.st()->mapping.addMapping(m);

    r.vc.holdButton(37);                // press at t≈0
    r.eng.tick(0.05);                   // too early — nothing
    bool longFired = false;
    for (auto& [id, v] : r.cmds) if (id == 400) longFired = true;
    REQUIRE(!longFired);

    r.eng.tick(0.25);                   // past threshold → longCmd on
    longFired = false;
    for (auto& [id, v] : r.cmds)
        if (id == 400 && v > 0.5f) longFired = true;
    REQUIRE(longFired);

    // held longer — poll does not re-fire
    const size_t n = r.cmds.size();
    r.eng.tick(0.5);
    for (size_t i = n; i < r.cmds.size(); ++i)
        REQUIRE(r.cmds[i].first != 400);

    r.vc.releaseButton(37);             // release → longCmd off too
    bool longReleased = false;
    for (auto& [id, v] : r.cmds)
        if (id == 400 && v <= 0.5f) longReleased = true;
    REQUIRE(longReleased);
}

// ------------------------------------------------------------------
TEST_CASE("Priority: same-input dispatch is priority-ordered",
          "[ctrl27]") {
    Rig r;
    auto lo = Rig::cc(20, 0x10);
    lo.priority = 0; lo.outMin = 0; lo.outMax = 10;
    auto hi = Rig::cc(20, 0x11);
    hi.priority = 9; hi.outMin = 0; hi.outMax = 10;
    r.st()->mapping.addMapping(lo);
    r.st()->mapping.addMapping(hi);

    r.params.clear();
    r.vc.turnKnob(20, 127);
    REQUIRE(r.params.size() == 2);
    // higher priority dispatches first (deterministic order, §53)
    REQUIRE(r.params[0].first == 0x11);
    REQUIRE(r.params[1].first == 0x10);
}

// ------------------------------------------------------------------
TEST_CASE("findIssues: duplicates, conflicts, invalid combos, unverified",
          "[ctrl27]") {
    std::vector<Mapping> maps;
    auto dup1 = Rig::note(40, 1); maps.push_back(dup1);
    auto dup2 = Rig::note(40, 2); maps.push_back(dup2);   // same layer
    auto otherLayer = Rig::note(41, 3);
    otherLayer.layer = 1; maps.push_back(otherLayer);
    auto third = Rig::note(41, 4); maps.push_back(third); // layer 0 ≠ 1

    auto badHi = Rig::cc(0x70, 0x10); badHi.hiRes = true;
    maps.push_back(badHi);                                 // LSB wraps
    auto encOnNote = Rig::note(50, 5);
    encOnNote.mode = MappingMode::Relative; maps.push_back(encOnNote);
    auto longNoCmd = Rig::note(51, 6, MappingMode::Momentary);
    longNoCmd.longMs = 300; maps.push_back(longNoCmd);
    auto unv = Rig::note(52, 7); unv.verified = false;
    maps.push_back(unv);

    const auto issues = MappingEngine::findIssues(maps);
    const std::string all = [&] {
        std::string s; for (auto& i : issues) s += i + "\n"; return s;
    }();
    REQUIRE(all.find("conflict") != std::string::npos);
    REQUIRE(all.find("hiRes pair exceeds") != std::string::npos);
    REQUIRE(all.find("encoder modes expect") != std::string::npos);
    REQUIRE(all.find("longMs set but longCmd") != std::string::npos);
    REQUIRE(all.find("UNVERIFIED") != std::string::npos);
    // note 41: one binding on layer 1, one on default — legal, no
    // conflict reported for that input
    REQUIRE(all.find("#41") == std::string::npos);
}

// ------------------------------------------------------------------
TEST_CASE("SysEx: routed to hook + monitor, never to mappings",
          "[ctrl27]") {
    Rig r;
    SysExMsg sx;
    sx.len = 6;
    const uint8_t bytes[6] = { 0xF0, 0x00, 0x40, 0x05, 0x02, 0xF7 };
    std::memcpy(sx.data, bytes, 6);

    int seen = 0;
    r.eng.onSysex = [&](uint32_t dev, const SysExMsg& m) {
        ++seen;
        REQUIRE(dev == 0xF00D);
        REQUIRE(m.len == 6);
        REQUIRE(m.data[5] == 0xF7);
        return true;
    };
    r.eng.processSysex(0xF00D, sx);
    REQUIRE(seen == 1);
    REQUIRE(r.eng.totalIn() >= 1);

    // monitor ring holds a sysex entry with hex dump
    const auto& ring = r.eng.diag().ring();
    REQUIRE(!ring.empty());
    REQUIRE(ring.back().sysex);
    REQUIRE(ring.back().mapped);        // consumed by the hook
    const std::string line =
        ControllerDiagnostics::describe(ring.back());
    REQUIRE(line.find("SysEx") != std::string::npos);
    REQUIRE(line.find("F0") != std::string::npos);
    REQUIRE(r.eng.diag().dump().find("SysEx") != std::string::npos);

    // detached device → dropped silently, no crash
    r.vc.detach(2.0);
    r.eng.processSysex(0xF00D, sx);
}

// ------------------------------------------------------------------
TEST_CASE("FeedbackEngine: blink patterns alternate at rate",
          "[ctrl27]") {
    FeedbackEngine fb;
    std::vector<std::pair<uint32_t, uint8_t>> sent;
    int state = 1;
    fb.setPorts({
        [&sent](const MidiAddr& a, uint8_t v) {
            sent.emplace_back(a.pack(), v);
        },
        [&state](uint32_t) { return state; },
        [](uint16_t) { return 0.0f; }});
    FeedbackEngine::Watch w;
    w.out = { AddrKind::Note, 0, 60 };
    w.cmd = 1;
    w.onValue = 0x7F; w.offValue = 0x00;
    w.pattern = FeedbackEngine::LedPattern::FastBlink; // 4 Hz
    w.minIntervalSec = 0.001;
    fb.setWatches({ w });

    // phase <0.5 → on, ≥0.5 → off at 4 Hz (period 250 ms)
    fb.tick(0.010);  REQUIRE(!sent.empty());
    REQUIRE(sent.back().second == 0x7F);
    fb.tick(0.130);  // mid second half → off
    REQUIRE(sent.back().second == 0x00);
    fb.tick(0.260);  // new period, first half → on again
    REQUIRE(sent.back().second == 0x7F);

    // source off → solid offValue regardless of phase
    state = 0;
    fb.tick(0.300);
    REQUIRE(sent.back().second == 0x00);

    // solid pattern (Off) never toggles while on
    state = 1;
    w.pattern = FeedbackEngine::LedPattern::Off;
    fb.setWatches({ w });
    fb.tick(0.400, /*force*/ true);
    REQUIRE(sent.back().second == 0x7F);
    fb.tick(0.530);
    REQUIRE(sent.back().second == 0x7F);   // still on — no blink
}

// ------------------------------------------------------------------
TEST_CASE("JogWheelEngine: modes, sensitivity, touch gate, browse",
          "[ctrl27]") {
    JogWheelEngine jw;
    struct Out { int deck, mode; double delta; };
    std::vector<Out> outs; int browsed = 0;
    jw.setPorts({
        [&outs](int d, int m, double delta) {
            outs.push_back({ d, m, delta }); },
        [&browsed](int dir) { browsed += dir; }});

    // Vinyl + touch → scratch; free platter → decoder's request
    jw.cfg(0).mode = JogWheelEngine::HwMode::Vinyl;
    jw.turn(0, 5.0, JogMode::JogSeek, true);
    REQUIRE(outs.back().mode == (int)JogMode::JogSeek);
    jw.touch(0, true);
    jw.turn(0, 5.0, JogMode::JogSeek, true);
    REQUIRE(outs.back().mode == (int)JogMode::JogScratch);

    // side-edge (not platter) keeps the requested mode while touched
    jw.turn(0, 2.0, JogMode::JogPitchBend, false);
    REQUIRE(outs.back().mode == (int)JogMode::JogPitchBend);
    jw.touch(0, false);

    // Cdj: platter always seeks — no scratch ever
    jw.cfg(0).mode = JogWheelEngine::HwMode::Cdj;
    jw.touch(0, true);
    jw.turn(0, 5.0, JogMode::JogScratch, true);
    REQUIRE(outs.back().mode == (int)JogMode::JogSeek);
    jw.touch(0, false);

    // Search: amplified seek
    jw.cfg(0).mode = JogWheelEngine::HwMode::Search;
    jw.cfg(0).searchBoost = 4.0f;
    jw.turn(0, 2.0, JogMode::JogSeek, true);
    REQUIRE(outs.back().delta == Catch::Approx(8.0));

    // Sensitivity scales everything
    jw.cfg(0).sensitivity = 0.5f;
    jw.turn(0, 2.0, JogMode::JogSeek, true);
    REQUIRE(outs.back().delta == Catch::Approx(4.0));

    // Browse: deck never receives jog; library steps accumulate
    jw.cfg(0).sensitivity = 1.0f;
    jw.cfg(0).mode = JogWheelEngine::HwMode::Browse;
    const size_t before = outs.size();
    jw.turn(0, 4.5, JogMode::JogSeek, true);
    REQUIRE(outs.size() == before);       // deck untouched (§25)
    REQUIRE(browsed == 1);
    jw.turn(0, -4.5, JogMode::JogSeek, true);
    REQUIRE(browsed == 0);

    // no touch sensor → vinyl behaves like free-spin
    jw.cfg(1).mode = JogWheelEngine::HwMode::Vinyl;
    jw.cfg(1).touchCapable = false;
    jw.touch(1, true);                    // ignored
    REQUIRE(!jw.touching(1));
    jw.turn(1, 1.0, JogMode::JogSeek, true);
    REQUIRE(outs.back().mode == (int)JogMode::JogSeek);
}

// ------------------------------------------------------------------
TEST_CASE("PadEngine: normalized events, state, colors, range-safe",
          "[ctrl27]") {
    PadEngine pe;
    std::vector<std::pair<uint32_t, float>> sent;
    pe.setPorts({ [&sent](uint32_t c, float v) {
        sent.emplace_back(c, v); }});

    REQUIRE(pe.pad(0, 2, false, 0.8f));
    REQUIRE(sent.back().first == ui::cmd::forPad(0, 2, false));
    REQUIRE(sent.back().second == Catch::Approx(0.8f));
    REQUIRE(pe.padState(0, 2).pressed);
    REQUIRE(pe.padState(0, 2).velocity == Catch::Approx(0.8f));
    REQUIRE(pe.anyPressed(0));

    pe.pad(0, 2, false, 0.0f);            // release
    REQUIRE(!pe.padState(0, 2).pressed);
    REQUIRE(!pe.anyPressed(0));

    pe.tap(1, 5, true, 1.0f);
    REQUIRE(sent[sent.size() - 2].first ==
            ui::cmd::forPad(1, 5, true));

    // LED-facing model
    pe.setPadColor(0, 0, 0x45, true);
    REQUIRE(pe.padState(0, 0).color == 0x45);
    REQUIRE(pe.padState(0, 0).active);

    // range safety — never crashes, never emits
    REQUIRE(!pe.pad(9, 0, false, 1.0f));
    REQUIRE(!pe.pad(0, 9, false, 1.0f));
    REQUIRE(pe.padState(9, 9).color == 0);
}

// ------------------------------------------------------------------
TEST_CASE("ProfileManager: factory/user split, dup, rename, shadow",
          "[ctrl27]") {
    const fs::path dir = tempDir("pm") / "profiles";
    ProfileManager pm(dir.string());
    std::string err;

    ControllerProfile p;
    p.meta.manufacturer = "Acme"; p.meta.model = "One";
    p.mappings.push_back(Rig::note(36, 1));

    // factory install → memory only, no file on disk
    const std::string fid = pm.installFactory(p, err);
    REQUIRE(!fid.empty());
    REQUIRE(pm.originOf(fid) == "factory");
    REQUIRE(!fs::exists(dir / (fid + ".json")));
    // factory cannot be deleted or renamed (§48)
    REQUIRE(!pm.deleteProfile(fid, err));
    REQUIRE(!pm.renameProfile(fid, "Two", err));

    // duplicate factory → user copy on disk, original untouched
    const std::string uid =
        pm.duplicateProfile(fid, "One (custom)", err);
    REQUIRE(!uid.empty());
    REQUIRE(pm.originOf(uid) == "user");
    REQUIRE(fs::exists(dir / (uid + ".json")));

    // user profile shadows factory on autoMatch (same nameContains)
    ControllerProfile u;
    u.meta.manufacturer = "Acme"; u.meta.model = "One User";
    u.meta.nameContains = "acme";
    const std::string uuid = pm.installProfile(u, err);
    REQUIRE(!uuid.empty());
    ControllerProfile f2;
    f2.meta.manufacturer = "Acme"; f2.meta.model = "One";
    f2.meta.nameContains = "acme";
    REQUIRE(!pm.installFactory(f2, err).empty());   // same match hint
    REQUIRE(pm.autoMatch("ACME CONTROLLER", err) == uuid);

    // rename works for user profiles — file + id move together
    REQUIRE(pm.renameProfile(uid, "Renamed", err));
    ControllerProfile rq;
    rq.meta.manufacturer = "Acme"; rq.meta.model = "Renamed";
    const std::string rid = ProfileManager::idFor(rq);
    REQUIRE(pm.originOf(rid) == "user");
    REQUIRE(fs::exists(dir / (rid + ".json")));
    REQUIRE(!fs::exists(dir / (uid + ".json")));

    // resetToFactory removes the user copy
    pm.resetToFactory(rid);
    REQUIRE(!fs::exists(dir / (rid + ".json")));
}

// ------------------------------------------------------------------
TEST_CASE("Profile JSON: phase-27 fields roundtrip + unverified count",
          "[ctrl27]") {
    ControllerProfile p;
    p.meta.manufacturer = "T"; p.meta.model = "JSON27";
    p.meta.protocolRev = "rev-2";
    p.meta.origin = "user";
    Mapping m;
    m.in = { AddrKind::Note, 3, 60 };
    m.target.kind = Target::Kind::Command;
    m.target.cmd = 42; m.mode = MappingMode::Momentary;
    m.layer = 1; m.priority = 5;
    m.longMs = 300; m.longCmd = 77;
    m.verified = false;
    m.target.control = "decka.play";
    p.mappings.push_back(m);
    p.mappings.push_back(Rig::note(61, 7)); // verified default

    REQUIRE(p.unverifiedCount() == 1);
    const std::string j = p.toJson();
    REQUIRE(j.find("layer") != std::string::npos);
    REQUIRE(j.find("priority") != std::string::npos);
    REQUIRE(j.find("longMs") != std::string::npos);
    REQUIRE(j.find("verified") != std::string::npos);
    REQUIRE(j.find("decka.play") != std::string::npos);
    REQUIRE(j.find("rev-2") != std::string::npos);

    ControllerProfile q;
    std::string err;
    REQUIRE(q.fromJson(j, err));
    REQUIRE(q.mappings.size() == 2);
    REQUIRE(q.mappings[0].layer == 1);
    REQUIRE(q.mappings[0].priority == 5);
    REQUIRE(q.mappings[0].longMs == 300);
    REQUIRE(q.mappings[0].longCmd == 77);
    REQUIRE(!q.mappings[0].verified);
    REQUIRE(q.mappings[0].target.control == "decka.play");
    REQUIRE(q.mappings[1].verified);
    REQUIRE(q.meta.protocolRev == "rev-2");
}

// ------------------------------------------------------------------
TEST_CASE("FLX4 profile: UNVERIFIED marks on community-only ids",
          "[ctrl27]") {
    const ControllerProfile p = flx4::buildProfile();
    REQUIRE(p.unverifiedCount() > 0);
    REQUIRE(p.meta.origin == "factory");
    REQUIRE(!p.meta.protocolRev.empty());

    // N_PLAY [OFF] verified; N_PFL [MIX] unverified
    bool playVerified = false, pflUnverified = false;
    for (const auto& m : p.mappings) {
        if (m.in.kind == AddrKind::Note && m.in.number == flx4::N_PLAY &&
            m.in.channel == 0)
            playVerified = m.verified;
        if (m.in.kind == AddrKind::Note && m.in.number == flx4::N_PFL &&
            m.in.channel == 0)
            pflUnverified = !m.verified;
    }
    REQUIRE(playVerified);
    REQUIRE(pflUnverified);

    // findIssues reports the unverified entries (§9) — profile stays
    // valid, the flag is informational
    const auto issues = p.findIssues();
    bool hasUnv = false;
    for (auto& i : issues)
        if (i.find("UNVERIFIED") != std::string::npos) hasUnv = true;
    REQUIRE(hasUnv);
    REQUIRE(p.validate().empty());
}

// ------------------------------------------------------------------
TEST_CASE("VirtualFlx4: lifecycle READY + pads/jog through engines",
          "[ctrl27]") {
    ControllerEngine eng { (tempDir("flx") / "profiles").string() };
    flx4::Flx4Plugin  plug;
    flx4::VirtualFlx4 vc { eng };
    ui::CommandManager cm;
    std::unordered_map<uint32_t, int> cmdStates;
    std::vector<std::array<double,3>>  jogs;

    // minimal command surface — every cmdName the FLX4 profile uses
    // (applyProfile resolves names through CommandManager::idFor)
    for (int d = 0; d < 2; ++d) {
        const std::string dn = std::string("deck") + char('a' + d);
        auto add = [&](uint32_t id, const std::string& tail) {
            ui::Command c; c.id = id;
            c.name = dn + "." + tail; c.category = "Deck";
            c.run = [](float, ui::CommandSource) {};
            cm.add(std::move(c));
        };
        add(ui::cmd::forDeck(ui::cmd::DeckPlayToggle, d),  "playToggle");
        add(ui::cmd::forDeck(ui::cmd::DeckReverseTgl, d),  "reverseToggle");
        add(ui::cmd::forDeck(ui::cmd::DeckCueTrigger, d),  "cueTrigger");
        add(ui::cmd::forDeck(ui::cmd::DeckStop, d),        "stop");
        add(ui::cmd::forDeck(ui::cmd::DeckSyncToggle, d),  "syncToggle");
        add(ui::cmd::forDeck(ui::cmd::DeckLoopIn, d),      "loopIn");
        add(ui::cmd::forDeck(ui::cmd::DeckLoopOut, d),     "loopOut");
        add(ui::cmd::forDeck(ui::cmd::DeckLoopToggle, d),  "loopToggle");
        add(ui::cmd::forDeck(ui::cmd::DeckLoopHalve, d),   "loopHalve");
        add(ui::cmd::forDeck(ui::cmd::DeckLoopDouble, d),  "loopDouble");
        add(ui::cmd::forDeck(ui::cmd::DeckPflToggle, d),   "pflToggle");
        add(ui::cmd::forDeck(ui::cmd::DeckQuantizeTgl, d), "quantizeToggle");
        add(ui::cmd::forDeck(ui::cmd::DeckJumpFwd, d),     "jumpFwd");
        add(ui::cmd::forDeck(ui::cmd::DeckJumpBack, d),    "jumpBack");
        add(ui::cmd::forDeck(ui::cmd::DeckLoad, d),        "load");
    }

    ControllerEngine::Ports p;
    p.sendCommand = [&](uint32_t id, float v) {
        cmdStates[id] = v > 0.5f ? 1 : 0; };
    p.commandState = [&](uint32_t id) { return cmdStates[id]; };
    p.commandIdFor = [&](const std::string& n) { return cm.idFor(n); };
    p.setParam  = [](uint16_t, float) {};
    p.paramState= [](uint16_t) { return 0.0f; };
    p.jog = [&](int d, int m, double delta) {
        jogs.push_back({ (double)d, (double)m, delta }); };
    p.notify = [](const std::string&, const std::string&, int) {};
    eng.setPorts(std::move(p));
    plug.attach(eng);

    vc.attach(1.0);
    const auto s = eng.state(flx4::VirtualFlx4::kId);
    REQUIRE(s.lifecycle == DeviceLifecycle::Ready);   // profile applied
    REQUIRE(s.connected);
    REQUIRE(s.profileId.find("FLX4") != std::string::npos);
    REQUIRE(s.mappingCount > 20);

    // pad → PadEngine state (pressed) + forPad command
    vc.pad(0, flx4::kPadBankHotCue, 3, false, 5000);
    // hold one to observe the pressed state
    eng.processInput(flx4::VirtualFlx4::kId,
                     { MsgType::NoteOn, (uint8_t)flx4::kPadCh(0),
                       (uint8_t)(flx4::kPadBankHotCue + 2), 0x70, 0 });
    REQUIRE(eng.pads().padState(0, 2).pressed);
    REQUIRE(eng.pads().padState(0, 2).velocity == Catch::Approx(0x70/127.0f)
                .margin(0.01));
    eng.processInput(flx4::VirtualFlx4::kId,
                     { MsgType::NoteOff, (uint8_t)flx4::kPadCh(0),
                       (uint8_t)(flx4::kPadBankHotCue + 2), 0, 0 });
    REQUIRE(!eng.pads().padState(0, 2).pressed);

    // jog touch feeds the generic JogWheelEngine
    vc.jogTouch(0, true);
    REQUIRE(eng.jogWheel().touching(0));
    vc.jogPlatter(0, 4);
    REQUIRE(!jogs.empty());
    REQUIRE((int)jogs.back()[1] == (int)JogMode::JogScratch);
    vc.jogTouch(0, false);
    vc.jogPlatter(0, 4);
    REQUIRE((int)jogs.back()[1] == (int)JogMode::JogSeek);
}

// ------------------------------------------------------------------
TEST_CASE("Monitor: decoded control column + bounded raw dump",
          "[ctrl27]") {
    Rig r;
    auto m = Rig::note(44, 1);
    m.target.cmdName = "decka.playToggle";
    r.st()->mapping.addMapping(m);
    r.vc.pressButton(44);

    const auto& ring = r.eng.diag().ring();
    bool found = false;
    for (auto& e : ring)
        if (e.in && e.mapped &&
            e.decoded.find("playToggle") != std::string::npos)
            found = true;
    REQUIRE(found);

    // dump is bounded to the ring size and line-per-entry
    const std::string d = r.eng.diag().dump();
    REQUIRE(!d.empty());
    REQUIRE(d.size() < 256 * 200);   // ≤ ring × line — bounded
}

// ------------------------------------------------------------------
TEST_CASE("Multi-controller: independent profiles, layers, state",
          "[ctrl27]") {
    Rig r;
    VirtualMidiController vc2(r.eng, "Second Deck");
    vc2.info().id = 0x2222;
    vc2.attach(1.0);
    REQUIRE(r.eng.devices().count() == 2);

    // per-device layer state
    r.eng.setLayer(0xF00D, 1);
    r.eng.setLayer(0x2222, 3);
    REQUIRE(r.eng.layer(0xF00D) == 1);
    REQUIRE(r.eng.layer(0x2222) == 3);

    // independent state snapshots
    REQUIRE(r.eng.state(0xF00D).name != r.eng.state(0x2222).name);
    r.eng.detachDevice(0x2222, 2.0);
    REQUIRE(!r.eng.state(0x2222).connected);
    REQUIRE(r.eng.state(0xF00D).connected);
}

// ------------------------------------------------------------------
TEST_CASE("Flood: 40k msgs across 2 devices stay O(1), nothing blocks",
          "[ctrl27][stress]") {
    Rig r;
    VirtualMidiController vc2(r.eng, "Flood2");
    vc2.info().id = 0x3333;
    vc2.attach(1.0);
    r.st()->mapping.addMapping(Rig::note(60, 1));
    r.eng.device(0x3333)->mapping.addMapping(Rig::cc(16, 0x20));

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 20000; ++i) {
        vc2.turnKnob(16, i & 0x7F, i);
        r.vc.pressButton(60, i);
    }
    const double ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
    WARN("40k mixed msgs in " << ms << " ms");
    REQUIRE(r.eng.totalIn() >= 40000);
    REQUIRE(r.eng.totalActed() >= 20000); // knob CCs + note presses
    REQUIRE(r.eng.diag().ring().size() <= ControllerDiagnostics::kRing);
}

// ------------------------------------------------------------------
TEST_CASE("ControllerState snapshot: full field coverage (§46)",
          "[ctrl27]") {
    Rig r;
    ControllerProfile p;
    p.meta.manufacturer = "T"; p.meta.model = "Snap";
    p.mappings.push_back(Rig::note(40, 1));
    std::string err;
    REQUIRE(r.eng.applyProfile(0xF00D, p, err));

    r.vc.pressButton(40);
    const auto s = r.eng.state(0xF00D);
    REQUIRE(s.id == 0xF00D);
    REQUIRE(s.name == "Virtual Ctrl 1");
    REQUIRE(s.manufacturer == "OndaTests");
    REQUIRE(s.model == "Virtual-1");
    REQUIRE(s.transport == Transport::Virtual);
    REQUIRE(s.connected);
    REQUIRE(s.initialized);
    REQUIRE(s.profileId == "T-Snap");
    REQUIRE(s.mappingCount == 1);
    REQUIRE(s.inCount > 0);
    REQUIRE(s.actedCount > 0);
}

} // namespace
