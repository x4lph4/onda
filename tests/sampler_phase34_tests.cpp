// Phase 34 — Professional Sampler deltas: Retrigger mode, extended
// quantize (1/2, 2bar, 4bar), per-slot envelope, non-destructive
// trim/loop windows, per-slot tilt filter, quantized stop, missing-
// sample handling, metadata undo/redo, repository roundtrip.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

#include "core/rt/CommandQueue.h"
#include "core/data/Database.h"
#include "core/sampler/SampleBank.h"
#include "core/sampler/SampleRepository.h"
#include "core/sampler/SamplerEngine.h"
#include "core/sampler/SamplerRt.h"
#include "core/sampler/SamplerTypes.h"
#include "platform/decode/FFmpegDecoder.h"

#include "test_wav.h"

using namespace onda;
namespace fs = std::filesystem;

namespace {

fs::path tempDir34(const char* n) {
    fs::path d = fs::temp_directory_path() / "onda_smp34" / n;
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

std::shared_ptr<const sampler::SampleData>
makeConst(int64_t frames, float amp = 0.5f) {
    auto d = std::make_shared<sampler::SampleData>();
    auto* l = new float[(size_t)frames];
    auto* r = new float[(size_t)frames];
    std::fill(l, l + frames, amp);
    std::fill(r, r + frames, amp);
    d->l = std::shared_ptr<const float[]>(l);
    d->r = std::shared_ptr<const float[]>(r);
    d->frames = frames;
    d->sampleRate = 48000.0;
    d->channels = 2;
    d->peak = amp;
    return d;
}

/// value ramps 0→amp across the sample — position is audible
std::shared_ptr<const sampler::SampleData>
makeRamp(int64_t frames, float amp = 0.9f) {
    auto d = std::make_shared<sampler::SampleData>();
    auto* l = new float[(size_t)frames];
    auto* r = new float[(size_t)frames];
    for (int64_t i = 0; i < frames; ++i)
        l[i] = r[i] = amp * (float)i / (float)frames;
    d->l = std::shared_ptr<const float[]>(l);
    d->r = std::shared_ptr<const float[]>(r);
    d->frames = frames;
    d->sampleRate = 48000.0;
    d->channels = 2;
    d->peak = amp;
    return d;
}

/// silence except [a,b) filled with amp — loop regions are audible
std::shared_ptr<const sampler::SampleData>
makeZoned(int64_t frames, int64_t a, int64_t b, float amp = 0.5f) {
    auto d = std::make_shared<sampler::SampleData>();
    auto* l = new float[(size_t)frames];
    auto* r = new float[(size_t)frames];
    std::fill(l, l + frames, 0.0f);
    std::fill(r, r + frames, 0.0f);
    std::fill(l + a, l + b, amp);
    std::fill(r + a, r + b, amp);
    d->l = std::shared_ptr<const float[]>(l);
    d->r = std::shared_ptr<const float[]>(r);
    d->frames = frames;
    d->sampleRate = 48000.0;
    d->channels = 2;
    d->peak = amp;
    return d;
}

std::shared_ptr<const sampler::SampleData>
makeSine(int64_t frames, double period, float amp = 0.5f) {
    auto d = std::make_shared<sampler::SampleData>();
    auto* l = new float[(size_t)frames];
    auto* r = new float[(size_t)frames];
    for (int64_t i = 0; i < frames; ++i)
        l[i] = r[i] = amp *
            (float)std::sin(6.28318530718 * (double)i / period);
    d->l = std::shared_ptr<const float[]>(l);
    d->r = std::shared_ptr<const float[]>(r);
    d->frames = frames;
    d->sampleRate = 48000.0;
    d->channels = 2;
    d->peak = amp;
    return d;
}

struct Buf {
    std::vector<float> l, r;
    explicit Buf(int n) : l((size_t)n, 0.0f), r((size_t)n, 0.0f) {}
    void render(sampler::SamplerRt& rt, int n) {
        std::fill(l.begin(), l.end(), 0.0f);
        std::fill(r.begin(), r.end(), 0.0f);
        rt.renderPcm(l.data(), r.data(), n);
    }
    double energy() const {
        double e = 0.0;
        for (float v : l) e += (double)std::fabs(v);
        for (float v : r) e += (double)std::fabs(v);
        return e;
    }
};

struct Rig {
    sampler::SamplerRt rt { 48000.0 };
    Buf b { 512 };
    void load(int slot, std::shared_ptr<const sampler::SampleData> d,
              uint8_t mode = (uint8_t)sampler::PlaybackMode::OneShot) {
        rt.applyCommand(rt::SampleSlotParam{(uint8_t)slot,
                                            (uint8_t)sampler::SP_Mode,
                                            (float)mode});
        rt.publishSlot(slot, std::move(d));
        b.render(rt, 64);
    }
    void trig(int slot, float vel = 1.0f) {
        rt.applyCommand(rt::SampleTrigger{(uint8_t)slot, vel});
    }
    void rel(int slot) { rt.applyCommand(rt::SampleRelease{(uint8_t)slot}); }
    void stop(int slot) { rt.applyCommand(rt::SampleStop{(uint8_t)slot}); }
    void par(int slot, uint8_t kind, float v) {
        rt.applyCommand(rt::SampleSlotParam{(uint8_t)slot, kind, v});
    }
};

} // namespace

// ============================ enums / tables ============================

TEST_CASE("Smp34: Retrigger mode + extended quantize values", "[smp34]") {
    // appended enum values keep persisted indices stable
    REQUIRE((int)sampler::PlaybackMode::Retrigger == 4);
    REQUIRE((int)sampler::PlaybackMode::OneShot == 0);
    REQUIRE((int)sampler::PlaybackMode::Gate == 3);

    REQUIRE((int)sampler::QuantizeMode::Q_1_2 == 6);
    REQUIRE((int)sampler::QuantizeMode::Q_2Bar == 7);
    REQUIRE((int)sampler::QuantizeMode::Q_4Bar == 8);
    REQUIRE(sampler::quantizeBeats(sampler::QuantizeMode::Q_1_2)
            == Catch::Approx(2.0));
    REQUIRE(sampler::quantizeBeats(sampler::QuantizeMode::Q_2Bar)
            == Catch::Approx(8.0));
    REQUIRE(sampler::quantizeBeats(sampler::QuantizeMode::Q_4Bar)
            == Catch::Approx(16.0));
    // pre-existing values unchanged
    REQUIRE(sampler::quantizeBeats(sampler::QuantizeMode::Q_1_16)
            == Catch::Approx(0.25));
    REQUIRE(sampler::quantizeBeats(sampler::QuantizeMode::Bar)
            == Catch::Approx(4.0));
    REQUIRE(sampler::quantizeBeats(sampler::QuantizeMode::Off) == 0.0);
}

// ============================ retrigger ============================

TEST_CASE("Smp34: Retrigger stacks same-slot voices; OneShot chokes",
          "[smp34]") {
    Rig poly, mono;
    poly.load(0, makeConst(48000),
              (uint8_t)sampler::PlaybackMode::Retrigger);
    mono.load(0, makeConst(48000));                    // OneShot
    for (int i = 0; i < 3; ++i) { poly.trig(0); mono.trig(0); }
    poly.b.render(poly.rt, 512);
    mono.b.render(mono.rt, 512);
    REQUIRE(poly.rt.activeVoices() == 3);   // polyphonic pile-up
    REQUIRE(mono.rt.activeVoices() == 1);   // choke, replaced
}

TEST_CASE("Smp34: Retrigger rapid-fire respects the voice cap",
          "[smp34]") {
    Rig r;
    r.load(0, makeConst(96000),
           (uint8_t)sampler::PlaybackMode::Retrigger);
    for (int i = 0; i < 64; ++i) r.trig(0);  // more than kMaxVoices
    r.b.render(r.rt, 512);
    REQUIRE(r.rt.activeVoices() <= sampler::SamplerRt::kMaxVoices);
    REQUIRE(r.rt.activeVoices() > 8);        // real polyphony
    // steal never corrupts the render — output stays finite
    double peak = 0.0;
    for (int i = 0; i < 8; ++i) {
        r.b.render(r.rt, 512);
        for (float v : r.b.l) peak = std::max(peak, (double)std::fabs(v));
    }
    REQUIRE(std::isfinite(peak));
}

// ============================ extended quantize ============================

TEST_CASE("Smp34: quantize 1/2, 2bar, 4bar delay to their boundary",
          "[smp34]") {
    // 120 bpm → 24000 fr/beat. beatPos 0.5 → Q_1_2 waits 1.5 beats.
    Rig half;
    half.load(0, makeConst(48000));
    half.rt.setClock(120.0, 0.5);
    half.par(0, sampler::SP_Quantize,
             (float)sampler::QuantizeMode::Q_1_2);
    half.trig(0);
    half.b.render(half.rt, 512);
    REQUIRE(half.rt.activeVoices() == 1);      // scheduled
    REQUIRE(half.b.energy() == 0.0);           // not audible yet
    for (int i = 0; i < 60; ++i) half.b.render(half.rt, 512); // 30720f
    REQUIRE(half.b.energy() == 0.0);           // still before 1.5 beats
    double e = 0.0;
    for (int i = 0; i < 16; ++i) {
        half.b.render(half.rt, 512);
        e += half.b.energy();
    }
    REQUIRE(e > 0.0);                          // started at boundary

    // 240 bpm → 12000 fr/beat. beatPos 3.0 → Q_2Bar waits 5 beats.
    Rig two;
    two.load(0, makeConst(48000));
    two.rt.setClock(240.0, 3.0);
    two.par(0, sampler::SP_Quantize,
            (float)sampler::QuantizeMode::Q_2Bar);
    two.trig(0);
    two.b.render(two.rt, 512);
    REQUIRE(two.rt.activeVoices() == 1);
    REQUIRE(two.b.energy() == 0.0);
    for (int i = 0; i < 110; ++i) two.b.render(two.rt, 512);  // 56320f
    REQUIRE(two.b.energy() == 0.0);            // still before 5 beats
    e = 0.0;
    for (int i = 0; i < 12; ++i) {
        two.b.render(two.rt, 512);
        e += two.b.energy();
    }
    REQUIRE(e > 0.0);

    // 4bar: park the clock just before a 16-beat boundary so the
    // wait is only 0.5 beat (beatPos 15.5 → boundary at 16).
    Rig four;
    four.load(0, makeConst(48000));
    four.rt.setClock(240.0, 15.5);
    four.par(0, sampler::SP_Quantize,
             (float)sampler::QuantizeMode::Q_4Bar);
    four.trig(0);
    four.b.render(four.rt, 512);
    REQUIRE(four.rt.activeVoices() == 1);
    REQUIRE(four.b.energy() == 0.0);
    e = 0.0;
    for (int i = 0; i < 14; ++i) {
        four.b.render(four.rt, 512);           // ~7168f ≈ 0.6 beat
        e += four.b.energy();
    }
    REQUIRE(e > 0.0);                          // fired at bar 16
}

TEST_CASE("Smp34: quantized stop holds until the next boundary",
          "[smp34]") {
    Rig q, free;
    q.load(0, makeConst(96000),
           (uint8_t)sampler::PlaybackMode::Hold);
    free.load(0, makeConst(96000),
              (uint8_t)sampler::PlaybackMode::Hold);
    q.rt.setClock(120.0, 0.1);   // Q_1_4 → 0.9 beats ≈ 21600f to wait
    q.trig(0); free.trig(0);     // start unquantized, playing now
    // enable quantize for the STOP only
    q.par(0, sampler::SP_Quantize,
          (float)sampler::QuantizeMode::Q_1_4);
    for (int i = 0; i < 4; ++i) {
        q.b.render(q.rt, 512);
        free.b.render(free.rt, 512);
    }
    q.rel(0); free.rel(0);
    // free hold dies inside ~10 blocks (default release); the
    // quantized one is still playing well past that.
    for (int i = 0; i < 10; ++i) {
        q.b.render(q.rt, 512);
        free.b.render(free.rt, 512);
    }
    REQUIRE(free.rt.activeVoices() == 0);
    REQUIRE(q.rt.activeVoices() == 1);
    double e = 0.0;
    for (int i = 0; i < 8; ++i) {
        q.b.render(q.rt, 512);
        e += q.b.energy();
    }
    REQUIRE(e > 0.0);            // ~9728f in — still before 21600f
    // burn past the boundary → release completes
    for (int i = 0; i < 60; ++i) q.b.render(q.rt, 512);
    REQUIRE(q.rt.activeVoices() == 0);
}

// ============================ per-slot envelope ============================

TEST_CASE("Smp34: per-slot attack/release reshape the envelope",
          "[smp34]") {
    Rig fast, slow;
    fast.load(0, makeConst(48000));
    slow.load(0, makeConst(48000));
    fast.par(0, sampler::SP_Attack, 0.001f);    // ~instant envelope
    slow.par(0, sampler::SP_Attack, 400.0f);    // ≈ 19200 frames
    fast.trig(0); slow.trig(0);
    fast.b.render(fast.rt, 512);
    slow.b.render(slow.rt, 512);
    REQUIRE(slow.b.energy() < fast.b.energy() * 0.5);
    for (int i = 0; i < 8; ++i) {
        fast.b.render(fast.rt, 512);
        slow.b.render(slow.rt, 512);
    }
    REQUIRE(slow.b.energy() > 0.0);            // ramped up eventually

    // release: 200ms tail vs ~21ms default
    Rig tail, snap;
    tail.load(0, makeConst(48000));
    snap.load(0, makeConst(48000));
    tail.par(0, sampler::SP_Attack, 0.001f);    // env = 1 at stop
    tail.par(0, sampler::SP_Release, 200.0f);   // ≈ 9600 frames
    tail.trig(0); snap.trig(0);
    for (int i = 0; i < 4; ++i) {
        tail.b.render(tail.rt, 512);
        snap.b.render(snap.rt, 512);
    }
    tail.stop(0); snap.stop(0);
    for (int i = 0; i < 10; ++i) {
        tail.b.render(tail.rt, 512);
        snap.b.render(snap.rt, 512);
    }
    REQUIRE(snap.rt.activeVoices() == 0);       // ~5120f >> 21ms
    REQUIRE(tail.rt.activeVoices() == 1);       // 200ms tail still up
}

// ============================ trim + loop window ============================

TEST_CASE("Smp34: trim start/end bounds playback non-destructively",
          "[smp34]") {
    // ramp data: position is audible — trim start = half → ~0.45 amp
    Rig r;
    r.load(0, makeRamp(48000));
    r.par(0, sampler::SP_Attack, 0.001f);       // ~instant envelope
    r.par(0, sampler::SP_TrimStart, 24000.0f);
    r.par(0, sampler::SP_TrimEnd, 34000.0f);
    r.trig(0);
    r.b.render(r.rt, 512);
    double mx = 0.0;
    for (float v : r.b.l) mx = std::max(mx, (double)v);
    REQUIRE(mx == Catch::Approx(0.45).margin(0.05));
    // 10000-frame window → voice ends inside ~22 blocks
    for (int i = 0; i < 22; ++i) r.b.render(r.rt, 512);
    REQUIRE(r.rt.activeVoices() == 0);
}

TEST_CASE("Smp34: loop plays only its in/out region", "[smp34]") {
    Rig r;
    // audio exists ONLY inside [8000,16000)
    r.load(0, makeZoned(48000, 8000, 16000, 0.5f),
           (uint8_t)sampler::PlaybackMode::Loop);
    r.par(0, sampler::SP_Attack, 0.001f);
    r.par(0, sampler::SP_LoopIn, 8000.0f);
    r.par(0, sampler::SP_LoopOut, 16000.0f);
    r.trig(0);
    // first 8000 frames are silent (starts at pos 0 → region later)
    r.b.render(r.rt, 512);
    REQUIRE(r.b.energy() == 0.0);
    // render well past one full pass — the voice must loop inside
    // the region forever, so sustained energy ≈ 0.5·gain·env
    double tail = 0.0;
    for (int i = 0; i < 80; ++i) {              // 40k+ frames
        r.b.render(r.rt, 512);
        if (i > 40) tail += r.b.energy();
    }
    REQUIRE(r.rt.activeVoices() == 1);          // still looping
    REQUIRE(tail / 39.0 > 200.0);               // ~0.5·512·smoothing
    // a non-region loop would be silent most of the time
}

// ============================ tilt filter ============================

TEST_CASE("Smp34: per-slot filter — HP kills DC, LP darkens HF",
          "[smp34]") {
    Rig off, hp, lp;
    off.load(0, makeConst(48000));              // DC sample
    hp.load(0, makeConst(48000));
    lp.load(0, makeConst(48000));
    for (auto* r : { &off, &hp, &lp })
        r->par(0, sampler::SP_Attack, 0.001f);
    hp.par(0, sampler::SP_Filter, 1.0f);        // HP → DC rejected
    lp.par(0, sampler::SP_Filter, -1.0f);       // LP → DC passes
    off.trig(0); hp.trig(0); lp.trig(0);
    double eo = 0, eh = 0, el = 0;
    for (int i = 0; i < 20; ++i) {              // ~10k frames settle
        off.b.render(off.rt, 512); eo += off.b.energy();
        hp.b.render(hp.rt, 512);   eh += hp.b.energy();
        lp.b.render(lp.rt, 512);   el += lp.b.energy();
    }
    REQUIRE(eo > 1000.0);
    REQUIRE(eh < eo * 0.02);                    // HP removed the DC
    REQUIRE(el > eo * 0.8);                     // LP keeps the DC

    // HF sine (~6kHz, period 8) — dark LP attenuates hard
    Rig hf, hfLp;
    hf.load(0, makeSine(48000, 8.0));
    hfLp.load(0, makeSine(48000, 8.0));
    hf.par(0, sampler::SP_Attack, 0.001f);
    hfLp.par(0, sampler::SP_Attack, 0.001f);
    hfLp.par(0, sampler::SP_Filter, -1.0f);     // fc → 80Hz
    hf.trig(0); hfLp.trig(0);
    double e1 = 0, e2 = 0;
    for (int i = 0; i < 20; ++i) {
        hf.b.render(hf.rt, 512);   e1 += hf.b.energy();
        hfLp.b.render(hfLp.rt, 512); e2 += hfLp.b.energy();
    }
    REQUIRE(e2 < e1 * 0.05);                    // 6kHz >> 80Hz → gone
}

// ============================ engine APIs ============================

TEST_CASE("Smp34: missing samples flag, relink keeps metadata",
          "[smp34][engine]") {
    const fs::path dir = tempDir34("missing");
    const fs::path wav = dir / "a.wav";
    std::vector<int16_t> pcm(4800, 8000);
    writeTestWav(wav, 48000, 2, pcm);

    rt::CommandQueue q;
    sampler::SamplerEngine e;
    e.init([] { return std::make_unique<FFmpegDecoder>(); }, 48000.0);
    e.setPorts({ &q, nullptr });

    // point a slot at a real file, another at a ghost path
    e.slot(0).path = wav.string();
    e.slot(0).name = "real";
    e.slot(1).path = (dir / "gone.wav").string();
    e.slot(1).name = "ghost";
    e.slot(1).colorArgb = 0xFF00FF00;
    REQUIRE(e.checkMissing() == 1);
    REQUIRE(e.slot(0).missing.load() == 0);
    REQUIRE(e.slot(1).missing.load() == 1);
    REQUIRE(e.missingCount() == 1);

    // relink keeps the assignment + metadata, reloads the file
    const int64_t req = e.relinkSample(1, wav.string());
    REQUIRE(req > 0);
    REQUIRE(e.slot(1).missing.load() == 0);
    REQUIRE(e.slot(1).name == "ghost");              // name kept
    REQUIRE(e.slot(1).colorArgb == 0xFF00FF00);      // color kept
    for (int i = 0; i < 400 &&
         e.slot(1).state.load() != (int)sampler::SlotState::Ready; ++i) {
        e.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(e.slot(1).state.load() == (int)sampler::SlotState::Ready);

    // Remove stays an explicit user action (unloadSample)
    e.unloadSample(1);
    REQUIRE(e.slot(1).state.load() == (int)sampler::SlotState::Empty);
    REQUIRE(e.slot(1).missing.load() == 0);
    REQUIRE(e.missingCount() == 0);
}

TEST_CASE("Smp34: undo/redo restores slot metadata edits",
          "[smp34][engine]") {
    rt::CommandQueue q;
    sampler::SamplerEngine e;
    e.init([] { return std::make_unique<FFmpegDecoder>(); }, 48000.0);
    e.setPorts({ &q, nullptr });

    e.beginSlotEdit(0);
    e.setSlotParam(0, sampler::SP_TrimStart, 1200.0f);
    e.setSlotParam(0, sampler::SP_Pitch, -6.0f);
    e.setSlotParam(0, sampler::SP_Filter, -0.8f);
    e.commitSlotEdit(0);
    REQUIRE(e.slotParam(0, sampler::SP_TrimStart)
            == Catch::Approx(1200.0f));
    REQUIRE(e.slotParam(0, sampler::SP_Pitch)
            == Catch::Approx(-6.0f));

    REQUIRE(e.undoSlotEdit());
    REQUIRE(e.slotParam(0, sampler::SP_TrimStart)
            == Catch::Approx(-1.0f));            // default restored
    REQUIRE(e.slotParam(0, sampler::SP_Pitch)
            == Catch::Approx(0.0f));
    REQUIRE(e.slotParam(0, sampler::SP_Filter)
            == Catch::Approx(0.0f));
    REQUIRE(e.redoSlotEdit());
    REQUIRE(e.slotParam(0, sampler::SP_TrimStart)
            == Catch::Approx(1200.0f));
    REQUIRE(e.slotParam(0, sampler::SP_Pitch)
            == Catch::Approx(-6.0f));

    // no-op gesture → nothing pushed
    e.beginSlotEdit(1);
    e.commitSlotEdit(1);
    REQUIRE(e.undoSlotEdit());                   // pops the pad-0 edit
    REQUIRE(e.slotParam(0, sampler::SP_Pitch)
            == Catch::Approx(0.0f));
    REQUIRE_FALSE(e.undoSlotEdit());             // stack drained

    // undo also mirrored params back onto the rt queue
    rt::Command c;
    int slotParams = 0;
    while (q.tryPop(c))
        if (std::holds_alternative<rt::SampleSlotParam>(c))
            ++slotParams;
    REQUIRE(slotParams > 0);
}

// ============================ repository roundtrip ============================

TEST_CASE("Smp34: new slot fields persist through the repository",
          "[smp34][db]") {
    const fs::path dbp = tempDir34("repo") / "s.db";
    Database db;
    REQUIRE(db.open(dbp.string()));
    sampler::SampleRepository repo(db);
    REQUIRE(repo.init());

    const int64_t sid = repo.upsertSample("/tmp/hat.wav", "hat",
                                          0.25, 0.6f, 128.0, 48000, 2);
    const int64_t bid = repo.upsertBank(-1, "Perf", 0, 8);
    REQUIRE(sid > 0);
    REQUIRE(bid > 0);

    sampler::SampleSlot s;
    s.slot = 2; s.sampleId = sid; s.name = "hat";
    s.mode = sampler::PlaybackMode::Retrigger;
    s.quant = sampler::QuantizeMode::Q_2Bar;
    s.filterPos = -0.65f;
    s.attackMs = 12.5f; s.releaseMs = 250.0f;
    s.startF = 480.0; s.endF = 44100.0;
    s.loopIn = 9600.0; s.loopOut = 38400.0;
    REQUIRE(repo.saveSlot(bid, s));

    auto banks = repo.loadAll();
    REQUIRE(banks.size() == 1);
    const sampler::SampleSlot& l = banks[0].slot(2);
    REQUIRE(l.mode == sampler::PlaybackMode::Retrigger);
    REQUIRE(l.quant == sampler::QuantizeMode::Q_2Bar);
    REQUIRE(l.filterPos == Catch::Approx(-0.65f));
    REQUIRE(l.attackMs == Catch::Approx(12.5f));
    REQUIRE(l.releaseMs == Catch::Approx(250.0f));
    REQUIRE(l.startF == Catch::Approx(480.0));
    REQUIRE(l.endF == Catch::Approx(44100.0));
    REQUIRE(l.loopIn == Catch::Approx(9600.0));
    REQUIRE(l.loopOut == Catch::Approx(38400.0));

    // init() is idempotent on the migrated schema
    REQUIRE(repo.init());
    db.close();
}

// ============================ engine → RT integration ============================

TEST_CASE("Smp34: engine setSlotParam drives retrigger + window on RT",
          "[smp34][engine]") {
    rt::CommandQueue q;
    sampler::SamplerEngine e;
    e.init([] { return std::make_unique<FFmpegDecoder>(); }, 48000.0);
    e.setPorts({ &q, nullptr });
    // trigger() gates on Empty — mark the slot playable
    e.slot(0).state.store((int)sampler::SlotState::Ready);

    e.setSlotParam(0, sampler::SP_Mode,
                   (float)sampler::PlaybackMode::Retrigger);
    e.setSlotParam(0, sampler::SP_Attack, 30.0f);
    e.setSlotParam(0, sampler::SP_LoopIn, 1000.0f);
    e.trigger(0, 0.9f);
    e.trigger(0, 0.9f);
    e.release(0);
    e.stopAll();

    // drain the queue exactly like AudioEngine does
    sampler::SamplerRt rt(48000.0);
    rt::Command c;
    int triggers = 0, params = 0;
    bool sawRetrig = false, sawStopAll = false;
    while (q.tryPop(c)) {
        if (auto* t = std::get_if<rt::SampleTrigger>(&c)) {
            ++triggers;
            REQUIRE(t->vel == Catch::Approx(0.9f));
        } else if (auto* p = std::get_if<rt::SampleSlotParam>(&c)) {
            ++params;
            rt.applyCommand(*p);
            if (p->kind == sampler::SP_Mode)
                sawRetrig = p->v ==
                    (float)sampler::PlaybackMode::Retrigger;
        } else if (auto* s = std::get_if<rt::SampleStop>(&c)) {
            sawStopAll = s->slot == 255;
        }
    }
    REQUIRE(triggers == 2);
    REQUIRE(params >= 3);
    REQUIRE(sawRetrig);
    REQUIRE(sawStopAll);
}
