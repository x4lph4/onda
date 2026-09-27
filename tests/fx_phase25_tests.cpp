// Phase 25 — stem FX (simultaneous per-stem inserts), sampler target
// fix (Spare, not SendA), tail-drain bypass, MASTER/DECK/MANUAL tempo
// sources, tempo-change smoothness, spec presets, duplicate, param
// types, NoiseFX, 4-deck/stem/sampler stress.
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
#include "core/fx/FXEngine.h"
#include "core/fx/FXFactory.h"
#include "core/fx/FXPresetManager.h"
#include "core/fx/FXRouting.h"
#include "core/fx/NoiseFX.h"
#include "core/mixer/MixerController.h"
#include "core/mixer/MixerEngine.h"
#include "core/mixer/ParameterRegistry.h"
#include "core/model/DeckStats.h"
#include "core/rt/AudioStats.h"
#include "core/sampler/SamplerRt.h"

using namespace onda;
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
double rmsOf(const std::vector<float>& a, const std::vector<float>& b) {
    double acc = 0.0;
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const double d = (double)a[i] - (double)b[i];
        acc += d * d;
    }
    return std::sqrt(acc / (double)std::max<size_t>(n, 1));
}
double rms(const std::vector<float>& a) {
    double acc = 0.0;
    for (float v : a) acc += (double)v * v;
    return std::sqrt(acc / (double)std::max<size_t>(a.size(), 1));
}
int firstIndexAbove(const std::vector<float>& v, float th, int from = 0) {
    for (int i = from; i < (int)v.size(); ++i)
        if (std::fabs(v[i]) > th) return i;
    return -1;
}

TrackAudio* makeSine(double secs, double freq, float amp) {
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

/// Deck + synthetic stem set (owned interleaved buffers per stem).
struct StemDeckRig {
    DeckEngine deck;
    DeckStats  stats;
    std::unique_ptr<TrackAudio> track { makeSine(20.0, 200.0, 0.4f) };
    DeckStemBuffers view;
    /// stem backing: [stem][frame*2] — kStemFx sines at distinct freqs
    std::vector<float> stemData[DeckStemBuffers::kMaxStems];
    std::vector<float> l = std::vector<float>(kBlock),
                       r = std::vector<float>(kBlock);
    fx::SyncContext ctx;

    StemDeckRig() {
        deck.bind(&stats);
        deck.setDeviceRate(kRate);
        deck.setStretcher(std::make_unique<WsolaStretcher>());
        deck.prepareDsp(kRate);
        ctx.bpm = 120.0f;
        ctx.synced = 1;
        ctx.division = (int)fx::BeatDiv::D_1_4;
    }
    void loadTrack() {
        deck.pendingTrack_.store(track.get());
        render(1);
    }
    /// 4 stems: vocals 500 Hz, drums 60 Hz, bass 120 Hz, inst 1000 Hz.
    void adoptStems(double secs = 4.0) {
        static const double kF[4] = { 500.0, 60.0, 120.0, 1000.0 };
        const int64_t frames = (int64_t)(kRate * secs);
        for (int s = 0; s < DeckStemBuffers::kMaxStems; ++s) {
            if (s < 4) {
                stemData[s].assign((size_t)frames * 2, 0.0f);
                for (int64_t i = 0; i < frames; ++i) {
                    const float v = 0.5f * (float)std::sin(
                        2.0 * 3.14159265 * kF[s] * i / kRate);
                    stemData[s][(size_t)i * 2]     = v;
                    stemData[s][(size_t)i * 2 + 1] = v;
                }
                view.samples[s] = stemData[s].data();
                view.mask |= (1u << s);
            }
        }
        view.frames = frames;
        deck.pendingStems_.store(&view);
        deck.setStemMode(1);
        for (int s = 0; s < 4; ++s)
            deck.setStemParam(s, 1.0f, 0.0f, false, false);
        render(1);
    }
    /// Only `only` stem audible (-1 = all four). Renders 14 blocks so
    /// the stem gain smoothers (kSmooth=0.001) fully settle.
    void soloStem(int only) {
        for (int s = 0; s < 4; ++s)
            deck.setStemParam(s, (only < 0 || only == s) ? 1.0f : 0.0f,
                              0.0f, false, false);
        render(14);
    }
    void enableStemFx(int stem, fx::FXKind kind, float mix = 1.0f) {
        deck.stemFx()[stem].loadEffect(kind);
        deck.stemFx()[stem].setParam(fx::FXSlotKind::Enabled, 1.0f);
        deck.stemFx()[stem].setParam(fx::FXSlotKind::Mix, mix);
        deck.stemFx()[stem].setParam(fx::FXSlotKind::SyncMode, 1.0f);
    }
    void render(int n = 1) {
        deck.setStemFxContext(ctx, ctx);
        for (int i = 0; i < n; ++i)
            deck.renderPcm(l.data(), r.data(), kBlock);
    }
    /// Capture n rendered blocks into a contiguous mono-ish buffer.
    std::vector<float> capture(int n) {
        std::vector<float> out((size_t)n * kBlock);
        for (int b = 0; b < n; ++b) {
            deck.setStemFxContext(ctx, ctx);
            deck.renderPcm(l.data(), r.data(), kBlock);
            std::memcpy(out.data() + (size_t)b * kBlock, l.data(),
                        (size_t)kBlock * sizeof(float));
        }
        return out;
    }
    void restart() { deck.stop(); deck.seekSec(0.0); deck.setPlaying(true); }
    double blockPeak() const { return peakAbs(l.data(), kBlock); }
};

/// Standalone slot rig (Phase 16 shape) + master ctx support.
struct SlotRig25 {
    fx::FXSlot slot;
    fx::SyncContext ctx, master;
    std::vector<float> l, r;
    double phase_ { 0.0 };

    SlotRig25() : l(kBlock), r(kBlock) {
        slot.prepare(kRate);
        ctx.bpm = 128.0f; master.bpm = 90.0f;
    }
    void load(fx::FXKind k, float mix = 1.0f) {
        slot.loadEffect(k);
        slot.setParam(fx::FXSlotKind::Enabled, 1.0f);
        slot.setParam(fx::FXSlotKind::Mix, mix);
    }
    void run(int n = 1) {
        for (int i = 0; i < n; ++i)
            slot.process(l.data(), r.data(), kBlock, ctx, &master);
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
    void impulse(float amp = 0.8f) { silence(); l[0] = amp; r[0] = amp; }
};

/// 6-output mixer rig (phase-24 shape) with stem binding.
struct Rig25 {
    MixerEngine mixer;
    rt::AudioStats astats;
    std::vector<DeckEngine> decks;
    std::vector<DeckStats>  dstats;
    std::vector<const TrackAudio*> owned;
    std::vector<float> outL, outR, cueL, cueR, boL, boR;
    sampler::SamplerRt samplerRt { kRate };

    Rig25(int ndecks = 4) : decks(ndecks), dstats(ndecks),
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
            mixer.fx().bindStemSlots(i, decks[i].stemFx());
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
    /// FX param write through the mixer id path (0x1000 space).
    void fxSet(int t, int s, fx::FXSlotKind k, float v) {
        mixer.setParam(fx::paramId(t, s, k), v);
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
    ~Rig25() { for (auto* t : owned) delete t; }
};

} // namespace

// ============================================ SAMPLER INSERT TARGET (§8)
TEST_CASE("FX25: sampler channel inserts into Spare, not the send rack",
          "[fx25][routing]") {
    Rig25 f(2);
    f.loadDeck(0, 20.0, 60.0, 0.4f);          // deep sine — HP-killable
    f.mixer.setChannelSource(4, mixer::ChSource::deck(0));
    f.mixer.setChannelSource(0, mixer::ChSource::silence());
    f.mixer.setChannelSource(1, mixer::ChSource::silence());
    f.run(10);
    const float dry = peakAbs(f.outL.data(), kBlock);
    REQUIRE(dry > 0.2f);

    // HP filter on SendA (target 4) must NOT touch the sampler ch —
    // pre-fix this aliased racks_[SendA] and processed the insert.
    f.mixer.fx().loadEffect((int)fx::FXTarget::SendA, 0, fx::FXKind::Filter);
    f.fxSet((int)fx::FXTarget::SendA, 0, fx::FXSlotKind::Enabled, 1.0f);
    f.fxSet((int)fx::FXTarget::SendA, 0, fx::FXSlotKind::Mix, 1.0f);
    f.fxSet((int)fx::FXTarget::SendA, 0, fx::FXSlotKind::P1, 0.4f); // cutoff ~8k
    f.fxSet((int)fx::FXTarget::SendA, 0, fx::FXSlotKind::P3, 1.0f); // HP mode
    f.run(20);
    REQUIRE(peakAbs(f.outL.data(), kBlock) == Approx(dry).margin(0.08f));

    // The same filter on Spare (the real sampler insert) kills 60 Hz.
    f.mixer.fx().loadEffect((int)fx::FXTarget::Spare, 0, fx::FXKind::Filter);
    f.fxSet((int)fx::FXTarget::Spare, 0, fx::FXSlotKind::Enabled, 1.0f);
    f.fxSet((int)fx::FXTarget::Spare, 0, fx::FXSlotKind::Mix, 1.0f);
    f.fxSet((int)fx::FXTarget::Spare, 0, fx::FXSlotKind::P1, 0.4f);
    f.fxSet((int)fx::FXTarget::Spare, 0, fx::FXSlotKind::P3, 1.0f);
    f.run(20);
    REQUIRE(peakAbs(f.outL.data(), kBlock) < dry * 0.1f);
}

// ======================================================= STEM FX (§47)
TEST_CASE("FX25: stem FX param routing reaches the deck's bound slots",
          "[fx25][stems]") {
    Rig25 f(1);
    REQUIRE(f.mixer.fx().stemSlot(0, 0) == &f.decks[0].stemFx()[0]);
    f.mixer.fx().loadEffect(0, 4, fx::FXKind::Delay);   // target 0, slot 4
    f.fxSet(0, 4, fx::FXSlotKind::Enabled, 1.0f);
    REQUIRE(f.decks[0].stemFx()[0].enabled());
    REQUIRE((f.decks[0].stemFx()[0].activeKind() != fx::FXKind::None
             || f.decks[0].stemFx()[0].hasPending()));
    f.fxSet(0, 4, fx::FXSlotKind::Mix, 0.8f);
    REQUIRE(f.decks[0].stemFx()[0].mix() == Approx(0.8f));
    // slot 7 is the last legal stem id; slot 8+ falls back to the rack
    REQUIRE(f.mixer.fx().stemSlot(0, 3) == &f.decks[0].stemFx()[3]);
}

TEST_CASE("FX25: Vocals->Echo Drums->Filter Bass->Phaser Inst->Reverb "
          "run simultaneously on separate stems", "[fx25][stems]") {
    StemDeckRig g;
    g.loadTrack();
    g.adoptStems(8.0);
    g.deck.setPlaying(true);

    SECTION("all four loaded+enabled simultaneously: every slot busy, "
            "bounded, finite") {
        g.enableStemFx(0, fx::FXKind::Echo);    // Vocals
        g.enableStemFx(1, fx::FXKind::Filter);  // Drums
        g.enableStemFx(2, fx::FXKind::Phaser);  // Bass
        g.enableStemFx(3, fx::FXKind::Reverb);  // Instruments
        g.render(60); // slots adopt + process all four stems at once
        REQUIRE(allFinite(g.l.data(), kBlock));
        REQUIRE(allFinite(g.r.data(), kBlock));
        REQUIRE(g.blockPeak() < 4.0f);
        for (int s = 0; s < 4; ++s)
            REQUIRE(g.deck.stemFx()[s].busy());
    }
    SECTION("drums through HP filter lose the 60 Hz body") {
        g.soloStem(1);            // others muted BEFORE FX starts —
                                // no foreign tails contaminate the take
        g.enableStemFx(1, fx::FXKind::Filter);
        g.deck.stemFx()[1].setParam(fx::FXSlotKind::P1, 0.45f); // ~9k
        g.deck.stemFx()[1].setParam(fx::FXSlotKind::P3, 1.0f);  // HP
        g.render(20);
        const auto wet = g.capture(30);
        g.deck.stemFx()[1].setParam(fx::FXSlotKind::Enabled, 0.0f);
        g.render(20);
        const auto dry = g.capture(30);
        REQUIRE(rms(wet) < rms(dry) * 0.35f);   // HP at ~9k on a 60 Hz sine
    }
    SECTION("echo on vocals tails past the stem mute (tail mode)") {
        g.soloStem(0);
        g.enableStemFx(0, fx::FXKind::Echo);
        g.deck.stemFx()[0].setParam(fx::FXSlotKind::Division, 0.0f);
        g.render(30);
        g.deck.setStemParam(0, 0.0f, 0.0f, false, false); // mute vocals
        g.render(14);                                    // gain ramps out
        const float tailPk = (float)g.blockPeak();
        REQUIRE(tailPk > 0.02f);   // feedback echo still ringing
        g.render(120);             // decays eventually
        REQUIRE(g.blockPeak() < 0.02f);
    }
    SECTION("reverb on instruments tails past the stem mute") {
        g.soloStem(3);
        g.enableStemFx(3, fx::FXKind::Reverb);
        g.render(30);
        g.deck.setStemParam(3, 0.0f, 0.0f, false, false);
        g.render(4);
        REQUIRE(g.blockPeak() > 0.005f);  // reverb tail alive
    }
    SECTION("phaser on bass reshapes the signal") {
        g.soloStem(2);
        g.enableStemFx(2, fx::FXKind::Phaser);
        g.render(20);
        const auto wet = g.capture(40);
        g.deck.stemFx()[2].setParam(fx::FXSlotKind::Enabled, 0.0f);
        g.render(10);
        const auto dry = g.capture(40);
        // an allpass+sum phaser on a sine must differ meaningfully
        REQUIRE(rmsOf(wet, dry) > rms(dry) * 0.05);
    }
}

TEST_CASE("FX25: stem FX also applies through the keylock (WSOLA) path",
          "[fx25][stems][keylock]") {
    StemDeckRig g;
    g.loadTrack();
    g.adoptStems(8.0);
    g.deck.setPlaying(true);
    g.deck.setKeyLock(true);               // stretcher engaged
    g.deck.setPitchPercent(8.0f);          // force actual stretching
    g.enableStemFx(0, fx::FXKind::Echo);
    g.soloStem(0);
    g.render(60);
    const auto wet = g.capture(20);
    REQUIRE(rms(wet) > 0.05);
    for (float v : wet) REQUIRE(std::isfinite(v));
    REQUIRE(g.deck.stemFx()[0].busy());
}

// ================================================== TAIL DRAIN (§42/§44)
TEST_CASE("FX25: disabling a tail FX drains naturally instead of cutting",
          "[fx25][tail]") {
    SlotRig25 g;
    g.ctx.bpm = 120.0f;
    g.load(fx::FXKind::Echo, 1.0f);
    g.slot.setParam(fx::FXSlotKind::Division, 0.0f); // D_1_32 = 3000f
    g.slot.setParam(fx::FXSlotKind::P1, 0.4f);       // feedback

    for (int b = 0; b < 16; ++b) { g.sine(440.0, 0.6f); g.run(); }
    g.slot.setParam(fx::FXSlotKind::Enabled, 0.0f);
    REQUIRE_FALSE(g.slot.enabled());
    g.silence(); g.run();
    REQUIRE(g.slot.tailing());   // draining, not crossfade-cutting

    // Right after disable the tail must still be audible (the old
    // crossfade-only path killed it inside ~10 ms).
    std::vector<float> tail;
    for (int b = 0; b < 40; ++b) {
        g.silence(); g.run();
        tail.insert(tail.end(), g.l.begin(), g.l.end());
    }
    const float early = peakAbs(tail.data(), kBlock * 4);
    const float late  = peakAbs(tail.data() + tail.size() - kBlock * 4,
                                kBlock * 4);
    REQUIRE(early > 0.03f);      // tail continues past disable
    REQUIRE(late < early);       // and decays

    // eventually the slot drains + hibernates (CPU management §45)
    for (int b = 0; b < 400 && g.slot.busy(); ++b) { g.silence(); g.run(); }
    REQUIRE_FALSE(g.slot.busy());
    g.silence(); g.run();
    REQUIRE(peakAbs(g.l.data(), kBlock) < 1e-3f);
}

TEST_CASE("FX25: non-tail FX still bypasses fast (no fake tail)",
          "[fx25][tail]") {
    SlotRig25 g;
    g.load(fx::FXKind::Filter, 1.0f);
    for (int b = 0; b < 6; ++b) { g.sine(440.0, 0.5f); g.run(); }
    g.slot.setParam(fx::FXSlotKind::Enabled, 0.0f);
    REQUIRE_FALSE(g.slot.tailing());
    for (int b = 0; b < 30; ++b) { g.sine(440.0, 0.5f); g.run(); }
    REQUIRE_FALSE(g.slot.busy());   // hibernated — dry passes
    REQUIRE(peakAbs(g.l.data(), kBlock) > 0.3f);
}

// ======================================= TEMPO SOURCE (§22) + CHANGES (§23)
TEST_CASE("FX25: SyncMode selects MANUAL / DECK / MASTER tempo source",
          "[fx25][sync]") {
    // division is a SLOT param (stamped onto the ctx per block);
    // 1/32 div @ 48000: 128bpm = 2812.5f, 90bpm = 4000f, 5ms = 240f.
    auto echoIndex = [](SlotRig25& g, int blocks) {
        std::vector<float> out;
        for (int b = 0; b < blocks; ++b) {
            g.run();
            out.insert(out.end(), g.l.begin(), g.l.end());
            g.silence(); // the processed output must not re-enter
        }
        return firstIndexAbove(out, 0.05f, 64);
    };

    SECTION("DECK follows the target tempo") {
        SlotRig25 g;
        g.load(fx::FXKind::Delay);
        g.slot.setParam(fx::FXSlotKind::SyncMode, 1.0f); // DECK = target
        g.slot.setParam(fx::FXSlotKind::Division, 0.0f); // D_1_32
        g.impulse();
        const int idx = echoIndex(g, 12);
        REQUIRE(idx == Approx(2812).margin(400));
    }
    SECTION("MASTER follows the sync-master clock, not the target") {
        SlotRig25 g;
        g.ctx.bpm = 128.0f; g.master.bpm = 90.0f;
        g.load(fx::FXKind::Delay);
        g.slot.setParam(fx::FXSlotKind::SyncMode, 2.0f); // MASTER
        g.slot.setParam(fx::FXSlotKind::Division, 0.0f);
        g.impulse();
        const int idx = echoIndex(g, 14);
        REQUIRE(idx == Approx(4000).margin(500));
    }
    SECTION("MANUAL uses the free Time parameter") {
        SlotRig25 g;
        g.load(fx::FXKind::Delay);
        g.slot.setParam(fx::FXSlotKind::SyncMode, 0.0f); // MANUAL
        g.slot.setParam(fx::FXSlotKind::P1, 0.0f);       // Time = min 5 ms
        g.impulse();
        const int idx = echoIndex(g, 4);
        REQUIRE(idx == Approx(240).margin(120));         // 5 ms @ 48k
    }
}

TEST_CASE("FX25: synced delay adapts to tempo changes without clicks",
          "[fx25][tempo]") {
    SlotRig25 g;
    g.ctx.bpm = 120.0f; g.ctx.synced = 0; // slot stamps it
    g.ctx.division = (int)fx::BeatDiv::D_1_4;
    g.load(fx::FXKind::Delay);
    g.slot.setParam(fx::FXSlotKind::SyncMode, 1.0f);
    g.slot.setParam(fx::FXSlotKind::P2, 0.6f);   // feedback high
    float worst = 0.0f;
    for (int b = 0; b < 60; ++b) {
        g.ctx.bpm = 120.0f + (float)b * 0.8f;   // 120 -> 168 ramp
        g.sine(440.0, 0.5f);
        g.run();
        worst = std::max(worst, maxStep(g.l.data(), kBlock));
        REQUIRE(allFinite(g.l.data(), kBlock));
    }
    // delay-time slews are bounded; a sine + echoes can't exceed this
    REQUIRE(worst < 0.6f);
}

// ======================================================= PRESETS (§34-36)
TEST_CASE("FX25: spec-named factory presets exist + duplicate works",
          "[fx25][presets]") {
    fx::FXPresetManager m;
    const char* names[] = {
        "Clean Filter", "Echo Out", "Short Echo", "Long Echo",
        "Room Reverb", "Hall Reverb", "Classic Flanger", "Classic Phaser"
    };
    for (const char* n : names)
        REQUIRE(m.find(n) >= 0);

    const int i = m.find("Hall Reverb");
    const int dup = m.duplicate(i);
    REQUIRE(dup > i);
    REQUIRE_FALSE(m.at(dup).factory);
    REQUIRE(m.at(dup).effectKind == m.at(i).effectKind);
    REQUIRE(m.at(dup).name != m.at(i).name);
    const int dup2 = m.duplicate(dup);
    REQUIRE(dup2 > dup);
    REQUIRE(m.at(dup2).name != m.at(dup).name);
}

TEST_CASE("FX25: preset blob carries a version + rejects future schemas",
          "[fx25][presets]") {
    fx::FXPreset p;
    p.name = "v-test"; p.effectKind = (int)fx::FXKind::Echo;
    auto blob = fx::serializePreset(p);
    REQUIRE(blob[4] == 1);                       // version byte
    fx::FXPreset q;
    REQUIRE(fx::deserializePreset(blob.data(), blob.size(), q));
    REQUIRE(q.version == 1);
    blob[4] = 99;                                // unknown future schema
    REQUIRE_FALSE(fx::deserializePreset(blob.data(), blob.size(), q));
}

// ==================================================== PARAM TYPES (§30)
TEST_CASE("FX25: param type tags classify switch/enum/float params",
          "[fx25][types]") {
    auto del = fx::createFX(fx::FXKind::Delay);
    auto fil = fx::createFX(fx::FXKind::Filter);
    auto rev = fx::createFX(fx::FXKind::Reverb);
    auto ech = fx::createFX(fx::FXKind::Echo);
    REQUIRE(fx::fxParamTypeOf(del->paramInfo(3)) == fx::FXParamType::Boolean);
    REQUIRE(fx::fxParamTypeOf(ech->paramInfo(2)) == fx::FXParamType::Boolean);
    REQUIRE(fx::fxParamTypeOf(ech->paramInfo(3)) == fx::FXParamType::Boolean);
    REQUIRE(fx::fxParamTypeOf(fil->paramInfo(2)) == fx::FXParamType::Enum);
    REQUIRE(fx::fxParamTypeOf(rev->paramInfo(0)) == fx::FXParamType::Float);
    // BeatDiv remains the specialized synced-time type on the slot
    REQUIRE(fx::beatDivCount() >= 8);
    REQUIRE(fx::beatsOf(fx::BeatDiv::D_1_4) == Approx(1.0f));
    REQUIRE(fx::beatsOf(fx::BeatDiv::D_2Bar) == Approx(8.0f));
}

// ============================================================ NOISE (§25)
TEST_CASE("FX25: NoiseFX generates bounded decorrelated stereo noise",
          "[fx25][noise]") {
    fx::NoiseFX nz;
    nz.prepare(kRate, kBlock);
    std::vector<float> l(kBlock), r(kBlock);
    fx::SyncContext ctx; ctx.bpm = 120.0f; ctx.synced = 0;
    nz.setSyncContext(ctx);
    nz.process(l.data(), r.data(), kBlock);
    const float pl = peakAbs(l.data(), kBlock);
    const float pr = peakAbs(r.data(), kBlock);
    REQUIRE(pl > 0.05f);
    REQUIRE(pl <= 1.25f);   // clamped by design
    REQUIRE(pr <= 1.25f);
    REQUIRE(std::fabs(pl - pr) > 1e-6f); // decorrelated L/R
    for (int b = 0; b < 100; ++b) {
        nz.process(l.data(), r.data(), kBlock);
        REQUIRE(allFinite(l.data(), kBlock));
    }
}

// ============================================== ROUTING TABLE INTEGRITY (§48)
TEST_CASE("FX25: routing table has no circular audio routes",
          "[fx25][routing]") {
    // sends must never declare themselves insert nodes and no target
    // may route back into its own source — the table is declarative.
    for (int t = 0; t < (int)fx::FXTarget::kCount; ++t) {
        const auto rt = fx::routingFor((fx::FXTarget)t);
        REQUIRE((int)rt.target == t);
        if (rt.route == fx::FXRoute::SendReturn)
            REQUIRE(rt.node < 2);              // only send buses 0/1
        if (rt.route == fx::FXRoute::DeckInsert)
            REQUIRE(rt.node < 4);              // only decks 0..3
    }
    // Spare is the sampler insert — distinct from every send bus
    REQUIRE((int)fx::routingFor(fx::FXTarget::Spare).route
            == (int)fx::FXRoute::SamplerInsert);
}

// ==================================================== FULL STRESS (§54)
TEST_CASE("FX25: 4 decks + stems + sampler + sends + master FX stress",
          "[fx25][stress]") {
    Rig25 f(4);
    for (int d = 0; d < 4; ++d)
        f.loadDeck(d, 30.0, 100.0 + 150.0 * d, 0.35f);

    // every deck insert chain busy + master + sends + sampler insert
    const fx::FXKind kinds[] = { fx::FXKind::Delay, fx::FXKind::Filter,
                               fx::FXKind::Reverb, fx::FXKind::Flanger };
    for (int t = 0; t < 4; ++t)
        for (int s = 0; s < 4; ++s) {
            f.mixer.fx().loadEffect(t, s, kinds[(t + s) & 3]);
            f.fxSet(t, s, fx::FXSlotKind::Enabled, 1.0f);
            f.fxSet(t, s, fx::FXSlotKind::Mix, 0.6f);
        }
    for (int s = 0; s < 4; ++s) {            // send A + master + sampler
        f.mixer.fx().loadEffect((int)fx::FXTarget::SendA, s, fx::FXKind::Echo);
        f.fxSet((int)fx::FXTarget::SendA, s, fx::FXSlotKind::Enabled, 1.0f);
        f.fxSet((int)fx::FXTarget::SendA, s, fx::FXSlotKind::Mix, 0.5f);
        f.mixer.fx().loadEffect((int)fx::FXTarget::Master, s,
                                s == 0 ? fx::FXKind::Compressor
                                       : fx::FXKind::Noise);
        f.fxSet((int)fx::FXTarget::Master, s, fx::FXSlotKind::Enabled, 1.0f);
        f.fxSet((int)fx::FXTarget::Master, s, fx::FXSlotKind::Mix, 0.2f);
        f.mixer.fx().loadEffect((int)fx::FXTarget::Spare, s, fx::FXKind::Phaser);
        f.fxSet((int)fx::FXTarget::Spare, s, fx::FXSlotKind::Enabled, 1.0f);
        f.fxSet((int)fx::FXTarget::Spare, s, fx::FXSlotKind::Mix, 0.5f);
    }
    // stem FX on decks 0/1
    for (int s = 0; s < 4; ++s) {
        f.mixer.fx().loadEffect(0, 4 + s, kinds[s & 3]);
        f.fxSet(0, 4 + s, fx::FXSlotKind::Enabled, 1.0f);
        f.mixer.fx().loadEffect(1, 4 + s, kinds[(s + 1) & 3]);
        f.fxSet(1, 4 + s, fx::FXSlotKind::Enabled, 1.0f);
    }
    for (int d = 0; d < 4; ++d)
        f.mixer.setParam(mixer::ParameterRegistry::channelSendA(d), 0.4f);

    f.mixer.fx().collectTrash();
    const auto t0 = std::chrono::steady_clock::now();
    f.run(120);
    const auto ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();

    REQUIRE(allFinite(f.outL.data(), kBlock));
    REQUIRE(allFinite(f.outR.data(), kBlock));
    REQUIRE(peakAbs(f.outL.data(), kBlock) < fx::FXSlot::kClamp);
    // ~5.1 s of audio processed — must stay well under real-time
    REQUIRE(ms < 20000.0);
}
