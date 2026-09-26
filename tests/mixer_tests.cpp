// Phase 5 — professional mixer engine tests.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include "core/audio/TrackAudio.h"
#include "core/dsp/WsolaStretcher.h"
#include "core/engine/DeckEngine.h"
#include "core/mixer/ChannelStrip.h"
#include "core/mixer/MixerController.h"
#include "core/mixer/MixerEngine.h"
#include "core/mixer/ParameterRegistry.h"
#include "core/model/DeckStats.h"
#include "core/rt/CommandQueue.h"

using namespace onda;
using namespace onda::mixer;
using Catch::Approx;

namespace {

constexpr int    kBlock = 512;
constexpr double kRate  = 48000.0;

// ---------------------------------------------------------------- synths
void fillSine(float* l, float* r, int n, double freq, float amp,
              double phase0 = 0.0) {
    for (int i = 0; i < n; ++i) {
        const float v = amp * (float)std::sin(phase0 + 2.0 * 3.14159265 * freq * i / kRate);
        l[i] = v; r[i] = v;
    }
}

void fillNoise(float* l, float* r, int n, float amp, uint32_t& st) {
    // xorshift32 — deterministic white noise
    for (int i = 0; i < n; ++i) {
        st ^= st << 13; st ^= st >> 17; st ^= st << 5;
        const float v = ((float)(st & 0xffffff) / 8388608.0f - 1.0f) * amp;
        l[i] = v; r[i] = v;
    }
}

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
double zeroCrossFreq(const std::vector<float>& x) {
    int c = 0;
    for (size_t i = 1; i < x.size(); ++i)
        if (x[i - 1] <= 0.0f && x[i] > 0.0f) ++c;
    return (double)c / ((double)x.size() / kRate);
}

// ------------------------------------------------------------- strip rig
// Drives ONE ChannelStrip with synthetic buffers — no deck needed.
// The source re-generates every block (the strip processes in place,
// so reusing a buffer would compound the filtering).
struct StripRig {
    ChannelStrip strip;
    CrossfaderEngine xf;
    std::vector<float> mixL, mixR, cueL, cueR;
    std::function<void(float*, float*, int)> gen;

    StripRig() : mixL(kBlock), mixR(kBlock), cueL(kBlock), cueR(kBlock) {
        strip.prepare(kRate);
        xf.prepare(kRate);
        // isolate from crossfader by default
        strip.setXfAssign(XfAssign::Thru);
    }

    void run(int blocks = 1) {
        for (int b = 0; b < blocks; ++b) {
            if (gen) gen(strip.inL(), strip.inR(), kBlock);
            std::fill(mixL.begin(), mixL.end(), 0.0f);
            std::fill(mixR.begin(), mixR.end(), 0.0f);
            std::fill(cueL.begin(), cueL.end(), 0.0f);
            std::fill(cueR.begin(), cueR.end(), 0.0f);
            xf.tickPos(kBlock);
            strip.process(kBlock, mixL.data(), mixR.data(),
                          cueL.data(), cueR.data(), xf, false);
        }
    }
    // continuous-phase sine so block boundaries don't excite filters
    void setInput(double freq, float amp) {
        phase_ = 0.0;
        gen = [this, freq, amp](float* l, float* r, int n) {
            for (int i = 0; i < n; ++i) {
                const float v = amp * (float)std::sin(phase_);
                l[i] = v; r[i] = v;
                phase_ += 2.0 * 3.14159265358979 * freq / kRate;
            }
        };
        gen(strip.inL(), strip.inR(), kBlock);
    }
    void setConst(float v) {
        gen = [v](float* l, float* r, int n) {
            std::fill_n(l, n, v); std::fill_n(r, n, v);
        };
        gen(strip.inL(), strip.inR(), kBlock);
    }
    double phase_ { 0.0 };
};

} // namespace

// ============================================================== GAIN
TEST_CASE("Strip: input gain min/max and smoothing", "[mixer][gain]") {
    StripRig g;

    // unity default
    g.setConst(0.5f);
    g.run(4);
    REQUIRE(peak(g.mixL) == Approx(0.5f).margin(0.02f));

    // min gain silences
    g.strip.setInputGain(0.0f);
    g.run(60); // > smoothing settle
    REQUIRE(peak(g.mixL) < 0.005f);

    // +6 dB boost (x2)
    g.strip.setInputGain(2.0f);
    g.run(60);
    REQUIRE(peak(g.mixL) == Approx(1.0f).margin(0.05f));

    // smoothing: immediately after a step the gain must not have jumped
    g.strip.setInputGain(0.0f);
    g.setConst(0.5f);
    g.run(1);
    // still decaying through the block: early samples near the old gain,
    // late samples clearly attenuated
    REQUIRE(std::fabs(g.mixL[10]) > std::fabs(g.mixL.back()) + 0.05f);
    REQUIRE(std::fabs(g.mixL.back()) > 0.01f); // not a click-to-zero
}

// ============================================================== EQ
TEST_CASE("Strip: EQ bands cut the right frequencies", "[mixer][eq]") {
    SECTION("low cut attenuates 100 Hz") {
        StripRig g;
        g.setInput(100.0, 0.5f);
        g.strip.eq().setGainDb(0, -26.0);
        g.run(40);
        REQUIRE(peak(g.mixL) < 0.1f);
    }
    SECTION("mid cut attenuates 1 kHz, keeps 100 Hz") {
        StripRig g;
        g.setInput(1000.0, 0.5f);
        g.strip.eq().setGainDb(1, -26.0);
        g.run(40);
        REQUIRE(peak(g.mixL) < 0.15f);

        // same cut on a 100 Hz input: mostly untouched
        StripRig g2;
        g2.setInput(100.0, 0.5f);
        g2.strip.eq().setGainDb(1, -26.0);
        g2.run(40);
        REQUIRE(peak(g2.mixL) > 0.3f);
    }
    SECTION("high cut attenuates 10 kHz") {
        StripRig g;
        g.setInput(10000.0, 0.5f);
        g.strip.eq().setGainDb(2, -26.0);
        g.run(40);
        REQUIRE(peak(g.mixL) < 0.15f);
    }
    SECTION("bypass restores the band") {
        StripRig g;
        g.setInput(100.0, 0.5f);
        g.strip.eq().setGainDb(0, -26.0);
        g.run(30);
        REQUIRE(peak(g.mixL) < 0.1f);
        g.strip.eq().setBypass(0, true);
        g.run(10);
        REQUIRE(peak(g.mixL) == Approx(0.5f).margin(0.05f));
    }
    SECTION("gain transitions are stepwise, not instant") {
        StripRig g;
        g.setInput(1000.0, 0.5f);
        g.strip.eq().setGainDb(1, -24.0);
        g.run(1); // first block: gain steps <= 3 dB, not the full -24
        REQUIRE(peak(g.mixL) > 0.2f); // partial attenuation only
    }
    SECTION("boost raises level") {
        StripRig g;
        g.setInput(1000.0, 0.2f);
        g.strip.eq().setGainDb(1, 6.0);
        g.run(40);
        REQUIRE(peak(g.mixL) > 0.3f);
    }
}

// ============================================================== FILTER
TEST_CASE("Strip: filter LP/HP and cutoff response", "[mixer][filter]") {
    SECTION("low-pass position removes highs") {
        StripRig g;
        g.setInput(8000.0, 0.5f);
        g.strip.filter().setPosition(-0.8f); // LP sweep ~deep
        g.run(80); // smoothing + biquad settle
        REQUIRE(peak(g.mixL) < 0.15f);
    }
    SECTION("high-pass position removes lows") {
        StripRig g;
        g.setInput(50.0, 0.5f);
        g.strip.filter().setPosition(0.9f); // HP sweep ~deep
        g.run(80);
        REQUIRE(peak(g.mixL) < 0.1f);
    }
    SECTION("centre position is transparent") {
        StripRig g;
        g.setInput(440.0, 0.5f);
        g.strip.filter().setPosition(0.0f);
        g.run(20);
        REQUIRE(peak(g.mixL) == Approx(0.5f).margin(0.05f));
    }
    SECTION("typed LP mode honours explicit cutoff") {
        StripRig g;
        g.strip.filter().setType(FilterType::LowPass);
        g.strip.filter().setCutoffHz(300.0);
        g.setInput(5000.0, 0.5f);
        g.run(20);
        REQUIRE(peak(g.mixL) < 0.1f);
        g.setInput(100.0, 0.5f);
        g.run(20);
        REQUIRE(peak(g.mixL) > 0.35f);
    }
    SECTION("position sweeps without discontinuity") {
        StripRig g;
        g.setInput(1000.0, 0.5f);
        g.strip.filter().setPosition(0.9f); // deep HP sweep
        g.run(1);
        // per-sample delta stays bounded -> no click on the sweep edge
        float maxDelta = 0.0f;
        for (int i = 1; i < kBlock; ++i)
            maxDelta = std::max(maxDelta, std::fabs(g.mixL[i] - g.mixL[i - 1]));
        REQUIRE(maxDelta < 0.4f);
        // and it eventually attenuates the low-mid content
        g.setInput(50.0, 0.5f);
        g.run(60);
        REQUIRE(peak(g.mixL) < 0.15f);
    }
}

// ============================================================== FADER
TEST_CASE("Strip: fader curves, mute, instant cut", "[mixer][fader]") {
    SECTION("fader 0 silences, fader 1 passes") {
        StripRig g;
        g.setConst(0.5f);
        g.strip.setFader(0.0f);
        g.run(60);
        REQUIRE(peak(g.mixL) < 0.005f);
        g.strip.setFader(1.0f);
        g.run(60);
        REQUIRE(peak(g.mixL) == Approx(0.5f).margin(0.02f));
    }
    SECTION("audio taper curve attenuates mid-travel") {
        StripRig lin, tap;
        lin.setConst(0.5f); tap.setConst(0.5f);
        lin.strip.setFader(0.5f);
        tap.strip.setFaderCurve(FaderCurve::AudioTaper);
        tap.strip.setFader(0.5f);
        lin.run(60); tap.run(60);
        const float pl = peak(lin.mixL), pt = peak(tap.mixL);
        REQUIRE(pl == Approx(0.25f).margin(0.03f));
        REQUIRE(pt == Approx(0.125f).margin(0.02f)); // x^2
        REQUIRE(pt < pl);
    }
    SECTION("custom fader curve is honoured") {
        StripRig g;
        g.setConst(0.5f);
        g.strip.setCustomFaderCurve([](float x) { return x * x * x; });
        g.strip.setFaderCurve(FaderCurve::Custom);
        g.strip.setFader(0.5f);
        g.run(60);
        REQUIRE(peak(g.mixL) == Approx(0.5f * 0.125f).margin(0.02f));
    }
    SECTION("mute silences smoothly") {
        StripRig g;
        g.setConst(0.5f);
        g.run(10);
        g.strip.setMute(true);
        g.run(1);
        REQUIRE(peak(g.mixL) > 0.05f); // still fading
        g.run(40);
        REQUIRE(peak(g.mixL) < 0.005f);
    }
    SECTION("instant cut reaches silence within a block") {
        StripRig g;
        g.setConst(0.5f);
        g.run(10);
        g.strip.setInstantCut(true);
        g.run(1);
        // 0.4 ms ramp: the tail of the first block is already silent
        REQUIRE(std::fabs(g.mixL.back()) < 0.01f);
        g.run(1);
        REQUIRE(peak(g.mixL) < 0.005f);
        // while a mute takes longer (4 ms smoother vs 0.4 ms cut)
        StripRig g2;
        g2.setConst(0.5f);
        g2.run(10);
        g2.strip.setMute(true);
        g2.run(1);
        REQUIRE(std::fabs(g2.mixL.back()) > std::fabs(g.mixL.back()) + 0.01f);
    }
    SECTION("fader moves are smoothed") {
        StripRig g;
        g.setConst(0.5f);
        g.strip.setFader(0.0f);
        g.run(1);
        REQUIRE(peak(g.mixL) > 0.05f); // still fading out, no click
    }
}

// ============================================================== CROSSFADER
TEST_CASE("Strip: crossfader assignment A/B/Thru", "[mixer][xfader]") {
    SECTION("A-side silent when fader at B") {
        StripRig g;
        g.setConst(0.5f);
        g.strip.setXfAssign(XfAssign::A);
        g.xf.setCurve(XfCurve::Linear);
        g.xf.setPosition(1.0f); // full B
        g.run(80);
        REQUIRE(peak(g.mixL) < 0.01f);
    }
    SECTION("B-side silent when fader at A") {
        StripRig g;
        g.setConst(0.5f);
        g.strip.setXfAssign(XfAssign::B);
        g.xf.setCurve(XfCurve::Linear);
        g.xf.setPosition(-1.0f); // full A
        g.run(80);
        REQUIRE(peak(g.mixL) < 0.01f);
    }
    SECTION("Thru ignores position") {
        for (float pos : { -1.0f, 0.0f, 1.0f }) {
            StripRig g;
            g.setConst(0.5f);
            g.strip.setXfAssign(XfAssign::Thru);
            g.xf.setCurve(XfCurve::Linear);
            g.xf.setPosition(pos);
            g.run(80);
            REQUIRE(peak(g.mixL) == Approx(0.5f).margin(0.03f));
        }
    }
    SECTION("cut curve keeps both full until far rail") {
        StripRig a, b;
        a.setConst(0.5f); b.setConst(0.5f);
        a.strip.setXfAssign(XfAssign::A);
        b.strip.setXfAssign(XfAssign::B);
        a.xf.setCurve(XfCurve::Cut);
        b.xf.setCurve(XfCurve::Cut);
        a.xf.setPosition(0.4f); b.xf.setPosition(0.4f);
        a.run(80); b.run(80);
        REQUIRE(peak(a.mixL) == Approx(0.5f).margin(0.03f));
        REQUIRE(peak(b.mixL) == Approx(0.5f).margin(0.03f));
    }
}

// ============================================================== CUE
TEST_CASE("Strip: cue taps pre-fader audio", "[mixer][cue]") {
    StripRig g;
    g.setConst(0.5f);
    g.strip.setCue(true);
    g.strip.setFader(0.0f); // master path muted by fader
    g.run(60);
    REQUIRE(peak(g.mixL) < 0.005f);          // master silent
    REQUIRE(peak(g.cueL) == Approx(0.5f).margin(0.05f)); // cue hears it
}

// ============================================================== SOLO
TEST_CASE("Strip: solo logic (anySolo silences non-solo)", "[mixer][solo]") {
    StripRig g;
    g.setConst(0.5f);
    // another strip soloed -> this one contributes 0
    g.run(0); // prepare only
    std::fill(g.mixL.begin(), g.mixL.end(), 0.0f);
    g.xf.tickPos(kBlock);
    g.strip.process(kBlock, g.mixL.data(), g.mixR.data(),
                    g.cueL.data(), g.cueR.data(), g.xf, /*anySolo*/true);
    REQUIRE(peak(g.mixL) < 0.01f);
    // solo this one -> contributes again
    g.strip.setSolo(true);
    g.strip.process(kBlock, g.mixL.data(), g.mixR.data(),
                    g.cueL.data(), g.cueR.data(), g.xf, true);
    REQUIRE(peak(g.mixL) > 0.2f);
}

// ============================================================== METERING
TEST_CASE("Strip: meters publish peak/rms/hold", "[mixer][meter]") {
    StripRig g;
    g.setInput(440.0, 0.5f);
    g.run(10);
    const auto& m = g.strip.meters();
    REQUIRE(m.peakL.load() == Approx(0.5).margin(0.05));
    REQUIRE(m.rmsL.load() == Approx(0.354).margin(0.05)); // sine rms
    REQUIRE(m.holdL.load() >= m.peakL.load() - 1e-3f);
    // silence -> hold decays, peak resets
    g.setConst(0.0f);
    g.run(400); // ~4 s
    REQUIRE(m.peakL.load() < 0.001f);
    REQUIRE(m.holdL.load() < 0.1f);
    REQUIRE(m.clip.load() == false);
}

// ====================================================== mixer-level rig
struct MixRig {
    MixerEngine mixer;
    rt::AudioStats astats;
    std::vector<DeckEngine> decks;  // vector(N) value-inits in place
    std::vector<DeckStats>  dstats; // (atomics inside -> never resize)
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
        auto* t = makeSineRaw(secs, freq, amp);
        owned.push_back(t);
        decks[d].pendingTrack_.store(t);
        float zl[8]{}, zr[8]{};
        decks[d].renderPcm(zl, zr, 8); // adopt pending
        decks[d].setPlaying(true);
    }
    static TrackAudio* makeSineRaw(double secs, double freq, float amp) {
        auto* t = new TrackAudio();
        t->fileSampleRate = kRate; t->deviceRate = kRate;
        t->numFrames = (int64_t)(kRate * secs);
        t->samples.resize((size_t)t->numFrames * 2);
        for (int64_t i = 0; i < t->numFrames; ++i) {
            const float v = amp * (float)std::sin(2.0 * 3.14159265 * freq * i / kRate);
            t->samples[(size_t)i * 2] = v;
            t->samples[(size_t)i * 2 + 1] = v;
        }
        return t;
    }
    void render() {
        float* outs[4] = { outL.data(), outR.data(), cueL.data(), cueR.data() };
        mixer.render(outs, 4, kBlock, decks.data(), (int)decks.size(), astats);
    }
    ~MixRig() { for (auto* t : owned) delete t; }
};

// ============================================================== ROUTING
TEST_CASE("Mixer: end-to-end deck -> strip -> master routing", "[mixer][route]") {
    MixRig f;
    f.loadDeck(0, 3.0, 440.0, 0.5f);
    f.loadDeck(1, 3.0, 440.0, 0.5f);
    f.mixer.setXfCurve((int)XfCurve::Linear);

    // deck A only on master when xf full A
    f.mixer.setCrossfader(-1.0f);
    for (int i = 0; i < 30; ++i) f.render();
    float pk = peak(f.outL);
    REQUIRE(pk > 0.3f);
    REQUIRE(pk < 0.8f);

    // deck B only when xf full B
    f.mixer.setCrossfader(1.0f);
    for (int i = 0; i < 80; ++i) f.render();
    pk = peak(f.outL);
    REQUIRE(pk > 0.3f);
    REQUIRE(pk < 0.8f);
}

TEST_CASE("Mixer: mute kills channel without touching others", "[mixer][route]") {
    MixRig f;
    f.loadDeck(0, 6.0, 220.0, 0.4f);
    f.loadDeck(1, 6.0, 440.0, 0.4f);
    f.mixer.setXfCurve((int)XfCurve::Linear);
    f.mixer.setCrossfader(0.0f); // both audible (0.5 each)
    for (int i = 0; i < 80; ++i) f.render();
    const float both = rms(f.outL);
    f.mixer.setParam(mixer::chParam(1, ChParam::Mute), 1.0f);
    for (int i = 0; i < 80; ++i) f.render();
    const float aOnly = rms(f.outL);
    // Deck B (440 Hz) muted: accumulate zero crossings over 40 blocks
    // (single 512-sample block can't resolve 220 vs 187.5 Hz)
    std::vector<float> tail;
    tail.reserve(40 * kBlock);
    for (int i = 0; i < 40; ++i) {
        f.render();
        tail.insert(tail.end(), f.outL.begin(), f.outL.end());
    }
    REQUIRE(zeroCrossFreq(tail) == Approx(220.0).margin(15.0));
    REQUIRE(aOnly < both);
}

TEST_CASE("Mixer: cue bus isolation — PFL heard, master untouched", "[mixer][route]") {
    MixRig f;
    f.loadDeck(1, 3.0, 440.0, 0.5f);
    f.mixer.setXfCurve((int)XfCurve::Linear);
    f.mixer.setCrossfader(-1.0f); // master hears A only (empty)
    f.mixer.setPfl(1, true);
    f.mixer.setCueVol(1.0f);
    f.mixer.setCueMix(0.0f);      // phones = pure cue
    for (int i = 0; i < 30; ++i) f.render();
    REQUIRE(peak(f.outL) < 0.02f);
    REQUIRE(peak(f.cueL) > 0.3f);
}

TEST_CASE("Mixer: cue mix blends toward master", "[mixer][route]") {
    MixRig f;
    f.loadDeck(0, 3.0, 440.0, 0.5f);
    f.mixer.setXfCurve((int)XfCurve::Linear);
    f.mixer.setCrossfader(-1.0f);
    f.mixer.setCueVol(1.0f);
    f.mixer.setCueMix(1.0f);      // phones = pure master
    for (int i = 0; i < 80; ++i) f.render();
    REQUIRE(peak(f.cueL) > 0.3f); // master program heard in phones
    REQUIRE(peak(f.cueL) == Approx(peak(f.outL)).margin(0.05f));
}

TEST_CASE("Mixer: limiter and clip accounting still work", "[mixer][master]") {
    MixRig f;
    f.loadDeck(0, 3.0, 220.0, 0.9f);
    f.loadDeck(1, 3.0, 220.0, 0.9f); // in-phase -> hot sum
    f.mixer.setXfCurve((int)XfCurve::EqualPower); // both pass at centre
    f.mixer.setCrossfader(0.0f);
    f.mixer.setLimiterEnabled(true);
    for (int i = 0; i < 80; ++i) f.render();
    REQUIRE(peak(f.outL) <= 1.0f);
    // clips are DETECTED even with the limiter active (post-limiter meter)
    f.mixer.setLimiterEnabled(false);
    for (int i = 0; i < 80; ++i) f.render();
    REQUIRE(f.astats.clipCount.load() > 0);
    REQUIRE(f.mixer.masterStats().peakL.load() > 1.0f);
}

TEST_CASE("Mixer: routing graph describes the topology", "[mixer][route]") {
    MixRig f;
    const auto& r = f.mixer.routing();
    REQUIRE(r.nodes().size() >= 8); // 2 src + 2 strips + xf + master + cue + outs
    REQUIRE(r.findNode(mixer::RoutingEngine::NodeType::ChannelStrip, 0) >= 0);
    REQUIRE(r.findNode(mixer::RoutingEngine::NodeType::ChannelStrip, 1) >= 0);
    REQUIRE(r.findNode(mixer::RoutingEngine::NodeType::MasterBus, -1) >= 0);
    REQUIRE(r.findNode(mixer::RoutingEngine::NodeType::CueBus, -1) >= 0);
    // each strip has a pre-fader tap into the cue bus
    int cueEdges = 0;
    for (const auto& e : r.edges())
        if (e.tap == mixer::RoutingEngine::Tap::PreFader) ++cueEdges;
    REQUIRE(cueEdges >= 2);
}

// ====================================================== setParam routing
TEST_CASE("Mixer: setParam routes channel params", "[mixer][params]") {
    MixRig f;
    f.loadDeck(0, 3.0, 440.0, 0.5f);
    // fader via param id
    f.mixer.setParam(ParameterRegistry::channelFader(0), 0.0f);
    f.mixer.setXfCurve((int)XfCurve::Linear);
    f.mixer.setCrossfader(-1.0f);
    for (int i = 0; i < 80; ++i) f.render();
    REQUIRE(peak(f.outL) < 0.01f);
    // mute param
    f.mixer.setParam(ParameterRegistry::channelFader(0), 1.0f);
    f.mixer.setParam(ParameterRegistry::channelMute(0), 1.0f);
    for (int i = 0; i < 80; ++i) f.render();
    REQUIRE(peak(f.outL) < 0.01f);
    // un-mute recovers
    f.mixer.setParam(ParameterRegistry::channelMute(0), 0.0f);
    for (int i = 0; i < 80; ++i) f.render();
    REQUIRE(peak(f.outL) > 0.3f);
}

// ==================================================== parameter registry
TEST_CASE("ParameterRegistry: ids, defs, sanitize", "[mixer][params]") {
    using PR = ParameterRegistry;
    // required spec ids exist
    for (int ch = 0; ch < 4; ++ch) {
        REQUIRE(PR::exists(PR::channelGain(ch)));
        REQUIRE(PR::exists(PR::channelLow(ch)));
        REQUIRE(PR::exists(PR::channelMid(ch)));
        REQUIRE(PR::exists(PR::channelHigh(ch)));
        REQUIRE(PR::exists(PR::channelFilter(ch)));
        REQUIRE(PR::exists(PR::channelFader(ch)));
        REQUIRE(PR::exists(PR::channelCue(ch)));
    }
    REQUIRE(PR::exists(PR::CROSSFADER));
    REQUIRE(PR::exists(PR::MASTER_GAIN));
    REQUIRE(PR::exists(PR::CUE_VOLUME));
    REQUIRE(PR::exists(PR::CUE_MIX));

    // sanitize clamps + quantizes
    REQUIRE(PR::sanitize(PR::MASTER_GAIN, 5.0f) == Approx(2.0f));
    REQUIRE(PR::sanitize(PR::channelLow(0), -99.0f) == Approx(-26.0f));
    REQUIRE(PR::sanitize(PR::channelCue(0), 0.7f) == Approx(1.0f)); // step=1
    REQUIRE(PR::sanitize(PR::CROSSFADER, 2.5f) == Approx(1.0f));

    // defs carry metadata
    const ParamDef* d = PR::find(PR::MASTER_GAIN);
    REQUIRE(d != nullptr);
    REQUIRE(d->def == Approx(1.0f));
    REQUIRE(d->step > 0.0f);
    REQUIRE(std::string(d->name) == "MasterGain");
}

TEST_CASE("MixerController: validates and forwards to sink", "[mixer][params]") {
    MixerController ctl;
    std::vector<std::pair<uint16_t, float>> sent;
    ctl.setSink([&](uint16_t id, float v) { sent.emplace_back(id, v); });

    ctl.channelFader(0, 0.8f);
    ctl.crossfader(5.0f);           // out of range -> clamped to +1
    ctl.channelCue(1, true);
    ctl.masterGain(1.2f);

    REQUIRE(sent.size() == 4);
    REQUIRE(sent[0] == std::make_pair(ParameterRegistry::channelFader(0), 0.8f));
    REQUIRE(sent[1] == std::make_pair(ParameterRegistry::CROSSFADER, 1.0f));
    REQUIRE(sent[2] == std::make_pair(ParameterRegistry::channelCue(1), 1.0f));
    REQUIRE(sent[3] == std::make_pair(ParameterRegistry::MASTER_GAIN, 1.2f));
    // read-back
    REQUIRE(ctl.value(ParameterRegistry::channelFader(0)) == Approx(0.8f));
    REQUIRE(ctl.value(ParameterRegistry::channelLow(2)) == Approx(0.0f)); // default
}

// ======================================================= audio integrity
TEST_CASE("Mixer: synthetic signals stay clean end-to-end", "[mixer][audio]") {
    SECTION("silence in, silence out") {
        MixRig f;
        for (int i = 0; i < 10; ++i) f.render();
        REQUIRE(peak(f.outL) == 0.0f);
        REQUIRE(peak(f.cueL) == 0.0f);
    }
    SECTION("stereo signal passes L->L R->R") {
        // load a stereo-correlated track: makeSine is mono-dual; build
        // a decorrelated stereo track manually.
        MixRig f;
        auto* t = new TrackAudio();
        t->fileSampleRate = kRate; t->deviceRate = kRate;
        t->numFrames = (int64_t)kRate;
        t->samples.resize((size_t)t->numFrames * 2);
        for (int64_t i = 0; i < t->numFrames; ++i) {
            t->samples[(size_t)i * 2]     = 0.4f * (float)std::sin(2 * 3.14159265 * 300.0 * i / kRate);
            t->samples[(size_t)i * 2 + 1] = 0.4f * (float)std::sin(2 * 3.14159265 * 900.0 * i / kRate);
        }
        f.owned.push_back(t);
        f.decks[0].pendingTrack_.store(t);
        float zl[8]{}, zr[8]{};
        f.decks[0].renderPcm(zl, zr, 8);
        f.decks[0].setPlaying(true);
        f.mixer.setXfCurve((int)XfCurve::Linear);
        f.mixer.setCrossfader(-1.0f);
        for (int i = 0; i < 30; ++i) f.render();
        REQUIRE(zeroCrossFreq(f.outL) == Approx(300.0).margin(40.0));
        REQUIRE(zeroCrossFreq(f.outR) == Approx(900.0).margin(60.0));
    }
    SECTION("impulse does not explode") {
        MixRig f;
        auto* t = new TrackAudio();
        t->fileSampleRate = kRate; t->deviceRate = kRate;
        t->numFrames = (int64_t)kRate;
        t->samples.assign((size_t)t->numFrames * 2, 0.0f);
        t->samples[0] = 1.0f; t->samples[1] = 1.0f; // single-sample click
        f.owned.push_back(t);
        f.decks[0].pendingTrack_.store(t);
        float zl[8]{}, zr[8]{};
        f.decks[0].renderPcm(zl, zr, 8);
        f.decks[0].setPlaying(true);
        for (int i = 0; i < 5; ++i) f.render();
        REQUIRE(peak(f.outL) <= 1.0f);
    }
    SECTION("white noise sums without unexpected clipping") {
        MixRig f;
        uint32_t st0 = 12345, st1 = 67890;
        for (int d = 0; d < 2; ++d) {
            auto* t = new TrackAudio();
            t->fileSampleRate = kRate; t->deviceRate = kRate;
            t->numFrames = (int64_t)kRate;
            t->samples.resize((size_t)t->numFrames * 2);
            uint32_t& st = d == 0 ? st0 : st1;
            for (int64_t i = 0; i < t->numFrames; ++i) {
                st ^= st << 13; st ^= st >> 17; st ^= st << 5;
                const float v = ((float)(st & 0xffffff) / 8388608.0f - 1.0f) * 0.3f;
                t->samples[(size_t)i * 2] = v;
                t->samples[(size_t)i * 2 + 1] = v;
            }
            f.owned.push_back(t);
            f.decks[d].pendingTrack_.store(t);
            float zl[8]{}, zr[8]{};
            f.decks[d].renderPcm(zl, zr, 8);
            f.decks[d].setPlaying(true);
        }
        f.mixer.setXfCurve((int)XfCurve::Linear);
        f.mixer.setCrossfader(0.0f);
        for (int i = 0; i < 30; ++i) f.render();
        REQUIRE(peak(f.outL) < 1.0f); // two -10 dBFS noise sources sum fine
        REQUIRE(peak(f.outL) > 0.1f);
    }
}

// ============================================================== 4-channel
TEST_CASE("Mixer: 4 active channels process without errors", "[mixer][4ch]") {
    MixRig f(4);
    for (int d = 0; d < 4; ++d)
        f.loadDeck(d, 3.0, 200.0 + 100.0 * d, 0.3f);
    f.mixer.setXfCurve((int)XfCurve::Linear);
    f.mixer.setCrossfader(0.0f);
    // ch2/ch3 default assignments (A/B) — all audible at centre
    for (int i = 0; i < 60; ++i) f.render();
    REQUIRE(peak(f.outL) > 0.05f);
    // channel meters published for all four
    for (int d = 0; d < 4; ++d)
        REQUIRE(f.mixer.channel(d).meters().peakL.load() > 0.05f);
}

// ============================================================== perf
TEST_CASE("Mixer: 4-strip block cost stays bounded", "[mixer][perf]") {
    MixRig f(4);
    for (int d = 0; d < 4; ++d)
        f.loadDeck(d, 30.0, 200.0 + 150.0 * d, 0.4f);
    // everything on: EQ cut + filter + fader + cue + solo paths
    for (int d = 0; d < 4; ++d) {
        f.mixer.channel(d).eq().setGainDb(0, -6.0);
        f.mixer.channel(d).filter().setPosition(0.3f);
        f.mixer.channel(d).setCue(d % 2 == 0);
    }
    f.mixer.setCrossfader(0.0f);
    const int kBlocks = 400;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kBlocks; ++i) f.render();
    const double us = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - t0).count() / kBlocks;
    // 512 frames @48k = 10.7 ms real-time budget; a few hundred us of CPU
    // is several orders below. Generous bound for CI machines.
    REQUIRE(us < 4000.0);
    // audio stays sane under full processing
    REQUIRE(peak(f.outL) > 0.01f);
    REQUIRE(peak(f.outL) <= 1.0f);
}

// ======================================================= rt integrity
TEST_CASE("Mixer: long render run stays finite (no NaN/denormal blowup)",
          "[mixer][rt]") {
    MixRig f(4);
    for (int d = 0; d < 4; ++d)
        f.loadDeck(d, 10.0, 180.0 + 120.0 * d, 0.4f);
    f.mixer.setXfCurve((int)XfCurve::EqualPower);
    f.mixer.setCrossfader(0.0f);
    // exercise every processor: eq cut, filter sweep, cue, solo toggles
    for (int d = 0; d < 4; ++d) {
        f.mixer.channel(d).eq().setGainDb(d % 3, -12.0);
        f.mixer.channel(d).filter().setPosition(d % 2 ? 0.4f : -0.4f);
        f.mixer.channel(d).setCue(d == 0);
    }
    for (int i = 0; i < 500; ++i) {
        if (i == 200) f.mixer.channel(1).setSolo(true);
        if (i == 300) f.mixer.channel(1).setSolo(false);
        if (i == 350) f.mixer.setCrossfader(-1.0f);
        f.render();
        for (float v : f.outL) REQUIRE(std::isfinite(v));
        for (float v : f.cueL) REQUIRE(std::isfinite(v));
    }
    REQUIRE(peak(f.outL) > 0.005f);
}
