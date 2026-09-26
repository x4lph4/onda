// Phase 17 — professional mixer engine: routing/remap, cue system,
// filter resonance, EQ range, phase invert, scenes/presets/diag,
// deck modes, param transactions, command surface, stress.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <numeric>
#include <string>
#include <vector>

#include "core/audio/TrackAudio.h"
#include "core/dsp/WsolaStretcher.h"
#include "core/engine/DeckEngine.h"
#include "core/mixer/CueManager.h"
#include "core/mixer/MixerController.h"
#include "core/mixer/MixerDiagnostics.h"
#include "core/mixer/MixerEngine.h"
#include "core/mixer/MixerPresetManager.h"
#include "core/mixer/MixerScene.h"
#include "core/mixer/ParameterRegistry.h"
#include "core/model/DeckStats.h"
#include "core/rt/AudioStats.h"
#include "ui/CommandIds.h"
#include "ui/CommandManager.h"
#include "ui/EngineCommands.h"

using namespace onda;
using namespace onda::mixer;
using Catch::Approx;

namespace {

constexpr int    kBlock = 512;
constexpr double kRate  = 48000.0;

float peak(const std::vector<float>& v) {
    float p = 0.0f;
    for (float x : v) p = std::max(p, std::fabs(x));
    return p;
}
float rms(const std::vector<float>& v) {
    double s = 0.0;
    for (float x : v) s += (double)x * x;
    return v.empty() ? 0.0f : (float)std::sqrt(s / (double)v.size());
}

// Engine-level rig: N decks -> mixer -> 4 outs (master L/R, cue L/R).
struct MixRig {
    MixerEngine mixer;
    rt::AudioStats astats;
    std::vector<DeckEngine> decks;
    std::vector<DeckStats>  dstats;
    std::vector<const TrackAudio*> owned;
    std::vector<float> outL, outR, cueL, cueR;

    MixRig(int ndecks = 2) : decks(ndecks), dstats(ndecks),
                             outL(kBlock), outR(kBlock),
                             cueL(kBlock), cueR(kBlock) {
        mixer.prepare(kRate);
        mixer.setActiveChannels(std::max(ndecks, 5)); // keep sampler ch live
        for (int i = 0; i < ndecks; ++i) {
            decks[i].bind(&dstats[i]);
            mixer.channel(i).bindStats(&dstats[i]);
            decks[i].setDeviceRate(kRate);
            decks[i].setStretcher(std::make_unique<WsolaStretcher>());
            decks[i].prepareDsp(kRate);
        }
    }
    void loadDeck(int d, double secs, double freq, float amp) {
        auto* t = makeSineRaw(secs, freq, amp);
        owned.push_back(t);
        decks[d].pendingTrack_.store(t);
        float zl[8]{}, zr[8]{};
        decks[d].renderPcm(zl, zr, 8);
        decks[d].setPlaying(true);
    }
    static TrackAudio* makeSineRaw(double secs, double freq, float amp) {
        auto* t = new TrackAudio();
        t->fileSampleRate = kRate; t->deviceRate = kRate;
        t->numFrames = (int64_t)(kRate * secs);
        t->samples.resize((size_t)t->numFrames * 2);
        for (int64_t i = 0; i < t->numFrames; ++i) {
            const float v = amp * (float)std::sin(2.0 * 3.14159265 * freq * i / kRate);
            t->samples[(size_t)i * 2]     = v;
            t->samples[(size_t)i * 2 + 1] = v;
        }
        return t;
    }
    void render() {
        float* outs[4] = { outL.data(), outR.data(), cueL.data(), cueR.data() };
        mixer.render(outs, 4, kBlock, decks.data(), (int)decks.size(), astats);
    }
    void run(int blocks) { for (int i = 0; i < blocks; ++i) render(); }
    ~MixRig() { for (auto* t : owned) delete t; }
};

// Controller rig: MixerController wired to a real MixerEngine through
// the same sinks the app uses — setParam lands on the RT engine.
struct CtlRig {
    MixerEngine engine;
    MixerController ctl;
    std::vector<std::pair<uint16_t, float>> sent;

    CtlRig() {
        engine.prepare(kRate);
        ctl.setSink([this](uint16_t id, float v) {
            sent.push_back({ id, v });
            engine.setParam(id, v);
        });
        ctl.setSourceSink([this](int ch, ChSource s) {
            engine.setChannelSource(ch, s);
        });
        ctl.setDeckModeSink([this](int n) { engine.setDeckMode(n); });
    }
};

} // namespace

// ======================================================== SOURCE ROUTING
TEST_CASE("P17: deck->channel remap routes audio to the new strip",
          "[mixer17][routing]") {
    MixRig f(4);
    f.loadDeck(0, 6.0, 220.0, 0.5f); // deck A
    f.loadDeck(2, 6.0, 880.0, 0.5f); // deck C
    // default: deck0->ch0 (xf A). Remap deck 0 -> channel 3 (xf B side).
    f.mixer.mapDeckToChannel(0, 3);
    f.mixer.setChannelSource(0, ChSource::silence());
    REQUIRE(f.mixer.channelSource(3).type == ChSourceType::Deck);
    REQUIRE(f.mixer.channelSource(3).index == 0);
    f.mixer.setCrossfader(-1.0f); // A side only: ch0 (silence) + ch2 (deckC)
    f.run(60);
    REQUIRE(peak(f.outL) > 0.1f);   // deck C still on ch2 side A
    // switch to B side: remapped deck 0 (220 Hz) appears, ch1 silent
    f.mixer.setCrossfader(1.0f);
    f.run(80);
    const float pk = peak(f.outL);
    REQUIRE(pk > 0.3f);
}

TEST_CASE("P17: silence source produces quiet post taps", "[mixer17][routing]") {
    MixRig f(2);
    f.loadDeck(0, 4.0, 440.0, 0.5f);
    // silence deck B's channel
    f.mixer.setChannelSource(1, ChSource::silence());
    f.run(30);
    const auto& m = f.mixer.channel(1).meters();
    REQUIRE(m.peakL.load() < 1e-4f);
    // post-fader taps must carry silence so sends read nothing stale
    float postPeak = 0.0f;
    for (int i = 0; i < kBlock; ++i)
        postPeak = std::max(postPeak, std::fabs(f.mixer.channel(1).postL()[i]));
    REQUIRE(postPeak < 1e-4f);
    // external source seam renders silence until a device feeds it
    f.mixer.setChannelSource(5, ChSource::external(0));
    f.mixer.setActiveChannels(6);
    f.run(30);
    REQUIRE(f.mixer.channel(5).meters().peakL.load() < 1e-4f);
}

TEST_CASE("P17: routing graph tags source types", "[mixer17][routing]") {
    MixRig f(2);
    bool sawDeck = false, sawSmpl = false;
    for (const auto& n : f.mixer.routing().nodes()) {
        if (n.type == RoutingEngine::NodeType::Source) {
            if (n.tag == (uint32_t)'deck') sawDeck = true;
            if (n.tag == (uint32_t)'smpl') sawSmpl = true;
        }
    }
    REQUIRE(sawDeck);
    REQUIRE(sawSmpl); // channel 4 default = sampler source
}

// ============================================================= DECK MODE
TEST_CASE("P17: deck mode 2 silences C/D without removing them",
          "[mixer17][deckmode]") {
    MixRig f(4);
    for (int d = 0; d < 4; ++d) f.loadDeck(d, 6.0, 220.0 + 110.0 * d, 0.4f);
    f.mixer.setCrossfader(0.0f);
    f.mixer.setXfCurve((int)XfCurve::Linear);
    f.run(60);
    const float four = rms(f.outL);
    f.mixer.setDeckMode(2);
    REQUIRE(f.mixer.deckMode() == 2);
    REQUIRE(f.mixer.channelSource(2).type == ChSourceType::Silence);
    f.run(80);
    const float two = rms(f.outL);
    // incoherent sines sum in quadrature: 4→2 decks ≈ √2/√4 ≈ 0.71×
    REQUIRE(two < four * 0.85f);  // C/D contribution gone
    REQUIRE(two > 0.05f);         // A/B still playing
    // restore
    f.mixer.setDeckMode(4);
    REQUIRE(f.mixer.channelSource(3).type == ChSourceType::Deck);
    f.run(80);
    REQUIRE(rms(f.outL) > two);
}

// ================================================================ CUE
TEST_CASE("P17: master cue injects program into phones independently",
          "[mixer17][cue]") {
    MixRig f(2);
    f.loadDeck(0, 5.0, 440.0, 0.5f);
    f.mixer.setCrossfader(-1.0f);
    f.mixer.setCueVol(1.0f);
    f.mixer.setCueMix(0.0f);      // phones = pure cue bus
    f.run(30);
    // no PFL, no master cue -> phones silent while master plays
    REQUIRE(peak(f.outL) > 0.3f);
    REQUIRE(peak(f.cueL) < 0.02f);
    // master cue on -> phones carry post-master program
    f.mixer.setParam((uint16_t)BusParam::MasterCue, 1.0f);
    f.run(60);
    const float cuePk = peak(f.cueL);
    REQUIRE(cuePk > 0.3f);
    // master output unaffected by the cue tap
    REQUIRE(peak(f.outL) > 0.3f);
    // off -> phones go quiet again
    f.mixer.setParam((uint16_t)BusParam::MasterCue, 0.0f);
    f.run(60);
    REQUIRE(peak(f.cueL) < 0.05f);
}

TEST_CASE("P17: master cue is smoothed (no instant step)", "[mixer17][cue]") {
    MixRig f(1);
    f.loadDeck(0, 3.0, 440.0, 0.5f);
    f.mixer.setCrossfader(-1.0f);
    f.mixer.setCueVol(1.0f);
    f.mixer.setCueMix(0.0f);
    f.mixer.setParam((uint16_t)BusParam::MasterCue, 1.0f);
    f.render(); // first block after the toggle
    // gain ramps across ~6 ms: the block must start below full program
    const float head = std::fabs(f.cueL[8]);
    const float tail = peak(f.cueL);
    REQUIRE(tail > head);
}

// =========================================================== FILTER RES
TEST_CASE("P17: filter resonance emphasizes the cutoff", "[mixer17][filter]") {
    // typed LP at 1 kHz; sweep a 1 kHz sine through Q 0.707 vs Q 5.
    auto levelAt = [](double q) {
        ChannelStrip s; s.prepare(kRate); s.setXfAssign(XfAssign::Thru);
        s.filter().setType(FilterType::LowPass);
        s.filter().setCutoffHz(1000.0);
        s.filter().setResonance(q);
        CrossfaderEngine xf; xf.prepare(kRate);
        std::vector<float> mixL(kBlock), mixR(kBlock);
        double phase = 0.0;
        float lastPeak = 0.0f;
        for (int b = 0; b < 60; ++b) {
            for (int i = 0; i < kBlock; ++i) {
                const float v = 0.4f * (float)std::sin(phase);
                s.inL()[i] = v; s.inR()[i] = v;
                phase += 2.0 * 3.14159265358979 * 1000.0 / kRate;
            }
            std::fill(mixL.begin(), mixL.end(), 0.0f);
            std::fill(mixR.begin(), mixR.end(), 0.0f);
            xf.tickPos(kBlock);
            s.process(kBlock, mixL.data(), mixR.data(), nullptr, nullptr, xf, false);
            lastPeak = peak(mixL);
        }
        return lastPeak;
    };
    const float flat = levelAt(0.5);
    const float reso = levelAt(5.0);
    REQUIRE(reso > flat * 1.5f); // resonant peak at the corner
}

// ============================================================== EQ RANGE
TEST_CASE("P17: EQ gain range is configurable", "[mixer17][eq]") {
    EqProcessor eq; eq.prepare(kRate);
    eq.setGainDb(0, -50.0);
    REQUIRE(eq.gainDb(0) == Approx(-30.0));   // default clamp
    eq.setRangeDb(-60.0, 6.0);                // isolator-style deep cut
    eq.setGainDb(0, -50.0);
    REQUIRE(eq.gainDb(0) == Approx(-50.0));
    // narrowing the range re-clamps existing targets
    eq.setRangeDb(-12.0, 6.0);
    REQUIRE(eq.gainDb(0) == Approx(-12.0));
}

// ========================================================== PHASE INVERT
TEST_CASE("P17: phase invert flips polarity clicklessly", "[mixer17][phase]") {
    auto run = [](bool inv) {
        ChannelStrip s; s.prepare(kRate); s.setXfAssign(XfAssign::Thru);
        s.setPhaseInvert(inv);
        CrossfaderEngine xf; xf.prepare(kRate);
        std::vector<float> mixL(kBlock), mixR(kBlock), cueL(kBlock), cueR(kBlock);
        std::vector<float> out;
        for (int b = 0; b < 30; ++b) {
            std::fill_n(s.inL(), kBlock, 0.5f);
            std::fill_n(s.inR(), kBlock, 0.5f);
            std::fill(mixL.begin(), mixL.end(), 0.0f);
            std::fill(mixR.begin(), mixR.end(), 0.0f);
            xf.tickPos(kBlock);
            s.process(kBlock, mixL.data(), mixR.data(),
                      cueL.data(), cueR.data(), xf, false);
            if (b > 10) out.insert(out.end(), mixL.begin(), mixL.end());
        }
        return out;
    };
    const auto pos = run(false), neg = run(true);
    REQUIRE(pos.size() == neg.size());
    for (size_t i = 0; i < pos.size(); ++i)
        REQUIRE(neg[i] == Approx(-pos[i]).margin(0.02f));
}

// ======================================================= PARAM REGISTRY
TEST_CASE("P17: new params are registered and sanitized", "[mixer17][reg]") {
    using R = ParameterRegistry;
    REQUIRE(R::exists(R::channelFilterRes(2)));
    REQUIRE(R::exists(R::channelPhase(3)));
    REQUIRE(R::exists(R::MASTER_CUE));
    REQUIRE(R::sanitize(R::channelFilterRes(0), 99.0f) == Approx(8.0f));
    REQUIRE(R::sanitize(R::channelFilterRes(0), 0.0f) == Approx(0.2f));
    REQUIRE(R::sanitize(R::MASTER_CUE, 9.0f) == 1.0f);
    // setParam dispatch reaches the strip
    MixerEngine m; m.prepare(kRate);
    m.setParam(chParam(0, ChParam::FilterRes), 4.0f);
    REQUIRE(m.channel(0).filter().resonance() == Approx(4.0));
    m.setParam(chParam(0, ChParam::PhaseInvert), 1.0f);
    REQUIRE(m.channel(0).phaseInverted());
}

// ============================================================ SCENES
TEST_CASE("P17: scene capture/apply round-trips mixer state",
          "[mixer17][scene]") {
    CtlRig r;
    r.ctl.channelGain(0, 0.65f);
    r.ctl.channelLow(1, -12.0f);
    r.ctl.channelFilterRes(2, 3.5f);
    r.ctl.channelFader(3, 0.4f);
    r.ctl.channelPhase(0, true);
    r.ctl.channelSend(1, 0, 0.7f);
    r.ctl.masterCue(true);
    r.ctl.crossfader(-0.4f);
    r.ctl.setChannelSource(5, ChSource::external(1));
    MixerScene snap = r.ctl.captureScene();

    // scramble everything
    r.ctl.channelGain(0, 1.0f); r.ctl.channelLow(1, 0.0f);
    r.ctl.channelFilterRes(2, 0.707f); r.ctl.channelFader(3, 1.0f);
    r.ctl.channelPhase(0, false); r.ctl.masterCue(false);
    r.ctl.crossfader(1.0f);
    r.ctl.setChannelSource(5, ChSource::silence());
    REQUIRE(r.ctl.value(ParameterRegistry::MASTER_CUE) == 0.0f);

    r.ctl.applyScene(snap);
    using R = ParameterRegistry;
    REQUIRE(r.ctl.value(R::channelGain(0)) == Approx(0.65f));
    REQUIRE(r.ctl.value(R::channelLow(1)) == Approx(-12.0f));
    REQUIRE(r.ctl.value(R::channelFilterRes(2)) == Approx(3.5f));
    REQUIRE(r.ctl.value(R::channelFader(3)) == Approx(0.4f));
    REQUIRE(r.ctl.value(R::channelPhase(0)) == 1.0f);
    REQUIRE(r.ctl.value(R::channelSendA(1)) == Approx(0.7f));
    REQUIRE(r.ctl.value(R::MASTER_CUE) == 1.0f);
    REQUIRE(r.ctl.value(R::CROSSFADER) == Approx(-0.4f));
    REQUIRE(r.ctl.channelSource(5).type == ChSourceType::External);
    // and the engine actually received the writes
    REQUIRE(r.engine.channel(0).phaseInverted());
    REQUIRE(r.engine.channelSource(5).type == ChSourceType::External);
}

// ============================================================ PRESETS
TEST_CASE("P17: factory mixer presets apply through the controller",
          "[mixer17][presets]") {
    CtlRig r;
    MixerPresetManager pm;
    REQUIRE(pm.count() >= 4);
    bool sawFlat = false, sawScratch = false;
    for (int i = 0; i < pm.count(); ++i) {
        if (pm.name(i) == "Flat")    sawFlat = true;
        if (pm.name(i) == "Scratch") sawScratch = true;
    }
    REQUIRE(sawFlat); REQUIRE(sawScratch);
    // apply Scratch -> xf curve becomes Cut on the engine
    for (int i = 0; i < pm.count(); ++i)
        if (pm.name(i) == "Scratch") r.ctl.applyScene(pm.scene(i));
    using R = ParameterRegistry;
    REQUIRE(r.ctl.value(R::XF_CURVE) == (float)(int)XfCurve::Cut);
    // apply Flat -> restored
    for (int i = 0; i < pm.count(); ++i)
        if (pm.name(i) == "Flat") r.ctl.applyScene(pm.scene(i));
    REQUIRE(r.ctl.value(R::XF_CURVE) == 1.0f); // EqualPower default
}

// ========================================================== DIAGNOSTICS
TEST_CASE("P17: diagnostics snapshot reflects live mixer state",
          "[mixer17][diag]") {
    MixRig f(2);
    f.loadDeck(0, 4.0, 440.0, 0.5f);
    f.mixer.setPfl(0, true);
    f.mixer.setParam((uint16_t)BusParam::MasterCue, 1.0f);
    f.run(40);
    MixerBusDiag bus; MixerChannelDiag ch[8];
    f.mixer.snapshot(bus, ch, 8);
    REQUIRE(bus.activeChannels >= 5);
    REQUIRE(bus.masterCue);
    REQUIRE(bus.cueCount == 1);
    REQUIRE(bus.masterPeakL > 0.1f);
    REQUIRE(ch[0].active);
    REQUIRE(ch[0].sourceType == (int)ChSourceType::Deck);
    REQUIRE(ch[0].cue);
    REQUIRE(ch[0].peakL > 0.1f);
    REQUIRE(ch[4].sourceType == (int)ChSourceType::Sampler);
    REQUIRE(ch[0].latencySamples == 0); // all processors latency-free
    const std::string rep = f.mixer.diagnosticReport();
    REQUIRE(rep.find("MIXER DIAGNOSTICS") != std::string::npos);
    REQUIRE(rep.find("MASTER-CUE") != std::string::npos);
    REQUIRE(rep.find("ch0") != std::string::npos);
}

// =========================================================== TRANSACTIONS
TEST_CASE("P17: grouped edits coalesce a burst into one delta",
          "[mixer17][undo]") {
    MixerController mc;
    mc.setSink([](uint16_t, float) {});
    using R = ParameterRegistry;
    const uint16_t id = R::CROSSFADER;
    // simulate a fader drag: 30 writes -> one recorded edit
    for (int i = 0; i < 30; ++i)
        mc.setParamEdit(id, -1.0f + 2.0f * (float)i / 29.0f);
    REQUIRE(mc.paramEditOpen());
    const auto e = mc.endParamEdit();
    REQUIRE(e.id == id);
    REQUIRE(e.from == Approx(0.0f));   // registry default
    REQUIRE(e.to == Approx(1.0f).margin(0.01f));
    // a write on a different param auto-closes the open edit
    mc.beginParamEdit(id);
    mc.setParamEdit(R::MASTER_GAIN, 0.5f);
    REQUIRE(mc.paramEditOpen()); // reopened on the new id
    REQUIRE(mc.endParamEdit().id == R::MASTER_GAIN);
}

// ============================================================== COMMANDS
TEST_CASE("P17: command surface drives cue/mode/scenes/presets",
          "[mixer17][commands]") {
    CtlRig r;
    int savedSlot = -1, recalledSlot = -1, presetDelta = 0, diagCopied = 0;
    MixerScene bank[8];

    ui::EnginePorts ports;
    ports.mixer = &r.ctl;
    ports.saveMixScene   = [&](int s) { savedSlot = s; r.ctl.captureScene(bank[s]); };
    ports.recallMixScene = [&](int s) { recalledSlot = s; r.ctl.applyScene(bank[s]); };
    ports.applyMixPreset = [&](int d) { presetDelta += d; };
    ports.copyMixDiag    = [&] { ++diagCopied; };
    ui::CommandManager cm;
    ui::registerEngineCommands(cm, ports);
    using CS = ui::CommandSource;

    using R = ParameterRegistry;
    REQUIRE(cm.invoke("mixer.masterCueToggle", CS::Midi));
    REQUIRE(r.ctl.value(R::MASTER_CUE) == 1.0f);
    REQUIRE(cm.invoke("mixer.deckModeToggle", CS::Midi));
    REQUIRE(r.ctl.deckMode() == 2);
    REQUIRE(cm.invoke("mixer.deckModeToggle", CS::Midi));
    REQUIRE(r.ctl.deckMode() == 4);
    REQUIRE(cm.invoke("mixer.sceneSave", CS::Midi, 3.0f));
    REQUIRE(savedSlot == 3);
    REQUIRE(cm.invoke("mixer.sceneRecall", CS::Midi, 3.0f));
    REQUIRE(recalledSlot == 3);
    REQUIRE(cm.invoke("mixer.presetNext", CS::Midi));
    REQUIRE(cm.invoke("mixer.presetPrev", CS::Midi));
    REQUIRE(presetDelta == 0);
    REQUIRE(cm.invoke("mixer.diagCopy", CS::Midi));
    REQUIRE(diagCopied == 1);
}

// ================================================================= STRESS
TEST_CASE("P17: 4-deck + cue + scenes stress stays bounded",
          "[mixer17][stress]") {
    MixRig f(4);
    for (int d = 0; d < 4; ++d) f.loadDeck(d, 8.0, 110.0 * (d + 1), 0.4f);
    f.mixer.setParam((uint16_t)BusParam::MasterCue, 1.0f);
    f.mixer.setCueVol(1.0f); f.mixer.setCueMix(0.3f);
    for (int d = 0; d < 4; ++d) {
        f.mixer.setParam(chParam(d, ChParam::Cue), 1.0f);
        f.mixer.setParam(chParam(d, ChParam::SendA), 0.6f);
        f.mixer.setParam(chParam(d, ChParam::FilterRes), 2.0f);
        f.mixer.setParam(chParam(d, ChParam::Filter), (d & 1) ? 0.5f : -0.5f);
    }
    f.mixer.setCrossfader(0.0f);

    const auto t0 = std::chrono::steady_clock::now();
    constexpr int kBlocks = 4 * 200; // ~17 s of 48k audio
    for (int i = 0; i < kBlocks; ++i) {
        f.render();
        if (i % 100 == 0) f.mixer.setCrossfader(-1.0f + 2.0f * (i % 200) / 199.0f);
    }
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    // all outputs finite + bounded; master meter sane
    for (float v : f.outL) REQUIRE(std::isfinite(v));
    REQUIRE(peak(f.outL) < 4.0f);
    const auto& mm = f.mixer.master().meters();
    REQUIRE(mm.rmsL.load() < 4.0f);
    INFO("4-deck + cue + sends render took " << ms << " ms");
    REQUIRE(ms < 30000.0); // generous CI bound — Debug build
}

TEST_CASE("P17: master cue never leaks into the master bus",
          "[mixer17][cue]") {
    MixRig f(1);
    f.loadDeck(0, 3.0, 440.0, 0.4f);
    f.mixer.setCrossfader(-1.0f);
    f.run(30);
    f.mixer.setParam((uint16_t)BusParam::MasterCue, 1.0f);
    f.run(30);
    const float withCue = rms(f.outL);
    f.mixer.setParam((uint16_t)BusParam::MasterCue, 0.0f);
    f.run(30);
    const float without = rms(f.outL);
    // identical program: cue tap is strictly read-only
    REQUIRE(withCue == Approx(without).margin(0.01f));
}
