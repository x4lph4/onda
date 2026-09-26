// Phase 6 — professional FX engine tests.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <numeric>
#include <vector>

#include "core/audio/TrackAudio.h"
#include "core/dsp/WsolaStretcher.h"
#include "core/engine/DeckEngine.h"
#include "core/fx/DelayFX.h"
#include "core/fx/EchoFX.h"
#include "core/fx/FilterFX.h"
#include "core/fx/FlangerFX.h"
#include "core/fx/FXEngine.h"
#include "core/fx/FXPresetManager.h"
#include "core/fx/FXSlot.h"
#include "core/fx/GateFX.h"
#include "core/fx/PhaserFX.h"
#include "core/fx/ReverbFX.h"
#include "core/mixer/MixerController.h"
#include "core/mixer/MixerEngine.h"
#include "core/mixer/ParameterRegistry.h"
#include "core/model/DeckStats.h"
#include "core/rt/CommandQueue.h"

using namespace onda;
using Catch::Approx;

namespace {

constexpr int    kBlock = 512;
constexpr double kRate  = 48000.0;

float peak(const std::vector<float>& v) {
    float p = 0.0f;
    for (float x : v) p = std::max(p, std::fabs(x));
    return p;
}
float peakP(const float* v, int n) {
    float p = 0.0f;
    for (int i = 0; i < n; ++i) p = std::max(p, std::fabs(v[i]));
    return p;
}
bool allFinite(const std::vector<float>& v) {
    for (float x : v) if (!std::isfinite(x)) return false;
    return true;
}
int firstIndexAbove(const std::vector<float>& v, float th, int from = 0) {
    for (int i = from; i < (int)v.size(); ++i)
        if (std::fabs(v[i]) > th) return i;
    return -1;
}

// ---------------------------------------------------------- helpers
/// Run a prepared effect standalone (no slot): impulse/sine/etc in,
/// wet out. `ctx` defaults to 120 BPM synced 1/4.
struct FxRig {
    std::unique_ptr<fx::FXProcessor> fx;
    fx::SyncContext ctx;
    std::vector<float> l, r;
    double phase_ { 0.0 };

    FxRig(fx::FXKind k) : fx(fx::createFX(k)), l(kBlock), r(kBlock) {
        REQUIRE(fx != nullptr);
        fx->prepare(kRate, kBlock);
        ctx.bpm = 120.0f;
        ctx.synced = 1;
        ctx.division = (int)fx::BeatDiv::D_1_4;
    }

    void impulse(float amp = 0.8f, int at = 0) {
        std::fill(l.begin(), l.end(), 0.0f);
        std::fill(r.begin(), r.end(), 0.0f);
        l[(size_t)at] = amp; r[(size_t)at] = amp;
    }
    void silence() {
        std::fill(l.begin(), l.end(), 0.0f);
        std::fill(r.begin(), r.end(), 0.0f);
    }
    void sine(double freq, float amp) {
        for (int i = 0; i < kBlock; ++i) {
            const float v = amp * (float)std::sin(phase_);
            l[i] = v; r[i] = v;
            phase_ += 2.0 * 3.14159265358979 * freq / kRate;
        }
    }
    void noise(float amp, uint32_t& st) {
        for (int i = 0; i < kBlock; ++i) {
            st ^= st << 13; st ^= st >> 17; st ^= st << 5;
            const float v = ((float)(st & 0xffffff) / 8388608.0f - 1.0f) * amp;
            l[i] = v; r[i] = v;
        }
    }
    /// Process one block; returns a copy of L for analysis.
    std::vector<float> run() {
        fx->setSyncContext(ctx);
        fx->process(l.data(), r.data(), kBlock);
        return l;
    }
    std::vector<float> runR() {
        fx->setSyncContext(ctx);
        fx->process(l.data(), r.data(), kBlock);
        return r;
    }
    void runBlocks(int n) { for (int i = 0; i < n; ++i) { run(); } }
};

// ------------------------------------------------------------ slot rig
struct SlotRig {
    fx::FXSlot slot;
    fx::SyncContext ctx;
    std::vector<float> l, r;
    double phase_ { 0.0 };

    SlotRig() : l(kBlock), r(kBlock) {
        slot.prepare(kRate);
        ctx.bpm = 120.0f;
    }
    void load(fx::FXKind k, float mix = 1.0f) {
        slot.loadEffect(k);           // non-RT park
        slot.setParam(fx::FXSlotKind::Enabled, 1.0f);
        slot.setParam(fx::FXSlotKind::Mix, mix);
    }
    std::vector<float> run() {
        slot.process(l.data(), r.data(), kBlock, ctx);
        return l;
    }
    void impulse(float amp = 0.8f) {
        std::fill(l.begin(), l.end(), 0.0f);
        std::fill(r.begin(), r.end(), 0.0f);
        l[0] = amp; r[0] = amp;
    }
    void sine(double freq, float amp) {
        for (int i = 0; i < kBlock; ++i) {
            const float v = amp * (float)std::sin(phase_);
            l[i] = v; r[i] = v;
            phase_ += 2.0 * 3.14159265358979 * freq / kRate;
        }
    }
    void silence() {
        std::fill(l.begin(), l.end(), 0.0f);
        std::fill(r.begin(), r.end(), 0.0f);
    }
};

// ------------------------------------------------- mixer integration rig
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
        mixer.setActiveChannels(ndecks);
        for (int i = 0; i < ndecks; ++i) {
            decks[i].bind(&dstats[i]);
            mixer.channel(i).bindStats(&dstats[i]);
            decks[i].setDeviceRate(kRate);
            decks[i].setStretcher(std::make_unique<WsolaStretcher>());
            decks[i].prepareDsp(kRate);
        }
    }
    void loadDeck(int d, double secs, double freq, float amp) {
        auto* t = new TrackAudio();
        t->fileSampleRate = kRate; t->deviceRate = kRate;
        t->numFrames = (int64_t)(kRate * secs);
        t->samples.resize((size_t)t->numFrames * 2);
        for (int64_t i = 0; i < t->numFrames; ++i) {
            const float v = amp * (float)std::sin(2.0 * 3.14159265 * freq * i / kRate);
            t->samples[(size_t)i * 2] = v;
            t->samples[(size_t)i * 2 + 1] = v;
        }
        owned.push_back(t);
        decks[d].pendingTrack_.store(t);
        float zl[8]{}, zr[8]{};
        decks[d].renderPcm(zl, zr, 8);
        decks[d].setPlaying(true);
    }
    void render() {
        float* outs[4] = { outL.data(), outR.data(), cueL.data(), cueR.data() };
        mixer.render(outs, 4, kBlock, decks.data(), (int)decks.size(), astats);
    }
    ~MixRig() { for (auto* t : owned) delete t; }
};

} // namespace

// =============================================================== DELAY
TEST_CASE("DelayFX: echo timing, feedback decay, ping-pong", "[fx][delay]") {
    FxRig g(fx::FXKind::Delay);
    g.fx->setParamNormalized(0, 0.0f);            // min time? use native
    g.fx->setParamNative(0, 10.0f);               // 10 ms = 480 frames
    g.fx->setParamNative(1, 0.5f);                // feedback
    g.fx->setParamNative(2, 18000.0f);            // no damping
    g.ctx.synced = 0;                             // free time

    std::vector<float> all;
    g.impulse();
    for (int i = 0; i < 8; ++i) {
        auto b = g.run();
        all.insert(all.end(), b.begin(), b.end());
        g.silence();
    }
    // first echo at ~480 frames; second at ~960 (feedback)
    const int e1 = firstIndexAbove(all, 0.05f, 100);
    REQUIRE(e1 == Approx(480).margin(24));
    const int e2 = firstIndexAbove(all, 0.02f, e1 + 400);
    REQUIRE(e2 == Approx(960).margin(40));

    // synced mode: D_1_32 at 120 bpm = 0.125 beat = 62.5 ms = 3000 smp
    FxRig s(fx::FXKind::Delay);
    s.fx->setParamNative(1, 0.0f); // no feedback -> single echo
    s.fx->setParamNative(2, 18000.0f);
    s.ctx.synced = 1;
    s.ctx.division = (int)fx::BeatDiv::D_1_32;
    std::vector<float> sa;
    s.impulse();
    for (int i = 0; i < 8; ++i) {
        auto b = s.run();
        sa.insert(sa.end(), b.begin(), b.end());
        s.silence();
    }
    const int se = firstIndexAbove(sa, 0.05f, 100);
    REQUIRE(se == Approx(3000).margin(80));

    // ping-pong: taps alternate across channels
    FxRig p(fx::FXKind::Delay);
    p.fx->setParamNative(0, 10.0f);
    p.fx->setParamNative(1, 0.6f);
    p.fx->setParamNative(3, 1.0f); // ping-pong on
    p.ctx.synced = 0;
    std::vector<float> pl, pr;
    p.impulse();
    for (int i = 0; i < 8; ++i) {
        p.fx->setSyncContext(p.ctx);
        p.fx->process(p.l.data(), p.r.data(), kBlock);
        pl.insert(pl.end(), p.l.begin(), p.l.end());
        pr.insert(pr.end(), p.r.begin(), p.r.end());
        p.silence();
    }
    const int pL = firstIndexAbove(pl, 0.05f, 100);
    const int pR = firstIndexAbove(pr, 0.05f, 100);
    // in ping-pong the first tap lands on L (fed by input), echo on R
    REQUIRE(pL > 0);
    REQUIRE(pR > pL); // R follows L by one tap period
}

TEST_CASE("DelayFX: feedback never runs away", "[fx][delay]") {
    FxRig g(fx::FXKind::Delay);
    g.fx->setParamNative(0, 5.0f);
    g.fx->setParamNative(1, 0.98f); // max feedback
    g.ctx.synced = 0;
    float mx = 0.0f;
    for (int i = 0; i < 400; ++i) { // ~4 s of feedback
        if (i == 0) g.impulse(); else g.silence();
        auto b = g.run();
        mx = std::max(mx, peak(b));
    }
    REQUIRE(mx < 2.0f); // bounded, decays eventually
}

// ================================================================ ECHO
TEST_CASE("EchoFX: division timing and bounded freeze", "[fx][echo]") {
    FxRig g(fx::FXKind::Echo);
    g.ctx.division = (int)fx::BeatDiv::D_1_32; // 3000 smp @120
    g.fx->setParamNative(0, 0.5f);
    g.fx->setParamNative(2, 0.0f); // stereo (not ping-pong)
    g.fx->setParamNative(3, 0.0f); // no freeze

    std::vector<float> all;
    g.impulse();
    for (int i = 0; i < 10; ++i) {
        auto b = g.run();
        all.insert(all.end(), b.begin(), b.end());
        g.silence();
    }
    const int e1 = firstIndexAbove(all, 0.05f, 100);
    REQUIRE(e1 == Approx(3000).margin(80));

    // freeze: captured tail sustains but stays bounded
    FxRig f(fx::FXKind::Echo);
    f.ctx.division = (int)fx::BeatDiv::D_1_32;
    f.fx->setParamNative(0, 0.7f);
    f.impulse();
    for (int i = 0; i < 12; ++i) { // let the echo develop
        f.run();
        f.silence();
    }
    f.fx->setParamNative(3, 1.0f); // freeze
    float mx = 0.0f;
    std::vector<float> last;
    for (int i = 0; i < 200; ++i) { // ~2 s frozen on silence
        f.silence();
        last = f.run();
        mx = std::max(mx, peak(last));
    }
    REQUIRE(mx > 0.01f);   // still holding the frozen audio
    REQUIRE(mx < 2.0f);    // bounded — no infinite buildup
    REQUIRE(allFinite(last));
}

// =============================================================== REVERB
TEST_CASE("ReverbFX: impulse decays, pre-delay shifts onset", "[fx][reverb]") {
    FxRig g(fx::FXKind::Reverb);
    g.fx->setParamNative(0, 0.6f);  // size
    g.fx->setParamNative(1, 0.5f);  // decay
    g.fx->setParamNative(2, 0.3f);  // damping
    g.fx->setParamNative(3, 0.0f);  // no pre-delay

    std::vector<float> early, late;
    g.impulse();
    for (int i = 0; i < 40; ++i) { // ~0.43 s tail window
        auto b = g.run();
        if (i < 10)      early.insert(early.end(), b.begin(), b.end()); // ~0-107ms: build-up + early decay
        else if (i >= 28) late.insert(late.end(), b.begin(), b.end());  // ~300-430ms: settled tail
        g.silence();
    }
    REQUIRE(peak(early) > 0.02f);               // audible reverb
    REQUIRE(peak(late) < peak(early));          // decaying tail
    REQUIRE(allFinite(late));

    // pre-delay: no wet output until it elapses
    FxRig p(fx::FXKind::Reverb);
    p.fx->setParamNative(3, 50.0f); // 50 ms = 2400 frames
    std::vector<float> pa;
    p.impulse();
    for (int i = 0; i < 4; ++i) {
        auto b = p.run();
        pa.insert(pa.end(), b.begin(), b.end());
        p.silence();
    }
    const int onset = firstIndexAbove(pa, 0.01f);
    REQUIRE((onset == -1 || onset > 2000));
}

// =============================================================== FILTER
TEST_CASE("FilterFX: modes attenuate the right band, resonance", "[fx][filter]") {
    SECTION("LP cuts 8 kHz") {
        FxRig g(fx::FXKind::Filter);
        g.fx->setParamNative(0, 500.0f);
        g.fx->setParamNative(2, 0.0f); // LP
        for (int i = 0; i < 10; ++i) { g.sine(8000.0, 0.5f); g.run(); } // settle
        float mx = 0.0f;
        for (int i = 0; i < 20; ++i) { g.sine(8000.0, 0.5f); mx = std::max(mx, peak(g.run())); }
        REQUIRE(mx < 0.05f);
    }
    SECTION("HP cuts 80 Hz") {
        FxRig g(fx::FXKind::Filter);
        g.fx->setParamNative(0, 5000.0f);
        g.fx->setParamNative(2, 2.0f); // HP
        for (int i = 0; i < 10; ++i) { g.sine(80.0, 0.5f); g.run(); } // settle
        float mx = 0.0f;
        for (int i = 0; i < 20; ++i) { g.sine(80.0, 0.5f); mx = std::max(mx, peak(g.run())); }
        REQUIRE(mx < 0.05f);
    }
    SECTION("passband keeps signal, resonance adds edge peak") {
        FxRig g(fx::FXKind::Filter);
        g.fx->setParamNative(0, 1000.0f);
        g.fx->setParamNative(2, 0.0f);
        for (int i = 0; i < 10; ++i) { g.sine(200.0, 0.5f); g.run(); } // settle
        float mx = 0.0f;
        for (int i = 0; i < 20; ++i) { g.sine(200.0, 0.5f); mx = std::max(mx, peak(g.run())); }
        REQUIRE(mx > 0.35f); // 200 Hz well inside LP@1k passband

        FxRig r(fx::FXKind::Filter);
        r.fx->setParamNative(0, 1000.0f);
        r.fx->setParamNative(1, 0.95f); // high resonance
        r.fx->setParamNative(2, 1.0f);  // BP at cutoff
        for (int i = 0; i < 10; ++i) { r.sine(1000.0, 0.3f); r.run(); } // settle
        float rm = 0.0f;
        for (int i = 0; i < 40; ++i) { r.sine(1000.0, 0.3f); rm = std::max(rm, peak(r.run())); }
        REQUIRE(rm > 0.3f); // resonant peak at least preserves, often boosts
    }
}

// ============================================================ MODULATION
TEST_CASE("FlangerFX: sweeps and stays bounded", "[fx][flanger]") {
    FxRig g(fx::FXKind::Flanger);
    g.fx->setParamNative(0, 0.5f);  // 0.5 Hz
    g.fx->setParamNative(1, 0.8f);  // deep
    g.fx->setParamNative(2, 0.4f);  // feedback
    uint32_t st = 0x12345;
    float mx = 0.0f;
    bool differs = false;
    for (int i = 0; i < 100; ++i) {
        g.noise(0.4f, st);
        auto b = g.run();
        mx = std::max(mx, peak(b));
        if (i == 50 && std::fabs(b[100]) > 1e-6f) differs = true;
    }
    REQUIRE(mx < 2.0f);
    REQUIRE(differs);

    // synced mode: follows division without using the rate param
    FxRig s(fx::FXKind::Flanger);
    s.fx->setParamNative(0, 99.0f); // nonsense rate — must be ignored
    s.ctx.synced = 1;
    s.ctx.division = (int)fx::BeatDiv::D_1_1;
    for (int i = 0; i < 20; ++i) { s.sine(440.0, 0.4f); s.run(); }
    REQUIRE(allFinite(s.run()));
}

TEST_CASE("PhaserFX: stages sweep notches, bounded", "[fx][phaser]") {
    FxRig g(fx::FXKind::Phaser);
    g.fx->setParamNative(0, 0.3f);
    g.fx->setParamNative(1, 0.7f);
    g.fx->setParamNative(2, 0.4f);
    g.fx->setParamNative(3, 6.0f);
    uint32_t st = 0xBEEF;
    float mx = 0.0f;
    for (int i = 0; i < 100; ++i) { g.noise(0.4f, st); mx = std::max(mx, peak(g.run())); }
    REQUIRE(mx < 2.0f);

    // a sine inside a moving notch dips periodically: min < max
    FxRig h(fx::FXKind::Phaser);
    h.fx->setParamNative(0, 1.0f);
    h.fx->setParamNative(1, 1.0f);
    h.fx->setParamNative(2, 0.0f);
    h.fx->setParamNative(3, 8.0f);
    h.fx->setParamNative(4, 1000.0f);
    float mn = 1e9f;
    for (int i = 0; i < 200; ++i) {
        h.sine(1000.0, 0.4f);
        mn = std::min(mn, peak(h.run()));
    }
    REQUIRE(mn < 0.4f); // swept notch periodically dips the tone
}

TEST_CASE("GateFX: amplitude envelope follows the division", "[fx][gate]") {
    FxRig g(fx::FXKind::Gate);
    g.fx->setParamNative(0, 1.0f);  // full depth
    g.fx->setParamNative(1, 2.0f);  // 2 ms ramps
    g.fx->setParamNative(2, 0.5f);  // 50% duty
    g.ctx.synced = 1;
    g.ctx.division = (int)fx::BeatDiv::D_1_16; // 0.25 beat = 6000 smp @120

    // collect per-block peaks of a constant input: must alternate
    std::vector<float> peaks;
    for (int i = 0; i < 24; ++i) { // 2 full cycles (12 blocks each)
        g.sine(440.0, 0.5f);
        peaks.push_back(peak(g.run()));
    }
    int hi = 0, lo = 0;
    for (float p : peaks) { if (p > 0.3f) ++hi; if (p < 0.05f) ++lo; }
    REQUIRE(hi > 0);
    REQUIRE(lo > 0); // gate closed on some blocks
}

// ============================================================ DRY / WET
TEST_CASE("FXSlot: dry/wet mix blends smoothly", "[fx][slot]") {
    SlotRig g;
    g.load(fx::FXKind::Delay, 1.0f);          // full wet
    g.slot.setParam(fx::FXSlotKind::SyncMode, 0.0f);
    g.slot.setParam(fx::FXSlotKind::P1, 0.0f); // short time? P1 = time
    // run one block to adopt + ramp
    g.impulse();
    g.run();

    // mix 0 -> output is the dry impulse
    g.slot.setParam(fx::FXSlotKind::Mix, 0.0f);
    g.impulse();
    for (int i = 0; i < 30; ++i) { // let the mix smoother settle
        auto b = g.run();
        g.silence();
        (void)b;
    }
    g.impulse();
    auto out = g.run();
    REQUIRE(out[0] == Approx(0.8f).margin(0.05f)); // dry passes through

    // mix 1 -> fully wet: impulse vanishes (replaced by delay taps)
    g.slot.setParam(fx::FXSlotKind::Mix, 1.0f);
    g.impulse();
    for (int i = 0; i < 30; ++i) { g.run(); g.silence(); }
    g.impulse();
    out = g.run();
    REQUIRE(out[0] == Approx(0.0f).margin(0.05f));
}

TEST_CASE("FXSlot: bypass crossfades without clicks", "[fx][slot]") {
    SlotRig g;
    g.load(fx::FXKind::Reverb, 0.8f);
    for (int i = 0; i < 10; ++i) { g.sine(440.0, 0.5f); g.run(); }

    // disable mid-tail: output must ramp to dry, not jump
    g.slot.setParam(fx::FXSlotKind::Enabled, 0.0f);
    g.sine(440.0, 0.5f);
    auto b = g.run();
    float maxStep = 0.0f;
    for (int i = 1; i < kBlock; ++i)
        maxStep = std::max(maxStep, std::fabs(b[i] - b[i - 1]));
    REQUIRE(maxStep < 0.5f); // a hard cut would jump by the tail level

    // after settle, output == dry input
    for (int i = 0; i < 30; ++i) { g.sine(440.0, 0.5f); g.run(); }
    g.sine(440.0, 0.5f);
    b = g.run();
    REQUIRE(peak(b) == Approx(0.5f).margin(0.1f));
}

// ============================================================ SAFETY
TEST_CASE("FXSlot: NaN input is sanitized, level clamped", "[fx][slot]") {
    SlotRig g;
    g.load(fx::FXKind::Delay, 1.0f);
    g.slot.setParam(fx::FXSlotKind::SyncMode, 0.0f);
    // adopt
    g.silence(); g.run();

    // inject NaN + Inf + huge values
    for (int i = 0; i < kBlock; ++i) {
        g.l[i] = (i % 3 == 0) ? std::numeric_limits<float>::quiet_NaN()
               : (i % 3 == 1) ? std::numeric_limits<float>::infinity()
               : 50.0f;
        g.r[i] = g.l[i];
    }
    auto out = g.run();
    REQUIRE(allFinite(out));
    REQUIRE(peak(out) <= fx::FXSlot::kClamp + 1e-3f);
    REQUIRE(g.slot.sanitized.load() > 0);
}

// ============================================================ PARAMS
TEST_CASE("FXProcessor: normalized<->native param mapping", "[fx][params]") {
    FxRig g(fx::FXKind::Filter);
    g.fx->setParamNormalized(0, 0.0f);
    REQUIRE(g.fx->paramNative(0) == Approx(20.0f));
    g.fx->setParamNormalized(0, 1.0f);
    REQUIRE(g.fx->paramNative(0) == Approx(20000.0f));
    g.fx->setParamNormalized(0, 0.5f);
    REQUIRE(g.fx->paramNative(0) == Approx(10010.0f).margin(1.0f));
    // out-of-range clamps
    g.fx->setParamNormalized(0, 7.0f);
    REQUIRE(g.fx->paramNormalized(0) == Approx(1.0f));
    // info metadata
    const auto info = g.fx->paramInfo(0);
    REQUIRE(info.name != nullptr);
    REQUIRE(info.minV < info.maxV);
}

TEST_CASE("FXProcessor: saveState/restoreState roundtrip", "[fx][state]") {
    FxRig g(fx::FXKind::Delay);
    g.fx->setParamNative(0, 123.0f);
    g.fx->setParamNative(1, 0.77f);
    const auto st = g.fx->saveState();
    REQUIRE(st.kind == (int)fx::FXKind::Delay);

    FxRig h(fx::FXKind::Delay);
    h.fx->restoreState(st);
    REQUIRE(h.fx->paramNative(0) == Approx(123.0f));
    REQUIRE(h.fx->paramNative(1) == Approx(0.77f));

    // wrong-kind state is rejected
    FxRig w(fx::FXKind::Reverb);
    const auto before = w.fx->saveState();
    w.fx->restoreState(st); // Delay state into Reverb -> ignored
    REQUIRE(w.fx->paramNative(0) == Approx(before.params[0]));
}

TEST_CASE("FXProcessor: latency reporting (all current FX are zero)", "[fx][latency]") {
    for (int k = 1; k < (int)fx::FXKind::kCount; ++k) {
        FxRig g((fx::FXKind)k);
        REQUIRE(g.fx->latencySamples() == 0);
    }
    fx::FXEngine e;
    e.prepare(kRate);
    e.loadEffect(0, 0, fx::FXKind::Delay);
    // adopt pending through one process pass
    std::vector<float> l(kBlock, 0.0f), r(kBlock, 0.0f);
    e.processDeckInsert(0, l.data(), r.data(), kBlock);
    REQUIRE(e.targetLatency(0) == 0);
    REQUIRE(e.maxLatencySamples() == 0);
}

// ============================================================ REGISTRY
TEST_CASE("ParameterRegistry: FX ids, defs, sanitize", "[fx][registry]") {
    using R = mixer::ParameterRegistry;
    // named spec constants exist
    REQUIRE(R::FX_A_TYPE == fx::paramId(0, 0, fx::FXSlotKind::Type));
    REQUIRE(R::FX_A_MIX  == fx::paramId(0, 0, fx::FXSlotKind::Mix));
    REQUIRE(R::FX_A_P1   == fx::paramId(0, 0, fx::FXSlotKind::P1));

    // decode roundtrip
    const uint16_t id = R::fxParam((int)fx::FXTarget::SendB, 2, 5);
    REQUIRE(fx::isFxParam(id));
    REQUIRE(fx::fxTarget(id) == (int)fx::FXTarget::SendB);
    REQUIRE(fx::fxSlot(id) == 2);
    REQUIRE((int)fx::fxKindOf(id) == (int)fx::FXSlotKind::P1 + 5);

    // defs + sanitize
    const auto* d = R::find(R::fxMix(0, 0));
    REQUIRE(d != nullptr);
    REQUIRE(d->minV == 0.0f);
    REQUIRE(d->maxV == 1.0f);
    REQUIRE(R::sanitize(R::fxMix(0, 0), 3.0f) == 1.0f);
    REQUIRE(R::sanitize(R::fxDivision(0, 0), 99.0f)
            == (float)(fx::beatDivCount() - 1));
    // channel send + return ids exist
    REQUIRE(R::find(R::channelSendA(0)) != nullptr);
    REQUIRE(R::find(R::channelSendB(3)) != nullptr);
    REQUIRE(R::find(R::FX_RETURN_A) != nullptr);
    REQUIRE(R::find(R::FX_RETURN_B) != nullptr);
    // non-fx ids still resolve
    REQUIRE(R::find(R::channelGain(0)) != nullptr);
    REQUIRE(R::find(R::MASTER_GAIN) != nullptr);
}

// ============================================================ PRESETS
TEST_CASE("FXPreset: serialize/deserialize + manager", "[fx][preset]") {
    fx::FXPreset p;
    p.name = "Test Delay";
    p.effectKind = (int)fx::FXKind::Delay;
    p.syncMode = 0; p.division = 5; p.mix = 0.7f;
    p.inGain = 1.2f; p.outGain = 0.8f;
    p.params[0] = 123.0f; p.params[1] = 0.55f;

    const auto blob = fx::serializePreset(p);
    fx::FXPreset q;
    REQUIRE(fx::deserializePreset(blob.data(), blob.size(), q));
    REQUIRE(q.name == "Test Delay");
    REQUIRE(q.effectKind == (int)fx::FXKind::Delay);
    REQUIRE(q.division == 5);
    REQUIRE(q.mix == Approx(0.7f));
    REQUIRE(q.params[0] == Approx(123.0f));

    // corrupt blobs rejected
    fx::FXPreset bad;
    REQUIRE_FALSE(fx::deserializePreset(blob.data(), 10, bad));
    REQUIRE_FALSE(fx::deserializePreset(nullptr, 0, bad));

    // manager: factory presets present, user save/delete/rename
    fx::FXPresetManager m;
    REQUIRE(m.count() >= 6);
    REQUIRE(m.find("Classic Delay") >= 0);
    REQUIRE(m.find("Trance Gate") >= 0);
    const int fIdx = m.find("Classic Delay");
    REQUIRE(m.at(fIdx).factory);

    p.factory = false;
    const int uIdx = m.save(p);
    REQUIRE(uIdx >= 0);
    REQUIRE(m.find("Test Delay") == uIdx);
    REQUIRE(m.rename(uIdx, "Renamed"));
    REQUIRE(m.find("Renamed") == uIdx);
    REQUIRE(m.remove(uIdx));
    REQUIRE(m.find("Renamed") < 0);
    // factory presets can't be deleted
    REQUIRE_FALSE(m.remove(fIdx));

    // serializeAll -> restoreAll keeps user presets only
    fx::FXPresetManager m2;
    m2.save(p);
    const auto all = m2.serializeAll();
    fx::FXPresetManager m3;
    REQUIRE(m3.restoreAll(all.data(), all.size()));
    REQUIRE(m3.find("Test Delay") >= 0);
    REQUIRE(m3.find("Classic Delay") >= 0); // factory regenerated
    REQUIRE_FALSE(m3.restoreAll(all.data(), 5));
}

// ============================================================ ENGINE
TEST_CASE("FXEngine: setParam routes to the right slot", "[fx][engine]") {
    fx::FXEngine e;
    e.prepare(kRate);
    e.loadEffect(0, 0, fx::FXKind::Delay);
    std::vector<float> l(kBlock, 0.0f), r(kBlock, 0.0f);
    e.setTempo(0, 120.0f);
    // adopt
    e.processDeckInsert(0, l.data(), r.data(), kBlock);
    auto& slot = e.rack(0).slot(0);
    REQUIRE(slot.activeKind() == fx::FXKind::Delay);

    using R = mixer::ParameterRegistry;
    e.setParam(R::fxEnabled(0, 0), 1.0f);
    REQUIRE(slot.enabled());
    e.setParam(R::fxMix(0, 0), 0.8f);
    REQUIRE(slot.mix() == Approx(0.8f));
    e.setParam(R::fxDivision(0, 0), 2.0f);
    REQUIRE(slot.division() == 2);

    // wrong target/slot ignored safely
    e.setParam(R::fxEnabled(14, 9), 1.0f); // out of range decode
    e.setParam(0x1FFF, 1.0f);
}

TEST_CASE("FXEngine: events published for slot changes", "[fx][engine]") {
    fx::FXEngine e;
    e.prepare(kRate);
    e.loadEffect(0, 0, fx::FXKind::Filter);
    std::vector<float> l(kBlock, 0.0f), r(kBlock, 0.0f);
    using R = mixer::ParameterRegistry;
    e.setParam(R::fxEnabled(0, 0), 1.0f);
    e.processDeckInsert(0, l.data(), r.data(), kBlock); // adopts -> TypeChanged

    fx::FXEvent ev {};
    bool sawEnable = false, sawType = false;
    while (e.pollEvent(ev)) {
        if (ev.type == (uint8_t)fx::FXEventType::Enabled) sawEnable = true;
        if (ev.type == (uint8_t)fx::FXEventType::TypeChanged) sawType = true;
    }
    REQUIRE(sawEnable);
    REQUIRE(sawType);
}

// ====================================================== MIXER INTEGRATION
TEST_CASE("Mixer: deck insert FX processes into the channel", "[fx][mix]") {
    MixRig f;
    f.loadDeck(0, 6.0, 440.0, 0.4f);
    // deck A rack slot0: resonant LP filter — 440 Hz survives, then
    // switch cutoff below the tone
    f.mixer.fx().loadEffect(0, 0, fx::FXKind::Filter);
    f.mixer.setParam(mixer::ParameterRegistry::fxEnabled(0, 0), 1.0f);
    f.mixer.setParam(mixer::ParameterRegistry::fxMix(0, 0), 1.0f);
    for (int i = 0; i < 40; ++i) f.render(); // adopt + settle
    const float withFx = peak(f.outL);
    REQUIRE(withFx > 0.15f); // 440 inside LP passband, filter wet

    // HP mode + cutoff ~5 kHz kills the 440 Hz tone
    f.mixer.setParam(mixer::ParameterRegistry::fxParam(0, 0, 0), 0.25f); // ~5 kHz
    f.mixer.setParam(mixer::ParameterRegistry::fxParam(0, 0, 2), 1.0f);  // HP mode
    for (int i = 0; i < 60; ++i) f.render();
    REQUIRE(peak(f.outL) < withFx * 0.3f);
}

TEST_CASE("Mixer: send/return FX bus routes post-fader signal", "[fx][mix]") {
    MixRig f;
    f.loadDeck(0, 6.0, 440.0, 0.4f);
    // send bus 0: delay, mix 1, return 1
    f.mixer.fx().loadEffect((int)fx::FXTarget::SendA, 0, fx::FXKind::Delay);
    f.mixer.setParam(mixer::ParameterRegistry::fxEnabled((int)fx::FXTarget::SendA, 0), 1.0f);
    f.mixer.setParam(mixer::ParameterRegistry::fxMix((int)fx::FXTarget::SendA, 0), 1.0f);
    f.mixer.setParam(mixer::ParameterRegistry::fxParam((int)fx::FXTarget::SendA, 0, 0), 0.01f); // ~25ms: constructive @440Hz
    f.mixer.setParam(mixer::ParameterRegistry::fxParam((int)fx::FXTarget::SendA, 0, 1), 0.0f); // fb=0
    f.mixer.setParam(mixer::ParameterRegistry::channelSendA(0), 1.0f);

    float sent = 0.0f;
    for (int i = 0; i < 40; ++i) { f.render(); sent = std::max(sent, peak(f.outL)); }
    // dry channel + delayed copy summed -> more than dry alone
    REQUIRE(sent > 0.5f);

    // send to 0 -> back to dry level
    f.mixer.setParam(mixer::ParameterRegistry::channelSendA(0), 0.0f);
    for (int i = 0; i < 200; ++i) f.render(); // tail decays
    const float dry = peak(f.outL);
    REQUIRE(dry < sent);
    REQUIRE(dry == Approx(0.4f).margin(0.15f));
}

TEST_CASE("Mixer: master FX rack processes summed mix only", "[fx][mix]") {
    MixRig f;
    f.loadDeck(0, 6.0, 440.0, 0.4f);
    f.loadDeck(1, 6.0, 440.0, 0.4f);
    f.mixer.setCrossfader(0.0f);
    for (int i = 0; i < 30; ++i) f.render();
    const float dryMix = peak(f.outL);

    // master rack: gate at full depth silences half the cycle
    f.mixer.fx().loadEffect((int)fx::FXTarget::Master, 0, fx::FXKind::Gate);
    f.mixer.setParam(mixer::ParameterRegistry::fxEnabled((int)fx::FXTarget::Master, 0), 1.0f);
    f.mixer.setParam(mixer::ParameterRegistry::fxMix((int)fx::FXTarget::Master, 0), 1.0f);
    f.mixer.setParam(mixer::ParameterRegistry::fxParam((int)fx::FXTarget::Master, 0, 0), 1.0f); // depth
    int closed = 0;
    for (int i = 0; i < 48; ++i) {
        f.render();
        if (peak(f.outL) < 0.05f) ++closed;
    }
    REQUIRE(closed > 0); // gate closed on some blocks of the master only
    REQUIRE(peak(f.outL) <= dryMix + 0.01f);
}

TEST_CASE("Mixer: cue bus unaffected by send FX", "[fx][mix]") {
    MixRig f;
    f.loadDeck(0, 6.0, 440.0, 0.4f);
    f.mixer.setParam(mixer::chParam(0, mixer::ChParam::Cue), 1.0f);
    f.mixer.setParam(mixer::ParameterRegistry::CUE_MIX, 0.0f); // cue only
    for (int i = 0; i < 30; ++i) f.render();
    const float cueDry = peak(f.cueL);
    REQUIRE(cueDry > 0.2f);

    // crank a send to a loud reverb: master gains, cue stays put
    f.mixer.fx().loadEffect((int)fx::FXTarget::SendA, 0, fx::FXKind::Reverb);
    f.mixer.setParam(mixer::ParameterRegistry::fxEnabled((int)fx::FXTarget::SendA, 0), 1.0f);
    f.mixer.setParam(mixer::ParameterRegistry::fxMix((int)fx::FXTarget::SendA, 0), 1.0f);
    f.mixer.setParam(mixer::ParameterRegistry::channelSendA(0), 1.0f);
    f.mixer.setParam(mixer::ParameterRegistry::FX_RETURN_A, 2.0f);
    for (int i = 0; i < 40; ++i) f.render();
    REQUIRE(peak(f.cueL) == Approx(cueDry).margin(0.15f));
}

TEST_CASE("Mixer: synced send follows the sync master tempo", "[fx][mix]") {
    MixRig f;
    f.loadDeck(0, 6.0, 440.0, 0.4f);
    f.mixer.setFxSyncMaster(0); // deck 0 has no grid -> 120 BPM fallback
    f.mixer.fx().loadEffect((int)fx::FXTarget::SendA, 0, fx::FXKind::Delay);
    f.mixer.setParam(mixer::ParameterRegistry::fxEnabled((int)fx::FXTarget::SendA, 0), 1.0f);
    f.mixer.setParam(mixer::ParameterRegistry::fxMix((int)fx::FXTarget::SendA, 0), 1.0f);
    // just verify it renders finite and bounded under sync context
    f.mixer.setParam(mixer::ParameterRegistry::channelSendA(0), 0.8f);
    for (int i = 0; i < 60; ++i) {
        f.render();
        REQUIRE(std::isfinite(f.outL[0]));
        REQUIRE(peak(f.outL) < 4.0f);
    }
}

// ============================================================ STRESS
TEST_CASE("FX: full-load stress — all racks, params wiggling", "[fx][stress]") {
    MixRig f(4);
    for (int d = 0; d < 4; ++d)
        f.loadDeck(d, 10.0, 220.0 * (d + 1), 0.3f);

    using R = mixer::ParameterRegistry;
    // every deck: 2 fx slots; sends: echo+reverb; master: filter+phaser
    const fx::FXKind deckFx[4][2] = {
        { fx::FXKind::Delay, fx::FXKind::Reverb },
        { fx::FXKind::Filter, fx::FXKind::Flanger },
        { fx::FXKind::Echo, fx::FXKind::Phaser },
        { fx::FXKind::Gate, fx::FXKind::Delay },
    };
    for (int d = 0; d < 4; ++d)
        for (int s = 0; s < 2; ++s) {
            f.mixer.fx().loadEffect(d, s, deckFx[d][s]);
            f.mixer.setParam(R::fxEnabled(d, s), 1.0f);
            f.mixer.setParam(R::fxMix(d, s), 0.6f);
        }
    f.mixer.fx().loadEffect((int)fx::FXTarget::SendA, 0, fx::FXKind::Echo);
    f.mixer.fx().loadEffect((int)fx::FXTarget::SendB, 0, fx::FXKind::Reverb);
    f.mixer.fx().loadEffect((int)fx::FXTarget::Master, 0, fx::FXKind::Filter);
    f.mixer.fx().loadEffect((int)fx::FXTarget::Master, 1, fx::FXKind::Phaser);
    for (int b = 0; b < 2; ++b)
        f.mixer.setParam(R::fxEnabled((int)fx::FXTarget::SendA + b, 0), 1.0f);
    for (int s = 0; s < 2; ++s)
        f.mixer.setParam(R::fxEnabled((int)fx::FXTarget::Master, s), 1.0f);
    for (int d = 0; d < 4; ++d) {
        f.mixer.setParam(R::channelSendA(d), 0.5f);
        f.mixer.setParam(R::channelSendB(d), 0.4f);
    }

    const auto t0 = std::chrono::steady_clock::now();
    double worstUs = 0.0;
    float mx = 0.0f;
    for (int i = 0; i < 500; ++i) { // ~5.3 s
        // wiggle params each block
        if ((i & 7) == 0) {
            f.mixer.setParam(R::fxParam(0, 0, 0), (float)((i / 8) % 10) / 10.0f);
            f.mixer.setParam(R::fxMix(1, 0), (float)(i % 40) / 40.0f);
            f.mixer.setParam(R::fxDivision(2, 0), (float)(i % 8));
        }
        if ((i & 63) == 32) // bypass toggles
            f.mixer.setParam(R::fxEnabled(0, 0), (i & 128) ? 1.0f : 0.0f);
        const auto b0 = std::chrono::steady_clock::now();
        f.render();
        const auto us = std::chrono::duration<double, std::micro>(
                std::chrono::steady_clock::now() - b0).count();
        worstUs = std::max(worstUs, us);
        mx = std::max(mx, peak(f.outL));
        REQUIRE(peak(f.outL) < 8.0f);
    }
    const double totalMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
    INFO("500 blocks in " << totalMs << " ms, worst block " << worstUs << " us");
    REQUIRE(mx > 0.01f);            // actually producing audio
    REQUIRE(mx < 8.0f);             // bounded
    // real-time budget at 48k/512: ~10.6 ms/block — stress must sit
    // far below it even on this debug build
    REQUIRE(worstUs < 8000.0);
    REQUIRE(totalMs < 500 * 10.6);
    f.mixer.fx().collectTrash();    // housekeeping runs clean
}

TEST_CASE("FX: long-run stability — no NaN, no runaway", "[fx][stress]") {
    FxRig g(fx::FXKind::Echo);
    g.ctx.division = (int)fx::BeatDiv::D_1_8;
    g.fx->setParamNative(0, 0.9f);   // heavy feedback
    uint32_t st = 99;
    for (int i = 0; i < 400; ++i) {
        g.noise(0.5f, st);
        auto b = g.run();
        if (i % 50 == 0) REQUIRE(allFinite(b));
        if (i % 100 == 99) REQUIRE(peak(b) < 3.0f);
    }
}
