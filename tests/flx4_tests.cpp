// Phase 10 — DDJ-FLX4 integration tests via VirtualFlx4.
// Byte values come ONLY from documented public sources (Pioneer
// official MIDI list + Mixxx mapping + slipmate hw measurements);
// no physical hardware was used — see docs/controllers/ddj-flx4.md.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include "controller/ControllerEngine.h"
#include "controller/ParamNames.h"
#include "controller/flx4/Flx4Ids.h"
#include "controller/flx4/Flx4Inventory.h"
#include "controller/flx4/Flx4Profile.h"
#include "controller/flx4/Flx4Plugin.h"
#include "controller/flx4/Flx4Audio.h"
#include "controller/flx4/ConnectionFlow.h"
#include "controller/flx4/VirtualFlx4.h"
#include "core/rt/CommandQueue.h"
#include "ui/CommandIds.h"
#include "ui/CommandManager.h"
#include "ui/pads/PadLayer.h"

using namespace onda;
using namespace onda::ctrl;
namespace fs = std::filesystem;

namespace {

fs::path tempDir2(const char* n) {
    fs::path d = fs::temp_directory_path() / "onda_flx4test" / n;
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

/// Rig: engine + virtual FLX4 + spy ports (commands, params, jogs).
struct Rig {
    ControllerEngine eng { (tempDir2("rig") / "profiles").string() };
    flx4::Flx4Plugin   plug;
    flx4::VirtualFlx4  vc { eng };
    ui::CommandManager cm;
    ui::pads::PadLayer pads;
    std::unordered_map<uint32_t, int>   cmdStates;
    std::unordered_map<uint16_t, float> parVals;
    std::vector<std::array<double,3>>   jogs;
    std::vector<std::string>            notes;
    std::vector<uint32_t>               cmds;      // wire-level sends
    std::vector<uint32_t>               resolved;  // every invoked id
    std::vector<std::array<float,2>>    trigs;     // (slot, vel)
    std::vector<int>                    releases, stops;

    Rig() {
        // minimal command surface matching EngineCommands naming
        for (int d = 0; d < 2; ++d) {
            const std::string dn = std::string("deck") + char('a' + d);
            auto add = [&](uint32_t id, const std::string& tail) {
                ui::Command c; c.id = id;
                c.name = dn + "." + tail; c.category = "Deck";
                c.run  = [](float, ui::CommandSource) {};
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
            for (int s = 0; s < 8; ++s)
                for (int a = 0; a < 3; ++a)
                    add(ui::cmd::forHotCue(d, s, a),
                        "hotcue" + std::to_string(s) +
                        (a == 0 ? ".trig" : a == 1 ? ".set" : ".del"));
        }
        {   ui::Command c; c.id = ui::cmd::BrowserNextTrack;
            c.name = "browser.nextTrack"; c.category = "Browser";
            c.run = [](float, ui::CommandSource) {}; cm.add(std::move(c)); }
        {   ui::Command c; c.id = ui::cmd::BrowserPrevTrack;
            c.name = "browser.prevTrack"; c.category = "Browser";
            c.run = [](float, ui::CommandSource) {}; cm.add(std::move(c)); }

        // domain pad layer — resolves pad.press/pad.mode commands the
        // plugin emits into deck commands or sampler actions
        pads.setPorts({
            [this](int slot, float vel) { trigs.push_back({(float)slot, vel}); },
            [this](int slot)            { releases.push_back(slot); },
            [this](int slot)            { stops.push_back(slot); },
        });
        pads.registerCommands(cm);
        cm.addListener([this](uint32_t id, ui::CommandSource) {
            resolved.push_back(id);
        });

        ControllerEngine::Ports p;
        p.sendCommand = [this](uint32_t id, float v) {
            cm.invoke(id, ui::CommandSource::Midi, v);
            cmds.push_back(id);
            cmdStates[id] = v > 0.5f ? 1 : 0;
        };
        p.commandState = [this](uint32_t id) { return cmdStates[id]; };
        p.commandIdFor = [this](const std::string& n) {
            return cm.idFor(n);
        };
        p.setParam = [this](uint16_t id, float v) { parVals[id] = v; };
        p.paramState = [this](uint16_t id) { return parVals[id]; };
        p.jog = [this](int deck, int mode, double d) {
            jogs.push_back({ (double)deck, (double)mode, d });
        };
        p.notify = [this](const std::string& t,
                          const std::string& d, int lvl) {
            notes.push_back(std::to_string(lvl) + ":" + t + ":" + d);
        };
        eng.setPorts(std::move(p));
        plug.attach(eng);
    }
};

} // namespace

// ------------------------------------------------------------------
TEST_CASE("FLX4: profile builds, validates, JSON roundtrips",
          "[flx4]") {
    ControllerProfile p = flx4::buildProfile();
    std::string err;
    REQUIRE(p.validate().empty());
    REQUIRE(p.meta.nameContains == "DDJ-FLX4");
    REQUIRE((p.capabilities & CAP_JOG_WHEEL) != 0);
    REQUIRE((p.capabilities & CAP_PADS) != 0);
    REQUIRE(p.mappings.size() > 30);

    const std::string j = p.toJson();
    REQUIRE(j.find("\"profileVersion\"") != std::string::npos);
    ControllerProfile q;
    REQUIRE(q.fromJson(j, err));
    REQUIRE(q.mappings.size() == p.mappings.size());
    REQUIRE(q.mappings[0].target.cmdName == "decka.playToggle");
    // hires flag survives the roundtrip
    bool anyHiRes = false;
    for (auto& m : q.mappings) anyHiRes |= m.hiRes;
    REQUIRE(anyHiRes);
}

TEST_CASE("FLX4: name match + autoMatch + install", "[flx4]") {
    REQUIRE(flx4::Flx4Plugin::nameMatches("DDJ-FLX4"));
    REQUIRE(flx4::Flx4Plugin::nameMatches("DDJ-FLX4 WASAPI"));
    REQUIRE(flx4::Flx4Plugin::nameMatches("pioneer ddj-flx4 midi"));
    REQUIRE_FALSE(flx4::Flx4Plugin::nameMatches("DDJ-400"));

    ProfileManager pm((tempDir2("pm") / "profiles").string());
    std::string err;
    const std::string id = flx4::installProfile(pm, err);
    REQUIRE_FALSE(id.empty());
    REQUIRE(pm.autoMatch("DDJ-FLX4", err) == id);
    REQUIRE(pm.autoMatch("DDJ-FLX4 WASAPI", err) == id);
}

TEST_CASE("FLX4: transport buttons → commands on connect", "[flx4]") {
    Rig r;
    r.vc.attach();
    REQUIRE(r.plug.bound());
    const uint32_t dev = flx4::VirtualFlx4::kId;
    auto* st = r.eng.device(dev);
    REQUIRE(st != nullptr);
    REQUIRE_FALSE(st->profileId.empty());

    r.vc.pressPlay(0);
    REQUIRE(std::find(r.cmds.begin(), r.cmds.end(),
        ui::cmd::forDeck(ui::cmd::DeckPlayToggle, 0)) != r.cmds.end());
    r.vc.pressCue(0);
    REQUIRE(std::find(r.cmds.begin(), r.cmds.end(),
        ui::cmd::forDeck(ui::cmd::DeckCueTrigger, 0)) != r.cmds.end());
    r.vc.pressSync(1);
    REQUIRE(std::find(r.cmds.begin(), r.cmds.end(),
        ui::cmd::forDeck(ui::cmd::DeckSyncToggle, 1)) != r.cmds.end());
    r.vc.loadDeckB();
    REQUIRE(std::find(r.cmds.begin(), r.cmds.end(),
        ui::cmd::forDeck(ui::cmd::DeckLoad, 1)) != r.cmds.end());
}

TEST_CASE("FLX4: 14-bit merge on tempo/EQ/fader/crossfader", "[flx4]") {
    Rig r;
    r.vc.attach();
    const uint16_t t = deckParamId(0, kDeckParamPitch);
    r.vc.tempo(0, 16383);            // fader bottom = +25 pct
    REQUIRE(r.parVals[t] == Catch::Approx(25.0f).margin(0.2));
    r.vc.tempo(0, 0);
    REQUIRE(r.parVals[t] == Catch::Approx(-25.0f).margin(0.2));

    const uint16_t hi = mixer::ParameterRegistry::channelHigh(1);
    r.vc.eqHi(1, 16383);
    REQUIRE(r.parVals[hi] == Catch::Approx(6.0f).margin(0.01));
    r.vc.eqHi(1, 8192);
    REQUIRE(r.parVals[hi] ==
            Catch::Approx(-26.f + 32.f * 8192.f / 16383.f).margin(0.2));

    const uint16_t f = mixer::ParameterRegistry::channelFader(0);
    r.vc.chFader(0, 16383);
    REQUIRE(r.parVals[f] == Catch::Approx(1.0f).margin(0.001));

    const uint16_t xf = mixer::ParameterRegistry::CROSSFADER;
    r.vc.crossfader(16383);
    REQUIRE(r.parVals[xf] == Catch::Approx(1.0f).margin(0.001));
    r.vc.crossfader(0);
    REQUIRE(r.parVals[xf] == Catch::Approx(-1.0f).margin(0.001));
}

TEST_CASE("FLX4: pad modes — hotcue trig/del, beatjump, loop, sampler",
          "[flx4]") {
    // Phase 11: pads emit pad.press commands; the domain PadLayer
    // resolves them against the current mode (single semantic truth).
    Rig r;
    r.vc.attach();
    auto hit = [&](uint32_t id) {
        return std::find(r.resolved.begin(), r.resolved.end(), id)
               != r.resolved.end();
    };
    r.vc.padMode(0, flx4::PadMode::HotCue);
    r.vc.pad(0, flx4::kPadBankHotCue, 2, false);    // pad3 → hotcue2 trig
    REQUIRE(hit(ui::cmd::forHotCue(0, 2, 0)));
    r.vc.pad(0, flx4::kPadBankHotCue, 2, true);     // shift → delete
    REQUIRE(hit(ui::cmd::forHotCue(0, 2, 2)));

    r.vc.padMode(1, flx4::PadMode::BeatJump);
    r.vc.pad(1, flx4::kPadBankBeatJump, 1, false);  // pad2 → back
    REQUIRE(hit(ui::cmd::forDeck(ui::cmd::DeckJumpBack, 1)));
    r.vc.pad(1, flx4::kPadBankBeatJump, 6, false);  // pad7 → fwd
    REQUIRE(hit(ui::cmd::forDeck(ui::cmd::DeckJumpFwd, 1)));

    r.vc.padMode(1, flx4::PadMode::BeatLoop);
    r.vc.pad(1, flx4::kPadBankBeatLoop, 0, false);  // pad1 → halve
    REQUIRE(hit(ui::cmd::forDeck(ui::cmd::DeckLoopHalve, 1)));
    r.vc.pad(1, flx4::kPadBankBeatLoop, 5, false);  // pad6 → toggle
    REQUIRE(hit(ui::cmd::forDeck(ui::cmd::DeckLoopToggle, 1)));

    // sampler mode → domain sampler ports (not deck commands)
    r.vc.padMode(0, flx4::PadMode::Sampler);
    r.vc.pad(0, flx4::kPadBankSampler, 4, false);   // press → trig(4)
    REQUIRE(r.trigs.size() == 1);
    REQUIRE((int)r.trigs[0][0] == 4);
    r.vc.pad(0, flx4::kPadBankSampler, 4, true);    // shift → stop(4)
    REQUIRE(r.stops.size() == 1);
    REQUIRE(r.stops[0] == 4);
}

TEST_CASE("FLX4: pad-mode selector LEDs repaint (echo scheme)",
          "[flx4]") {
    Rig r;
    r.vc.attach();
    r.vc.clearOutbox();
    r.vc.padMode(0, flx4::PadMode::Sampler);
    // sampler selector lit, hotcue selector dark
    REQUIRE(r.vc.outValueFor(
        { AddrKind::Note, 0, flx4::N_PM_SAMPLER }) == flx4::kLedOn);
    REQUIRE(r.vc.outValueFor(
        { AddrKind::Note, 0, flx4::N_PM_HOTCUE }) == flx4::kLedOff);
}

TEST_CASE("FLX4: shift layer + tempo range cycle + browse dir",
          "[flx4]") {
    Rig r;
    r.vc.attach();
    r.vc.shiftDown(0);
    r.vc.shiftUp(0);       // held state tracked (no command emitted)

    const uint16_t rng = deckParamId(0, kDeckParamPitchRange);
    r.parVals[rng] = -1.0f;
    r.vc.note(flx4::kDeckCh(0), flx4::N_TEMPO_RANGE, 0x7F);
    REQUIRE(r.parVals[rng] == Catch::Approx(10.0f).margin(0.01));
    r.vc.note(flx4::kDeckCh(0), flx4::N_TEMPO_RANGE, 0x7F);
    REQUIRE(r.parVals[rng] == Catch::Approx(16.0f).margin(0.01));
    r.vc.note(flx4::kDeckCh(0), flx4::N_TEMPO_RANGE, 0x7F);
    REQUIRE(r.parVals[rng] == Catch::Approx(25.0f).margin(0.01));
    r.vc.note(flx4::kDeckCh(0), flx4::N_TEMPO_RANGE, 0x7F);
    REQUIRE(r.parVals[rng] == Catch::Approx(6.0f).margin(0.01));

    r.cmds.clear();
    r.vc.browse(+2);
    r.vc.browse(-2);
    REQUIRE(std::find(r.cmds.begin(), r.cmds.end(),
                      ui::cmd::BrowserNextTrack) != r.cmds.end());
    REQUIRE(std::find(r.cmds.begin(), r.cmds.end(),
                      ui::cmd::BrowserPrevTrack) != r.cmds.end());
}

TEST_CASE("FLX4: jog touch + vinyl/cdj/search modes → jog port",
          "[flx4]") {
    Rig r;
    r.vc.attach();
    r.vc.jogPlatter(0, +4);            // vinyl on, no touch → seek
    REQUIRE(r.jogs.size() == 1);
    REQUIRE(r.jogs[0][1] == (double)JogMode::JogSeek);
    r.vc.jogTouch(0, true);
    r.vc.jogPlatter(0, +4);            // touching → scratch
    REQUIRE(r.jogs[1][1] == (double)JogMode::JogScratch);
    r.vc.jogTouch(0, false);
    r.vc.jogSide(0, -2);               // wheel side → pitch bend
    REQUIRE(r.jogs[2][1] == (double)JogMode::JogPitchBend);
    r.vc.jogSearch(1, +1);             // shift jog → seek, 3x gain
    REQUIRE(r.jogs[3][0] == 1.0);
    REQUIRE(r.jogs[3][1] == (double)JogMode::JogSeek);
    REQUIRE(r.jogs[3][2] == Catch::Approx(3.0));
}

TEST_CASE("FLX4: LED feedback — sync toggle lights echo", "[flx4]") {
    Rig r;
    r.vc.attach();
    // engine emits feedback watch output when cmd state changes
    r.cmdStates[ui::cmd::forDeck(ui::cmd::DeckSyncToggle, 0)] = 0;
    r.eng.tick(1.0);
    REQUIRE(r.vc.outValueFor({ AddrKind::Note, 0, flx4::N_SYNC })
            == flx4::kLedOff);
    r.cmdStates[ui::cmd::forDeck(ui::cmd::DeckSyncToggle, 0)] = 1;
    r.eng.tick(2.0);
    REQUIRE(r.vc.outValueFor({ AddrKind::Note, 0, flx4::N_SYNC })
            == flx4::kLedOn);
}

TEST_CASE("FLX4: hot-plug — disconnect safe, reconnect restores",
          "[flx4]") {
    Rig r;
    r.vc.attach();
    r.vc.pressPlay(0);
    r.vc.detach(2.0);
    auto* st = r.eng.device(flx4::VirtualFlx4::kId);
    REQUIRE(st != nullptr);
    REQUIRE_FALSE(st->connected);
    // input while detached is ignored, no crash
    r.vc.pressPlay(0);
    // reconnect → same device id, profile re-applied, LEDs repaint
    r.vc.attach(3.0);
    st = r.eng.device(flx4::VirtualFlx4::kId);   // attach replaces the
                                               // DeviceState — refetch
    REQUIRE(st->connected);
    REQUIRE(r.plug.bound());
    REQUIRE_FALSE(st->profileId.empty());
    r.vc.pressPlay(1);
    REQUIRE(std::find(r.cmds.begin(), r.cmds.end(),
        ui::cmd::forDeck(ui::cmd::DeckPlayToggle, 1)) != r.cmds.end());
}

TEST_CASE("FLX4: audio profile + connection flow diagnostics",
          "[flx4]") {
    flx4::Flx4AudioProfile ap;
    REQUIRE(ap.matches("DDJ-FLX4 WASAPI"));
    REQUIRE(ap.matches("Speakers (DDJ-FLX4)"));
    REQUIRE_FALSE(ap.matches("Realtek Audio"));
    REQUIRE(ap.scan({ "Realtek", "DDJ-FLX4 WASAPI" }));
    REQUIRE(ap.present());
    REQUIRE(flx4::Flx4AudioProfile::roleFor(0) == 0);
    REQUIRE(flx4::Flx4AudioProfile::roleFor(2) == 1);
    REQUIRE(flx4::Flx4AudioProfile::roleFor(4) == -1);
    REQUIRE_FALSE(ap.micToHost);   // mic never reaches USB input

    flx4::ConnectionFlow cf;
    cf.advance(flx4::ConnectionFlow::S_UsbConnected, "usb ok");
    cf.advance(flx4::ConnectionFlow::S_DeviceDetected, "DDJ-FLX4");
    cf.fail(flx4::ConnectionFlow::S_ProfileLoaded, "no profile");
    REQUIRE(cf.summary().find("profile-loaded") !=
            std::string::npos);
    cf.advance(flx4::ConnectionFlow::S_Ready, "ok");
    REQUIRE(cf.rep.ok);
    REQUIRE(cf.summary() == "ready");
}

TEST_CASE("FLX4: diagnostic decoder names controls", "[flx4]") {
    REQUIRE(flx4::controlNameFor(
        { AddrKind::Note, 0, flx4::N_PLAY }) == "deck0.play");
    REQUIRE(flx4::controlNameFor(
        { AddrKind::CC, 6, flx4::CC_XF }) == "mix.crossfader");
    REQUIRE(flx4::controlNameFor(
        { AddrKind::CC, 6, flx4::CC_XF + 0x20 }) ==
        "mix.crossfader.lsb");
    REQUIRE(flx4::controlNameFor(
        { AddrKind::Note, 9, 0x21 }) ==
        "deck1.pad.beatjump1");
    REQUIRE(flx4::controlNameFor(
        { AddrKind::Note, 10, 0x35 }) ==
        "deck1.pad.sampler5.shift");
    REQUIRE(flx4::controlNameFor(
        { AddrKind::Note, 2, 0x01 }) == "");
    // inventory sanity: every mapped control has documented source
    for (auto& c : flx4::inventory())
        REQUIRE(c.source != nullptr);
}

TEST_CASE("FLX4: safety — garbage bytes never crash or emit",
          "[flx4]") {
    Rig r;
    r.vc.attach();
    // random-ish stream: bad channels, bad types, extreme values
    const uint8_t types[] = { 0x90, 0x80, 0xB0, 0xC0, 0xD0, 0xE0, 0xF8 };
    for (int i = 0; i < 2000; ++i) {
        MidiMsg m;
        m.type = (MsgType)(i % 12);       // includes realtime + junk
        m.channel = (uint8_t)(i % 16);
        m.data1 = (uint8_t)((i * 37) & 0x7F);
        m.data2 = (uint8_t)((i * 53) & 0x7F);
        r.eng.processInput(flx4::VirtualFlx4::kId, m);
    }
    // every param value stayed finite + in declared range
    for (auto& [id, v] : r.parVals) {
        REQUIRE(std::isfinite(v));
        REQUIRE(v >= -64.0f); REQUIRE(v <= 64.0f);
    }
}

TEST_CASE("FLX4: scratch model is device-decoupled", "[flx4]") {
    flx4::ScratchEngine s;
    s.touchStart();
    const double d = s.apply(4.0);
    REQUIRE(d == Catch::Approx(4.0 * 0.0018));
    s.touchEnd();
    REQUIRE(s.apply(4.0) == Catch::Approx(4.0 * 0.04));
    s.mode = flx4::ScratchEngine::SEARCH;
    REQUIRE(s.apply(4.0) == Catch::Approx(4.0 * 0.12));
}
