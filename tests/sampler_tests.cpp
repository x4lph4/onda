// Phase 11 — Professional Sampler + Performance Pads tests.
// RT engine (SamplerRt) is exercised directly: publish → collect →
// command → render, exactly as AudioEngine drives it. Loader tests use
// the real FFmpegDecoder + generated WAV files (test_wav.h).
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/rt/CommandQueue.h"
#include "core/data/Database.h"
#include "core/sampler/SampleBank.h"
#include "core/sampler/SampleCache.h"
#include "core/sampler/SampleGain.h"
#include "core/sampler/SampleLoader.h"
#include "core/sampler/SamplePitchProcessor.h"
#include "core/sampler/SampleRecorder.h"
#include "core/sampler/SampleRepository.h"
#include "core/sampler/SampleSync.h"
#include "core/sampler/SamplerEngine.h"
#include "core/sampler/SamplerRt.h"
#include "core/sampler/SamplerTypes.h"
#include "platform/decode/FFmpegDecoder.h"
#include "ui/CommandIds.h"
#include "ui/CommandManager.h"
#include "ui/pads/PadLayer.h"
#include "ui/pads/PadModeManager.h"
#include "ui/pads/PadTypes.h"

#include "test_wav.h"

using namespace onda;
namespace fs = std::filesystem;

namespace {

fs::path tempDirS(const char* n) {
    fs::path d = fs::temp_directory_path() / "onda_smptest" / n;
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

/// Planar stereo sample — constant amplitude (sine-optional).
std::shared_ptr<const sampler::SampleData>
makeSample(int64_t frames, float amp = 0.5f, double bpm = 0.0,
           bool sine = false) {
    auto d = std::make_shared<sampler::SampleData>();
    auto* l = new float[(size_t)frames];
    auto* r = new float[(size_t)frames];
    for (int64_t i = 0; i < frames; ++i) {
        const float v = sine ? amp * std::sin(2.0 * 3.14159265 * i / 64.0)
                             : amp;
        l[i] = v; r[i] = v;
    }
    d->l = std::shared_ptr<const float[]>(l);
    d->r = std::shared_ptr<const float[]>(r);
    d->frames = frames;
    d->sampleRate = 48000.0;
    d->channels = 2;
    d->peak = amp;
    d->bpm = bpm;
    return d;
}

/// Render helper — zeroes buffers first (SamplerRt sums into them).
struct Buf {
    std::vector<float> l, r;
    Buf(int n) : l((size_t)n, 0.0f), r((size_t)n, 0.0f) {}
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
    bool finite() const {
        for (float v : l) if (!std::isfinite(v)) return false;
        for (float v : r) if (!std::isfinite(v)) return false;
        return true;
    }
};

/// RT rig: publish a slot, run one render block to collect the mailbox.
struct RtRig {
    sampler::SamplerRt rt { 48000.0 };
    Buf b { 512 };
    void load(int slot, std::shared_ptr<const sampler::SampleData> d,
              uint8_t mode = (uint8_t)sampler::PlaybackMode::OneShot) {
        rt.applyCommand(rt::SampleSlotParam{(uint8_t)slot,
                                            (uint8_t)sampler::SP_Mode,
                                            (float)mode});
        rt.publishSlot(slot, std::move(d));
        b.render(rt, 64);      // collectInbox → Ready
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

// ============================ types & math ============================

TEST_CASE("Sampler: enum surfaces — quantize, beatjump, mode names",
          "[sampler][types]") {
    REQUIRE(sampler::quantizeBeats(sampler::QuantizeMode::Off) == 0.0);
    REQUIRE(sampler::quantizeBeats(sampler::QuantizeMode::Q_1_4) == 1.0);
    REQUIRE(sampler::quantizeBeats(sampler::QuantizeMode::Q_1_16) == 0.25);
    REQUIRE(sampler::quantizeBeats(sampler::QuantizeMode::Bar) == 4.0);

    REQUIRE(ui::pads::beatJumpBeats(0) == -1.0f);
    REQUIRE(ui::pads::beatJumpBeats(3) == -8.0f);
    REQUIRE(ui::pads::beatJumpBeats(4) == 1.0f);
    REQUIRE(ui::pads::beatJumpBeats(7) == 8.0f);

    REQUIRE(std::string(ui::pads::padModeName(
                ui::pads::PadMode::Sampler)) == "SAMPLER");
    REQUIRE(ui::pads::padModeImplemented(ui::pads::PadMode::Loop));
    REQUIRE(!ui::pads::padModeImplemented(ui::pads::PadMode::Stems));
}

TEST_CASE("Sampler: SampleSync math", "[sampler][sync]") {
    using SS = sampler::SampleSync;
    REQUIRE(SS::speedFor(120.0, 60.0) == Catch::Approx(0.5));
    REQUIRE(SS::speedFor(60.0, 120.0) == Catch::Approx(2.0));
    REQUIRE(SS::speedFor(0.0, 120.0) == Catch::Approx(1.0));  // no bpm
    // @120bpm, 1 beat = 24000 frames; pos 0.75 → 0.25 beat = 6000 fr
    REQUIRE(SS::framesToBoundary(0.75, 1.0, 120.0, 48000.0) == 6000);
    // already on boundary → full quantum wait
    REQUIRE(SS::framesToBoundary(1.0, 1.0, 120.0, 48000.0) == 24000);
    REQUIRE(SS::framesToBoundary(0.5, 0.0, 120.0, 48000.0) == 0);
    REQUIRE(SS::framesToBoundary(0.5, 1.0, 0.0, 48000.0) == 0); // no clock
}

TEST_CASE("Sampler: gain + pitch helpers", "[sampler][dsp]") {
    REQUIRE(sampler::SampleGain::normalizeGain(0.5f) ==
            Catch::Approx(0.99f / 0.5f));
    REQUIRE(sampler::SampleGain::normalizeGain(0.001f) ==
            Catch::Approx(2.0f));                       // clamp
    REQUIRE(sampler::SampleGain::normalizeGain(0.0f) == 1.0f);
    REQUIRE(sampler::SampleGain::peakDb(1.0f) == Catch::Approx(0.0));
    REQUIRE(sampler::SampleGain::peakDb(0.5f) ==
            Catch::Approx(-6.02).margin(0.01));
    REQUIRE(sampler::SampleGain::peakDb(0.0f) == -120.0f);

    sampler::SamplePitchProcessor p;
    REQUIRE(p.stepFor(0.0f) == Catch::Approx(1.0));
    REQUIRE(p.stepFor(12.0f) == Catch::Approx(2.0));
    REQUIRE(p.stepFor(-12.0f) == Catch::Approx(0.5));
    REQUIRE(p.stepFor(7.0f) == Catch::Approx(1.4983).margin(1e-3));
}

// ============================ containers ============================

TEST_CASE("Sampler: bank sizes 8/16/32", "[sampler][bank]") {
    sampler::SampleBank b8(8);
    REQUIRE(b8.slotCount() == 8);
    REQUIRE(b8.slot(7).slot == 7);
    sampler::SampleBank b32(32);
    REQUIRE(b32.slotCount() == 32);
    REQUIRE(b32.slot(31).slot == 31);
    b32.resize(16);
    REQUIRE(b32.slotCount() == 16);
}

TEST_CASE("Sampler: cache LRU + pin + byte cap", "[sampler][cache]") {
    const int64_t bytesPer = 1024 * 2 * 4;   // 1024 frames stereo float
    sampler::SampleCache c(bytesPer * 2);
    c.put(1, makeSample(1024));
    c.put(2, makeSample(1024));
    c.put(3, makeSample(1024));              // over cap → evict LRU(1)
    REQUIRE(c.entries() == 2);
    REQUIRE(c.get(1) == nullptr);
    REQUIRE(c.get(2) != nullptr);
    REQUIRE(c.get(3) != nullptr);

    c.pin(2, true);
    c.put(4, makeSample(1024));              // evicts 3, keeps pinned 2
    REQUIRE(c.get(2) != nullptr);
    REQUIRE(c.get(3) == nullptr);
    REQUIRE(c.get(4) != nullptr);
    REQUIRE(c.bytesUsed() == bytesPer * 2);
    REQUIRE(c.remove(4));
    REQUIRE(c.bytesUsed() == bytesPer);
}

// ============================ RT playback ============================

TEST_CASE("SamplerRT: one-shot plays to end, state cycles", "[sampler]") {
    RtRig r;
    r.load(0, makeSample(4800));
    REQUIRE(r.rt.slotState(0) == (int)sampler::SlotState::Ready);
    r.trig(0);
    r.b.render(r.rt, 512);
    REQUIRE(r.rt.activeVoices() == 1);
    REQUIRE(r.b.energy() > 0.0);
    REQUIRE(r.rt.slotState(0) == (int)sampler::SlotState::Playing);
    // play through the whole sample
    for (int i = 0; i < 12; ++i) r.b.render(r.rt, 512);
    REQUIRE(r.rt.activeVoices() == 0);
    REQUIRE(r.rt.slotState(0) == (int)sampler::SlotState::Ready);
    REQUIRE(r.b.finite());
}

TEST_CASE("SamplerRT: modes — loop, hold, gate", "[sampler]") {
    RtRig r;
    const int64_t N = 1024;

    SECTION("loop keeps playing past the end") {
        r.load(0, makeSample(N), (uint8_t)sampler::PlaybackMode::Loop);
        r.trig(0);
        for (int i = 0; i < 6; ++i) r.b.render(r.rt, 512);   // 3072 > N
        REQUIRE(r.rt.activeVoices() == 1);
        REQUIRE(r.b.energy() > 0.0);
        REQUIRE(r.rt.slotState(0) == (int)sampler::SlotState::Looping);
        r.stop(0);
        for (int i = 0; i < 20; ++i) r.b.render(r.rt, 512);  // fade out
        REQUIRE(r.rt.activeVoices() == 0);
    }

    SECTION("hold stops on pad release") {
        r.load(0, makeSample(N * 100), (uint8_t)sampler::PlaybackMode::Hold);
        r.trig(0);
        r.b.render(r.rt, 512);
        REQUIRE(r.rt.activeVoices() == 1);
        r.rel(0);
        for (int i = 0; i < 10; ++i) r.b.render(r.rt, 512);
        REQUIRE(r.rt.activeVoices() == 0);
    }

    SECTION("gate toggles on second press") {
        r.load(0, makeSample(N * 100), (uint8_t)sampler::PlaybackMode::Gate);
        r.trig(0);
        r.b.render(r.rt, 512);
        REQUIRE(r.rt.activeVoices() == 1);
        r.trig(0);                                 // press again → off
        for (int i = 0; i < 10; ++i) r.b.render(r.rt, 512);
        REQUIRE(r.rt.activeVoices() == 0);
    }
}

TEST_CASE("SamplerRT: retrigger replaces same-slot voice (no pileup)",
          "[sampler]") {
    RtRig r;
    r.load(0, makeSample(48000));
    for (int i = 0; i < 8; ++i) {                  // rapid-fire pad
        r.trig(0);
        r.b.render(r.rt, 64);
    }
    REQUIRE(r.rt.slotVoices(0) <= 1);
    REQUIRE(r.rt.activeVoices() <= 1);
    REQUIRE(r.b.finite());
}

TEST_CASE("SamplerRT: voice pool cap + steal policies", "[sampler]") {
    RtRig r;
    for (int s = 0; s < sampler::SamplerRt::kMaxSlots; ++s)
        r.load(s, makeSample(48000));

    SECTION("pool caps at kMaxVoices") {
        for (int s = 0; s < sampler::SamplerRt::kMaxSlots; ++s) r.trig(s);
        r.b.render(r.rt, 512);
        REQUIRE(r.rt.activeVoices() ==
                sampler::SamplerRt::kMaxVoices);
    }
    SECTION("Deny refuses beyond maxVoices") {
        r.rt.stealPolicy = sampler::StealPolicy::Deny;
        r.rt.applyCommand(rt::SamplerParam{1, 4.0f});   // maxVoices=4
        for (int s = 0; s < 8; ++s) r.trig(s);
        r.b.render(r.rt, 512);
        REQUIRE(r.rt.activeVoices() == 4);
    }
    SECTION("Oldest steals — count stays at cap") {
        r.rt.applyCommand(rt::SamplerParam{1, 4.0f});
        for (int s = 0; s < 8; ++s) { r.trig(s); r.b.render(r.rt, 32); }
        REQUIRE(r.rt.activeVoices() <= 4);
        REQUIRE(r.rt.activeVoices() > 0);
    }
    SECTION("stop-all releases every voice") {
        for (int s = 0; s < 16; ++s) r.trig(s);
        r.b.render(r.rt, 512);
        r.stop(255);
        for (int i = 0; i < 10; ++i) r.b.render(r.rt, 512);
        REQUIRE(r.rt.activeVoices() == 0);
    }
}

TEST_CASE("SamplerRT: mute, solo, volume, pan, velocity", "[sampler]") {
    RtRig r;
    r.load(0, makeSample(48000));
    r.load(1, makeSample(48000));

    SECTION("muted slot never triggers") {
        r.par(0, sampler::SP_Mute, 1.0f);
        r.trig(0);
        r.b.render(r.rt, 512);
        REQUIRE(r.rt.activeVoices() == 0);
        REQUIRE(r.b.energy() == 0.0);
    }
    SECTION("mute stops a playing voice") {
        r.trig(0);
        r.b.render(r.rt, 512);
        r.par(0, sampler::SP_Mute, 1.0f);
        for (int i = 0; i < 10; ++i) r.b.render(r.rt, 512);
        REQUIRE(r.rt.activeVoices() == 0);
    }
    SECTION("solo silences other slots") {
        r.trig(0); r.trig(1);
        r.b.render(r.rt, 512);
        REQUIRE(r.rt.activeVoices() == 2);
        r.par(0, sampler::SP_Solo, 1.0f);
        for (int i = 0; i < 10; ++i) r.b.render(r.rt, 512);
        REQUIRE(r.rt.slotVoices(1) == 0);
        REQUIRE(r.rt.slotVoices(0) == 1);
    }
    SECTION("zero volume renders silence") {
        r.par(0, sampler::SP_Volume, 0.0f);
        r.trig(0);
        for (int i = 0; i < 4; ++i) r.b.render(r.rt, 512);
        REQUIRE(r.b.energy() < 1e-3);
    }
    SECTION("hard pan puts energy on one side") {
        r.par(0, sampler::SP_Pan, -1.0f);
        r.trig(0);
        for (int i = 0; i < 4; ++i) r.b.render(r.rt, 512);
        double el = 0, er = 0;
        for (float v : r.b.l) el += std::fabs(v);
        for (float v : r.b.r) er += std::fabs(v);
        REQUIRE(el > er * 10.0);
    }
    SECTION("velocity scales loudness") {
        RtRig r2;
        r2.load(0, makeSample(48000, 0.5f));
        r.trig(0, 1.0f);
        r2.trig(0, 0.5f);
        double e1 = 0, e2 = 0;
        for (int i = 0; i < 8; ++i) {
            r.b.render(r.rt, 512);   e1 += r.b.energy();
            r2.b.render(r2.rt, 512); e2 += r2.b.energy();
        }
        REQUIRE(e2 < e1 * 0.7);
    }
}

TEST_CASE("SamplerRT: quantize delays trigger to grid", "[sampler]") {
    RtRig r;
    r.load(0, makeSample(96000));
    // 120bpm → 24000 fr/beat. beatPos 0.5, quant 1/4 → 0.5 beat = 12000
    r.rt.setClock(120.0, 0.5);
    r.par(0, sampler::SP_Quantize, (float)sampler::QuantizeMode::Q_1_4);
    r.trig(0);
    r.b.render(r.rt, 512);
    REQUIRE(r.rt.activeVoices() == 1);           // scheduled, held off
    REQUIRE(r.b.energy() == 0.0);                // nothing audible yet
    // burn ~11000 frames — still before the boundary
    for (int i = 0; i < 20; ++i) r.b.render(r.rt, 512);
    REQUIRE(r.b.energy() == 0.0);
    // pass the boundary → voice starts
    double e = 0.0;
    for (int i = 0; i < 8; ++i) { r.b.render(r.rt, 512); e += r.b.energy(); }
    REQUIRE(e > 0.0);
}

TEST_CASE("SamplerRT: sync scales playback speed to master clock",
          "[sampler]") {
    RtRig synced, plain;
    auto sd = makeSample(48000, 0.5f, 60.0);     // 60bpm sample
    plain.load(0, sd);
    synced.load(0, sd);
    synced.rt.setClock(120.0, 0.0);              // master 120bpm → 2x
    synced.par(0, sampler::SP_SyncOn, 1.0f);
    plain.trig(0); synced.trig(0);
    // synced voice runs at 2x → finishes at ~24000 frames (46 blocks
    // of 512); unsynced needs the full 48000
    for (int i = 0; i < 50; ++i) {
        plain.b.render(plain.rt, 512);
        synced.b.render(synced.rt, 512);
    }
    REQUIRE(synced.rt.activeVoices() == 0);      // finished early
    REQUIRE(plain.rt.activeVoices() == 1);
}

TEST_CASE("SamplerRT: garbage input stays safe", "[sampler][fuzz]") {
    RtRig r;
    r.load(0, makeSample(4800));
    for (int i = 0; i < 500; ++i) {
        const uint8_t s = (uint8_t)(i * 37);
        r.rt.applyCommand(rt::SampleTrigger{s, (float)(i % 5) - 2.f});
        r.rt.applyCommand(rt::SampleRelease{s});
        r.rt.applyCommand(rt::SampleSlotParam{s, (uint8_t)(i % 11),
                                              (float)(i % 7) - 3.f});
        r.rt.applyCommand(rt::SamplerParam{(uint8_t)(i % 4),
                                           (float)(i % 3) - 1.f});
        if (i % 3 == 0) r.b.render(r.rt, 64);
    }
    for (int i = 0; i < 8; ++i) r.b.render(r.rt, 512);
    REQUIRE(r.b.finite());
    REQUIRE(r.rt.activeVoices() <= sampler::SamplerRt::kMaxVoices);
}

// ============================ loader (real decoder) ============================

TEST_CASE("SampleLoader: async decode of a real WAV", "[sampler][loader]") {
    const fs::path dir = tempDirS("loader");
    const fs::path wav = dir / "kick.wav";
    std::vector<int16_t> pcm(96000);               // 48000 stereo frames
    for (size_t i = 0; i < pcm.size(); i += 2) {   // stereo, decaying
        const float a = 0.8f * std::exp(-(float)i / 8000.0f);
        pcm[i] = pcm[i + 1] = (int16_t)(a * 30000);
    }
    writeTestWav(wav, 48000, 2, pcm);

    sampler::SampleLoader loader(
        [] { return std::make_unique<FFmpegDecoder>(); }, 48000.0);
    std::vector<sampler::SampleLoader::Result> out;
    std::mutex mu; std::condition_variable cv;
    loader.start([&](sampler::SampleLoader::Result&& r) {
        std::lock_guard lk(mu); out.push_back(std::move(r)); cv.notify_all();
    });
    loader.load(0, wav.string(), 124.0);

    std::unique_lock lk(mu);
    REQUIRE(cv.wait_for(lk, std::chrono::seconds(10),
                        [&] { return !out.empty(); }));
    lk.unlock();
    loader.stop();

    REQUIRE(out[0].data != nullptr);
    REQUIRE(out[0].data->frames > 40000);
    REQUIRE(out[0].data->channels == 2);
    REQUIRE(out[0].data->peak == Catch::Approx(0.8f).margin(0.1f));
    REQUIRE(out[0].data->bpm == Catch::Approx(124.0));  // hint carried
    REQUIRE(out[0].data->waveCount > 0);
}

TEST_CASE("SampleLoader: bad path reports error, never throws",
          "[sampler][loader]") {
    sampler::SampleLoader loader(
        [] { return std::make_unique<FFmpegDecoder>(); }, 48000.0);
    std::vector<sampler::SampleLoader::Result> out;
    std::mutex mu; std::condition_variable cv;
    loader.start([&](sampler::SampleLoader::Result&& r) {
        std::lock_guard lk(mu); out.push_back(std::move(r)); cv.notify_all();
    });
    loader.load(2, "no/such/file.wav");
    std::unique_lock lk(mu);
    REQUIRE(cv.wait_for(lk, std::chrono::seconds(10),
                        [&] { return !out.empty(); }));
    lk.unlock();
    loader.stop();
    REQUIRE(out[0].data == nullptr);
    REQUIRE(!out[0].error.empty());
}

// ============================ repository ============================

TEST_CASE("SampleRepository: banks + slots persist and roundtrip",
          "[sampler][db]") {
    const fs::path dbp = tempDirS("repo") / "s.db";
    Database db;
    REQUIRE(db.open(dbp.string()));
    sampler::SampleRepository repo(db);
    REQUIRE(repo.init());

    const int64_t sid = repo.upsertSample("/tmp/kick.wav", "kick",
                                          0.5, 0.9f, 124.0, 48000, 2);
    REQUIRE(sid > 0);
    // upsert same path → same id (UNIQUE path)
    REQUIRE(repo.upsertSample("/tmp/kick.wav", "kick2",
                              0.5, 0.9f, 124.0, 48000, 2) == sid);

    const int64_t bid = repo.upsertBank(-1, "Main", 0, 8);
    REQUIRE(bid > 0);

    sampler::SampleSlot s;
    s.slot = 3; s.sampleId = sid; s.name = "kick";
    s.colorArgb = 0xFF112233; s.volume = 0.7f; s.pan = -0.5f;
    s.gain = 1.4f; s.pitch = 3.0f;
    s.mode = sampler::PlaybackMode::Loop;
    s.quant = sampler::QuantizeMode::Q_1_16;
    s.syncOn = true; s.loopOn = true;
    REQUIRE(repo.saveSlot(bid, s));

    auto banks = repo.loadAll();
    REQUIRE(banks.size() == 1);
    REQUIRE(banks[0].name == "Main");
    const sampler::SampleSlot& l = banks[0].slot(3);
    REQUIRE(l.sampleId == sid);
    REQUIRE(l.name == "kick");
    REQUIRE(l.colorArgb == 0xFF112233);
    REQUIRE(l.volume == Catch::Approx(0.7f));
    REQUIRE(l.pan == Catch::Approx(-0.5f));
    REQUIRE(l.gain == Catch::Approx(1.4f));
    REQUIRE(l.pitch == Catch::Approx(3.0f));
    REQUIRE(l.mode == sampler::PlaybackMode::Loop);
    REQUIRE(l.quant == sampler::QuantizeMode::Q_1_16);
    REQUIRE(l.syncOn);
    REQUIRE(l.loopOn);
    REQUIRE(l.path == "/tmp/kick.wav");
    REQUIRE(l.state.load() == (int)sampler::SlotState::Queued);

    REQUIRE(repo.deleteBank(bid));
    REQUIRE(repo.loadAll().empty());
    db.close();
}

// ============================ engine façade ============================

TEST_CASE("SamplerEngine: load → trigger → rt queue → persist",
          "[sampler][engine]") {
    const fs::path dir = tempDirS("eng");
    const fs::path wav = dir / "snare.wav";
    std::vector<int16_t> pcm(9600);
    for (size_t i = 0; i < pcm.size(); ++i)
        pcm[i] = (int16_t)((i % 64 < 32 ? 1 : -1) * 12000);
    writeTestWav(wav, 48000, 2, pcm);

    Database db;
    REQUIRE(db.open((dir / "s.db").string()));
    sampler::SampleRepository repo(db);
    REQUIRE(repo.init());

    rt::CommandQueue q;
    sampler::SamplerEngine e;
    e.init([] { return std::make_unique<FFmpegDecoder>(); }, 48000.0);
    e.setPorts({ &q, nullptr });
    e.attachRepository(&repo);

    e.loadSample(0, wav.string(), 120.0);
    REQUIRE(e.slot(0).state.load() == (int)sampler::SlotState::Loading);
    for (int i = 0; i < 400 &&
         e.slot(0).state.load() != (int)sampler::SlotState::Ready; ++i) {
        e.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(e.slot(0).state.load() == (int)sampler::SlotState::Ready);
    REQUIRE(e.slot(0).sampleId > 0);

    // performance actions → rt queue
    e.trigger(0, 0.8f);
    rt::Command c;
    REQUIRE(q.tryPop(c));
    REQUIRE(std::holds_alternative<rt::SampleTrigger>(c));
    REQUIRE(std::get<rt::SampleTrigger>(c).vel == Catch::Approx(0.8f));
    e.setSlotParam(0, sampler::SP_Volume, 0.5f);
    REQUIRE(q.tryPop(c));
    REQUIRE(std::holds_alternative<rt::SampleSlotParam>(c));
    REQUIRE(e.slotParam(0, sampler::SP_Volume) == Catch::Approx(0.5f));
    e.stopAll();
    REQUIRE(q.tryPop(c));
    REQUIRE(std::get<rt::SampleStop>(c).slot == 255);

    // persistence → fresh engine restores bank + slot state
    REQUIRE(e.saveAll());
    sampler::SamplerEngine e2;
    e2.init([] { return std::make_unique<FFmpegDecoder>(); }, 48000.0);
    rt::CommandQueue q2;
    e2.setPorts({ &q2, nullptr });
    e2.attachRepository(&repo);
    REQUIRE(e2.loadAll());
    REQUIRE(e2.bankCount() == 1);
    // reload runs through the loader → Ready again
    for (int i = 0; i < 400 &&
         e2.slot(0).state.load() != (int)sampler::SlotState::Ready; ++i) {
        e2.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(e2.slot(0).state.load() == (int)sampler::SlotState::Ready);
    REQUIRE(e2.slotParam(0, sampler::SP_Volume) == Catch::Approx(0.5f));
    REQUIRE(e2.slot(0).path == wav.string());
    db.close();
}

// ============================ pad domain ============================

TEST_CASE("PadModeManager: every mode resolves to the right action",
          "[sampler][pads]") {
    using namespace ui::pads;
    PadModeManager m;

    m.setMode(0, PadMode::HotCue);
    auto a = m.resolve(0, 3, false, true);
    REQUIRE(a.kind == PadAction::Command);
    REQUIRE(a.cmdId == ui::cmd::forHotCue(0, 3, 0));
    a = m.resolve(0, 3, true, true);
    REQUIRE(a.cmdId == ui::cmd::forHotCue(0, 3, 2));       // shift=delete
    REQUIRE(m.resolve(0, 3, false, false).kind == PadAction::None);

    m.setMode(0, PadMode::Sampler);
    a = m.resolve(0, 5, false, true, 0.6f);
    REQUIRE(a.kind == PadAction::SampleTrig);
    REQUIRE(a.slot == 5);
    REQUIRE(a.arg == Catch::Approx(0.6f));
    REQUIRE(m.resolve(0, 5, false, false).kind ==
            PadAction::SampleRelease);
    REQUIRE(m.resolve(0, 5, true, true).kind ==
            PadAction::SampleStop);

    m.setMode(1, PadMode::BeatJump);
    REQUIRE(m.resolve(1, 0, false, true).cmdId ==
            ui::cmd::forDeck(ui::cmd::DeckJumpBack, 1));
    REQUIRE(m.resolve(1, 7, false, true).cmdId ==
            ui::cmd::forDeck(ui::cmd::DeckJumpFwd, 1));

    m.setMode(1, PadMode::Loop);
    REQUIRE(m.resolve(1, 0, false, true).cmdId ==
            ui::cmd::forDeck(ui::cmd::DeckLoopHalve, 1));
    REQUIRE(m.resolve(1, 1, false, true).cmdId ==
            ui::cmd::forDeck(ui::cmd::DeckLoopDouble, 1));
    // Phase 20: pads 5-8 fire sized auto-loops (arg = 10+size index)
    auto lp = m.resolve(1, 6, false, true);
    REQUIRE(lp.cmdId ==
            ui::cmd::forDeck(ui::cmd::DeckLoopSizeTrig, 1));
    REQUIRE(lp.arg == Catch::Approx(16.0f)); // kSz[2]=6 → 4-beat loop

    m.setMode(0, PadMode::PadFx);                        // reserved
    REQUIRE(m.resolve(0, 0, false, true).kind == PadAction::None);
    m.setMode(0, PadMode::Stems);
    REQUIRE(m.resolve(0, 0, false, true).kind == PadAction::None);
}

TEST_CASE("PadLayer: pad.press commands dispatch through CommandManager",
          "[sampler][pads]") {
    using namespace ui::pads;
    ui::CommandManager cm;
    PadLayer layer;
    std::vector<std::pair<int, float>> trigs;
    std::vector<int> releases, stops;
    layer.setPorts({
        [&](int s, float v) { trigs.emplace_back(s, v); },
        [&](int s)          { releases.push_back(s); },
        [&](int s)          { stops.push_back(s); },
    });
    layer.registerCommands(cm);

    // spy on the hot-cue target the domain resolves to
    int cueHits = 0;
    for (int d = 0; d < 2; ++d)
        for (int s = 0; s < 8; ++s)
            for (int a = 0; a < 3; ++a) {
                ui::Command c;
                c.id = ui::cmd::forHotCue(d, s, a);
                c.name = "x"; c.run = [&](float, ui::CommandSource) {
                    ++cueHits;
                };
                cm.add(std::move(c));
            }

    // default mode HotCue → pad press hits the hot-cue command
    REQUIRE(cm.invoke(ui::cmd::forPad(0, 2, false),
                      ui::CommandSource::Ui, 1.0f));
    REQUIRE(cueHits == 1);

    // switch deck-A pads to SAMPLER → trig + release to the ports
    REQUIRE(cm.invoke(ui::cmd::PadModeSetBase + 0,
                      ui::CommandSource::Ui,
                      (float)(int)PadMode::Sampler));
    cm.invoke(ui::cmd::forPad(0, 4, false), ui::CommandSource::Ui, 0.9f);
    REQUIRE(trigs.size() == 1);
    REQUIRE(trigs[0].first == 4);
    cm.invoke(ui::cmd::forPad(0, 4, false), ui::CommandSource::Ui, 0.0f);
    REQUIRE(releases.size() == 1);
    cm.invoke(ui::cmd::forPad(0, 4, true), ui::CommandSource::Ui, 1.0f);
    REQUIRE(stops.size() == 1);
    // deck B untouched — still hot-cue mode
    cm.invoke(ui::cmd::forPad(1, 0, false), ui::CommandSource::Ui, 1.0f);
    REQUIRE(cueHits == 2);
}

// ============================ recorder ============================

TEST_CASE("SampleRecorder: ring → WAV file, no RT blocking", "[sampler][rec]") {
    const fs::path wav = tempDirS("rec") / "cap.wav";
    sampler::SampleRecorder rec;
    REQUIRE(rec.open(wav.string(), 48000.0));
    rec.setSource(sampler::SampleRecorder::Source::Master);

    std::vector<float> buf(4800 * 2, 0.25f);
    rec.push(buf.data(), 4800);                      // unarmed → ignored
    REQUIRE(rec.framesWritten() == 0);
    rec.arm(true);
    rec.push(buf.data(), 4800);
    REQUIRE(rec.drainToFile() == 4800);
    rec.close();

    REQUIRE(rec.framesWritten() == 4800);
    std::error_code ec;
    REQUIRE(fs::file_size(wav, ec) >= 4800 * 8);     // data + header
    // RIFF header sanity
    std::ifstream f(wav, std::ios::binary);
    char tag[4] = {};
    f.read(tag, 4);
    REQUIRE(std::string(tag, 4) == "RIFF");
}

// ============================ stress ============================

TEST_CASE("SamplerRT: stress — 8 pads rapid-fire → 32 voices, bounded",
          "[sampler][stress]") {
    RtRig r;
    for (int s = 0; s < 8; ++s)
        r.load(s, makeSample(48000, 0.3f, 0.0, true));

    const auto t0 = std::chrono::steady_clock::now();
    int maxVoices = 0;
    // 200 rounds of rapid-fire across all pads
    for (int round = 0; round < 200; ++round) {
        for (int s = 0; s < 8; ++s) r.trig(s, 1.0f);
        r.b.render(r.rt, 128);
        maxVoices = std::max(maxVoices, r.rt.activeVoices());
        if ((round & 15) == 0) r.stop(255);
    }
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    INFO("200 rounds x8 pads render: " << ms << " ms");
    REQUIRE(maxVoices <= sampler::SamplerRt::kMaxVoices);
    REQUIRE(r.b.finite());
    // sanity bound: 25.6k render calls' worth of work must stay cheap
    REQUIRE(ms < 2000.0);
}
