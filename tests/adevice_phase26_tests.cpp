// Phase 26 — audio device engine completion: booth pair in the domain
// mapping, typed errors + device status, DJ-controller audio profile,
// auto-route safety, headphone mono, stream-start fade, output guard
// (sanitize + DC), cue isolation/mix sweep/4-deck combos, failure
// recovery, and a mock DDJ-FLX4 profile end-to-end.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "core/adevice/AudioDeviceEngine.h"
#include "core/adevice/DjAudioProfile.h"
#include "core/audio/TrackAudio.h"
#include "core/dsp/WsolaStretcher.h"
#include "core/engine/DeckEngine.h"
#include "core/mixer/MixerEngine.h"
#include "core/mixer/OutputGuard.h"
#include "core/mixer/ParameterRegistry.h"
#include "core/model/DeckStats.h"
#include "core/rt/AudioStats.h"

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
float rms(const std::vector<float>& v) {
    double s = 0.0;
    for (float x : v) s += (double)x * x;
    return (float)std::sqrt(s / (double)v.size());
}

// ---------------- fake backend (same shape as Phase-13 tests) -------
struct FakeBackend26 : adevice::Backend {
    std::vector<adevice::RawDevice> devs;
    std::map<std::string, adevice::DeviceCapabilities> caps;
    adevice::BackendStatus st;
    int applies = 0, reopenDefaults = 0;
    std::string failApply;
    bool failStatusRunning = false;

    FakeBackend26() {
        st.backend = "FAKE"; st.outName = "Speaker";
        st.rate = 48000; st.buffer = 256; st.outCh = 4;
        st.running = true;
    }
    std::string backendName() const override { return "FAKE"; }
    std::vector<adevice::RawDevice> enumerate() override { return devs; }
    adevice::BackendStatus status() override {
        auto s = st;
        if (failStatusRunning) s.running = false;
        return s;
    }
    adevice::DeviceCapabilities probe(const std::string& n) override {
        const auto it = caps.find(n);
        return it == caps.end() ? adevice::DeviceCapabilities{}
                                : it->second;
    }
    std::string apply(const adevice::DesiredSetup& ds) override {
        ++applies;
        if (!failApply.empty()) {
            std::string e = failApply; failApply.clear(); return e;
        }
        st.outName = ds.outName.empty() ? "Speaker" : ds.outName;
        if (ds.rate > 0) st.rate = ds.rate;
        if (ds.buffer > 0) st.buffer = ds.buffer;
        st.running = true;
        return {};
    }
    std::string reopenDefault() override {
        ++reopenDefaults;
        if (!failReopen.empty()) return failReopen;
        st.outName = "Speaker"; st.running = true; return {};
    }
    std::string failReopen;
};

adevice::RawDevice mkDev(const char* name, int in, int out,
                         bool defOut = false) {
    adevice::RawDevice d;
    d.backend = "FAKE"; d.name = name;
    d.inCh = in; d.outCh = out; d.isDefaultOut = defOut;
    return d;
}
adevice::DeviceCapabilities mkCaps26(int out, int in = 0) {
    adevice::DeviceCapabilities c;
    c.known = true; c.outCh = out; c.inCh = in;
    c.sampleRates = { 44100.0, 48000.0 };
    c.bufferSizes = { 64, 128, 256, 512, 1024 };
    c.duplex = in > 0; c.lowLatency = true;
    return c;
}
adevice::DeviceInfo mkInfo(const char* name, int out) {
    adevice::DeviceInfo d;
    d.backend = "FAKE"; d.name = name;
    d.id = adevice::DeviceInfo::makeId("FAKE", name);
    d.outCh = out;
    return d;
}

// ---------------- mixer rig: 6 outs, master 0-1 / cue 2-3 / booth 4-5
struct Rig26 {
    MixerEngine mixer;
    rt::AudioStats astats;
    std::vector<DeckEngine> decks;
    std::vector<DeckStats>  dstats;
    std::vector<const TrackAudio*> owned;
    std::vector<float> outL, outR, cueL, cueR, boL, boR;

    Rig26(int ndecks = 2) : decks(ndecks), dstats(ndecks),
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
        }
    }
    void loadDeck(int d, double freq, float amp) {
        auto* t = makeSine(6.0, freq, amp);
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
            const float v = amp * (float)std::sin(
                2.0 * 3.14159265 * freq * i / kRate);
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
    void run(int n) { for (int i = 0; i < n; ++i) render(); }
    void chParam(int ch, uint16_t id, float v) { mixer.setParam(id, v); }
    ~Rig26() { for (auto* t : owned) delete t; }
};

} // namespace

// ============================================================ MAPPING
TEST_CASE("adev26: ChannelMapping booth pair — defaults + validation",
          "[adev26]") {
    using AR = adevice::AudioRouting;
    const auto m6 = AR::defaults(6);
    CHECK(m6.boothL == 4); CHECK(m6.boothR == 5); CHECK(m6.hasBooth());
    CHECK(AR::span(m6) == 6);
    CHECK(AR::check(m6, 6).empty());

    const auto m4 = AR::defaults(4);
    CHECK(m4.hasCue()); CHECK(!m4.hasBooth());
    CHECK(AR::span(m4) == 4);
    CHECK(AR::check(m4, 4).empty());

    const auto m2 = AR::defaults(2);
    CHECK(!m2.hasCue()); CHECK(!m2.hasBooth());
    CHECK(AR::check(m2, 2).empty());

    // invalid: booth outside range / half pair / overlapping pairs
    adevice::ChannelMapping bad = m6; bad.boothR = 6;
    CHECK(!AR::check(bad, 6).empty());
    bad = m6; bad.boothL = -1;
    CHECK(!AR::check(bad, 6).empty());
    bad = m6; bad.boothL = 2;      // collides with cueL
    CHECK(!AR::check(bad, 6).empty());
    bad = m6; bad.boothL = 0;      // collides with masterL
    CHECK(!AR::check(bad, 6).empty());
}

TEST_CASE("adev26: autoRoute — safe defaults or Manual-Required",
          "[adev26]") {
    using AR = adevice::AudioRouting;
    const auto zero = AR::autoRoute(0);
    CHECK(zero.manualRequired());
    CHECK(zero.reason.find("Manual") != std::string::npos);

    const auto two = AR::autoRoute(2);
    REQUIRE(two.ok);
    CHECK(two.map.masterL == 0); CHECK(two.map.masterR == 1);
    CHECK(!two.map.hasCue());

    const auto four = AR::autoRoute(4);
    REQUIRE(four.ok); CHECK(four.map.hasCue()); CHECK(!four.map.hasBooth());

    const auto six = AR::autoRoute(6);
    REQUIRE(six.ok); CHECK(six.map.hasBooth());

    const auto mono = AR::autoRoute(1);
    REQUIRE(mono.ok);
    CHECK(mono.map.masterL == 0);
    CHECK(mono.map.masterR == 0);   // mono collapse on out 0
}

// ====================================================== DJ PROFILES
TEST_CASE("adev26: DjAudioProfile — detected caps only, never invented",
          "[adev26]") {
    using DP = adevice::DjAudioProfile;
    SECTION("FLX4 with 4 detected outs -> controller profile") {
        const auto p = DP::detect(mkInfo("DDJ-FLX4", 4), mkCaps26(4));
        REQUIRE(p.role == DP::Role::DjController);
        CHECK(p.suggested.masterL == 0); CHECK(p.suggested.masterR == 1);
        CHECK(p.suggested.cueL == 2);    CHECK(p.suggested.cueR == 3);
        CHECK(p.headphonesOn34);
        CHECK(!p.hasBoothPair);          // FLX4 has no booth outs
        CHECK(!p.suggested.hasBooth());
    }
    SECTION("FLX4 seen with only 2 outs -> no profile (don't invent)") {
        const auto p = DP::detect(mkInfo("DDJ-FLX4", 2), mkCaps26(2));
        CHECK(!p.detected());
        CHECK(!p.suggested.hasCue());
    }
    SECTION("generic 6-out interface -> DjInterface with booth") {
        const auto p = DP::detect(mkInfo("USB Audio 8", 8), mkCaps26(6));
        REQUIRE(p.role == DP::Role::DjInterface);
        CHECK(p.suggested.hasCue());
        CHECK(p.hasBoothPair);
        CHECK(p.suggested.boothL == 4);
    }
    SECTION("plain stereo device -> none") {
        const auto p = DP::detect(mkInfo("Speakers", 2), mkCaps26(2));
        CHECK(!p.detected());
    }
}

// ==================================================== ERROR MODEL
TEST_CASE("adev26: typed errors + device status (§31-32)", "[adev26]") {
    FakeBackend26 b;
    adevice::AudioDeviceEngine e(b);
    b.devs = { mkDev("Speaker", 0, 2, true), mkDev("USB DJ", 2, 4) };
    b.caps["USB DJ"] = mkCaps26(4, 2);
    e.enumerate();

    SECTION("missing device -> DEVICE_NOT_FOUND") {
        adevice::DeviceSettings s; s.outputId = "FAKE|ghost";
        const auto r = e.selectEx(s);
        REQUIRE(r.code == adevice::AudioError::DeviceNotFound);
        CHECK(!r.message.empty());
        CHECK(e.lastResult().code == r.code);
    }
    SECTION("bad routing -> CHANNEL_CONFIGURATION_FAILED") {
        adevice::DeviceSettings s; s.outputId = "FAKE|USB DJ";
        s.map.cueL = 9;                     // outside 0..3
        const auto r = e.selectEx(s);
        CHECK(r.code == adevice::AudioError::ChannelConfigurationFailed);
    }
    SECTION("apply failure -> INITIALIZATION_FAILED + rollback") {
        adevice::DeviceSettings s; s.outputId = "FAKE|USB DJ";
        b.failApply = "driver busy";
        const auto r = e.selectEx(s);
        CHECK(r.code == adevice::AudioError::InitializationFailed);
        CHECK(b.reopenDefaults == 1);        // rolled back, not left dead
    }
    SECTION("stream refuses to start -> STREAM_START_FAILED") {
        adevice::DeviceSettings s; s.outputId = "FAKE|USB DJ";
        b.failStatusRunning = true;
        const auto r = e.selectEx(s);
        CHECK(r.code == adevice::AudioError::StreamStartFailed);
    }
    SECTION("success -> Ok + status ACTIVE") {
        adevice::DeviceSettings s; s.outputId = "FAKE|USB DJ";
        const auto r = e.selectEx(s);
        REQUIRE(r.ok());
        CHECK(e.lastResult().ok());
        CHECK(e.deviceStatus() == adevice::AudioDeviceStatus::Active);
    }
    SECTION("no probe -> auto-route manual required") {
        b.devs = { mkDev("Opaque", 0, 1, true) };
        b.caps["Opaque"] = mkCaps26(0);   // probed: zero outs
        e.enumerate();
        adevice::DeviceSettings s; s.outputId = "FAKE|Opaque";
        const auto r = e.selectEx(s);
        CHECK(r.code == adevice::AudioError::ManualConfigRequired);
    }
}

TEST_CASE("adev26: disconnect -> status + typed error + recovery",
          "[adev26]") {
    FakeBackend26 b;
    adevice::AudioDeviceEngine e(b);
    b.devs = { mkDev("Speaker", 0, 2, true), mkDev("USB DJ", 0, 4) };
    b.caps["USB DJ"] = mkCaps26(4);
    e.enumerate();
    adevice::DeviceSettings s; s.outputId = "FAKE|USB DJ";
    REQUIRE(e.selectEx(s).ok());
    CHECK(e.deviceStatus() == adevice::AudioDeviceStatus::Active);

    // device vanishes while streaming → typed error + automatic
    // recovery onto the default output (§12-13): never a crash.
    b.devs = { mkDev("Speaker", 0, 2, true) };
    b.st.running = false;
    e.tick(1000, 0.1, 0.05, 0, 0.2f, 0);
    CHECK(e.lastResult().code == adevice::AudioError::DeviceDisconnected);
    // first retry is immediate → fallback already streaming again
    CHECK(b.reopenDefaults >= 1);
    CHECK(e.deviceStatus() == adevice::AudioDeviceStatus::Active);

    // and when recovery itself cannot reopen anything -> ERROR state
    b.devs = { mkDev("Speaker", 0, 2, true), mkDev("USB DJ", 0, 4) };
    e.enumerate();
    adevice::DeviceSettings s2; s2.outputId = "FAKE|USB DJ";
    REQUIRE(e.selectEx(s2).ok());
    b.devs = { mkDev("Speaker", 0, 2, true) };
    b.st.running = false;
    b.failReopen = "no outputs available";
    e.tick(20000, 0.1, 0.05, 0, 0.2f, 0);
    CHECK(e.deviceStatus() == adevice::AudioDeviceStatus::Error);
}

// ==================================================== PROFILE JSON
TEST_CASE("adev26: DeviceProfile v2 — booth keys, v1 compatible",
          "[adev26]") {
    adevice::DeviceProfile p;
    p.deviceId = "FAKE|USB DJ"; p.sampleRate = 48000; p.bufferSize = 256;
    p.map = adevice::AudioRouting::defaults(6);
    const auto j = p.toJson();
    adevice::DeviceProfile q;
    REQUIRE(adevice::DeviceProfile::fromJson(j, q));
    CHECK(q.map.boothL == 4); CHECK(q.map.boothR == 5);
    CHECK(q.map == p.map);

    // v1 payload (no bL/bR keys) reads back as booth=-1 — not broken
    const std::string v1 =
        "{\"id\":\"FAKE|USB DJ\",\"rate\":48000,\"buffer\":256,"
        "\"pref\":1,\"mL\":0,\"mR\":1,\"cL\":2,\"cR\":3,"
        "\"iL\":-1,\"iR\":-1}";
    adevice::DeviceProfile r;
    REQUIRE(adevice::DeviceProfile::fromJson(v1, r));
    CHECK(r.map.boothL == -1); CHECK(r.map.boothR == -1);
    CHECK(r.map.cueL == 2);
}

// ==================================================== CUE LOGIC
TEST_CASE("adev26: cue bus is crossfader-independent (§52)",
          "[adev26][cue]") {
    Rig26 g(2);
    g.loadDeck(0, 220.0, 0.5f);    // deck A
    g.loadDeck(1, 440.0, 0.5f);    // deck B
    g.chParam(0, mixer::ParameterRegistry::channelCue(0), 1.0f); // A cue ON
    g.chParam(1, mixer::ParameterRegistry::channelCue(1), 0.0f); // B cue OFF
    g.mixer.setParam((uint16_t)mixer::BusParam::CrossfaderPos, 1.0f);
    g.mixer.setParam((uint16_t)mixer::BusParam::CueMix, 0.0f); // cue only
    g.run(30);                                    // settle xf + PFL taps

    // phones = cue bus = deck A only — no xf dependence, no deck B
    const float cuePk  = peakAbs(g.cueL.data(), kBlock);
    const float mastPk = peakAbs(g.outL.data(), kBlock);
    REQUIRE(cuePk > 0.1f);
    REQUIRE(mastPk > 0.1f);

    // isolate spectra: cue out must carry 220 Hz, not 440 Hz.
    // simple discriminator: count zero-crossings (440 Hz doubles 220).
    auto crossings = [](const float* v, int n) {
        int c = 0;
        for (int i = 1; i < n; ++i)
            if ((v[i-1] <= 0.0f) != (v[i] <= 0.0f)) ++c;
        return c;
    };
    const int zc = crossings(g.cueL.data(), kBlock);
    // 220 Hz @ 48k/block≈4.7 crossings; 440 Hz ≈9.4
    CHECK(zc < 8);
}

TEST_CASE("adev26: cue/master mix sweep 0-25-50-75-100 (§53)",
          "[adev26][cue]") {
    Rig26 g(1);
    g.loadDeck(0, 330.0, 0.5f);
    g.chParam(0, mixer::ParameterRegistry::channelCue(0), 1.0f);
    g.mixer.setParam((uint16_t)mixer::BusParam::MasterGain, 0.5f);

    auto cueRms = [&](float mix) {
        g.mixer.setParam((uint16_t)mixer::BusParam::CueMix, mix);
        g.run(40);
        return rms(g.cueL);
    };
    const float at0   = cueRms(0.0f);   // cue only
    const float at25  = cueRms(0.25f);
    const float at50  = cueRms(0.5f);
    const float at75  = cueRms(0.75f);
    const float at100 = cueRms(1.0f);   // master only

    // both endpoints must carry signal — cue (PFL taps, pre-fader) and
    // master (post-fader × masterGain) legitimately differ in level.
    REQUIRE(at0   > 0.05f);
    REQUIRE(at100 > 0.01f);
    // intermediate blends stay inside the endpoint envelope — the mix
    // knob interpolates, never mutes or explodes.
    const float lo = std::min(at0, at100), hi = std::max(at0, at100);
    for (float v : { at25, at50, at75 }) {
        CHECK(v > lo * 0.4f);
        CHECK(v < hi * 1.4f);
    }
}

TEST_CASE("adev26: four-deck cue — individual + combined (§54)",
          "[adev26][cue]") {
    Rig26 g(4);
    g.loadDeck(0, 200.0, 0.4f);
    g.loadDeck(1, 300.0, 0.4f);
    g.loadDeck(2, 400.0, 0.4f);
    g.loadDeck(3, 500.0, 0.4f);
    g.mixer.setParam((uint16_t)mixer::BusParam::CueMix, 0.0f);

    auto cuePk = [&] { g.run(20); return peakAbs(g.cueL.data(), kBlock); };

    // none cued -> silence on the cue bus
    REQUIRE(cuePk() < 0.05f);
    // each deck individually
    for (int d = 0; d < 4; ++d) {
        for (int i = 0; i < 4; ++i)
            g.mixer.setParam(mixer::ParameterRegistry::channelCue(i),
                             i == d ? 1.0f : 0.0f);
        REQUIRE(cuePk() > 0.08f);
    }
    // A+B cued -> louder than single (sum)
    for (int i = 0; i < 4; ++i)
        g.mixer.setParam(mixer::ParameterRegistry::channelCue(i),
                         (i < 2) ? 1.0f : 0.0f);
    CHECK(cuePk() > 0.2f);
    // all four -> bounded, finite
    for (int i = 0; i < 4; ++i)
        g.mixer.setParam(mixer::ParameterRegistry::channelCue(i), 1.0f);
    const float allPk = cuePk();
    CHECK(allPk > 0.3f);
    CHECK(allPk < 4.0f);
}

TEST_CASE("adev26: master cue hears the program (§22)",
          "[adev26][cue]") {
    Rig26 g(1);
    g.loadDeck(0, 260.0, 0.5f);
    // no PFL cues — only MasterCue ON
    g.mixer.setParam((uint16_t)mixer::BusParam::MasterCue, 1.0f);
    g.mixer.setParam((uint16_t)mixer::BusParam::CueMix, 0.0f);
    g.run(30);
    CHECK(peakAbs(g.cueL.data(), kBlock) > 0.1f);
    // master cue off -> cue bus goes silent (no PFL, mix=0)
    g.mixer.setParam((uint16_t)mixer::BusParam::MasterCue, 0.0f);
    g.run(30);
    CHECK(peakAbs(g.cueL.data(), kBlock) < 0.05f);
}

TEST_CASE("adev26: headphone mono folds L+R (§24)", "[adev26][cue]") {
    Rig26 g(1);
    g.loadDeck(0, 330.0, 0.5f);
    // pan hard left so L != R on the master pair; phones on master side
    g.chParam(0, mixer::ParameterRegistry::channelPan(0), -1.0f);
    g.mixer.setParam((uint16_t)mixer::BusParam::CueMix, 1.0f);
    g.run(30);
    const float lPk = peakAbs(g.cueL.data(), kBlock);
    const float rPk = peakAbs(g.cueR.data(), kBlock);
    REQUIRE(lPk > 0.1f);
    CHECK(rPk < lPk * 0.3f);          // panned left → right near-empty

    g.mixer.setParam((uint16_t)mixer::BusParam::HeadphoneMono, 1.0f);
    g.run(30);                        // smoothed fold-down
    const float ml = peakAbs(g.cueL.data(), kBlock);
    const float mr = peakAbs(g.cueR.data(), kBlock);
    CHECK(mr > 0.03f);                // fold-down carries the L content
    CHECK(ml == Approx(mr).margin(0.01f)); // L≈R sample-for-sample
    // registry: the param exists and quantizes to 0/1
    CHECK(mixer::ParameterRegistry::exists(
        (uint16_t)mixer::BusParam::HeadphoneMono));
    CHECK(mixer::ParameterRegistry::sanitize(
        (uint16_t)mixer::BusParam::HeadphoneMono, 0.7f) == 1.0f);
}

// ================================================ OUTPUT PROTECTION
TEST_CASE("adev26: OutputGuard sanitizes + detects DC (§40-41)",
          "[adev26][guard]") {
    mixer::OutputGuard g;
    g.prepare(kRate);
    std::vector<float> l(kBlock), r(kBlock);
    for (int i = 0; i < kBlock; ++i) {
        l[i] = (i % 4 == 0) ? std::numeric_limits<float>::quiet_NaN()
              : (i % 4 == 1) ? 9.0f      // >kClamp → sanitized
              : (i % 4 == 2) ? 1.5f      // >1.0, <kClamp → overFull
              : 0.5f;
        r[i] = 0.5f;
    }
    g.process(l.data(), r.data(), kBlock);
    CHECK(g.stats().sanitized.load() > 0);
    CHECK(g.stats().overFull.load() > 0);
    for (float x : l) CHECK(std::isfinite(x));
    CHECK(peakAbs(l.data(), kBlock) <= mixer::OutputGuard::kClamp);
    // DC estimate on a constant +0.5 signal rises toward 0.5
    for (int b = 0; b < 40; ++b)
        g.process(l.data(), r.data(), kBlock);   // all-0.5 residual
    CHECK(g.stats().dc.load() > 0.1f);
}

TEST_CASE("adev26: stream restart re-arms the output fade (§38-39)",
          "[adev26][fade]") {
    Rig26 g(1);
    g.loadDeck(0, 330.0, 0.5f);
    g.run(40);
    const float steady = peakAbs(g.outL.data(), kBlock);
    REQUIRE(steady > 0.1f);

    g.mixer.onStreamRestart();        // device (re)start → fade-in
    g.render();
    const float firstBlock = peakAbs(g.outL.data(), kBlock);
    g.run(60);                        // ~40 ms ramp + settle
    const float settled = peakAbs(g.outL.data(), kBlock);
    CHECK(firstBlock < steady * 0.9f); // ramp started below program
    CHECK(settled == Approx(steady).margin(0.05f));
}

TEST_CASE("adev26: mono master mapping folds L+R (§15)",
          "[adev26][routing]") {
    Rig26 g(1);
    g.loadDeck(0, 330.0, 0.5f);
    g.chParam(0, mixer::ParameterRegistry::channelPan(0), -1.0f); // L-heavy
    // collapse master onto one physical out — must carry the sum
    g.mixer.setOutputMap(0, 0, 2, 3, 4, 5);
    g.run(30);
    CHECK(peakAbs(g.outL.data(), kBlock) > 0.05f);
}

// ================================================ END-TO-END DEVICE
TEST_CASE("adev26: mock DDJ-FLX4 — detection + routing + mapping to "
          "the mixer (§27-28, §56)", "[adev26][flx4]") {
    FakeBackend26 b;
    adevice::AudioDeviceEngine e(b);
    b.devs = { mkDev("Speaker", 0, 2, true),
               mkDev("DDJ-FLX4", 0, 4) };
    b.caps["DDJ-FLX4"] = mkCaps26(4);
    e.enumerate();

    // catalog finds it by name; profile only after caps confirm 4 outs
    REQUIRE(e.flx4() != nullptr);
    const auto prof = e.djProfile("FAKE|DDJ-FLX4");
    REQUIRE(prof.role == adevice::DjAudioProfile::Role::DjController);
    CHECK(prof.headphonesOn34);

    // select auto-picks the FLX4 topology → mixer receives the map
    adevice::ChannelMapping got;
    e.ports.mixerMap = [&](const adevice::ChannelMapping& m) { got = m; };
    adevice::DeviceSettings s; s.outputId = "FAKE|DDJ-FLX4";
    REQUIRE(e.selectEx(s).ok());
    CHECK(got.masterL == 0); CHECK(got.masterR == 1);
    CHECK(got.cueL == 2);    CHECK(got.cueR == 3);
    CHECK(got.boothL == -1); // FLX4 has no booth pair
    CHECK(e.deviceStatus() == adevice::AudioDeviceStatus::Active);
}

TEST_CASE("adev26: device failure preserves mixer/session state (§49-55)",
          "[adev26]") {
    FakeBackend26 b;
    adevice::AudioDeviceEngine e(b);
    b.devs = { mkDev("Speaker", 0, 2, true), mkDev("USB DJ", 0, 4) };
    b.caps["USB DJ"] = mkCaps26(4);
    e.enumerate();
    adevice::ChannelMapping got;
    e.ports.mixerMap = [&](const adevice::ChannelMapping& m) { got = m; };
    adevice::DeviceSettings s; s.outputId = "FAKE|USB DJ";
    REQUIRE(e.selectEx(s).ok());
    const auto savedMap = e.settings().map;
    CHECK(savedMap.hasCue());

    // stream dies mid-session → recovery lands on default;
    // settings/profile survive intact — nothing in decks/mixer touched
    b.devs = { mkDev("Speaker", 0, 2, true) };
    b.st.running = false;
    e.tick(1000, 0.1, 0.05, 0, 0.2f, 0);
    e.tick(11000, 0.1, 0.05, 0, 0.2f, 0);
    CHECK(b.reopenDefaults >= 1);
    CHECK(e.settings().map == savedMap);          // routing unchanged
    CHECK(e.settings().outputId == "FAKE|USB DJ"); // selection preserved
}

// ====================================================== PERFORMANCE
TEST_CASE("adev26: 4 decks + sampler + 6-out render stress (§57)",
          "[adev26][stress]") {
    Rig26 g(4);
    for (int d = 0; d < 4; ++d)
        g.loadDeck(d, 150.0 + 100.0 * d, 0.4f);
    for (int d = 0; d < 4; ++d)
        g.mixer.setParam(mixer::ParameterRegistry::channelCue(d), 1.0f);
    g.mixer.setParam((uint16_t)mixer::BusParam::MasterCue, 1.0f);
    g.mixer.onStreamRestart();

    const auto t0 = std::chrono::steady_clock::now();
    g.run(400);                                   // ~4.3 s of audio
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 2000.0);   // far under real-time budget
    CHECK(peakAbs(g.outL.data(), kBlock) < 4.5f);
    CHECK(peakAbs(g.cueL.data(), kBlock) < 4.5f);
    for (float x : g.outL) CHECK(std::isfinite(x));
    for (float x : g.boL)  CHECK(std::isfinite(x));
}
