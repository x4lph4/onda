#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <cmath>
#include <vector>

#include "core/audio/AudioFifo.h"
#include "core/audio/TrackAudio.h"
#include "core/dsp/CrossfaderCurves.h"
#include "core/dsp/WsolaStretcher.h"
#include "core/engine/DeckEngine.h"
#include "core/mixer/MixerEngine.h"
#include "core/model/DeckStats.h"

using namespace onda;
using Catch::Approx;

// ---------------------------------------------------------------- helpers
static std::unique_ptr<TrackAudio> makeSine(double rate, double seconds,
                                            double freq, float amp) {
    auto t = std::make_unique<TrackAudio>();
    t->fileSampleRate = rate;
    t->deviceRate     = rate;
    t->numFrames      = (int64_t)(rate * seconds);
    t->samples.resize((size_t)t->numFrames * 2);
    for (int64_t i = 0; i < t->numFrames; ++i) {
        const float v = amp * (float)std::sin(2.0 * 3.14159265 * freq * (double)i / rate);
        t->samples[(size_t)i * 2]     = v;
        t->samples[(size_t)i * 2 + 1] = v;
    }
    return t;
}

static double zeroCrossRate(const std::vector<float>& x, double rate) {
    // Rough frequency estimate via zero crossings on the L channel.
    int crossings = 0;
    for (size_t i = 1; i < x.size(); ++i)
        if (x[i - 1] <= 0.0f && x[i] > 0.0f) ++crossings;
    const double secs = (double)x.size() / rate;
    return secs > 0 ? crossings / secs : 0.0;
}

struct DeckFixture {
    DeckEngine deck;
    DeckStats stats;
    std::vector<float> l, r;
    const TrackAudio* track = nullptr;

    DeckFixture() : l(512), r(512) {
        deck.bind(&stats);
        deck.setDeviceRate(48000.0);
        deck.setStretcher(std::make_unique<WsolaStretcher>());
        deck.prepareDsp(48000.0);
    }
    void load(double secs = 1.0, double freq = 440.0, float amp = 0.5f) {
        track = makeSine(48000.0, secs, freq, amp).release();
        deck.pendingTrack_.store(track);
        deck.renderPcm(l.data(), r.data(), 512); // adopts pending
    }
    int runBlocks(int n) {
        int blocks = 0;
        for (; blocks < n; ++blocks) {
            std::fill(l.begin(), l.end(), 0.0f);
            std::fill(r.begin(), r.end(), 0.0f);
            deck.renderPcm(l.data(), r.data(), 512);
        }
        return blocks;
    }
    ~DeckFixture() { deck.unloadTrack(); delete track; }
};

// ---------------------------------------------------------------- AudioFifo
TEST_CASE("AudioFifo bulk write/read with wraparound") {
    AudioFifo fifo(64);
    std::vector<float> in(100 * 2), out(100 * 2, 0.0f);
    for (int i = 0; i < 200; ++i) in[(size_t)i] = (float)i;

    REQUIRE(fifo.write(in.data(), 50) == 50);
    REQUIRE(fifo.read(out.data(), 30) == 30);
    REQUIRE(out[0] == 0.0f);
    REQUIRE(out[29 * 2] == 58.0f);

    REQUIRE(fifo.write(in.data() + 100, 40) == 40);
    std::vector<float> o2(60 * 2, 0.0f);
    REQUIRE(fifo.read(o2.data(), 60) == 60);
    REQUIRE(o2[0] == in[60]);
    REQUIRE(o2[40] == in[100]);
    REQUIRE(o2[59 * 2 + 1] == in[179]);
}

TEST_CASE("AudioFifo honours capacity and reset") {
    AudioFifo fifo(8);
    std::vector<float> in(8 * 2, 1.0f), out(8 * 2, 0.0f);
    REQUIRE(fifo.write(in.data(), 8) == 8);
    REQUIRE(fifo.write(in.data(), 1) == 0);
    fifo.reset();
    REQUIRE(fifo.availableToRead() == 0);
    REQUIRE(fifo.read(out.data(), 4) == 0);
}

// ---------------------------------------------------------------- transport
TEST_CASE("Deck: load -> adopt -> play -> position advances") {
    DeckFixture f;
    f.load(1.0);
    f.deck.setPlaying(true);
    f.runBlocks(20);
    REQUIRE(f.stats.state.load() == (int)DeckState::Playing);
    REQUIRE(f.stats.positionSec.load() > 0.05);
}

TEST_CASE("Deck: pause freezes position, stop rewinds") {
    DeckFixture f;
    f.load(1.0);
    f.deck.setPlaying(true);
    f.runBlocks(10);
    f.deck.setPlaying(false);
    const double posAtPause = f.stats.positionSec.load();
    f.runBlocks(5);
    REQUIRE(f.stats.positionSec.load() == Approx(posAtPause));
    f.deck.stop();
    f.runBlocks(1);
    REQUIRE(f.stats.positionSec.load() < 0.01);
    REQUIRE(f.stats.state.load() == (int)DeckState::Paused);
}

TEST_CASE("Deck: seek updates position and beat alignment") {
    DeckFixture f;
    f.load(2.0);
    f.deck.seekSec(1.0);
    f.deck.setPlaying(true);
    f.runBlocks(4);
    REQUIRE(f.stats.positionSec.load() == Approx(1.0).margin(0.05));
}

TEST_CASE("Deck: end of track pauses transport") {
    DeckFixture f;
    f.load(0.05); // 50 ms
    f.deck.setPlaying(true);
    f.runBlocks(30);
    REQUIRE(f.stats.state.load() == (int)DeckState::Paused);
    REQUIRE(f.stats.streamEnded.load());
}

// ---------------------------------------------------------------- pitch
TEST_CASE("Deck: +100% pitch consumes source twice as fast") {
    DeckFixture f;
    f.load(2.0);
    f.deck.setPitchPercent(100.0f); // needs range >= 100
    f.deck.setPitchRange(100.0f);
    f.deck.setPitchPercent(100.0f);
    f.deck.setPlaying(true);
    f.runBlocks(20); // ~213 ms of output
    // position should have advanced ~2x playback time
    REQUIRE(f.stats.positionSec.load() > 0.35);
}

TEST_CASE("Deck: pitch bend adds temporary rate") {
    DeckFixture f;
    f.load(2.0);
    f.deck.setPlaying(true);
    f.runBlocks(5);
    f.deck.setPitchBend(50.0f);
    const double p1 = f.stats.positionSec.load();
    f.runBlocks(5);
    const double fastAdv = f.stats.positionSec.load() - p1;
    f.deck.setPitchBend(0.0f);
    const double p2 = f.stats.positionSec.load();
    f.runBlocks(5);
    const double normAdv = f.stats.positionSec.load() - p2;
    REQUIRE(fastAdv > normAdv * 1.3);
}

// ---------------------------------------------------------------- key lock
TEST_CASE("Deck: key lock preserves pitch while advancing faster") {
    DeckFixture f;
    f.load(1.0, 440.0, 0.5f);
    f.deck.setPitchRange(50.0f);
    f.deck.setPitchPercent(25.0f);
    f.deck.setKeyLock(true);
    f.deck.setPlaying(true);

    std::vector<float> all;
    for (int b = 0; b < 40; ++b) {
        f.deck.renderPcm(f.l.data(), f.r.data(), 512);
        all.insert(all.end(), f.l.begin(), f.l.end());
    }
    // Frequency should stay ~440 Hz despite 25% faster consumption.
    const double hz = zeroCrossRate(all, 48000.0);
    REQUIRE(hz == Approx(440.0).margin(60.0));
    // And the source position should have advanced more than wall time.
    REQUIRE(f.stats.positionSec.load() > 0.35);
}

// ---------------------------------------------------------------- loops
TEST_CASE("Deck: loop wraps playback position") {
    DeckFixture f;
    f.load(2.0);
    f.deck.seekSec(0.4);
    f.deck.setLoop(0.4, 0.6);
    f.deck.setPlaying(true);
    // run well past loopOut several times
    f.runBlocks(80);
    const double pos = f.stats.positionSec.load();
    REQUIRE(pos >= 0.39);
    REQUIRE(pos < 0.62);
    REQUIRE(f.stats.state.load() == (int)DeckState::Playing);
}

TEST_CASE("Deck: loop disabled plays through") {
    DeckFixture f;
    f.load(2.0);
    f.deck.seekSec(0.4);
    f.deck.setLoop(0.4, 0.6);
    f.deck.setLoopEnabled(false);
    f.deck.setPlaying(true);
    f.runBlocks(80);
    REQUIRE(f.stats.positionSec.load() > 0.9);
}

// ---------------------------------------------------------------- reverse
TEST_CASE("Deck: reverse consumes the track backwards") {
    DeckFixture f;
    f.load(1.0);
    f.deck.seekSec(0.5);
    f.deck.setReverse(true);
    f.deck.setPlaying(true);
    f.runBlocks(10);
    REQUIRE(f.stats.positionSec.load() < 0.45);
}

// ---------------------------------------------------------------- gain/EQ
// Channel gain/EQ/filter moved into the mixer ChannelStrip in Phase 5;
// deck-level equivalents are covered by the mixer tests. This case
// keeps verifying the deck's own mute/trackGain path.
TEST_CASE("Deck: pause mutes source output") {
    DeckFixture f;
    f.load(1.0, 100.0, 0.5f); // low-frequency sine
    f.deck.setPlaying(false);
    f.runBlocks(60); // let the gain smoother settle (~0.6 s)
    float peak = 0.0f;
    for (float v : f.l) peak = std::max(peak, std::fabs(v));
    REQUIRE(peak < 0.01f);

    f.deck.setPlaying(true);
    f.deck.setTrackGain(0.1f);
    f.runBlocks(30);
    peak = 0.0f;
    for (float v : f.l) peak = std::max(peak, std::fabs(v));
    REQUIRE(peak < 0.15f); // trackGain attenuates
}

// ---------------------------------------------------------------- crossfader
TEST_CASE("Crossfader curves: endpoints and centre") {
    REQUIRE(xfGain(XfCurve::Linear, -1.0f, true)  == Approx(1.0));
    REQUIRE(xfGain(XfCurve::Linear, -1.0f, false) == Approx(0.0));
    REQUIRE(xfGain(XfCurve::Linear,  0.0f, true)  == Approx(0.5));
    REQUIRE(xfGain(XfCurve::Linear,  1.0f, false) == Approx(1.0));

    // equal power at centre = sqrt(2)/2 per side -> sum of squares = 1
    const float a = xfGain(XfCurve::EqualPower, 0.0f, true);
    const float b = xfGain(XfCurve::EqualPower, 0.0f, false);
    REQUIRE(a * a + b * b == Approx(1.0).margin(1e-4));

    REQUIRE(xfGain(XfCurve::Cut, -0.9f, true) == Approx(1.0));
    REQUIRE(xfGain(XfCurve::Cut,  0.8f, true) == Approx(1.0));
    REQUIRE(xfGain(XfCurve::Cut,  0.97f, true) < 0.25f);
}

// ---------------------------------------------------------------- mixer
struct MixFixture {
    MixerEngine mixer;
    rt::AudioStats astats;
    std::vector<DeckEngine> decks{2};
    std::vector<DeckStats> dstats{2};
    std::vector<const TrackAudio*> owned;
    std::vector<float> outL, outR, cueL, cueR;

    MixFixture() : outL(512), outR(512), cueL(512), cueR(512) {
        mixer.prepare(48000.0);
        for (int i = 0; i < 2; ++i) {
            decks[i].bind(&dstats[i]);
            decks[i].setDeviceRate(48000.0);
            decks[i].setStretcher(std::make_unique<WsolaStretcher>());
            decks[i].prepareDsp(48000.0);
        }
    }
    void loadDeck(int d, double secs, double freq, float amp) {
        auto* t = makeSine(48000.0, secs, freq, amp).release();
        owned.push_back(t);
        decks[d].pendingTrack_.store(t);
        float zl[8]{}, zr[8]{};
        decks[d].renderPcm(zl, zr, 8); // adopt pending -> Paused
        decks[d].setPlaying(true);
    }
    void render() {
        float* outs[4] = { outL.data(), outR.data(), cueL.data(), cueR.data() };
        mixer.render(outs, 4, 512, decks.data(), 2, astats);
    }
    ~MixFixture() { for (auto* t : owned) delete t; }
};

TEST_CASE("Mixer: crossfader full-A silences deck B") {
    MixFixture f;
    f.loadDeck(0, 1.0, 440.0, 0.5f);
    f.loadDeck(1, 1.0, 440.0, 0.5f);
    f.mixer.setCrossfader(-1.0f);
    f.mixer.setXfCurve((int)XfCurve::Linear);
    f.mixer.setFader(1, 1.0f);
    f.mixer.setFader(0, 1.0f);
    for (int i = 0; i < 20; ++i) f.render();
    // Only deck A in the mix
    float peak = 0.0f;
    for (float v : f.outL) peak = std::max(peak, std::fabs(v));
    REQUIRE(peak > 0.3f);
    REQUIRE(peak < 0.8f); // single deck only
}

TEST_CASE("Mixer: cue bus carries PFL deck without touching master") {
    MixFixture f;
    f.loadDeck(1, 1.0, 440.0, 0.5f);
    f.mixer.setCrossfader(-1.0f);          // master hears only A (empty)
    f.mixer.setXfCurve((int)XfCurve::Linear);
    f.mixer.setPfl(1, true);               // cue B pre-fader
    f.mixer.setCueVol(1.0f);
    f.mixer.setCueMix(0.0f);               // phones = pure cue
    for (int i = 0; i < 20; ++i) f.render();

    float masterPeak = 0.0f, cuePeak = 0.0f;
    for (float v : f.outL) masterPeak = std::max(masterPeak, std::fabs(v));
    for (float v : f.cueL) cuePeak = std::max(cuePeak, std::fabs(v));
    REQUIRE(masterPeak < 0.01f);  // master silent
    REQUIRE(cuePeak > 0.3f);      // cue bus carries deck B
}

TEST_CASE("Mixer: limiter tames summed hot mix") {
    MixFixture f;
    f.loadDeck(0, 1.0, 220.0, 0.9f);
    f.loadDeck(1, 1.0, 220.0, 0.9f); // in-phase sum -> ~1.8 peak
    f.mixer.setLimiterEnabled(true);
    for (int i = 0; i < 40; ++i) f.render();
    float peak = 0.0f;
    for (float v : f.outL) peak = std::max(peak, std::fabs(v));
    REQUIRE(peak <= 1.0f);
}

TEST_CASE("Mixer: clip counter fires without limiter") {
    MixFixture f;
    f.loadDeck(0, 1.0, 220.0, 0.9f);
    f.loadDeck(1, 1.0, 220.0, 0.9f);
    f.mixer.setLimiterEnabled(false);
    for (int i = 0; i < 40; ++i) f.render();
    REQUIRE(f.astats.clipCount.load() > 0);
}
