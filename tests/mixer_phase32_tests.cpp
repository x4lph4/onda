// Phase 32 — mixer deltas: crossfader hamster/reverse (§26), the
// AudioBus contract (§5), registry-driven UI ranges (§9), MixerState
// alias + scene coverage (§44), and deterministic latency/scheduling
// guarantees for parameter updates (§58-59).
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <cmath>
#include <type_traits>
#include <vector>

#include "core/audio/TrackAudio.h"
#include "core/dsp/WsolaStretcher.h"
#include "core/engine/DeckEngine.h"
#include "core/mixer/CrossfaderEngine.h"
#include "core/mixer/MixerController.h"
#include "core/mixer/MixerDiagnostics.h"
#include "core/mixer/MixerEngine.h"
#include "core/mixer/MixerScene.h"
#include "core/mixer/ParameterRegistry.h"
#include "core/model/DeckStats.h"
#include "core/rt/CommandQueue.h"

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

/// Strip-level rig — drives one ChannelStrip straight through its
/// AudioBus input (§5), no deck needed.
struct StripRig32 {
    ChannelStrip strip;
    CrossfaderEngine xf;
    std::vector<float> mixL, mixR, cueL, cueR;

    StripRig32() : mixL(kBlock), mixR(kBlock),
                   cueL(kBlock), cueR(kBlock) {
        strip.prepare(kRate);
        xf.prepare(kRate);
        strip.setXfAssign(XfAssign::Thru);
    }
    void feed(float v) {
        AudioBus b = strip.inBus(kBlock);
        std::fill_n(b.L, b.frames, v);
        std::fill_n(b.R, b.frames, v);
    }
    void run(int blocks = 1) {
        for (int b = 0; b < blocks; ++b) {
            std::fill(mixL.begin(), mixL.end(), 0.0f);
            std::fill(mixR.begin(), mixR.end(), 0.0f);
            xf.tickPos(kBlock);
            strip.process(kBlock, mixL.data(), mixR.data(),
                          cueL.data(), cueR.data(), xf, false);
        }
    }
};

/// Full-mixer rig with real decks — the source layer only fills a
/// strip input while a live source is bound, so end-to-end asserts
/// need the real DeckEngine path.
struct Rig32 {
    MixerEngine mixer;
    rt::AudioStats astats;
    std::vector<DeckEngine> decks;
    std::vector<DeckStats>  dstats;
    std::vector<const TrackAudio*> owned;
    std::vector<float> outL, outR, cueL, cueR, boL, boR;

    Rig32(int ndecks = 4) : decks(ndecks), dstats(ndecks),
                            outL(kBlock), outR(kBlock),
                            cueL(kBlock), cueR(kBlock),
                            boL(kBlock), boR(kBlock) {
        mixer.prepare(kRate);
        mixer.setActiveChannels(4);
        mixer.setOutputMap(0, 1, 2, 3, 4, 5);
        for (int i = 0; i < ndecks; ++i) {
            decks[i].bind(&dstats[i]);
            mixer.channel(i).bindStats(&dstats[i]);
            decks[i].setDeviceRate(kRate);
            decks[i].setStretcher(std::make_unique<WsolaStretcher>());
            decks[i].prepareDsp(kRate);
        }
    }
    void loadDeck(int d, float amp) {
        auto* t = makeSine(2.0, 220.0, amp);
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
            const float v =
                amp * (float)std::sin(2.0 * 3.14159265 * freq * i / kRate);
            t->samples[(size_t)i * 2]     = v;
            t->samples[(size_t)i * 2 + 1] = v;
        }
        return t;
    }
    void render() {
        float* outs[6] = { outL.data(), outR.data(), cueL.data(),
                           cueR.data(), boL.data(), boR.data() };
        mixer.render(outs, 6, kBlock, decks.data(), (int)decks.size(),
                     astats, nullptr);
    }
    void run(int blocks) { for (int i = 0; i < blocks; ++i) render(); }
    ~Rig32() { for (auto* t : owned) delete t; }
};

} // namespace

// ============================================================ §26 xf
TEST_CASE("P32: xf reverse swaps A/B rail gains on every curve",
          "[mix32][xf]") {
    for (XfCurve c : { XfCurve::Linear, XfCurve::EqualPower,
                       XfCurve::Cut }) {
        CrossfaderEngine xf;
        xf.prepare(kRate);
        xf.setCurve(c);
        for (float pos : { -1.0f, -0.4f, 0.0f, 0.6f, 1.0f }) {
            xf.setPosition(pos);
            xf.tickPos(480);            // converge the smoother
            const float aN = xf.gainFor(XfAssign::A);
            const float bN = xf.gainFor(XfAssign::B);
            xf.setReverse(true);
            const float aR = xf.gainFor(XfAssign::A);
            const float bR = xf.gainFor(XfAssign::B);
            xf.setReverse(false);
            REQUIRE(aR == Approx(bN).margin(1e-6));
            REQUIRE(bR == Approx(aN).margin(1e-6));
            xf.setReverse(true);
            REQUIRE(xf.gainFor(XfAssign::Thru) == 1.0f);
            xf.setReverse(false);
        }
    }
}

TEST_CASE("P32: xf reverse applies to custom curves too",
          "[mix32][xf]") {
    CrossfaderEngine xf;
    xf.prepare(kRate);
    xf.setCurve(XfCurve::Custom);
    bool sawSideB = false;
    xf.setCustomCurve([&](float p, bool sideA) {
        if (!sideA) sawSideB = true;
        return sideA ? (1.0f - p) * 0.5f : (1.0f + p) * 0.5f;
    });
    xf.setPosition(-0.5f);
    xf.tickPos(4800);               // ~5τ — fully converged
    xf.setReverse(true);
    // reversed A is evaluated through the side-B curve branch
    const float gA = xf.gainFor(XfAssign::A);
    REQUIRE(sawSideB);
    REQUIRE(gA == Approx(0.25f).margin(1e-4));   // (1 + -0.5) * 0.5
}

TEST_CASE("P32: xf reverse audible end-to-end through the mixer",
          "[mix32][xf]") {
    Rig32 r(2);
    r.loadDeck(0, 0.5f);
    r.mixer.setParam(ParameterRegistry::channelXfAssign(0),
                     (float)(int)XfAssign::A);
    r.mixer.setParam(ParameterRegistry::CROSSFADER, -1.0f);

    SECTION("normal: full-left travel passes rail A") {
        r.run(40);
        REQUIRE(peakAbs(r.outL.data(), kBlock) > 0.3f);
    }
    SECTION("reversed: full-left cuts rail A, full-right passes") {
        r.mixer.setParam(ParameterRegistry::XF_REVERSE, 1.0f);
        r.run(40);
        REQUIRE(peakAbs(r.outL.data(), kBlock) < 0.01f);
        r.mixer.setParam(ParameterRegistry::CROSSFADER, 1.0f);
        r.run(40);
        REQUIRE(peakAbs(r.outL.data(), kBlock) > 0.3f);
    }
}

TEST_CASE("P32: xfReverse is registered, quantized, snapshotted",
          "[mix32][xf][params]") {
    const ParamDef* d = ParameterRegistry::find(
        ParameterRegistry::XF_REVERSE);
    REQUIRE(d != nullptr);
    REQUIRE(d->minV == 0.0f);
    REQUIRE(d->maxV == 1.0f);
    REQUIRE(d->step == 1.0f);      // boolean quantized
    REQUIRE(d->def == 0.0f);

    MixerEngine eng;
    eng.prepare(kRate);
    MixerController ctl;
    ctl.setSink([&](uint16_t id, float v) { eng.setParam(id, v); });
    ctl.xfReverse(true);
    REQUIRE(ctl.value(ParameterRegistry::XF_REVERSE) == Approx(1.0f));
    ctl.setParam(ParameterRegistry::XF_REVERSE, 0.7f);
    REQUIRE(ctl.value(ParameterRegistry::XF_REVERSE) == Approx(1.0f));

    MixerBusDiag bus;
    std::vector<MixerChannelDiag> chans(8);
    eng.snapshot(bus, chans.data(), 8);
    REQUIRE(bus.xfReverse);

    MixerState s = ctl.captureScene();
    REQUIRE(s.xfReverse == Approx(1.0f));
    ctl.xfReverse(false);
    ctl.applyScene(s);
    REQUIRE(ctl.value(ParameterRegistry::XF_REVERSE) == Approx(1.0f));
}

// ============================================================ §5 bus
TEST_CASE("P32: AudioBus is the channel-strip input contract",
          "[mix32][bus]") {
    StripRig32 g;
    AudioBus b = g.strip.inBus(256);
    REQUIRE(b.valid());
    REQUIRE(b.frames == 256);
    REQUIRE(b.L == g.strip.inL());
    REQUIRE(b.R == g.strip.inR());

    AudioBus empty{};
    REQUIRE(!empty.valid());

    // audio written through the bus reaches the strip's mix tap
    g.feed(0.4f);
    g.run(40);
    REQUIRE(peakAbs(g.mixL.data(), kBlock) > 0.3f);
}

// ============================================================ §9 rng
TEST_CASE("P32: registry publishes the ranges the UI binds to",
          "[mix32][params]") {
    using R = ParameterRegistry;
    auto rng = [](uint16_t id) {
        const ParamDef* d = R::find(id);
        REQUIRE(d != nullptr);
        return std::pair<float, float>{ d->minV, d->maxV };
    };
    REQUIRE(rng(R::channelGain(0))   == std::pair{ 0.0f, 2.0f });
    REQUIRE(rng(R::channelHigh(2))   == std::pair{ -26.0f, 6.0f });
    REQUIRE(rng(R::channelMid(3))    == std::pair{ -26.0f, 6.0f });
    REQUIRE(rng(R::channelLow(1))    == std::pair{ -26.0f, 6.0f });
    REQUIRE(rng(R::channelFilter(0)) == std::pair{ -1.0f, 1.0f });
    REQUIRE(rng(R::channelPan(0))    == std::pair{ -1.0f, 1.0f });
    REQUIRE(rng(R::channelSendA(0))  == std::pair{ 0.0f, 1.0f });
    REQUIRE(rng(R::channelSendB(0))  == std::pair{ 0.0f, 1.0f });
    REQUIRE(rng(R::channelFader(0))  == std::pair{ 0.0f, 1.0f });
    REQUIRE(rng(R::MASTER_GAIN)      == std::pair{ 0.0f, 2.0f });
    REQUIRE(rng(R::CUE_VOLUME)       == std::pair{ 0.0f, 2.0f });
    REQUIRE(rng(R::CUE_MIX)          == std::pair{ 0.0f, 1.0f });
    REQUIRE(rng(R::BOOTH_GAIN)       == std::pair{ 0.0f, 2.0f });
    REQUIRE(rng(R::CROSSFADER)       == std::pair{ -1.0f, 1.0f });
}

// ====================================================== §58 latency
TEST_CASE("P32: SetParam survives the SPSC queue loss-free",
          "[mix32][rt]") {
    rt::CommandQueue q;
    REQUIRE(q.tryPush(rt::SetParam{ ParameterRegistry::XF_REVERSE,
                                  1.0f }));
    rt::Command cmd;
    REQUIRE(q.tryPop(cmd));
    const auto* sp = std::get_if<rt::SetParam>(&cmd);
    REQUIRE(sp != nullptr);
    REQUIRE(sp->id == ParameterRegistry::XF_REVERSE);
    REQUIRE(sp->v == Approx(1.0f));
}

TEST_CASE("P32: smoothed params converge inside a bounded window",
          "[mix32][rt][latency]") {
    // §58 — worst-case unscheduled update latency is deterministic:
    //   ≤1 block queue drain + smoother convergence (5 ms time
    //   constant; ~5τ ≈ 25 ms to full settle @ 48 kHz). Scheduled
    //   ParamEvents land on their exact target frame instead. Verify
    //   the smoother lands inside that budget, monotonically, without
    //   overshoot.
    CrossfaderEngine xf;
    xf.prepare(kRate);
    xf.setCurve(XfCurve::Linear);
    xf.setPosition(1.0f);
    const int tau    = (int)(kRate * 0.005);   // 240 samples
    const int settle = tau * 5;                // ~99% converged
    xf.tickPos(settle);
    REQUIRE(xf.gainFor(XfAssign::B) == Approx(1.0f).margin(0.02f));
    xf.setPosition(-1.0f);
    float prev = xf.gainFor(XfAssign::A);   // rail A opens at -1
    for (int i = 0; i < settle; ++i) {
        xf.tickPos(1);
        const float now = xf.gainFor(XfAssign::A);
        REQUIRE(now >= prev - 1e-4f);   // rises monotonically
        REQUIRE(now <= 1.0f + 1e-4f);   // never overshoots unity
        prev = now;
    }
    REQUIRE(xf.gainFor(XfAssign::A) == Approx(1.0f).margin(0.02f));
}

TEST_CASE("P32: rapid xf reverse flips stay finite mid-render",
          "[mix32][stress]") {
    Rig32 r(1);
    r.loadDeck(0, 0.5f);
    r.mixer.setParam(ParameterRegistry::channelXfAssign(0),
                     (float)(int)XfAssign::A);
    r.mixer.setParam(ParameterRegistry::CROSSFADER, 0.0f);
    bool rev = false;
    for (int b = 0; b < 60; ++b) {
        r.mixer.setParam(ParameterRegistry::XF_REVERSE,
                         rev ? 1.0f : 0.0f);
        rev = !rev;
        r.render();
        for (int i = 0; i < kBlock; ++i) {
            REQUIRE(std::isfinite(r.outL[(size_t)i]));
            REQUIRE(std::isfinite(r.outR[(size_t)i]));
        }
    }
}

TEST_CASE("P32: MixerState alias maps to MixerScene (§44)",
          "[mix32][state]") {
    static_assert(std::is_same_v<MixerState, MixerScene>);
    MixerState s;
    s.xfReverse = 1.0f;
    REQUIRE(sceneField(s, BusParam::XfReverse) == Approx(1.0f));
}
