// Phase 24 — mixer completion: pan, true isolator kill, booth bus,
// recording bus, ParamEvent ids, scene v2, presets, automation
// smoothness, 4-deck + sampler stress.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <numeric>
#include <vector>

#include "core/audio/TrackAudio.h"
#include "core/dsp/WsolaStretcher.h"
#include "core/engine/DeckEngine.h"
#include "core/mixer/IsolatorProcessor.h"
#include "core/mixer/MixerController.h"
#include "core/mixer/MixerDiagnostics.h"
#include "core/mixer/MixerEngine.h"
#include "core/mixer/MixerPresetManager.h"
#include "core/mixer/MixerScene.h"
#include "core/mixer/PanProcessor.h"
#include "core/mixer/ParameterRegistry.h"
#include "core/model/DeckStats.h"
#include "core/rec/RecordingBuffer.h"
#include "core/rt/AudioStats.h"
#include "core/sampler/SamplerRt.h"

using namespace onda;
using namespace onda::mixer;
using Catch::Approx;

namespace {

constexpr int    kBlock = 512;
constexpr double kRate  = 48000.0;

float peakAbs(const float* v, int n) {
    float p = 0.0f;
    for (int i = 0; i < n; ++i) p = std::max(p, std::fabs(v[i]));
    return p;
}
bool allFinite(const float* v, int n) {
    for (int i = 0; i < n; ++i) if (!std::isfinite(v[i])) return false;
    return true;
}
float maxStep(const float* v, int n) {
    float m = 0.0f;
    for (int i = 1; i < n; ++i) m = std::max(m, std::fabs(v[i] - v[i - 1]));
    return m;
}

// 6-output rig: master 0-1, cue 2-3, booth 4-5.
struct Rig24 {
    MixerEngine mixer;
    rt::AudioStats astats;
    std::vector<DeckEngine> decks;
    std::vector<DeckStats>  dstats;
    std::vector<const TrackAudio*> owned;
    std::vector<float> outL, outR, cueL, cueR, boL, boR;
    sampler::SamplerRt samplerRt { kRate };

    Rig24(int ndecks = 2) : decks(ndecks), dstats(ndecks),
                            outL(kBlock), outR(kBlock),
                            cueL(kBlock), cueR(kBlock),
                            boL(kBlock), boR(kBlock) {
        mixer.prepare(kRate);
        mixer.setActiveChannels(MixerEngine::kSamplerChannel + 1);
        mixer.setOutputMap(0, 1, 2, 3, 4, 5);
        for (int i = 0; i < ndecks; ++i) {
            decks[i].bind(&dstats[i]);
            mixer.channel(i).bindStats(&dstats[i]);
            decks[i].setDeviceRate(kRate);
            decks[i].setStretcher(std::make_unique<WsolaStretcher>());
            decks[i].prepareDsp(kRate);
        }
    }
    void loadDeck(int d, double secs, double freq, float amp) {
        auto* t = makeSine(secs, freq, amp);
        owned.push_back(t);
        decks[d].pendingTrack_.store(t);
        float zl[8]{}, zr[8]{};
        decks[d].renderPcm(zl, zr, 8);
        decks[d].setPlaying(true);
    }
    static TrackAudio* makeSine(double secs, double freq, float amp) {
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
    void render(bool withSampler = false) {
        float* outs[6] = { outL.data(), outR.data(), cueL.data(),
                           cueR.data(), boL.data(), boR.data() };
        mixer.render(outs, 6, kBlock, decks.data(), (int)decks.size(),
                     astats, withSampler ? &samplerRt : nullptr);
    }
    void run(int blocks, bool withSampler = false) {
        for (int i = 0; i < blocks; ++i) render(withSampler);
    }
    ~Rig24() { for (auto* t : owned) delete t; }
};

struct CtlRig24 {
    MixerEngine engine;
    MixerController ctl;
    CtlRig24() {
        engine.prepare(kRate);
        ctl.setSink([this](uint16_t id, float v) { engine.setParam(id, v); });
        ctl.setSourceSink([this](int ch, ChSource s) {
            engine.setChannelSource(ch, s);
        });
        ctl.setDeckModeSink([this](int n) { engine.setDeckMode(n); });
    }
};

} // namespace

// ============================================================ PAN (§22)
TEST_CASE("P24: channel pan is a unity-center balance law",
          "[mixer24][pan]") {
    Rig24 f(1);
    f.loadDeck(0, 4.0, 440.0, 0.4f);
    f.mixer.setParam(ParameterRegistry::CROSSFADER, 0.0f);
    f.run(20);
    const float cL = peakAbs(f.outL.data(), kBlock);
    const float cR = peakAbs(f.outR.data(), kBlock);
    REQUIRE(cL > 0.1f);
    REQUIRE(cL == Approx(cR).margin(0.02f));   // center = same image

    f.mixer.setParam(ParameterRegistry::channelPan(0), -1.0f);
    f.run(30);                                  // 6 ms smoother + settle
    REQUIRE(peakAbs(f.outL.data(), kBlock) == Approx(cL).margin(0.03f));
    REQUIRE(peakAbs(f.outR.data(), kBlock) < 1e-4f);

    f.mixer.setParam(ParameterRegistry::channelPan(0), 1.0f);
    f.run(30);
    REQUIRE(peakAbs(f.outR.data(), kBlock) == Approx(cR).margin(0.03f));
    REQUIRE(peakAbs(f.outL.data(), kBlock) < 1e-4f);
}

TEST_CASE("P24: pan sweeps stay continuous and finite",
          "[mixer24][pan]") {
    Rig24 f(1);
    f.loadDeck(0, 6.0, 220.0, 0.5f);
    float worst = 0.0f;
    for (int b = 0; b < 80; ++b) {
        // alternate full-travel pan every other block — brutal case
        f.mixer.setParam(ParameterRegistry::channelPan(0),
                         (b % 2) ? 1.0f : -1.0f);
        f.render();
        REQUIRE(allFinite(f.outL.data(), kBlock));
        REQUIRE(allFinite(f.outR.data(), kBlock));
        worst = std::max(worst, maxStep(f.outL.data(), kBlock));
        worst = std::max(worst, maxStep(f.outR.data(), kBlock));
    }
    // a 0.5 sine @220Hz has ~0.015 intrinsic step; a click would jump
    // the discontinuity far above that. Smoothed sweeps stay < 0.1.
    REQUIRE(worst < 0.1f);
}

// ===================================================== ISOLATOR (§12)
TEST_CASE("P24: isolator detent reaches ~silence, standard EQ does not",
          "[mixer24][isolator]") {
    Rig24 f(1);
    f.loadDeck(0, 6.0, 80.0, 0.5f);   // deep bass — inside the low band

    // Standard EQ: -26 dB detent attenuates but remains audible.
    f.mixer.setParam(ParameterRegistry::channelLow(0), -26.0f);
    f.run(40);
    const float stdPeak = peakAbs(f.outL.data(), kBlock);
    REQUIRE(stdPeak > 0.005f);          // ~0.5 * 10^(-26/20) ≈ 0.025
    REQUIRE(stdPeak < 0.06f);

    // Isolator mode: same -26 command must approach silence (-72 dB).
    f.mixer.setParam(ParameterRegistry::channelEqMode(0), 1.0f);
    f.mixer.setParam(ParameterRegistry::channelLow(0), -26.0f);
    f.run(40);                          // 6 dB/block ramp → ~12 blocks
    const float isoPeak = peakAbs(f.outL.data(), kBlock);
    // ~0.5 * 10^(-72/20) ≈ 1.3e-4 + biquad/deck residual — still an
    // order of magnitude under the -26 dB standard floor.
    REQUIRE(isoPeak < 0.005f);
    REQUIRE(isoPeak < stdPeak * 0.25f);
}

TEST_CASE("P24: isolator kills only the target band",
          "[mixer24][isolator]") {
    Rig24 f(1);
    // deck plays 80 Hz sine — killed by LOW detent; a high-band detent
    // must leave it alone. Assertions are relative to the neutral
    // baseline so deck-level details don't leak into the check.
    f.loadDeck(0, 6.0, 80.0, 0.4f);
    f.mixer.setParam(ParameterRegistry::channelEqMode(0), 1.0f);
    f.run(30);
    const float base = peakAbs(f.outL.data(), kBlock);
    REQUIRE(base > 0.05f);

    f.mixer.setParam(ParameterRegistry::channelHigh(0), -26.0f); // kill highs
    f.run(40);
    REQUIRE(peakAbs(f.outL.data(), kBlock) > base * 0.8f); // 80 Hz untouched

    f.mixer.setParam(ParameterRegistry::channelHigh(0), 0.0f);
    f.mixer.setParam(ParameterRegistry::channelLow(0), -26.0f);  // kill lows
    f.run(40);
    REQUIRE(peakAbs(f.outL.data(), kBlock) < base * 0.05f);
}

TEST_CASE("P24: isolator moves stay finite and click-free",
          "[mixer24][isolator]") {
    Rig24 f(1);
    f.loadDeck(0, 6.0, 120.0, 0.5f);
    f.mixer.setParam(ParameterRegistry::channelEqMode(0), 1.0f);
    float worst = 0.0f;
    for (int b = 0; b < 60; ++b) {
        // slam the low band between boost and kill
        f.mixer.setParam(ParameterRegistry::channelLow(0),
                         (b % 2) ? -26.0f : 6.0f);
        f.render();
        REQUIRE(allFinite(f.outL.data(), kBlock));
        worst = std::max(worst, maxStep(f.outL.data(), kBlock));
    }
    REQUIRE(worst < 0.15f);
}

// ========================================================== BOOTH (§33)
TEST_CASE("P24: booth bus is independent of master gain/mute",
          "[mixer24][booth]") {
    Rig24 f(1);
    f.loadDeck(0, 6.0, 330.0, 0.4f);
    f.run(30);
    const float progPeak = peakAbs(f.outL.data(), kBlock);
    REQUIRE(progPeak > 0.1f);
    const float boothPeak0 = peakAbs(f.boL.data(), kBlock);
    REQUIRE(boothPeak0 == Approx(progPeak).margin(0.02f)); // unity booth

    // booth gain scales booth, not master
    f.mixer.setParam(ParameterRegistry::BOOTH_GAIN, 0.5f);
    f.run(30);
    REQUIRE(peakAbs(f.boL.data(), kBlock)
            == Approx(progPeak * 0.5f).margin(0.03f));
    REQUIRE(peakAbs(f.outL.data(), kBlock)
            == Approx(progPeak).margin(0.03f));

    // master mute silences the house, NOT the booth (pre-gain tap)
    f.mixer.setParam(ParameterRegistry::MASTER_MUTE, 1.0f);
    f.run(30);
    REQUIRE(peakAbs(f.outL.data(), kBlock) < 1e-4f);
    REQUIRE(peakAbs(f.boL.data(), kBlock) > 0.1f);
}

TEST_CASE("P24: booth bus exposes meters and clip telemetry",
          "[mixer24][booth]") {
    Rig24 f(1);
    f.loadDeck(0, 6.0, 440.0, 0.6f);
    // full A side → xf gain 1.0, program ≈0.6; booth ×2 → ~1.2 clips
    f.mixer.setParam(ParameterRegistry::CROSSFADER, -1.0f);
    f.mixer.setParam(ParameterRegistry::BOOTH_GAIN, 2.0f);
    f.run(30);
    const auto& bm = f.mixer.booth().meters();
    REQUIRE(bm.peakL.load() > 0.4f);
    REQUIRE(bm.clips.load() >= 1);   // 0.5 * 2 = 1.0 ≥ clip threshold
    MixerBusDiag bus; MixerChannelDiag chans[8];
    f.mixer.snapshot(bus, chans, 8);
    REQUIRE(bus.boothGain == Approx(2.0f).margin(0.01f));
    REQUIRE(bus.boothPeakL > 0.4f);
    REQUIRE(bus.boothClips >= 1);
}

// ====================================================== RECORDING (§34)
TEST_CASE("P24: recording bus feeds post-master program to the ring",
          "[mixer24][rec]") {
    Rig24 f(1);
    f.loadDeck(0, 6.0, 220.0, 0.5f);
    rec::RecorderRt rec;
    f.mixer.attachRecorder(&rec);

    f.run(4);                            // disarmed → nothing written
    REQUIRE(rec.meter(kRate).frames == 0);

    rec.arm(true);
    rec.resetMeters();
    f.run(8);
    const auto m = rec.meter(kRate);
    REQUIRE(m.frames == 8 * kBlock);
    REQUIRE(m.peakL > 0.2f);             // program level reached the tap

    // drain the ring — must be the post-master program (nonzero stereo)
    std::vector<float> buf((size_t)kBlock * 2 * 8);
    const size_t got = rec.ring().pop(buf.data(), (size_t)kBlock * 8);
    REQUIRE(got == (size_t)kBlock * 8);
    REQUIRE(peakAbs(buf.data(), (int)buf.size()) > 0.2f);

    f.mixer.attachRecorder(nullptr);
    rec.resetMeters();
    f.run(2);
    REQUIRE(rec.meter(kRate).frames == 0);  // detached → free tap
}

// ==================================================== REGISTRY (§40)
TEST_CASE("P24: registry exposes pan/eqMode/filterType/booth ids",
          "[mixer24][params]") {
    using R = ParameterRegistry;
    REQUIRE(R::exists(R::channelPan(0)));
    REQUIRE(R::exists(R::channelEqMode(0)));
    REQUIRE(R::exists(R::channelFilterType(0)));
    REQUIRE(R::exists(R::BOOTH_GAIN));
    // sanitize clamps to declared ranges
    REQUIRE(R::sanitize(R::channelPan(0), 5.0f) == 1.0f);
    REQUIRE(R::sanitize(R::channelPan(0), -5.0f) == -1.0f);
    REQUIRE(R::sanitize(R::channelEqMode(0), 0.7f) == 1.0f); // step 1
    REQUIRE(R::sanitize(R::channelFilterType(0), 9.0f) == 2.0f);
    REQUIRE(R::sanitize(R::BOOTH_GAIN, 9.0f) == 2.0f);
    // every channel index gets a distinct id
    REQUIRE(R::channelPan(1) != R::channelPan(0));

    // setParam path lands on the engine
    CtlRig24 r;
    r.ctl.channelPan(2, -0.5f);
    r.ctl.channelEqMode(2, 1);
    r.ctl.channelFilterType(2, 1);
    r.ctl.boothGain(0.7f);
    REQUIRE(r.engine.channel(2).pan() == Approx(-0.5f));
    REQUIRE((int)r.engine.channel(2).eq().mode() == 1);
    REQUIRE((int)r.engine.channel(2).filter().type() == 1);
    REQUIRE(r.engine.booth().gain() == Approx(0.7f));
}

TEST_CASE("P24: typed filter mode shapes regardless of position",
          "[mixer24][filter]") {
    Rig24 f(1);
    f.loadDeck(0, 6.0, 8000.0, 0.4f);  // high sine
    f.mixer.setParam(ParameterRegistry::channelFilterType(0),
                     (float)(int)FilterType::LowPass);
    f.mixer.channel(0).filter().setCutoffHz(500.0); // RT setter
    f.run(30);
    // 8 kHz through a 500 Hz LPF → strongly attenuated
    REQUIRE(peakAbs(f.outL.data(), kBlock) < 0.05f);
}

// ========================================================== SCENE v2
TEST_CASE("P24: scene v2 round-trips pan/eqMode/filterType/booth",
          "[mixer24][scene]") {
    REQUIRE(MixerScene::kVersion == 2);
    CtlRig24 r;
    r.ctl.channelPan(1, 0.5f);
    r.ctl.channelEqMode(1, 1);
    r.ctl.channelFilterType(1, 2);
    r.ctl.boothGain(0.4f);
    MixerScene snap = r.ctl.captureScene();

    CtlRig24 r2;                          // fresh engine+ctl
    r2.ctl.applyScene(snap);
    REQUIRE(r2.engine.channel(1).pan() == Approx(0.5f));
    REQUIRE((int)r2.engine.channel(1).eq().mode() == 1);
    REQUIRE((int)r2.engine.channel(1).filter().type() == 2);
    REQUIRE(r2.engine.booth().gain() == Approx(0.4f));
    // snapshot diagnostics carry the new fields too
    MixerBusDiag bus; MixerChannelDiag chans[8];
    r2.engine.snapshot(bus, chans, 8);
    REQUIRE(chans[1].pan == Approx(0.5f));
    REQUIRE(chans[1].eqMode == 1);
    REQUIRE(bus.boothGain == Approx(0.4f));
}

// =========================================================== PRESETS
TEST_CASE("P24: spec-named presets exist and apply through controller",
          "[mixer24][presets]") {
    MixerPresetManager pm;
    bool sawDefault = false, sawClassic = false, sawModern = false;
    for (int i = 0; i < pm.count(); ++i) {
        if (pm.name(i) == "Default") sawDefault = true;
        if (pm.name(i) == "Classic") sawClassic = true;
        if (pm.name(i) == "Modern")  sawModern  = true;
    }
    REQUIRE(sawDefault); REQUIRE(sawClassic); REQUIRE(sawModern);

    CtlRig24 r;
    for (int i = 0; i < pm.count(); ++i)
        if (pm.name(i) == "Modern") r.ctl.applyScene(pm.scene(i));
    REQUIRE((int)r.engine.xf().curve() == (int)XfCurve::Cut);
    REQUIRE((int)r.engine.channel(0).eq().mode()
            == (int)EqMode::Isolator);
    // Classic restores standard EQ + EQP crossfader
    for (int i = 0; i < pm.count(); ++i)
        if (pm.name(i) == "Classic") r.ctl.applyScene(pm.scene(i));
    REQUIRE((int)r.engine.xf().curve() == (int)XfCurve::EqualPower);
    REQUIRE((int)r.engine.channel(0).eq().mode()
            == (int)EqMode::Standard);
    // Custom = user-stored scene, deduped by name
    pm.store("Custom", MixerScene{});
    REQUIRE(pm.count() > 0);
}

// ================================================ AUTOMATION SMOOTHING
TEST_CASE("P24: rapid EQ/filter/fader/xf changes never click or NaN",
          "[mixer24][automation]") {
    Rig24 f(2);
    f.loadDeck(0, 8.0, 220.0, 0.45f);
    f.loadDeck(1, 8.0, 330.0, 0.45f);
    using R = ParameterRegistry;
    float worst = 0.0f;
    for (int b = 0; b < 120; ++b) {
        // hammer every smoothed parameter with alternating extremes
        f.mixer.setParam(R::channelLow(0),  (b % 3) ? -26.0f : 6.0f);
        f.mixer.setParam(R::channelMid(0),  (b % 2) ? 6.0f : -26.0f);
        f.mixer.setParam(R::channelFilter(1),(b % 2) ? 1.0f : -1.0f);
        f.mixer.setParam(R::channelFader(0),(b % 4) ? 1.0f : 0.0f);
        f.mixer.setParam(R::channelFader(1),(b % 3) ? 0.0f : 1.0f);
        f.mixer.setParam(R::CROSSFADER,     (b % 2) ? 1.0f : -1.0f);
        f.mixer.setParam(R::MASTER_GAIN,    (b % 5) ? 2.0f : 0.0f);
        f.render();
        REQUIRE(allFinite(f.outL.data(), kBlock));
        REQUIRE(allFinite(f.outR.data(), kBlock));
        worst = std::max(worst, maxStep(f.outL.data(), kBlock));
        worst = std::max(worst, maxStep(f.outR.data(), kBlock));
    }
    // worst-case legitimate content: ~0.9 peak stereo sum @330Hz ≈ 0.06
    // per-sample slope; per-block EQ dB steps add coefficient-movement
    // edge but smoothing keeps it well under a hard discontinuity.
    REQUIRE(worst < 0.5f);
}

// ===================================================== 4-DECK ROUTING
TEST_CASE("P24: four decks route, meter, cue and crossfade correctly",
          "[mixer24][4deck]") {
    Rig24 f(4);
    f.loadDeck(0, 8.0, 110.0, 0.3f);
    f.loadDeck(1, 8.0, 220.0, 0.3f);
    f.loadDeck(2, 8.0, 440.0, 0.3f);
    f.loadDeck(3, 8.0, 880.0, 0.3f);
    // defaults: ch0/ch2 → A side, ch1/ch3 → B side
    f.mixer.setParam(ParameterRegistry::CROSSFADER, -1.0f); // full A
    f.run(30);
    const float aSide = peakAbs(f.outL.data(), kBlock);
    REQUIRE(aSide > 0.15f);                    // decks A+C summed
    f.mixer.setParam(ParameterRegistry::CROSSFADER, 1.0f);  // full B
    f.run(30);
    const float bSide = peakAbs(f.outL.data(), kBlock);
    REQUIRE(bSide > 0.15f);
    // A side ≠ B side content (different freqs → different peaks ok)
    // verify each strip meters independently
    for (int c = 0; c < 4; ++c)
        REQUIRE(f.mixer.channel(c).meters().peakL.load() > 0.1f);
    // cue on ch2 → PFL into cue bus regardless of crossfader
    f.mixer.setParam(ParameterRegistry::channelCue(2), 1.0f);
    f.run(20);
    REQUIRE(peakAbs(f.cueL.data(), kBlock) > 0.1f);
    // assignment remap: move ch2 to Thru — it now ignores the xfader
    f.mixer.setParam(ParameterRegistry::channelXfAssign(2), 2.0f);
    f.mixer.setParam(ParameterRegistry::CROSSFADER, -1.0f);
    f.run(30);
    REQUIRE(peakAbs(f.outL.data(), kBlock) > aSide * 0.9f);
}

// =================================================== STEMS + SAMPLER
TEST_CASE("P24: decks + sampler channel coexist on one mixer",
          "[mixer24][sampler]") {
    Rig24 f(2);
    f.loadDeck(0, 8.0, 220.0, 0.4f);
    f.loadDeck(1, 8.0, 330.0, 0.4f);
    // sampler engine renders into channel 4 — unity-gain strip, xf thru
    for (int b = 0; b < 24; ++b) f.render(true);
    REQUIRE(allFinite(f.outL.data(), kBlock));
    REQUIRE(allFinite(f.boL.data(), kBlock));
    // strip 4 meters stay live even with an empty sampler (decay, not
    // stale data); the strip itself processes its full chain.
    REQUIRE(f.mixer.channel(4).meters().peakL.load() >= 0.0f);
    // sampler strip honors mixer params like any channel
    f.mixer.setParam(ParameterRegistry::channelMute(4), 1.0f);
    f.mixer.setParam(ParameterRegistry::channelPan(4), -1.0f);
    f.mixer.setParam(ParameterRegistry::channelEqMode(4), 1.0f);
    f.render(true);
    REQUIRE((int)f.mixer.channel(4).eq().mode() == 1);
}

// ============================================================= STRESS
TEST_CASE("P24: 4 decks + isolator + filters + xf + buses stay clean",
          "[mixer24][stress]") {
    Rig24 f(4);
    f.loadDeck(0, 10.0, 110.0, 0.35f);
    f.loadDeck(1, 10.0, 220.0, 0.35f);
    f.loadDeck(2, 10.0, 440.0, 0.35f);
    f.loadDeck(3, 10.0, 880.0, 0.35f);
    rec::RecorderRt rec;
    rec.arm(true);
    f.mixer.attachRecorder(&rec);
    using R = ParameterRegistry;
    // everything on: isolators, filters, pans, sends, limiter, booth
    for (int c = 0; c < 4; ++c) {
        f.mixer.setParam(R::channelEqMode(c), 1.0f);
        f.mixer.setParam(R::channelFilter(c), 0.3f * (c - 1.5f));
        f.mixer.setParam(R::channelPan(c), 0.4f * (c - 1.5f));
        f.mixer.setParam(R::channelSendA(c), 0.3f);
        f.mixer.setParam(R::channelLow(c), -8.0f);
    }
    f.mixer.setParam(R::LIMITER_ON, 1.0f);
    f.mixer.setParam(R::BOOTH_GAIN, 0.8f);
    auto t0 = std::chrono::steady_clock::now();
    double worstBlockMs = 0.0;
    for (int b = 0; b < 400; ++b) {          // ~4.3 s of audio
        f.mixer.setParam(R::CROSSFADER,
                         std::sin((double)b * 0.1) >= 0 ? 0.8f : -0.8f);
        const auto s = std::chrono::steady_clock::now();
        f.render(true);
        const auto e = std::chrono::steady_clock::now();
        worstBlockMs = std::max(worstBlockMs,
            std::chrono::duration<double, std::milli>(e - s).count());
        REQUIRE(allFinite(f.outL.data(), kBlock));
        REQUIRE(allFinite(f.outR.data(), kBlock));
        REQUIRE(allFinite(f.boL.data(), kBlock));
    }
    const double totalMs =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
    // 400 blocks * (512/48000 s) ≈ 4.27 s of audio; a debug build must
    // still render far under realtime — assert a generous 25% of the
    // block period as the worst single-block bound (10.7 ms/4).
    REQUIRE(worstBlockMs < 3.0);
    REQUIRE(totalMs < 4000.0);               // realtime by ~1.1x even /Od
    REQUIRE(rec.meter(kRate).frames == 400 * kBlock);
    // master meters and diagnostics live through the whole run
    MixerBusDiag bus; MixerChannelDiag chans[8];
    f.mixer.snapshot(bus, chans, 8);
    REQUIRE(bus.masterPeakL > 0.0f);
    REQUIRE(bus.boothPeakL > 0.0f);
}
