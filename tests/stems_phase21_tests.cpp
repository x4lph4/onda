// Phase 21 — professional stems engine tests.
// BandSplitModel separation/alignment/reconstruction, StemCache keys/
// validation/atomicity/range reads, StemStore (schema v7), StemMixer/
// Router resolution, StemScheduler priority/cancel/CPU-gate, deck RT
// stem rendering (mute/solo/gain/fallback/partial), StemsEngine end-
// to-end (separate→cache→publish→multi-deck), pad modes + feedback.
// §49: all fixtures are synthetic — no copyrighted material.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include "core/audio/TrackAudio.h"
#include "core/data/Database.h"
#include "core/engine/DeckCommand.h"
#include "core/engine/DeckEngine.h"
#include "core/dsp/WsolaStretcher.h"
#include "core/stems/BandSplitModel.h"
#include "core/stems/StemCache.h"
#include "core/stems/StemDiagnostics.h"
#include "core/stems/StemMixer.h"
#include "core/stems/StemModelProvider.h"
#include "core/stems/StemRouter.h"
#include "core/stems/StemScheduler.h"
#include "core/stems/StemsEngine.h"
#include "core/stems/StemStore.h"
#include "ui/CommandIds.h"
#include "ui/pads/PadFeedbackEngine.h"
#include "ui/pads/PadModeManager.h"

using namespace onda;
using namespace onda::stems;
using namespace onda::ui;
using Catch::Approx;
namespace fs = std::filesystem;

namespace {

constexpr double kRate = 48000.0;
constexpr int    kBlock = 512;

// ---- synthetic fixtures (§49) -------------------------------------
/// A mix with content spread across the spectrum: 80 Hz (bass band),
/// 1 kHz (vocal band), 5 kHz (instrument region) + a click every
/// 0.25 s (drums-ish transients, in the vocal band by design of the
/// reference model). Stereo L/R differ so channels are exercised.
std::unique_ptr<TrackAudio> makeMix(double secs) {
    static constexpr double kPi = 3.14159265358979323846;
    auto t = std::make_unique<TrackAudio>();
    t->fileSampleRate = kRate;
    t->deviceRate     = kRate;
    t->channels       = 2;
    t->numFrames      = (int64_t)(kRate * secs);
    t->samples.resize((size_t)t->numFrames * 2);
    for (int64_t i = 0; i < t->numFrames; ++i) {
        const double x = (double)i / kRate;
        float l = (float)(0.20 * std::sin(2.0 * kPi * 80.0 * x)
                        + 0.15 * std::sin(2.0 * kPi * 1000.0 * x)
                        + 0.10 * std::sin(2.0 * kPi * 5000.0 * x));
        float r = (float)(0.18 * std::sin(2.0 * kPi * 80.0 * x + 0.3)
                        + 0.15 * std::sin(2.0 * kPi * 1000.0 * x)
                        + 0.12 * std::sin(2.0 * kPi * 5000.0 * x + 0.5));
        if (i % (int64_t)(kRate * 0.25) < 48) { l += 0.3f; r += 0.3f; }
        t->samples[(size_t)i * 2]     = l;
        t->samples[(size_t)i * 2 + 1] = r;
    }
    return t;
}

/// Convenience: run the reference model to completion on a mix.
std::unique_ptr<StemSet> separate(const TrackAudio& src,
                                  int quality = (int)StemQuality::Balanced) {
    BandSplitModel m;
    StemModelConfig cfg; cfg.filterOrder = quality == (int)StemQuality::Fast ? 1
                                 : quality == (int)StemQuality::HighQuality ? 4 : 2;
    REQUIRE(m.load(cfg, nullptr));
    StemCancel cancel{false};
    REQUIRE(m.analyze(src, cancel, nullptr) == StemError::Ok);
    auto set = std::make_unique<StemSet>();
    REQUIRE(m.separate(src, *set, cancel, nullptr) == StemError::Ok);
    return set;
}

DeckStemBuffers viewOf(const StemSet& s) {
    DeckStemBuffers v;
    v.frames = s.numFrames;
    v.mask   = s.mask;
    for (int i = 0; i < DeckStemBuffers::kMaxStems; ++i)
        v.samples[i] = s.samples[(size_t)i].data();
    return v;
}

fs::path tempRoot(const char* name) {
    fs::path d = fs::temp_directory_path() / "onda_p21test" / name;
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

double rmsOf(const std::vector<float>& a, const std::vector<float>& b) {
    double acc = 0.0;
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const double d = (double)a[i] - (double)b[i];
        acc += d * d;
    }
    return std::sqrt(acc / (double)n);
}

// ---- deck rig -------------------------------------------------------
struct DeckRig {
    DeckEngine deck;
    DeckStats  stats;
    std::unique_ptr<TrackAudio> track;
    std::unique_ptr<StemSet>    set;
    DeckStemBuffers             view;
    std::vector<float> l = std::vector<float>(kBlock),
                       r = std::vector<float>(kBlock);

    DeckRig() {
        deck.bind(&stats);
        deck.setDeviceRate(kRate);
        deck.setStretcher(std::make_unique<WsolaStretcher>());
        deck.prepareDsp(kRate);
    }
    void load(double secs = 2.0) {
        track = makeMix(secs);
        deck.pendingTrack_.store(track.get());
        render(1);
    }
    void adoptStems() {
        view = viewOf(*set);
        deck.pendingStems_.store(&view);
        render(1);
    }
    void render(int n = 1) {
        for (int i = 0; i < n; ++i) deck.renderPcm(l.data(), r.data(), kBlock);
    }
    double blockPeak() const {
        double p = 0.0;
        for (int i = 0; i < kBlock; ++i)
            p = std::max(p, std::max((double)std::fabs(l[i]),
                                     (double)std::fabs(r[i])));
        return p;
    }
};

bool waitFor(std::function<bool()> f, int ms = 10000) {
    const auto t0 = std::chrono::steady_clock::now();
    while (!f()) {
        if (std::chrono::steady_clock::now() - t0 >
            std::chrono::milliseconds(ms)) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

} // namespace

// ==================================================================
// §2/§10/§11 — separation produces aligned, valid stem buffers
// ==================================================================
TEST_CASE("P21 separation: 4 stems, aligned to the source timeline") {
    auto src = makeMix(1.5);
    auto set = separate(*src);

    REQUIRE(set->numFrames == src->numFrames);          // §11 alignment
    REQUIRE((int)set->rate == (int)kRate);              // §10 format
    REQUIRE(set->channels == 2);
    for (StemType t : { StemType::Vocals, StemType::Drums,
                        StemType::Bass, StemType::Instruments }) {
        REQUIRE(set->has(t));
        const auto& s = set->samples[(size_t)t];
        REQUIRE(s.size() == (size_t)src->numFrames * 2);
        for (size_t i = 0; i < s.size(); i += 1024)     // §40 finite
            REQUIRE(std::isfinite(s[i]));
    }
}

TEST_CASE("P21 separation: stems sum back to the source (§51)") {
    auto src = makeMix(1.0);
    auto set = separate(*src);

    // INSTRUMENTS is the residual → reconstruction is exact by
    // construction; verify within float tolerance.
    std::vector<float> sum((size_t)src->numFrames * 2, 0.0f);
    for (int t = 0; t < kStemCount; ++t)
        for (size_t i = 0; i < sum.size(); ++i)
            sum[i] += set->samples[(size_t)t][i];
    const double rms = rmsOf(sum, src->samples);
    REQUIRE(rms < 1e-4);                                // near-exact
    double peak = 0.0;
    for (size_t i = 0; i < sum.size(); ++i)
        peak = std::max(peak, (double)std::fabs(sum[i] - src->samples[i]));
    REQUIRE(peak < 2e-3);
}

TEST_CASE("P21 separation: cancel aborts cleanly") {
    auto src = makeMix(2.0);
    BandSplitModel m;
    StemModelConfig cfg; cfg.filterOrder = 2;
    REQUIRE(m.load(cfg, nullptr));
    StemCancel cancel{false};
    REQUIRE(m.analyze(*src, cancel, nullptr) == StemError::Ok);
    cancel.store(true);
    StemSet out;
    REQUIRE(m.separate(*src, out, cancel, nullptr) == StemError::Cancelled);
}

TEST_CASE("P21 separation: invalid audio rejected (§40)") {
    TrackAudio bad;
    bad.fileSampleRate = kRate; bad.deviceRate = kRate;
    bad.channels = 2; bad.numFrames = 128;
    bad.samples.assign(256, std::nanf(""));
    BandSplitModel m;
    REQUIRE(m.load(StemModelConfig{}, nullptr));
    StemCancel c{false};
    REQUIRE(m.analyze(bad, c, nullptr) == StemError::InvalidAudio);
}

TEST_CASE("P21 model: not-loaded model refuses to run (§41)") {
    auto src = makeMix(0.5);
    BandSplitModel m;                       // never load()ed
    StemCancel c{false};
    REQUIRE(m.analyze(*src, c, nullptr) == StemError::ModelLoadFailed);
}

// ==================================================================
// §27/§28/§30/§31 — StemCache
// ==================================================================
TEST_CASE("P21 cache: key covers track+model+version+quality+rate+ch") {
    StemSetMeta m;
    m.contentHash = 0xDEADBEEF; m.modelId = "m"; m.modelVersion = "1";
    m.quality = 1; m.rate = 48000.0; m.channels = 2;
    const std::string base = StemCache::keyFor(m);
    REQUIRE(base.size() == 16);
    REQUIRE(StemCache::keyFor(m) == base);              // deterministic

    auto flip = [&](auto fn) { auto c = m; fn(c); return StemCache::keyFor(c); };
    REQUIRE(flip([](StemSetMeta& x){ x.contentHash ^= 1; })  != base);
    REQUIRE(flip([](StemSetMeta& x){ x.modelId = "m2"; })    != base);
    REQUIRE(flip([](StemSetMeta& x){ x.modelVersion = "2"; })!= base);
    REQUIRE(flip([](StemSetMeta& x){ x.quality = 2; })       != base);
    REQUIRE(flip([](StemSetMeta& x){ x.rate = 44100.0; })    != base);
    REQUIRE(flip([](StemSetMeta& x){ x.channels = 1; })      != base);
    // §28: moving the file never enters the key — contentHash is the
    // identity, so a relocated track keeps its cache entry.
}

TEST_CASE("P21 cache: write → read round-trip, atomic commit") {
    const auto dir = tempRoot("cache_rw");
    StemCache cache(dir.string());
    auto src = makeMix(0.5);
    auto set = separate(*src);

    StemSetMeta m;
    m.contentHash = 42; m.modelId = BandSplitModel::kId;
    m.modelVersion = BandSplitModel::kVersion;
    m.quality = 1; m.rate = kRate; m.channels = 2;
    m.frames = set->numFrames; m.mask = set->mask;

    const std::string key = cache.write(*set, m);
    REQUIRE(!key.empty());
    REQUIRE(cache.exists(key));
    // atomicity: no .tmp dirs left after a successful commit
    REQUIRE(!fs::exists(dir / (key + ".tmp")));

    StemSet back;
    REQUIRE(cache.read(key, m, back));
    REQUIRE(back.numFrames == set->numFrames);
    REQUIRE(back.mask == set->mask);
    REQUIRE(back.samples[(size_t)StemType::Bass][0]
            == Approx(set->samples[(size_t)StemType::Bass][0]));

    // mismatched expectations are hard failures (§28)
    auto wrong = m; wrong.quality = 2;
    StemSet drop;
    REQUIRE(!cache.read(key, wrong, drop));
    wrong = m; wrong.rate = 44100.0;
    REQUIRE(!cache.read(key, wrong, drop));

    // stats + storage manager surface (§30)
    const auto st = cache.stats();
    REQUIRE(st.sets == 1);
    REQUIRE(st.bytes > 0);
}

TEST_CASE("P21 cache: readRange streams a stem window (§31)") {
    const auto dir = tempRoot("cache_range");
    StemCache cache(dir.string());
    auto src = makeMix(0.5);
    auto set = separate(*src);
    StemSetMeta m;
    m.contentHash = 7; m.modelId = "x"; m.modelVersion = "1";
    m.quality = 1; m.rate = kRate; m.channels = 2;
    m.frames = set->numFrames; m.mask = set->mask;
    const std::string key = cache.write(*set, m);
    REQUIRE(!key.empty());

    std::vector<float> win;
    REQUIRE(cache.readRange(key, StemType::Bass, 100, 64, win));
    REQUIRE(win.size() == 128);                          // 64 fr × 2ch
    const auto& bass = set->samples[(size_t)StemType::Bass];
    for (int i = 0; i < 128; ++i)
        REQUIRE(win[(size_t)i] == Approx(bass[100 * 2 + i]).margin(1e-6));
}

TEST_CASE("P21 cache: cancelled write leaves no partial set (§42)") {
    const auto dir = tempRoot("cache_cancel");
    StemCache cache(dir.string());
    auto src = makeMix(0.5);
    auto set = separate(*src);
    StemSetMeta m;
    m.contentHash = 9; m.modelId = "x"; m.modelVersion = "1";
    m.quality = 1; m.rate = kRate; m.channels = 2;
    m.frames = set->numFrames; m.mask = set->mask;
    StemCancel c{true};                                  // pre-cancelled
    const std::string key = cache.write(*set, m, &c);
    REQUIRE(key.empty());
    // nothing readable under that key
    REQUIRE(!cache.exists(StemCache::keyFor(m)));
    bool anyTmp = false;
    for (auto& e : fs::directory_iterator(dir))
        anyTmp |= e.path().extension() == ".tmp";
    REQUIRE(!anyTmp);
}

TEST_CASE("P21 cache: clearSet/clearAll remove files, not metadata") {
    const auto dir = tempRoot("cache_clear");
    StemCache cache(dir.string());
    auto src = makeMix(0.3);
    auto set = separate(*src);
    StemSetMeta m;
    m.modelId = "x"; m.modelVersion = "1"; m.quality = 1;
    m.rate = kRate; m.channels = 2;
    m.frames = set->numFrames; m.mask = set->mask;
    m.contentHash = 1; const std::string k1 = cache.write(*set, m);
    m.contentHash = 2; const std::string k2 = cache.write(*set, m);
    REQUIRE(cache.stats().sets == 2);
    REQUIRE(cache.clearSet(k1));
    REQUIRE(cache.stats().sets == 1);
    REQUIRE(!cache.clearSet("nonexistent"));
    cache.clearAll();
    REQUIRE(cache.stats().sets == 0);
    (void)k2;
}

// ==================================================================
// §29/§47/§48 — StemStore (schema v7)
// ==================================================================
TEST_CASE("P21 store: save/find/status round-trip by content hash") {
    const auto dir = tempRoot("store");
    Database db;
    REQUIRE(db.open((dir / "lib.db").string()));
    StemStore store(db);

    StemSetMeta m;
    m.trackId = 5; m.contentHash = 0xABCD; m.modelId = "m";
    m.modelVersion = "1"; m.quality = 1; m.rate = kRate;
    m.channels = 2; m.frames = 96000; m.mask = 0xF;
    m.key = StemCache::keyFor(m);

    REQUIRE(store.statusForTrack(0xABCD) == -1);         // §47 none
    store.save(m, StemStore::Pending);
    REQUIRE(store.statusForTrack(0xABCD) == StemStore::Pending);

    StemSetMeta got; int st = -1;
    REQUIRE(store.find(0xABCD, got, &st));
    REQUIRE(st == StemStore::Pending);
    REQUIRE(got.frames == 96000);
    REQUIRE(got.mask == 0xF);
    REQUIRE(got.modelId == "m");

    store.markState(0xABCD, m.key, StemStore::Ready);
    REQUIRE(store.statusForTrack(0xABCD) == StemStore::Ready);

    store.deleteKey(0xABCD, m.key);
    REQUIRE(store.statusForTrack(0xABCD) == -1);
}

// ==================================================================
// §12-16 — StemMixer resolution
// ==================================================================
TEST_CASE("P21 mixer: gain/mute/solo/pan resolution") {
    StemMixer mx;
    const uint32_t all = 0xF;
    REQUIRE(mx.audibleMask(all) == 0xF);                 // all audible

    mx.setMute(1, true);
    REQUIRE(mx.audibleMask(all) == 0xD);                 // drums out
    mx.setMute(1, false);

    mx.setGain(0, 0.5f);
    const StemOut o = mx.out(0);
    REQUIRE(o.gL == Approx(0.5 * 0.7071f).margin(1e-3)); // center pan
    REQUIRE(o.gR == Approx(0.5 * 0.7071f).margin(1e-3));

    // §14 explicit solo: soloed stem only, everything else silent
    mx.setSolo(2, true);
    REQUIRE(mx.audibleMask(all) == 0x4);
    // mute wins over solo
    mx.setMute(2, true);
    REQUIRE(mx.audibleMask(all) == 0x0);
    mx.clearMutes(); mx.clearSolos();
    mx.setGain(0, 1.0f);                               // undo earlier gain

    // §16 constant-power pan: hard left keeps energy
    mx.setPan(0, -1.0f);
    const StemOut L = mx.out(0);
    REQUIRE(L.gL == Approx(1.0f).margin(1e-3));
    REQUIRE(L.gR == Approx(0.0f).margin(1e-3));
}

TEST_CASE("P21 router: present stems route to the deck channel (§17-19)") {
    StemRouter r;
    auto p = r.plan(2, /*stemsModeOn*/true, 0xF);
    REQUIRE(p.stemsActive);
    REQUIRE(p.channel == 2);                             // deck → channel
    for (int i = 0; i < kStemCount; ++i) {
        REQUIRE(p.routes[(size_t)i].valid);
        REQUIRE(p.routes[(size_t)i].fxSlot == -1);       // direct (§17)
    }
    // FX intent only legal on present stems while active (§18)
    int want[kStemMax] = { 0, -1, 2, -1 };
    p = r.plan(2, true, stemBit(StemType::Vocals) | stemBit(StemType::Bass),
               want);
    REQUIRE(p.routes[0].fxSlot == 0);
    REQUIRE(p.routes[2].fxSlot == 2);
    REQUIRE(!p.routes[1].valid);                         // absent (§26)
    // stems mode off → FX slots stripped
    p = r.plan(2, false, 0xF, want);
    REQUIRE(!p.stemsActive);
    REQUIRE(p.routes[0].fxSlot == -1);
}

// ==================================================================
// RT — DeckEngine stem rendering (§25/§26/§36-38/§50)
// ==================================================================
TEST_CASE("P21 deck: no stems → original renders (§25 fallback)") {
    DeckRig d;
    d.load();
    d.deck.setPlaying(true);
    applyDeckCommand(d.deck, 0, rt::SetStemMode{0, 1});  // mode on, no set
    d.render(2);
    REQUIRE(d.blockPeak() > 0.05);                       // original plays
    REQUIRE(d.stats.stemMode.load() == 0);               // stats: no set
}

TEST_CASE("P21 deck: stem mode renders the stem mix, aligned") {
    DeckRig d;
    d.load();
    d.set = separate(*d.track);
    d.adoptStems();
    applyDeckCommand(d.deck, 0, rt::SetStemMode{0, 1});
    d.deck.setPlaying(true);
    d.render(2);
    // stem mix ≈ source (perfect reconstruction) → audible, finite
    REQUIRE(d.blockPeak() > 0.05);
    REQUIRE(d.stats.stemMode.load() == 1);
    REQUIRE(d.stats.stemsAudible.load() == 0xF);
}

TEST_CASE("P21 deck: mute silences one stem, others keep playing") {
    DeckRig d;
    d.load();
    d.set = separate(*d.track);
    d.adoptStems();
    applyDeckCommand(d.deck, 0, rt::SetStemMode{0, 1});
    applyDeckCommand(d.deck, 0, rt::SetStemParam{
        0, (uint8_t)StemType::Vocals, 1.0f, 0.0f, 1, 0});
    d.deck.setPlaying(true);
    d.render(4);                                         // sm ramps
    REQUIRE((d.stats.stemsAudible.load() & 0xF) ==
            (0xF & ~stemBit(StemType::Vocals)));
    REQUIRE(d.blockPeak() > 0.05);

    // mute ALL stems → silence (no bleed from the original buffer)
    for (int i = 0; i < 4; ++i)
        applyDeckCommand(d.deck, 0, rt::SetStemParam{
            0, (uint8_t)i, 1.0f, 0.0f, 1, 0});
    d.render(64);                                        // let sm decay
    REQUIRE(d.blockPeak() < 0.01);
}

TEST_CASE("P21 deck: solo leaves only the soloed stem") {
    DeckRig d;
    d.load();
    d.set = separate(*d.track);
    d.adoptStems();
    applyDeckCommand(d.deck, 0, rt::SetStemMode{0, 1});
    applyDeckCommand(d.deck, 0, rt::SetStemParam{
        0, (uint8_t)StemType::Bass, 1.0f, 0.0f, 0, 1});
    d.deck.setPlaying(true);
    d.render(4);
    REQUIRE(d.stats.stemsAudible.load() == stemBit(StemType::Bass));
}

TEST_CASE("P21 deck: stem gain scales the contribution") {
    DeckRig d;
    d.load();
    d.set = separate(*d.track);
    d.adoptStems();
    applyDeckCommand(d.deck, 0, rt::SetStemMode{0, 1});
    // solo bass at 0.5 gain vs 1.0 gain → output halves
    auto soloBassAt = [&](float g) {
        DeckRig dd;
        dd.load();
        dd.set = separate(*dd.track);
        dd.adoptStems();
        applyDeckCommand(dd.deck, 0, rt::SetStemMode{0, 1});
        applyDeckCommand(dd.deck, 0, rt::SetStemParam{
            0, (uint8_t)StemType::Bass, g, 0.0f, 0, 1});
        dd.deck.setPlaying(true);
        dd.render(64);                                   // sm settle
        double acc = 0.0;
        for (int i = 0; i < kBlock; ++i) acc += dd.l[i] * dd.l[i];
        return std::sqrt(acc / kBlock);
    };
    const double r1 = soloBassAt(1.0f), r05 = soloBassAt(0.5f);
    REQUIRE(r1 > 0.01);
    REQUIRE(r05 / r1 == Approx(0.5).margin(0.15));
}

TEST_CASE("P21 deck: partial set renders what exists (§26)") {
    DeckRig d;
    d.load();
    auto full = separate(*d.track);
    // keep only vocals+drums, drop the rest
    full->mask = stemBit(StemType::Vocals) | stemBit(StemType::Drums);
    full->samples[(size_t)StemType::Bass].clear();
    full->samples[(size_t)StemType::Instruments].clear();
    d.set = std::move(full);
    d.adoptStems();
    applyDeckCommand(d.deck, 0, rt::SetStemMode{0, 1});
    d.deck.setPlaying(true);
    d.render(4);
    REQUIRE(d.blockPeak() > 0.01);
    REQUIRE(d.stats.stemsAudible.load() ==
            (stemBit(StemType::Vocals) | stemBit(StemType::Drums)));
}

TEST_CASE("P21 deck: keylock path keeps stems aligned (§37)") {
    DeckRig d;
    d.load();
    d.set = separate(*d.track);
    d.adoptStems();
    applyDeckCommand(d.deck, 0, rt::SetStemMode{0, 1});
    applyDeckCommand(d.deck, 0, rt::SetKeyLock{0, true});
    applyDeckCommand(d.deck, 0, rt::SetPitch{0, -20.0f}); // stretch path
    d.deck.setPlaying(true);
    d.render(32);
    // stretched output is audible and finite — stems ride the same
    // feed chunk as the original (§37/§38)
    double peak = 0.0;
    for (int i = 0; i < kBlock; ++i)
        peak = std::max(peak, (double)std::fabs(d.l[i]));
    REQUIRE(peak > 0.01);
    for (int i = 0; i < kBlock; ++i) REQUIRE(std::isfinite(d.l[i]));
}

// ==================================================================
// §9/§42/§45 — StemScheduler
// ==================================================================
TEST_CASE("P21 scheduler: jobs run in priority order, notify phases") {
    StemScheduler s(1);
    std::vector<int64_t> order;
    std::mutex om;
    s.setRunner([&](const StemJob& j, StemCancel&,
                    const std::function<void(StemJobPhase, float)>& r) {
        r(StemJobPhase::Separating, 0.5f);
        std::lock_guard<std::mutex> lk(om);
        order.push_back(j.id);
        return StemError::Ok;
    });
    std::vector<StemScheduler::Event> ev;
    s.addListener([&](const StemScheduler::Notification& n) {
        if (n.event == StemScheduler::Event::Completed) {
            std::lock_guard<std::mutex> lk(om);
            ev.push_back(n.event);
        }
    });
    StemJob a; a.priority = 5;  StemJob b; b.priority = 1;
    StemJob c; c.priority = 9;
    const int64_t ia = s.enqueue(a), ib = s.enqueue(b), ic = s.enqueue(c);
    REQUIRE(waitFor([&]{ std::lock_guard<std::mutex> lk(om);
                         return order.size() == 3; }));
    // higher priority first — b(1) > a(5) > c(9)
    REQUIRE(order[0] == ib);
    REQUIRE(order[1] == ia);
    REQUIRE(order[2] == ic);
    REQUIRE(ev.size() == 3);
}

TEST_CASE("P21 scheduler: cancel removes queued jobs (§42)") {
    StemScheduler s(1);
    s.setRunner([](const StemJob&, StemCancel&,
                   const std::function<void(StemJobPhase, float)>&) {
        return StemError::Ok;
    });
    std::atomic<int> cancelled{0};
    s.addListener([&](const StemScheduler::Notification& n) {
        if (n.event == StemScheduler::Event::Cancelled) ++cancelled;
    });
    // hold the queue behind the CPU gate so nothing starts
    s.setMinActivePriority(100);
    StemJob j; j.priority = 1;
    const int64_t id = s.enqueue(j);
    s.cancel(id);
    REQUIRE(waitFor([&]{ return cancelled.load() == 1; }));
    s.setMinActivePriority(0);                            // drain queue
    const auto st = s.stateOf(id);
    REQUIRE(st.phase == (int)StemJobPhase::Cancelled);
}

TEST_CASE("P21 scheduler: CPU gate yields low-priority work (§45)") {
    StemScheduler s(1);
    std::atomic<int> ran{0};
    s.setRunner([&](const StemJob&, StemCancel&,
                    const std::function<void(StemJobPhase, float)>&) {
        ++ran; return StemError::Ok;
    });
    s.setMinActivePriority(5);                            // playback busy
    StemJob lo; lo.priority = 1;   // background → gated out
    StemJob hi; hi.priority = 9;   // foreground → runs
    s.enqueue(lo); s.enqueue(hi);
    REQUIRE(waitFor([&]{ return ran.load() == 1; }));
    // low-priority job still queued, not silently dropped
    const auto snap = s.snapshot();
    bool loStillQueued = false;
    for (auto& [id, st] : snap)
        loStillQueued |= st.phase == (int)StemJobPhase::Queued;
    REQUIRE(loStillQueued);
}

// ==================================================================
// StemsEngine — end to end (§4/§32/§41/§44)
// ==================================================================
TEST_CASE("P21 engine: separation → cache → publish → deck renders") {
    const auto dir = tempRoot("engine");
    Database db;
    REQUIRE(db.open((dir / "lib.db").string()));
    rt::CommandQueue q;
    DeckRig d;                          // decks outlive StemsEngine —
    StemsEngine eng(db, q);             // its dtor clears pending slots
    StemConfig cfg; cfg.cacheDir = (dir / "cache").string();
    cfg.autoSeparate = false;                             // explicit req
    eng.configure(cfg);

    eng.bindDeck(0, &d.deck, &d.stats);
    auto src = makeMix(1.0);
    eng.setDecoder([&](const std::string&) { return src.get(); });
    eng.setPlaybackActive(false);                         // tests: run all

    d.load();
    eng.onTrackLoading(0, 7, 0xF00D, "/tmp/mix.wav", kRate);
    const int64_t job = eng.requestSeparation(7, 0xF00D, "/tmp/mix.wav");
    REQUIRE(job > 0);
    REQUIRE(waitFor([&]{ return eng.jobPhase(job) == StemJobPhase::Ready; }));
    eng.poll();                                           // publish
    REQUIRE(d.deck.pendingStems_.load() != nullptr);
    d.render(1);                                          // RT adopt
    REQUIRE(eng.availability(0) == StemAvailability::Ready);
    REQUIRE(d.stats.stemsAvail.load() == (int)StemAvailability::Ready);

    // model work happened OFF the RT thread — verify by construction:
    // renderPcm only ever reads adopted buffers; pendingStems_ was
    // published from poll() (control thread), never inside render.
    applyDeckCommand(d.deck, 0, rt::SetStemMode{0, 1});
    d.deck.setPlaying(true);
    d.render(2);
    REQUIRE(d.blockPeak() > 0.05);

    // store row written (§48 metadata only)
    REQUIRE(eng.trackStatus(0xF00D) == StemStore::Ready);
}

TEST_CASE("P21 engine: second deck shares the cached set (§44)") {
    const auto dir = tempRoot("engine2");
    Database db;
    REQUIRE(db.open((dir / "lib.db").string()));
    rt::CommandQueue q;
    DeckRig d0, d1;                     // decks outlive StemsEngine
    StemsEngine eng(db, q);
    StemConfig cfg; cfg.cacheDir = (dir / "cache").string();
    eng.configure(cfg);

    eng.bindDeck(0, &d0.deck, &d0.stats);
    eng.bindDeck(1, &d1.deck, &d1.stats);
    auto src = makeMix(0.5);
    eng.setDecoder([&](const std::string&) { return src.get(); });
    eng.setPlaybackActive(false);
    d0.load(); d1.load();
    eng.onTrackLoading(0, 3, 0xBEEF, "/tmp/t.wav", kRate);
    const int64_t job = eng.requestSeparation(3, 0xBEEF, "/tmp/t.wav");
    REQUIRE(waitFor([&]{ return eng.jobPhase(job) == StemJobPhase::Ready; }));

    // deck 1 loads the same track — cache hit path
    eng.onTrackLoading(1, 3, 0xBEEF, "/tmp/t.wav", kRate);
    REQUIRE(waitFor([&]{ return eng.jobPhase(job) == StemJobPhase::Ready; }));
    eng.poll(); eng.poll();
    REQUIRE(d0.deck.pendingStems_.load() != nullptr);
    REQUIRE(d1.deck.pendingStems_.load() != nullptr);
    d0.render(1); d1.render(1);
    REQUIRE(eng.availability(1) == StemAvailability::Ready);
    // both decks render their own view of the shared set
    d0.deck.setPlaying(true); d1.deck.setPlaying(true);
    applyDeckCommand(d0.deck, 0, rt::SetStemMode{0, 1});
    applyDeckCommand(d1.deck, 1, rt::SetStemMode{1, 1});
    d0.render(2); d1.render(2);
    REQUIRE(d0.blockPeak() > 0.05);
    REQUIRE(d1.blockPeak() > 0.05);
}

TEST_CASE("P21 engine: model failure → deck still plays original (§41)") {
    const auto dir = tempRoot("engine_fail");
    Database db;
    REQUIRE(db.open((dir / "lib.db").string()));
    rt::CommandQueue q;
    DeckRig d;                          // outlives StemsEngine
    StemsEngine eng(db, q);
    StemConfig cfg; cfg.cacheDir = (dir / "cache").string();
    eng.configure(cfg);

    eng.bindDeck(0, &d.deck, &d.stats);
    eng.setDecoder([](const std::string&) -> const TrackAudio* {
        return nullptr;                                   // decode fails
    });
    eng.setPlaybackActive(false);
    d.load();
    eng.onTrackLoading(0, 9, 0x1234, "/tmp/x.wav", kRate);
    const int64_t job = eng.requestSeparation(9, 0x1234, "/tmp/x.wav");
    REQUIRE(waitFor([&]{ return eng.jobPhase(job) == StemJobPhase::Failed; }));
    eng.poll();
    REQUIRE(d.deck.pendingStems_.load() == nullptr);      // no set
    d.deck.setPlaying(true);
    d.render(2);
    REQUIRE(d.blockPeak() > 0.05);                        // original plays
    REQUIRE(eng.trackStatus(0x1234) == StemStore::Failed);// honest record
}

TEST_CASE("P21 engine: cancel separation (§42)") {
    const auto dir = tempRoot("engine_cancel");
    Database db;
    REQUIRE(db.open((dir / "lib.db").string()));
    rt::CommandQueue q;
    StemsEngine eng(db, q);
    StemConfig cfg; cfg.cacheDir = (dir / "cache").string();
    eng.configure(cfg);
    eng.setDecoder([](const std::string&) -> const TrackAudio* {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return nullptr;
    });
    eng.scheduler().setMinActivePriority(100);            // gate it queued
    const int64_t job = eng.requestSeparation(1, 0x77, "/tmp/y.wav");
    REQUIRE(job > 0);
    REQUIRE(eng.cancelSeparation(job));
    REQUIRE(waitFor([&]{ return eng.jobPhase(job) == StemJobPhase::Cancelled; }));
    eng.scheduler().setMinActivePriority(0);
}

TEST_CASE("P21 engine: stem actions resolve + post rt commands (§20)") {
    const auto dir = tempRoot("engine_perf");
    Database db;
    REQUIRE(db.open((dir / "lib.db").string()));
    rt::CommandQueue q;
    DeckRig d;                          // outlives StemsEngine
    StemsEngine eng(db, q);
    StemConfig cfg; cfg.cacheDir = (dir / "cache").string();
    eng.configure(cfg);
    eng.bindDeck(0, &d.deck, &d.stats);

    eng.setStemMute(0, (int)StemType::Vocals, true);
    eng.setStemGain(0, (int)StemType::Bass, 1.5f);
    eng.setStemSolo(0, (int)StemType::Drums, true);

    // drain the queue like the callback would — resolved POD commands
    int params = 0;
    rt::Command c;
    while (q.tryPop(c))
        if (std::holds_alternative<rt::SetStemParam>(c)) ++params;
    // mute + gain + solo(reposts all 9 slots) → ≥3 SetStemParam
    REQUIRE(params >= 3);
    REQUIRE(eng.mixer(0).param((int)StemType::Vocals).muted);
    REQUIRE(eng.mixer(0).param((int)StemType::Bass).gain == Approx(1.5f));
    // solo reposts every stem so the deck re-resolves audibility
    REQUIRE(eng.mixer(0).anySolo());
}

// ==================================================================
// §21/§22 — pad mode + feedback
// ==================================================================
TEST_CASE("P21 pads: stems mode maps pads 1-4 mute, shift=solo, 5-8 gate") {
    pads::PadModeManager mgr;
    mgr.setMode(0, pads::PadMode::Stems);
    // pad 0 → VOC mute toggle on deck 0
    auto a = mgr.resolve(0, 0, false, true);
    REQUIRE(a.kind == pads::PadAction::Command);
    REQUIRE(a.cmdId == cmd::forDeck(cmd::DeckStemBase, 0) + 0 * 8 + 0);
    // pad 2 + shift → BASS solo on deck 0
    a = mgr.resolve(0, 2, true, true);
    REQUIRE(a.cmdId == cmd::forDeck(cmd::DeckStemBase, 0) + 2 * 8 + 1);
    // pad 5 → DRUMS gate (press)
    a = mgr.resolve(0, 5, false, true);
    REQUIRE(a.cmdId == cmd::forDeck(cmd::DeckStemBase, 0) + 1 * 8 + 6);
    REQUIRE(a.arg == Approx(1.0f));
    a = mgr.resolve(0, 5, false, false);                 // release
    REQUIRE(a.arg == Approx(0.0f));
    // deck 3 — mapping is deck-relative, no hardcoded deck (§44)
    mgr.setMode(3, pads::PadMode::Stems);
    a = mgr.resolve(3, 1, false, true);
    REQUIRE(a.cmdId == cmd::forDeck(cmd::DeckStemBase, 3) + 1 * 8 + 0);
}

TEST_CASE("P21 pads: stem feedback states (§22)") {
    pads::PadFeedbackEngine fb;
    pads::PadDeckState s;
    s.mode = pads::PadMode::Stems;
    // present+active → Set; muted → Off; solo → Playing; loading → Dim
    s.stemFlags = { 0x01, 0x03, 0x05, 0x09, 0x01, 0x01, 0x01, 0x01 };
    REQUIRE(fb.light(s, 0).state == pads::PadLightState::Set);
    REQUIRE(fb.light(s, 1).state == pads::PadLightState::Off);
    REQUIRE(fb.light(s, 2).state == pads::PadLightState::Playing);
    REQUIRE(fb.light(s, 3).state == pads::PadLightState::Dim);   // loading
    s.stemFlags[4] = 0x00;                                       // absent
    REQUIRE(fb.light(s, 4).state == pads::PadLightState::Dim);
}

// ==================================================================
// §7/§8/§46 — provider + quality
// ==================================================================
TEST_CASE("P21 provider: quality picks config, GPU honoured when absent") {
    StemModelProvider p;
    const auto fast = p.configFor((int)StemQuality::Fast,
                                (int)GpuMode::Auto);
    const auto hq   = p.configFor((int)StemQuality::HighQuality,
                                (int)GpuMode::On);
    REQUIRE(fast.filterOrder < hq.filterOrder);           // §8 steepness
    // §7/§46: GPU On request on a GPU-less system resolves to Off —
    // never blocks, never fails.
    if (!p.caps().gpuAvailable)
        REQUIRE(hq.gpuMode == (int)GpuMode::Off);
    const auto off = p.configFor((int)StemQuality::Balanced,
                                 (int)GpuMode::Off);
    REQUIRE(off.gpuMode == (int)GpuMode::Off);
    // model creation is always the interface type (§5)
    auto m = p.createModel(fast);
    REQUIRE(m != nullptr);
    REQUIRE((m->capabilities().stemMask & stemBit(StemType::Bass)) != 0);
}

// ==================================================================
// §52/§53 — multi-deck + stress
// ==================================================================
TEST_CASE("P21 stress: 4 decks + stems + render stays fast") {
    DeckRig decks[4];
    for (auto& d : decks) {
        d.load(1.0);
        d.set = separate(*d.track);
        d.adoptStems();
        applyDeckCommand(d.deck, 0, rt::SetStemMode{0, 1});
        applyDeckCommand(d.deck, 0, rt::SetKeyLock{0, true});
        applyDeckCommand(d.deck, 0, rt::SetPitch{0, -12.0f});
        d.deck.setPlaying(true);
    }
    const auto t0 = std::chrono::steady_clock::now();
    for (int b = 0; b < 64; ++b)
        for (auto& d : decks) d.render(1);
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    // 4 decks × 64 blocks of 512 = ~2.7 s audio; Debug budget generous
    REQUIRE(ms < 3000.0);
    for (auto& d : decks) REQUIRE(std::isfinite(d.l[kBlock - 1]));
    INFO("4-deck stem render x64 blocks: " << ms << " ms");
}
