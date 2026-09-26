#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <cmath>
#include <vector>

#include "core/audio/AudioFifo.h"
#include "core/engine/DeckEngine.h"
#include "core/engine/MixerEngine.h"
#include "core/model/DeckStats.h"

using namespace onda;
using Catch::Approx;

static void fillSine(AudioFifo& fifo, size_t frames, float amp = 0.5f) {
    std::vector<float> buf(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        const float v = amp * std::sin(2.0 * 3.14159265 * 440.0 * i / 48000.0);
        buf[i * 2] = buf[i * 2 + 1] = v;
    }
    fifo.write(buf.data(), frames);
}

TEST_CASE("AudioFifo bulk write/read with wraparound") {
    AudioFifo fifo(64);
    std::vector<float> in(100 * 2), out(100 * 2, 0.0f);
    for (int i = 0; i < 200; ++i) in[(size_t)i] = (float)i;

    REQUIRE(fifo.write(in.data(), 50) == 50);
    REQUIRE(fifo.read(out.data(), 30) == 30);
    REQUIRE(out[0] == 0.0f);
    REQUIRE(out[29 * 2] == 58.0f);

    // write again -> wraps around the ring boundary
    REQUIRE(fifo.write(in.data() + 100, 40) == 40);
    std::vector<float> o2(60 * 2, 0.0f);
    REQUIRE(fifo.read(o2.data(), 60) == 60);
    // first 20 = remainder of first write (frames 30..49), next 40 = new write
    REQUIRE(o2[0] == in[60]);
    REQUIRE(o2[40] == in[100]);
    REQUIRE(o2[59 * 2 + 1] == in[179]); // last float of the second write
}

TEST_CASE("AudioFifo honours capacity and reset") {
    AudioFifo fifo(8);
    std::vector<float> in(8 * 2, 1.0f), out(8 * 2, 0.0f);
    REQUIRE(fifo.write(in.data(), 8) == 8);
    REQUIRE(fifo.write(in.data(), 1) == 0);      // full
    REQUIRE(fifo.availableToRead() == 8);
    fifo.reset();
    REQUIRE(fifo.availableToRead() == 0);
    REQUIRE(fifo.read(out.data(), 4) == 0);
}

TEST_CASE("DeckEngine streams from fifo into the mix") {
    AudioFifo fifo(48000);
    DeckStats stats;
    fillSine(fifo, 4800, 0.5f);

    DeckEngine deck;
    deck.bind(&fifo, &stats);
    deck.setDeviceRate(48000.0);
    stats.state.store((int)DeckState::Ready);
    deck.setPlaying(true);

    std::vector<float> mixL(512, 0.0f), mixR(512, 0.0f);
    for (int b = 0; b < 8; ++b)
        deck.addToMix(mixL.data(), mixR.data(), 512);

    REQUIRE(stats.framesRead.load() == 4096);
    REQUIRE(stats.positionSec.load() == Approx(4096.0 / 48000.0).epsilon(1e-6));
    REQUIRE(stats.underruns.load() == 0);
    REQUIRE(stats.levelL.load() > 0.2f); // sine present in the mix
    REQUIRE(stats.state.load() == (int)DeckState::Playing);
}

TEST_CASE("DeckEngine counts underruns when fifo starves") {
    AudioFifo fifo(1024);
    DeckStats stats;
    fillSine(fifo, 128, 0.5f);

    DeckEngine deck;
    deck.bind(&fifo, &stats);
    deck.setDeviceRate(48000.0);
    deck.setPlaying(true);

    std::vector<float> mixL(512, 0.0f), mixR(512, 0.0f);
    deck.addToMix(mixL.data(), mixR.data(), 512); // 128 available, 512 wanted

    REQUIRE(stats.framesRead.load() == 128);
    REQUIRE(stats.underruns.load() >= 1);
}

TEST_CASE("DeckEngine paused produces silence") {
    AudioFifo fifo(1024);
    DeckStats stats;
    fillSine(fifo, 512, 0.5f);

    DeckEngine deck;
    deck.bind(&fifo, &stats);
    deck.setDeviceRate(48000.0);
    // never set playing

    std::vector<float> mixL(512, 0.0f), mixR(512, 0.0f);
    deck.addToMix(mixL.data(), mixR.data(), 512);
    for (float v : mixL) REQUIRE(v == 0.0f);
    REQUIRE(stats.framesRead.load() == 0);
}

TEST_CASE("MixerEngine sums decks, applies master gain, counts clips") {
    AudioFifo fifoA(48000), fifoB(48000);
    DeckStats sa, sb;
    fillSine(fifoA, 4096, 0.6f);
    fillSine(fifoB, 4096, 0.6f); // identical in-phase -> sums to 1.2 amplitude

    DeckEngine decks[2];
    decks[0].bind(&fifoA, &sa);
    decks[1].bind(&fifoB, &sb);
    for (auto& d : decks) { d.setDeviceRate(48000.0); d.setPlaying(true); }

    MixerEngine mixer;
    mixer.setMasterGain(1.0f);
    mixer.setLimiterEnabled(false);

    rt::AudioStats stats;
    std::vector<float> L(256, 1.0f), R(256, 1.0f);
    float* outs[2] = { L.data(), R.data() };
    mixer.render(outs, 2, 256, decks, 2, stats);

    float peak = 0.0f;
    for (float v : L) peak = std::max(peak, std::fabs(v));
    REQUIRE(peak > 1.0f);               // truly summed
    REQUIRE(stats.clipCount.load() > 0); // clip detector fired
}

TEST_CASE("MixerEngine master gain scales output") {
    AudioFifo fifo(48000);
    DeckStats s;
    fillSine(fifo, 4096, 0.8f);

    DeckEngine deck;
    deck.bind(&fifo, &s);
    deck.setDeviceRate(48000.0);
    deck.setPlaying(true);

    MixerEngine mixer;
    mixer.setMasterGain(0.0f); // silence
    // force the smoothing to converge on repeated calls
    rt::AudioStats stats;
    std::vector<float> L(256), R(256);
    float* outs[2] = { L.data(), R.data() };
    for (int i = 0; i < 200; ++i)
        mixer.render(outs, 2, 256, &deck, 1, stats);

    float peak = 0.0f;
    for (float v : L) peak = std::max(peak, std::fabs(v));
    REQUIRE(peak < 0.05f);
}
