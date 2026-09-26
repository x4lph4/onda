#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <new>
#include <vector>

#include "core/audio/TrackAudio.h"
#include "core/dsp/CrossfaderCurves.h"
#include "core/dsp/WsolaStretcher.h"
#include "core/engine/DeckEngine.h"
#include "core/mixer/MixerEngine.h"
#include "core/model/DeckStats.h"
#include "core/rt/AudioStats.h"

using namespace onda;
using Catch::Approx;

// ---------------------------------------------------------------- allocation counter
// Global new/delete counters used to prove the RT path does not allocate.
// Allocation counting is active only while AllocProbe is armed.
static std::atomic<uint64_t> g_allocs{0};
static std::atomic<bool>     g_armed{false};

void* operator new(std::size_t sz) {
    if (g_armed.load(std::memory_order_relaxed))
        g_allocs.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(sz)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept          { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void* operator new[](std::size_t sz) {
    if (g_armed.load(std::memory_order_relaxed))
        g_allocs.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(sz)) return p;
    throw std::bad_alloc();
}
void operator delete[](void* p) noexcept          { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

struct AllocProbe {
    AllocProbe()  { g_allocs.store(0); g_armed.store(true); }
    ~AllocProbe() { g_armed.store(false); }
    uint64_t count() const { return g_allocs.load(); }
};

// ---------------------------------------------------------------- helpers
static std::unique_ptr<TrackAudio> makeLoopable(double rate, double seconds,
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

static void feed(DeckEngine& d, std::unique_ptr<TrackAudio> t) {
    const TrackAudio* p = t.release();
    d.pendingTrack_.store(p);
    float zl[8]{}, zr[8]{};
    d.renderPcm(zl, zr, 8); // adopt -> Paused
}

static void cleanup(DeckEngine& d) {
    d.unloadTrack();
    delete d.retiredTrack_.exchange(nullptr);
    delete d.pendingTrack_.exchange(nullptr);
}

// ---------------------------------------------------------------- RT safety
TEST_CASE("RT safety: render path performs zero allocations") {
    DeckEngine decks[2];
    DeckStats stats[2];
    rt::AudioStats astats;
    MixerEngine mixer;
    mixer.prepare(48000.0);

    for (int i = 0; i < 2; ++i) {
        decks[i].bind(&stats[i]);
        decks[i].setDeviceRate(48000.0);
        decks[i].setStretcher(std::make_unique<WsolaStretcher>());
        decks[i].prepareDsp(48000.0);
    }
    feed(decks[0], makeLoopable(48000.0, 2.0, 220.0, 0.5f));
    feed(decks[1], makeLoopable(48000.0, 2.0, 330.0, 0.5f));
    decks[0].setKeyLock(true);
    decks[0].setPlaying(true);
    decks[1].setPlaying(true);
    decks[0].setLoop(0.2, 0.6);

    std::vector<float> l(512), r(512), cl(512), cr(512);
    float* outs[4] = { l.data(), r.data(), cl.data(), cr.data() };

    // warmup: stretches/mixers may touch one-time lazy state
    for (int i = 0; i < 20; ++i)
        mixer.render(outs, 4, 512, decks, 2, astats);

    AllocProbe probe;
    for (int i = 0; i < 200; ++i)
        mixer.render(outs, 4, 512, decks, 2, astats);
    REQUIRE(probe.count() == 0);

    for (auto& d : decks) cleanup(d);
}

// ---------------------------------------------------------------- stress
TEST_CASE("Stress: two decks + keylock + loops + moving crossfader + cue, ~110 s of audio") {
    constexpr int kBlock = 512;
    constexpr double kRate = 48000.0;
    constexpr int kBlocks = (int)(kRate * 110.0 / kBlock); // ~110 s

    DeckEngine decks[2];
    DeckStats stats[2];
    auto& sa = stats[0]; auto& sb = stats[1];
    rt::AudioStats astats;
    MixerEngine mixer;
    mixer.prepare(kRate);

    decks[0].bind(&sa); decks[1].bind(&sb);
    for (auto& deck : decks) {
        deck.setDeviceRate(kRate);
        deck.setStretcher(std::make_unique<WsolaStretcher>());
        deck.prepareDsp(kRate);
    }

    feed(decks[0], makeLoopable(kRate, 3.0, 200.0, 0.5f));
    feed(decks[1], makeLoopable(kRate, 3.0, 300.0, 0.5f));
    auto& deckA = decks[0]; auto& deckB = decks[1];

    // deck A: +20% pitch with key lock, tight loop
    deckA.setPitchRange(50.0f);
    deckA.setPitchPercent(20.0f);
    deckA.setKeyLock(true);
    deckA.setLoop(0.4, 0.55);
    deckA.setPlaying(true);

    // deck B: -10% pitch, no key lock, wider loop
    deckB.setPitchRange(50.0f);
    deckB.setPitchPercent(-10.0f);
    deckB.setLoop(0.3, 0.9);
    deckB.setPlaying(true);

    std::vector<float> l(kBlock), r(kBlock), cl(kBlock), cr(kBlock);
    float* outs[4] = { l.data(), r.data(), cl.data(), cr.data() };

    float maxPeak = 0.0f;
    bool nan = false;
    double prevPosA = 0.0, prevPosB = 0.0;
    int posAdvancesA = 0, posAdvancesB = 0;

    for (int b = 0; b < kBlocks; ++b) {
        // Sweep crossfader A->B->A continuously (approx 4 sweeps).
        const float phase = (float)b / (float)kBlocks * 8.0f;
        const float tri = phase - std::floor(phase);
        const float x = tri < 0.5f ? -1.0f + 4.0f * tri : 3.0f - 4.0f * tri;
        mixer.setCrossfader(x);

        // Toggle PFL periodically; wiggle cue mix.
        mixer.setPfl(0, (b / 500) % 2 == 0);
        mixer.setPfl(1, (b / 700) % 2 == 1);
        mixer.setCueMix((float)((b % 1000) / 1000.0));
        mixer.setMasterGain(0.5f + 0.5f * std::fabs(std::sin(b * 0.0003f)));

        mixer.render(outs, 4, kBlock, decks, 2, astats);

        for (int i = 0; i < kBlock; ++i) {
            if (!std::isfinite(l[i]) || !std::isfinite(r[i]) ||
                !std::isfinite(cl[i]) || !std::isfinite(cr[i])) nan = true;
            maxPeak = std::max(maxPeak, std::fabs(l[i]));
        }

        const double pa = sa.positionSec.load(), pb = sb.positionSec.load();
        if (pa > prevPosA + 1e-6) ++posAdvancesA;
        if (pb > prevPosB + 1e-6) ++posAdvancesB;
        prevPosA = pa; prevPosB = pb;
    }

    REQUIRE_FALSE(nan);
    // Deck A loops forever -> position stays in loop or advances steadily.
    REQUIRE(posAdvancesA > kBlocks / 2);
    REQUIRE(posAdvancesB > kBlocks / 2);
    REQUIRE(sa.positionSec.load() > 0.3);
    REQUIRE(sa.positionSec.load() < 0.7); // confined by loop ~0.4..0.55
    REQUIRE(sb.positionSec.load() >= 0.28);
    REQUIRE(sb.positionSec.load() <= 0.95);
    // Limiter keeps the hot mix below clipping.
    REQUIRE(maxPeak <= 1.0f);
    REQUIRE(maxPeak > 0.05f); // something actually played
    REQUIRE(astats.clipCount.load() == 0); // limiter prevented clipping
    REQUIRE(mixer.masterStats().peakL.load() > 0.01f);

    cleanup(deckA);
    cleanup(deckB);
}

TEST_CASE("Stress: reverse + seek storm does not corrupt transport") {
    constexpr int kBlock = 512;
    DeckEngine deck;
    DeckStats stats;
    deck.bind(&stats);
    deck.setDeviceRate(48000.0);
    deck.setStretcher(std::make_unique<WsolaStretcher>());
    deck.prepareDsp(48000.0);

    feed(deck, makeLoopable(48000.0, 4.0, 250.0, 0.5f));
    deck.setPlaying(true);

    std::vector<float> l(kBlock), r(kBlock);
    for (int b = 0; b < 2000; ++b) {
        if (b % 50 == 0) deck.setReverse((b / 50) % 2 == 0);
        if (b % 37 == 0) deck.seekSec(std::fmod(b * 0.013, 3.0));
        if (b % 91 == 0) deck.setLoop(0.5, 0.5 + (b % 5) * 0.25 + 0.25);
        deck.renderPcm(l.data(), r.data(), kBlock);
        for (float v : l) REQUIRE(std::isfinite(v));
        const double pos = stats.positionSec.load();
        REQUIRE(pos >= -0.01);
        REQUIRE(pos <= 4.0);
    }
    cleanup(deck);
}

TEST_CASE("Stress: load/unload churn while mixer keeps running") {
    constexpr int kBlock = 512;
    DeckEngine deck;
    DeckStats stats;
    rt::AudioStats astats;
    MixerEngine mixer;
    mixer.prepare(48000.0);
    deck.bind(&stats);
    deck.setDeviceRate(48000.0);
    deck.setStretcher(std::make_unique<WsolaStretcher>());
    deck.prepareDsp(48000.0);

    std::vector<float> l(kBlock), r(kBlock);
    std::vector<const TrackAudio*> graveyard;

    for (int round = 0; round < 40; ++round) {
        auto t = makeLoopable(48000.0, 0.4, 180.0 + round * 10.0, 0.4f);
        deck.pendingTrack_.store(t.release());
        float zl[8]{}, zr[8]{};
        deck.renderPcm(zl, zr, 8);
        deck.setPlaying(true);
        for (int b = 0; b < 25; ++b) deck.renderPcm(l.data(), r.data(), kBlock);
        if (const TrackAudio* old = deck.retiredTrack_.exchange(nullptr))
            graveyard.push_back(old);
    }
    deck.unloadTrack();
    if (const TrackAudio* old = deck.retiredTrack_.exchange(nullptr))
        graveyard.push_back(old);
    for (auto* t : graveyard) delete t;
    REQUIRE(stats.state.load() == (int)DeckState::Empty);
}
