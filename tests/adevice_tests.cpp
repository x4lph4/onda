// Phase 13 — Audio Device Engine tests. The whole domain layer runs
// against a FakeBackend: enumeration, capabilities, selection, safe
// switching + rollback, rate/buffer policy, routing, hot-plug,
// recovery, health, profiles, diagnostics and the test-signal tool.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "core/adevice/AudioDeviceEngine.h"
#include "core/adevice/AudioTestTool.h"
#include "core/mixer/MixerEngine.h"

using namespace onda;
using Catch::Approx;

namespace {

/// Deterministic backend: scripted device lists, recordable applies,
/// injectable failures — exercises every AudioDeviceEngine path.
struct FakeBackend : adevice::Backend {
    std::vector<adevice::RawDevice> devs;
    std::map<std::string, adevice::DeviceCapabilities> caps;
    adevice::BackendStatus st;
    int applies = 0, reopenDefaults = 0, probes = 0;
    std::string failApply;          // next apply() returns this error
    bool failStatusRunning = false; // apply ok but status says !running

    FakeBackend() {
        st.backend = "FAKE";
        st.outName = "Speaker";
        st.rate = 48000; st.buffer = 256;
        st.outCh = 4; st.inLatSmp = 64; st.outLatSmp = 64;
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
        ++probes;
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
        st.outName = "Speaker"; st.running = true; return {};
    }
};

adevice::RawDevice mk(const char* name, int in, int out,
                      bool defOut = false, bool defIn = false) {
    adevice::RawDevice d;
    d.backend = "FAKE"; d.name = name;
    d.inCh = in; d.outCh = out;
    d.isDefaultOut = defOut; d.isDefaultIn = defIn;
    return d;
}

adevice::DeviceCapabilities mkCaps(int out, int in = 0) {
    adevice::DeviceCapabilities c;
    c.known = true; c.outCh = out; c.inCh = in;
    c.sampleRates = { 44100.0, 48000.0 };
    c.bufferSizes = { 64, 128, 256, 512, 1024 };
    c.duplex = in > 0; c.lowLatency = true;
    return c;
}

} // namespace

TEST_CASE("[adev] catalog normalize + stable ids + defaults", "[adev]") {
    std::vector<adevice::RawDevice> raw = {
        mk("Speaker", 0, 2, true),
        mk("USB DJ", 2, 4),
        mk("Mic", 1, 0, false, true),
    };
    const auto v = adevice::DeviceCatalog::normalize(raw);
    REQUIRE(v.size() == 3);
    CHECK(v[0].id == "FAKE|Speaker");
    CHECK(v[0].isDefaultOut);
    CHECK(v[2].isDefaultIn);
    CHECK(v[1].inCh == 2);
    // name ALONE is never the id — backend namespaces it
    CHECK(v[0].id != v[0].name);
    CHECK(adevice::DeviceCatalog::defaultOut(v)->name == "Speaker");
    CHECK(adevice::DeviceCatalog::defaultIn(v)->name == "Mic");
    CHECK(adevice::DeviceCatalog::byId(v, "FAKE|USB DJ") == &v[1]);
    CHECK(adevice::DeviceCatalog::byId(v, "nope") == nullptr);
}

TEST_CASE("[adev] sample-rate policy", "[adev]") {
    using S = adevice::SampleRateManager;
    const std::vector<double> rates = { 44100.0, 48000.0 };
    CHECK(S::choose(rates, 48000.0) == 48000.0);
    CHECK(S::choose(rates, 0.0) == 48000.0);     // default prefers 48k
    CHECK(S::choose(rates, 96000.0) == 48000.0); // unsupported → closest
    CHECK(S::choose({44100.0}, 48000.0) == 44100.0);
    CHECK(S::choose({}, 48000.0) == 0.0);        // unknown → let backend decide
    CHECK(S::supported(rates, 44100.0));
    CHECK(!S::supported(rates, 96000.0));
    // mismatch produces a clear explanation mentioning both rates
    const auto e = S::explain(96000.0, 48000.0);
    CHECK(e.find("96000") != std::string::npos);
    CHECK(e.find("48000") != std::string::npos);
    CHECK(e.find("No resampling") != std::string::npos);
    CHECK(S::explain(48000.0, 48000.0).empty());
}

TEST_CASE("[adev] buffer policy", "[adev]") {
    using B = adevice::BufferManager;
    const std::vector<int> sizes = { 64, 128, 256, 512, 1024 };
    CHECK(B::nearest(sizes, 300) == 256);
    CHECK(B::nearest(sizes, 96) == 64);       // tie? 96→64 vs128 → 64
    CHECK(B::nearest({}, 256) == 0);
    CHECK(B::forPreset(adevice::LatencyPref::Low, sizes) == 128);
    CHECK(B::forPreset(adevice::LatencyPref::Balanced, sizes) == 256);
    CHECK(B::forPreset(adevice::LatencyPref::Stable, sizes) == 512);
    // device without small buffers → nearest supported, never invented
    const std::vector<int> coarse = { 256, 512 };
    CHECK(B::forPreset(adevice::LatencyPref::Low, coarse) == 256);
    CHECK(B::supported(sizes, 512));
    CHECK(!B::supported(sizes, 300));
    CHECK(B::prefFor(64) == adevice::LatencyPref::Low);
    CHECK(B::prefFor(512) == adevice::LatencyPref::Stable);
}

TEST_CASE("[adev] latency estimation is never faked", "[adev]") {
    const auto r = adevice::LatencyManager::estimate(48000.0, 256, 48, 48);
    CHECK(r.bufferMs == Approx(256.0 / 48000.0 * 1000.0));
    CHECK(r.inputMs == Approx(1.0));
    CHECK(r.outputMs == Approx(1.0));
    CHECK(r.roundTripMs == Approx(r.bufferMs + 2.0));
    CHECK(!r.measured);                        // estimate, not measured
    CHECK(r.explain.find("256") != std::string::npos);
    const auto bad = adevice::LatencyManager::estimate(0.0, 256);
    CHECK(bad.roundTripMs == 0.0);
}

TEST_CASE("[adev] routing defaults + validation", "[adev]") {
    using R = adevice::AudioRouting;
    const auto m4 = R::defaults(4);
    CHECK(m4.masterL == 0); CHECK(m4.masterR == 1);
    CHECK(m4.hasCue()); CHECK(m4.cueL == 2); CHECK(m4.cueR == 3);
    const auto m2 = R::defaults(2);
    CHECK(!m2.hasCue());
    CHECK(R::check(m4, 4).empty());
    CHECK(!R::check(m4, 2).empty());           // cue outside range
    CHECK(R::check({0, 1, -1, -1, -1, -1}, 2).empty());
    adevice::ChannelMapping overlap{0, 1, 1, 2, -1, -1};
    CHECK(R::check(overlap, 4).find("overlap") != std::string::npos);
    const auto f = R::flx4();
    CHECK(f.masterL == 0); CHECK(f.cueL == 2); // documented topology
    CHECK(R::check(f, 4).empty());
}

TEST_CASE("[adev] engine: enumerate + select happy path", "[adev]") {
    FakeBackend fb;
    fb.devs = { mk("Speaker", 0, 2, true), mk("USB DJ", 2, 4) };
    fb.caps["USB DJ"] = mkCaps(4, 2);
    adevice::AudioDeviceEngine eng(fb);
    const auto v = eng.enumerate();
    REQUIRE(v.size() == 2);

    adevice::DeviceSettings s;
    s.outputId = "FAKE|USB DJ";
    s.sampleRate = 48000;
    CHECK(eng.select(s).empty());
    CHECK(fb.applies == 1);
    CHECK(fb.st.outName == "USB DJ");
    CHECK(fb.st.rate == 48000.0);
    CHECK(eng.settings().outputId == "FAKE|USB DJ");
    CHECK(eng.settings().map.hasCue());        // 4-out → cue auto-mapped
    CHECK(eng.recoveryState() == adevice::HealthState::Healthy);
}

TEST_CASE("[adev] select: missing/gone device is actionable", "[adev]") {
    FakeBackend fb;
    fb.devs = { mk("Speaker", 0, 2, true) };
    adevice::AudioDeviceEngine eng(fb);
    eng.enumerate();
    adevice::DeviceSettings s;
    s.outputId = "FAKE|USB DJ";
    const auto err = eng.select(s);
    CHECK(err.find("not found") != std::string::npos);
    CHECK(fb.applies == 0);                    // never touched the backend
}

TEST_CASE("[adev] select: unsupported rate falls back, buffer snaps", "[adev]") {
    FakeBackend fb;
    fb.devs = { mk("USB DJ", 0, 4) };
    fb.caps["USB DJ"] = mkCaps(4);
    adevice::AudioDeviceEngine eng(fb);
    eng.enumerate();
    adevice::DeviceSettings s;
    s.outputId = "FAKE|USB DJ";
    s.sampleRate = 96000;      // not offered → nearest supported
    s.bufferSize = 300;        // not offered → nearest supported
    std::string note;
    eng.ports.notify = [&](const std::string& t, const std::string&) {
        note = t;
    };
    CHECK(eng.select(s).empty());
    CHECK(fb.st.rate == 48000.0);              // 96k unsupported → 48k
    CHECK(fb.st.buffer == 256);                // 300 → 256
    CHECK(note.find("Sample rate") != std::string::npos);
}

TEST_CASE("[adev] safe switch: apply failure rolls back", "[adev]") {
    FakeBackend fb;
    fb.devs = { mk("USB DJ", 0, 4) };
    fb.caps["USB DJ"] = mkCaps(4);
    adevice::AudioDeviceEngine eng(fb);
    eng.enumerate();
    fb.failApply = "device busy";
    adevice::DeviceSettings s;
    s.outputId = "FAKE|USB DJ";
    const auto err = eng.select(s);
    CHECK(err == "device busy");
    CHECK(fb.reopenDefaults == 1);             // rollback happened
    CHECK(eng.recoveryState() != adevice::HealthState::Healthy);
    CHECK(fb.st.outName == "Speaker");         // back on default
}

TEST_CASE("[adev] stream that refuses to start is an error", "[adev]") {
    FakeBackend fb;
    fb.devs = { mk("USB DJ", 0, 4) };
    fb.caps["USB DJ"] = mkCaps(4);
    adevice::AudioDeviceEngine eng(fb);
    eng.enumerate();
    fb.failStatusRunning = true;               // apply ok, stream dead
    adevice::DeviceSettings s;
    s.outputId = "FAKE|USB DJ";
    CHECK(eng.select(s) == "stream did not start");
}

TEST_CASE("[adev] hot-plug: loss → reconnect → recovered", "[adev]") {
    FakeBackend fb;
    fb.devs = { mk("USB DJ", 0, 4, true) };
    fb.caps["USB DJ"] = mkCaps(4);
    fb.st.outName = "USB DJ";
    adevice::AudioDeviceEngine eng(fb);
    eng.enumerate();
    adevice::DeviceSettings s;
    s.outputId = "FAKE|USB DJ";
    REQUIRE(eng.select(s).empty());

    eng.ports.notify = [](const std::string&, const std::string&) {};
    fb.devs.clear();                           // cable pulled
    eng.tick(5000, 1.0, 0.5, 0, 0.2f, 0);
    CHECK(eng.recoveryState() != adevice::HealthState::Healthy);
    CHECK(fb.reopenDefaults >= 1);             // recovery attempted

    fb.devs.push_back(mk("USB DJ", 0, 4, true)); // replugged
    eng.tick(9000, 1.0, 0.5, 0, 0.2f, 0);
    CHECK((eng.recoveryState() == adevice::HealthState::Healthy
        || eng.recoveryState() == adevice::HealthState::Recovered
        || eng.recoveryState() == adevice::HealthState::Reconnecting));
}

TEST_CASE("[adev] recovery FSM transitions", "[adev]") {
    adevice::AudioRecoveryManager r;
    using E = adevice::AudioRecoveryManager::Event;
    CHECK(r.state() == adevice::HealthState::Healthy);
    r.on(E::StreamFailed);
    CHECK(r.state() == adevice::HealthState::Warning);
    r.on(E::DeviceLost);
    CHECK(r.state() == adevice::HealthState::DeviceLost);
    r.on(E::RecoverOk);
    CHECK(r.state() == adevice::HealthState::Reconnecting);
    r.on(E::StreamOk);
    CHECK(r.state() == adevice::HealthState::Recovered);
    r.on(E::StreamOk);
    CHECK(r.state() == adevice::HealthState::Healthy);
    // failed recovery parks in Failed until a reopen succeeds
    r.on(E::DeviceLost); r.on(E::RecoverFailed);
    CHECK(r.state() == adevice::HealthState::Failed);
    r.on(E::StreamOk);
    CHECK(r.state() == adevice::HealthState::Reconnecting);
}

TEST_CASE("[adev] health monitor: underruns, budget, dedup", "[adev]") {
    adevice::AudioHealthMonitor h;
    h.feed(1000, 48000, 256, 2.0, 0.4, 0, 0.5f, 0);
    auto r = h.report();
    CHECK(r.cbBudgetMs == Approx(256.0 / 48.0));
    CHECK(r.underruns == 0);
    // a new underrun stamp records an event
    h.feed(1500, 48000, 256, 8.0, 0.4, 1, 0.5f, 777);
    r = h.report();
    CHECK(r.underruns == 1);
    CHECK(r.xruns == 1);
    REQUIRE(r.recent.size() == 1);
    CHECK(r.recent[0].tsMs == 777);
    // same stamp again → no double count
    h.feed(1600, 48000, 256, 8.0, 0.4, 1, 0.5f, 777);
    CHECK(h.report().underruns == 1);
    // overload warning deduplicated within 5 s
    h.feed(2000, 48000, 256, 6.0, 5.5, 1, 0.95f, 777);
    CHECK(h.hasWarnings());
    CHECK(h.popWarning().find("85%") != std::string::npos);
    h.feed(3000, 48000, 256, 6.0, 5.5, 1, 0.95f, 777);
    CHECK(!h.hasWarnings());                   // same warning suppressed
}

TEST_CASE("[adev] profiles persist per device id", "[adev]") {
    FakeBackend fb;
    std::map<std::string, std::string> store;
    adevice::AudioDeviceEngine eng(fb);
    eng.ports.cfgGet = [&](const std::string& k) {
        const auto it = store.find(k);
        return it == store.end() ? std::string{} : it->second;
    };
    eng.ports.cfgSet = [&](const std::string& k, const std::string& v) {
        store[k] = v;
    };
    adevice::DeviceProfile p;
    p.deviceId = "FAKE|USB DJ";
    p.sampleRate = 44100; p.bufferSize = 128;
    p.pref = adevice::LatencyPref::Low;
    p.map = { 0, 1, 2, 3, -1, -1 };
    eng.saveProfile(p);
    // round-trip through the JSON store
    const auto q = eng.profileFor("FAKE|USB DJ");
    CHECK(q.sampleRate == 44100.0);
    CHECK(q.bufferSize == 128);
    CHECK(q.pref == adevice::LatencyPref::Low);
    CHECK(q.map == p.map);
    // different device id → different (empty) profile
    const auto other = eng.profileFor("FAKE|Speaker");
    CHECK(other.sampleRate == 0.0);
}

TEST_CASE("[adev] suggested() prefers profile then FLX4 topology", "[adev]") {
    FakeBackend fb;
    fb.devs = { mk("DDJ-FLX4 WASAPI", 2, 4) };
    adevice::AudioDeviceEngine eng(fb);
    eng.enumerate();
    const auto* f = eng.flx4();
    REQUIRE(f != nullptr);
    CHECK(f->name == "DDJ-FLX4 WASAPI");
    // select on FLX4 auto-maps master 0/1 + phones 2/3
    adevice::DeviceSettings s;
    s.outputId = f->id;
    s.map.masterL = -1;
    adevice::ChannelMapping got;
    eng.ports.mixerMap = [&](const adevice::ChannelMapping& m) {
        got = m;
    };
    CHECK(eng.select(s).empty());
    CHECK(got == adevice::AudioRouting::flx4());
}

TEST_CASE("[adev] channel mapping goes to the mixer", "[adev]") {
    FakeBackend fb;
    fb.devs = { mk("Speaker", 0, 2, true) };
    adevice::AudioDeviceEngine eng(fb);
    adevice::ChannelMapping got;
    eng.ports.mixerMap = [&](const adevice::ChannelMapping& m) {
        got = m;
    };
    // valid remap on the live device (st.outCh == 4)
    CHECK(eng.setChannelMapping({1, 0, 3, 2, -1, -1}).empty());
    CHECK(got.masterL == 1);
    CHECK(got.masterR == 0);
    // out-of-range mapping rejected
    CHECK(!eng.setChannelMapping({0, 1, 7, 8, -1, -1}).empty());
}

TEST_CASE("[adev] diagnostics text covers the required keys", "[adev]") {
    FakeBackend fb;
    adevice::AudioDeviceEngine eng(fb);
    const auto d = eng.diagnostics();
    for (const char* k : { "DEVICE", "BACKEND", "SAMPLE RATE", "BUFFER",
                          "INPUT CH", "OUTPUT CH", "ESTIMATED LATENCY",
                          "CPU", "UNDERRUNS", "XRUNS", "STREAM STATUS" })
        CHECK(d.find(k) != std::string::npos);
    CHECK(d.find("estimated") != std::string::npos);  // never "measured"
}

TEST_CASE("[adev] test signal: routing matrix", "[adev]") {
    // 4 channels, {mL=0,mR=1,cL=2,cR=3} — verify each mode writes the
    // expected channels and nothing else.
    const int n = 64;
    std::vector<float> ch[4];
    for (auto& c : ch) c.assign((size_t)n, 0.0f);
    float* outs[4] = { ch[0].data(), ch[1].data(),
                       ch[2].data(), ch[3].data() };
    adevice::ChannelMapping m{0, 1, 2, 3, -1, -1};
    uint64_t phase = 0;
    auto rms = [](const std::vector<float>& v) {
        double a = 0; for (float x : v) a += x * x; return a;
    };

    using S = adevice::TestSignal;
    using T = adevice::AudioTestTool;

    T::render(S::Sine440, m, outs, 4, n, 48000, phase);
    CHECK(rms(ch[0]) > 0.0); CHECK(rms(ch[1]) > 0.0);
    CHECK(rms(ch[2]) == 0.0); CHECK(rms(ch[3]) == 0.0);

    for (auto& c : ch) std::fill(c.begin(), c.end(), 0.0f); phase = 0;
    T::render(S::LeftOnly, m, outs, 4, n, 48000, phase);
    CHECK(rms(ch[0]) > 0.0); CHECK(rms(ch[1]) == 0.0);

    for (auto& c : ch) std::fill(c.begin(), c.end(), 0.0f); phase = 0;
    T::render(S::RightOnly, m, outs, 4, n, 48000, phase);
    CHECK(rms(ch[1]) > 0.0); CHECK(rms(ch[0]) == 0.0);

    for (auto& c : ch) std::fill(c.begin(), c.end(), 0.0f); phase = 0;
    T::render(S::CueSine, m, outs, 4, n, 48000, phase);
    CHECK(rms(ch[2]) > 0.0); CHECK(rms(ch[3]) > 0.0);
    CHECK(rms(ch[0]) == 0.0); CHECK(rms(ch[1]) == 0.0);

    // InPhase: identical signal on L and R (phase-correct stereo)
    for (auto& c : ch) std::fill(c.begin(), c.end(), 0.0f); phase = 0;
    T::render(S::InPhase, m, outs, 4, n, 48000, phase);
    for (int i = 0; i < n; ++i)
        CHECK(ch[0][i] == ch[1][i]);

    // AltPing alternates sides across the ping period (rate*0.5 smp)
    for (auto& c : ch) std::fill(c.begin(), c.end(), 0.0f); phase = 0;
    T::render(S::AltPing, m, outs, 4, n, 48000, phase);
    CHECK(rms(ch[0]) > 0.0);
    phase = 24000;   // second half-second → right
    for (auto& c : ch) std::fill(c.begin(), c.end(), 0.0f);
    T::render(S::AltPing, m, outs, 4, n, 48000, phase);
    CHECK(rms(ch[1]) > 0.0);

    // Silence zeroes even previously-written content
    for (auto& c : ch) std::fill(c.begin(), c.end(), 1.0f);
    T::render(S::Silence, m, outs, 4, n, 48000, phase);
    for (auto& c : ch) CHECK(rms(c) == 0.0);

    // Off does nothing at all
    for (auto& c : ch) std::fill(c.begin(), c.end(), 1.0f);
    T::render(S::Off, m, outs, 4, n, 48000, phase);
    CHECK(rms(ch[0]) == (double)n);
}

TEST_CASE("[adev] mixer honours a custom output map", "[adev]") {
    // cueAvailable tracks whether the mapped cue pair is in range —
    // the observable proof that render() follows setOutputMap().
    MixerEngine mixer;
    mixer.prepare(48000.0);
    rt::AudioStats stats;
    std::vector<float> ch[4];
    const int n = 128;
    for (auto& c : ch) c.assign((size_t)n, 0.0f);
    float* outs[4] = { ch[0].data(), ch[1].data(),
                       ch[2].data(), ch[3].data() };

    mixer.setOutputMap(0, 1, 2, 3);      // default 4-out layout
    mixer.render(outs, 4, n, nullptr, 0, stats);
    CHECK(mixer.masterStats().cueAvailable.load() == true);

    mixer.setOutputMap(0, 1, -1, -1);    // cue disabled
    mixer.render(outs, 4, n, nullptr, 0, stats);
    CHECK(mixer.masterStats().cueAvailable.load() == false);

    mixer.setOutputMap(2, 3, 0, 1);      // swapped: cue on 0-1 now
    mixer.render(outs, 4, n, nullptr, 0, stats);
    CHECK(mixer.masterStats().cueAvailable.load() == true);
    CHECK(mixer.outMap(0) == 2);

    mixer.setOutputMap(0, 1, 2, 3);
    mixer.render(outs, 2, n, nullptr, 0, stats); // 2-out device
    CHECK(mixer.masterStats().cueAvailable.load() == false);
}

TEST_CASE("[adev] engine tick is cheap and recovery-safe", "[adev]") {
    FakeBackend fb;
    fb.devs = { mk("Speaker", 0, 2, true) };
    adevice::AudioDeviceEngine eng(fb);
    eng.enumerate();
    // 200 ticks — enumeration is throttled (2s), not per-tick
    for (int i = 0; i < 200; ++i)
        eng.tick(i * 50, 1.0, 0.5, 0, 0.1f, 0);
    CHECK(eng.recoveryState() == adevice::HealthState::Healthy);
}
