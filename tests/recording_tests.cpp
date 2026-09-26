// Phase 12 — Professional Recording Engine + Session Management tests.
// The RT side (RecorderRt/SpscRing) is exercised exactly as AudioEngine
// drives it; the writer/engine/repo/recovery paths run on app threads.
// Known-signal tests verify input ≈ output through the WAV encoder.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "core/data/Database.h"
#include "core/library/MusicLibraryEngine.h"
#include "core/rec/RecordingBuffer.h"
#include "core/rec/RecordingEncoder.h"
#include "core/rec/RecordingEngine.h"
#include "core/rec/RecordingExport.h"
#include "core/rec/RecordingManager.h"
#include "core/rec/RecordingRecovery.h"
#include "core/rec/RecordingRepository.h"
#include "core/rec/RecordingSession.h"
#include "core/rec/RecordingWriter.h"
#include "ui/CommandIds.h"
#include "ui/CommandManager.h"
#include "ui/EngineCommands.h"

using namespace onda;
namespace fs = std::filesystem;

namespace {

fs::path tempDirR(const char* n) {
    fs::path d = fs::temp_directory_path() / "onda_rectest" / n;
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}
fs::path dbFile(const fs::path& d) {
    return d / "lib.db";
}
/// Minimal stereo buffer pair for pushPostMaster.
struct Sig {
    std::vector<float> l, r;
    Sig(size_t n, float v = 0.0f) : l(n, v), r(n, v) {}
    static Sig sine(size_t n, float amp, int period = 64) {
        Sig s(n);
        for (size_t i = 0; i < n; ++i)
            s.l[i] = s.r[i] =
                amp * std::sin(2.0 * 3.14159265358979 * i / period);
        return s;
    }
    static Sig ramp(size_t n) {        // left ≠ right, order-sensitive
        Sig s(n);
        for (size_t i = 0; i < n; ++i) {
            s.l[i] = (float)((int)(i % 200) - 100) / 200.0f;
            s.r[i] = (float)((int)(i % 150) - 75) / 150.0f;
        }
        return s;
    }
    void push(rec::RecorderRt& rt, int chunk = 512) {
        for (size_t off = 0; off < l.size(); off += (size_t)chunk) {
            const int n = (int)std::min((size_t)chunk, l.size() - off);
            rt.pushPostMaster(l.data() + off, r.data() + off, n);
        }
    }
};

rec::RecordingEngine::Ports fakePorts(double rate = 48000.0,
                                    int numDecks = 2) {
    rec::RecordingEngine::Ports p;
    p.sampleRate   = [rate] { return rate; };
    p.audioRunning = [] { return true; };
    p.numDecks     = numDecks;
    return p;
}
/// Drain helper — run engine+writer until the ring is empty (bounded).
void drain(rec::RecordingEngine& e, int iters = 400) {
    for (int i = 0; i < iters; ++i) {
        e.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (e.rt().ring().fillPct() == 0) break;
    }
}

} // namespace

// ---- pure state machine ---------------------------------------------

TEST_CASE("Rec: state machine allows only valid transitions", "[rec]") {
    using S = rec::State;
    CHECK(rec::RecordingEngine::canTransit(S::Idle, S::Armed));
    CHECK(rec::RecordingEngine::canTransit(S::Idle, S::Recording));
    CHECK(rec::RecordingEngine::canTransit(S::Armed, S::Recording));
    CHECK(rec::RecordingEngine::canTransit(S::Armed, S::Idle));
    CHECK(rec::RecordingEngine::canTransit(S::Recording, S::Paused));
    CHECK(rec::RecordingEngine::canTransit(S::Paused, S::Recording));
    CHECK(rec::RecordingEngine::canTransit(S::Recording, S::Stopping));
    CHECK(rec::RecordingEngine::canTransit(S::Paused, S::Stopping));
    CHECK(rec::RecordingEngine::canTransit(S::Stopping, S::Finalizing));
    CHECK(rec::RecordingEngine::canTransit(S::Finalizing, S::Completed));
    CHECK(rec::RecordingEngine::canTransit(S::Finalizing, S::Failed));
    CHECK(rec::RecordingEngine::canTransit(S::Recording, S::Failed));
    // impossible
    CHECK_FALSE(rec::RecordingEngine::canTransit(S::Idle, S::Paused));
    CHECK_FALSE(rec::RecordingEngine::canTransit(S::Idle, S::Finalizing));
    CHECK_FALSE(rec::RecordingEngine::canTransit(S::Armed, S::Paused));
    CHECK_FALSE(rec::RecordingEngine::canTransit(S::Paused, S::Idle));
    CHECK_FALSE(rec::RecordingEngine::canTransit(S::Completed, S::Recording));
    CHECK_FALSE(rec::RecordingEngine::canTransit(S::Failed, S::Paused));
    CHECK_FALSE(rec::RecordingEngine::canTransit(S::Recording, S::Completed));
}

// ---- SPSC ring --------------------------------------------------------

TEST_CASE("Rec: SPSC ring roundtrip preserves interleaved order", "[rec]") {
    rec::SpscRing ring(1024);
    Sig s = Sig::ramp(300);
    REQUIRE(ring.push(s.l.data(), s.r.data(), 300) == 300);
    std::vector<float> out(600);
    REQUIRE(ring.pop(out.data(), 300) == 300);
    for (int i = 0; i < 300; ++i) {
        CHECK(out[i * 2]     == Catch::Approx(s.l[i]));
        CHECK(out[i * 2 + 1] == Catch::Approx(s.r[i]));
    }
    CHECK(ring.fillPct() == 0);
}

TEST_CASE("Rec: SPSC ring overflow drops, never blocks", "[rec]") {
    rec::SpscRing ring(64);               // 63 usable frames
    Sig s(200, 0.5f);
    const size_t wrote = ring.push(s.l.data(), s.r.data(), 200);
    CHECK(wrote <= 63);
    CHECK(ring.fillPct() > 90);
    // consumer drains → producer can write again
    std::vector<float> out(128);
    ring.pop(out.data(), 128);
    CHECK(ring.push(s.l.data(), s.r.data(), 10) == 10);
}

// ---- RecorderRt -------------------------------------------------------

TEST_CASE("Rec: RecorderRt meters sine, tracks frames, idles disarmed",
          "[rec]") {
    rec::RecorderRt rt;
    Sig s = Sig::sine(2048, 0.5f);
    s.push(rt);                            // disarmed → nothing
    CHECK(rt.meter(48000.0).frames == 0);
    rt.arm(true);
    s.push(rt);
    const auto m = rt.meter(48000.0);
    CHECK(m.frames == 2048);
    CHECK(m.maxPeakL == Catch::Approx(0.5f).margin(0.01f));
    CHECK(m.rmsL == Catch::Approx(0.3535f).margin(0.01f)); // sine RMS
    CHECK(m.durationSec == Catch::Approx(2048.0 / 48000.0));
    CHECK(m.clipCount == 0);
    // silence records zero level
    rec::RecorderRt rt2;
    rt2.arm(true);
    Sig sil(1024, 0.0f); sil.push(rt2);
    CHECK(rt2.meter(48000.0).maxPeakL == 0.0f);
}

TEST_CASE("Rec: RecorderRt detects clips with event detail", "[rec]") {
    rec::RecorderRt rt;
    rt.arm(true);
    Sig hot(256, 1.05f);                   // over full-scale
    hot.push(rt);
    const auto m = rt.meter(48000.0);
    CHECK(m.clipCount > 0);
    CHECK(m.clipPeak >= 1.0f);
    rec::ClipEvent e;
    REQUIRE(rt.popClip(e));
    CHECK(e.peak >= 1.0f);
    CHECK(e.channel >= 0);
    CHECK_FALSE(rt.popClip(e));            // drained
}

// ---- WAV encoder -------------------------------------------------------

TEST_CASE("Rec: WAV encoder writes valid 16/24/32f headers + payload",
          "[rec]") {
    const auto dir = tempDirR("wav");
    for (auto depth : {rec::BitDepth::Pcm16, rec::BitDepth::Pcm24,
                       rec::BitDepth::F32}) {
        const auto path = (dir / ("t" + std::to_string(
                                 rec::bytesPerSample(depth)) + ".wav"))
                             .string();
        rec::WavEncoder e;
        REQUIRE(e.open(path, 48000, 2, depth));
        std::vector<float> fr = {0.25f, -0.25f, 0.5f, -0.5f, 1.0f, -1.0f};
        REQUIRE(e.writeFrames(fr.data(), 3));
        e.finalize();
        double rate = 0; int ch = 0, bits = 0; int64_t frames = 0;
        REQUIRE(rec::WavEncoder::validate(path, &rate, &ch, &frames,
                                          &bits));
        CHECK(rate == Catch::Approx(48000.0));
        CHECK(ch == 2);
        CHECK(bits == rec::bytesPerSample(depth) * 8);
        CHECK(frames == 3);
        CHECK(fs::file_size(path) == 44 + 3 * 2 * rec::bytesPerSample(depth));
    }
}

TEST_CASE("Rec: WAV payload decodes back to input within tolerance",
          "[rec]") {
    const auto dir = tempDirR("wavrt");
    const auto path = (dir / "rt.wav").string();
    rec::WavEncoder e;
    REQUIRE(e.open(path, 44100, 2, rec::BitDepth::Pcm24));
    Sig s = Sig::sine(1000, 0.8f);
    std::vector<float> inter(2000);
    for (int i = 0; i < 1000; ++i) { inter[i * 2] = s.l[i];
                                   inter[i * 2 + 1] = s.r[i]; }
    REQUIRE(e.writeFrames(inter.data(), 1000));
    e.finalize();
    // read raw 24-bit samples back
    FILE* f = std::fopen(path.c_str(), "rb");
    REQUIRE(f != nullptr);
    std::fseek(f, 44, SEEK_SET);
    std::vector<unsigned char> raw(6000);
    REQUIRE(std::fread(raw.data(), 1, 6000, f) == 6000);
    std::fclose(f);
    for (int i = 0; i < 2000; i += 61) {   // sample sparsely
        int32_t v = raw[i * 3] | (raw[i * 3 + 1] << 8)
                  | (raw[i * 3 + 2] << 16);
        if (v & 0x800000) v |= ~0xFFFFFF;  // sign-extend
        const float got = (float)v / 8388608.0f;
        CHECK(got == Catch::Approx(inter[i]).margin(1.0 / 8388608.0));
    }
}

// ---- journal + naming -------------------------------------------------

TEST_CASE("Rec: makeWavPath is safe, dated and collision-free", "[rec]") {
    const auto dir = tempDirR("names");
    const auto a = rec::makeWavPath(dir.string(), "Set/1:bad*?", 0);
    CHECK(a.find(dir.string()) == 0);
    const std::string fname = fs::path(a).filename().string();
    CHECK(fname.find(':') == std::string::npos);   // ':' scrubbed
    CHECK(fname.find('*') == std::string::npos);
    CHECK(fname.find('?') == std::string::npos);
    CHECK(fname.find("Set_1_bad__") == 0);         // stem sanitised
    CHECK((a.size() > 4 && a.substr(a.size() - 4) == ".wav"));
    // occupy the name → next call must not collide
    { FILE* f = std::fopen(a.c_str(), "wb"); std::fputc('x', f);
      std::fclose(f); }
    const auto b = rec::makeWavPath(dir.string(), "Set/1:bad*?", 0);
    CHECK(b != a);
}

TEST_CASE("Rec: diskFreeBytes reports positive space", "[rec]") {
    const auto dir = tempDirR("disk");
    CHECK(rec::diskFreeBytes(dir.string()) > 0);
    CHECK(rec::estimateBytes(48000, rec::BitDepth::Pcm24, 2, 60.0)
          == 48000LL * 2 * 3 * 60);
}

// ---- engine lifecycle --------------------------------------------------

TEST_CASE("Rec: arm→start→pause→resume→stop produces valid WAV",
          "[rec]") {
    const auto dir = tempDirR("life");
    rec::RecordingEngine e(nullptr);
    e.setPorts(fakePorts());
    e.setOutputDir(dir.string());
    REQUIRE(e.state() == rec::State::Idle);
    REQUIRE(e.arm());
    CHECK(e.state() == rec::State::Armed);
    REQUIRE(e.start());
    CHECK(e.state() == rec::State::Recording);
    Sig s = Sig::sine(48000, 0.4f);        // ~1 s of program
    s.push(e.rt());
    REQUIRE(e.pause());
    CHECK(e.state() == rec::State::Paused);
    Sig off(4800, 0.9f);                   // not captured while paused
    off.push(e.rt());
    REQUIRE(e.resume());
    s.push(e.rt());
    drain(e);
    REQUIRE(e.stop());
    CHECK(e.state() == rec::State::Completed);
    const auto sess = e.session();
    CHECK(sess.frames == 96000);           // paused audio excluded
    CHECK(sess.durationSec == Catch::Approx(2.0).margin(0.01));
    CHECK(sess.clipCount == 0);
    double rate; int ch; int64_t fr;
    REQUIRE(rec::WavEncoder::validate(sess.filePath, &rate, &ch, &fr));
    CHECK(fr == 96000);
    CHECK(fs::exists(sess.filePath + ".journal"));
    CHECK(fs::exists(sess.filePath + ".meta.json"));
}

TEST_CASE("Rec: invalid output dir fails safely", "[rec]") {
    rec::RecordingEngine e(nullptr);
    e.setPorts(fakePorts());
    e.setOutputDir("Z:\\nonexistent_onda_dir_12345\\x");
    CHECK_FALSE(e.start());
    CHECK(e.state() == rec::State::Failed);
}

TEST_CASE("Rec: split writes two valid files without losing frames",
          "[rec]") {
    const auto dir = tempDirR("split");
    rec::RecordingEngine e(nullptr);
    e.setPorts(fakePorts());
    e.setOutputDir(dir.string());
    REQUIRE(e.start());
    Sig s = Sig::sine(9600, 0.4f);
    s.push(e.rt());
    drain(e);
    REQUIRE(e.split());
    s.push(e.rt());
    drain(e);
    REQUIRE(e.stop());
    // two wavs in the dir, all frames accounted for
    int files = 0; int64_t totalFrames = 0;
    for (auto& ent : fs::directory_iterator(dir)) {
        if (ent.path().extension() != ".wav") continue;
        ++files;
        int64_t fr; REQUIRE(rec::WavEncoder::validate(
            ent.path().string(), nullptr, nullptr, &fr));
        totalFrames += fr;
    }
    CHECK(files == 2);
    CHECK(totalFrames == 19200);
}

// ---- commands ---------------------------------------------------------

TEST_CASE("Rec: CommandManager drives quick record end-to-end", "[rec]") {
    const auto dir = tempDirR("cmd");
    rec::RecordingEngine e(nullptr);
    e.setPorts(fakePorts());
    e.setOutputDir(dir.string());
    ui::CommandManager cm;
    ui::EnginePorts ports{};
    ports.recorder = &e;
    ui::registerEngineCommands(cm, ports);
    REQUIRE(cm.invoke("rec.toggle", ui::CommandSource::Ui));
    CHECK(e.state() == rec::State::Recording);
    Sig s = Sig::sine(2400, 0.3f); s.push(e.rt());
    drain(e);
    REQUIRE(cm.invoke("rec.toggle", ui::CommandSource::Keyboard));
    CHECK(e.state() == rec::State::Completed);
    // arm/pause/resume via commands too
    REQUIRE(cm.invoke("rec.arm", ui::CommandSource::Midi));
    CHECK(e.state() == rec::State::Armed);
    REQUIRE(cm.invoke("rec.start", ui::CommandSource::Midi));
    REQUIRE(cm.invoke("rec.pause", ui::CommandSource::Hid));
    CHECK(e.state() == rec::State::Paused);
    REQUIRE(cm.invoke("rec.resume", ui::CommandSource::Hid));
    REQUIRE(cm.invoke("rec.stop", ui::CommandSource::Ui));
    CHECK(e.state() == rec::State::Completed);
}

// ---- persistence + setlist + history ----------------------------------

TEST_CASE("Rec: repo stores session, files, events, setlist, history",
          "[rec]") {
    const auto dir = tempDirR("repo");
    Database db;
    REQUIRE(db.open(dbFile(dir).string()));
    rec::RecordingRepository repo(db);
    REQUIRE(repo.init());
    library::MusicLibraryEngine lib;
    REQUIRE(lib.open(dbFile(dir).string()));

    rec::RecordingEngine e(&repo);
    int64_t playedTrackId = -1, playedTs = 0; double playedSecs = 0;
    auto p = fakePorts();
    p.deckPlaying = [](int d) { return d == 0 ? 1 : 0; };  // deck A plays
    p.deckPath = [](int) { return std::string("/t/song.wav"); };
    p.trackMeta = [](int d, const std::string& path) {
        rec::TrackPlay t; t.deck = d; t.path = path;
        t.trackId = 42; t.title = "Song"; t.artist = "Artist";
        t.bpm = 128.0; t.key = "8A";
        return t;
    };
    p.recordPlay = [&](int64_t tid, int deck, int64_t ts, double sec) {
        playedTrackId = tid; playedTs = ts; playedSecs = sec;
        lib.history().recordPlay(tid, deck, ts, sec);
    };
    e.setPorts(std::move(p));
    e.setOutputDir(dir.string());
    REQUIRE(e.start());
    e.tick();                              // detect deck-A play edge
    Sig s = Sig::sine(4800, 0.5f); s.push(e.rt());
    drain(e);
    REQUIRE(e.stop());

    auto sessions = repo.sessions();
    REQUIRE(sessions.size() == 1);
    CHECK(sessions[0].state == rec::State::Completed);
    CHECK(sessions[0].frames == 4800);
    CHECK(sessions[0].peakLevel == Catch::Approx(0.5f).margin(0.02));
    auto tracks = repo.tracks(sessions[0].id);
    REQUIRE(tracks.size() == 1);
    CHECK(tracks[0].title == "Song");
    CHECK(tracks[0].bpm == Catch::Approx(128.0));
    CHECK(tracks[0].deck == 0);
    // HistoryManager link — recordPlay fired with the library track id
    CHECK(playedTrackId == 42);
    CHECK(playedSecs >= 0.0);
    CHECK(playedTs == tracks[0].ts);
    // manager export produces readable output
    rec::RecordingManager mgr(&repo);
    const auto js = mgr.exportSession(sessions[0], "json", "1.0-test");
    CHECK_FALSE(js.empty());
    CHECK(fs::exists(js));
    const auto csv = mgr.exportSession(sessions[0], "csv", "1.0");
    CHECK_FALSE(csv.empty());
    lib.close(); db.close();
}

// ---- recovery ----------------------------------------------------------

TEST_CASE("Rec: crash leaves journal → scan finds → recover validates",
          "[rec]") {
    const auto dir = tempDirR("crash");
    {
        rec::RecordingEngine e(nullptr);
        e.setPorts(fakePorts());
        e.setOutputDir(dir.string());
        REQUIRE(e.start());
        Sig s = Sig::sine(4800, 0.6f); s.push(e.rt());
        drain(e);
        e.simulateCrashForTest();          // abandon mid-session
    }
    // new engine instance scans the folder — journal lacks completion
    rec::RecordingEngine e2(nullptr);
    e2.setOutputDir(dir.string());
    auto items = e2.scanRecovery();
    REQUIRE(items.size() == 1);
    CHECK(items[0].events.size() > 0);     // inspect data available
    CHECK(items[0].dataBytes > 0);
    REQUIRE(e2.recover(items[0]));
    double rate; int ch; int64_t frames;
    REQUIRE(rec::WavEncoder::validate(items[0].wavPath, &rate, &ch,
                                      &frames));
    CHECK(rate == Catch::Approx(48000.0));
    CHECK(ch == 2);
    CHECK(frames > 0);
    // recovered item no longer shows as recoverable
    CHECK(e2.scanRecovery().empty());
    // discard path removes the artifacts (fresh crash)
    {
        rec::RecordingEngine e3(nullptr);
        e3.setPorts(fakePorts());
        e3.setOutputDir(dir.string());
        REQUIRE(e3.start());
        Sig t(1200, 0.3f); t.push(e3.rt());
        e3.tick();
        e3.simulateCrashForTest();
    }
    auto items2 = e2.scanRecovery();
    REQUIRE(items2.size() == 1);
    CHECK(e2.discardRecovery(items2[0]));
    CHECK_FALSE(fs::exists(items2[0].wavPath));
    CHECK(e2.scanRecovery().empty());
}

// ---- writer robustness --------------------------------------------------

TEST_CASE("Rec: writer drains ring fully on stop — no frame loss",
          "[rec]") {
    const auto dir = tempDirR("drainw");
    rec::RecordingEngine e(nullptr);
    e.setPorts(fakePorts());
    e.setOutputDir(dir.string());
    REQUIRE(e.start());
    // push ~2 s in one burst — ring (8 s) holds it while writer drains
    Sig s = Sig::sine(96000, 0.45f);
    s.push(e.rt(), 8192);
    REQUIRE(e.stop());
    int64_t fr;
    REQUIRE(rec::WavEncoder::validate(e.session().filePath, nullptr,
                                      nullptr, &fr));
    CHECK(fr == 96000);
    CHECK(e.meter().droppedFrames == 0);
}

// ---- audio-signal equivalence ------------------------------------------

TEST_CASE("Rec: recorded WAV reproduces the master program", "[rec]") {
    const auto dir = tempDirR("sig");
    rec::RecordingEngine e(nullptr);
    e.setPorts(fakePorts());
    e.setBitDepth(rec::BitDepth::Pcm24);
    e.setOutputDir(dir.string());
    REQUIRE(e.start());
    // mixed signal: sine on L, ramp-ish on R + a hot peak
    Sig s(8192);
    for (int i = 0; i < 8192; ++i) {
        s.l[i] = 0.6f * std::sin(2.0 * 3.14159265358979 * i / 128.0);
        s.r[i] = 0.4f * std::sin(2.0 * 3.14159265358979 * i / 96.0
                               + 0.7);
    }
    s.l[4000] = 1.0f; s.r[4000] = -1.0f;
    s.push(e.rt());
    drain(e);
    REQUIRE(e.stop());
    // read back and compare
    FILE* f = std::fopen(e.session().filePath.c_str(), "rb");
    REQUIRE(f != nullptr);
    std::fseek(f, 44, SEEK_SET);
    std::vector<unsigned char> raw(8192 * 6);
    REQUIRE(std::fread(raw.data(), 1, raw.size(), f) == raw.size());
    std::fclose(f);
    double err = 0;
    for (int i = 0; i < 8192; ++i) {
        for (int c = 0; c < 2; ++c) {
            const int o = (i * 2 + c) * 3;
            int32_t v = raw[o] | (raw[o + 1] << 8) | (raw[o + 2] << 16);
            if (v & 0x800000) v |= ~0xFFFFFF;
            const float got = (float)v / 8388608.0f;
            const float want = c ? s.r[i] : s.l[i];
            err = std::max(err, (double)std::fabs(got - want));
        }
    }
    CHECK(err < 1.5e-6);                    // 24-bit quantization bound
    const auto m = e.meter();
    CHECK(m.clipCount >= 1);                // the 1.0 peak trips
    CHECK(m.maxPeakL >= 1.0f);
}

// ---- stress: 4-hour-equivalent continuous recording ---------------------

TEST_CASE("Rec: 4h-equivalent stereo 24-bit stream stays bounded+valid",
          "[rec][.stress]") {
    const auto dir = tempDirR("stress");
    rec::RecordingEngine e(nullptr);
    e.setPorts(fakePorts());
    e.setBitDepth(rec::BitDepth::Pcm24);
    e.setOutputDir(dir.string());
    REQUIRE(e.start());
    // 4 h @48 kHz stereo = 691,200,000 frames ≈ 4.15 GB — exercises the
    // 3.8 GB auto-split as well as writer/ring/memory boundedness.
    const int64_t total = 48000LL * 4 * 3600;
    Sig blk(8192);
    for (int i = 0; i < 8192; ++i)
        blk.l[i] = blk.r[i] = 0.3f
            * std::sin(2.0 * 3.14159265358979 * i / 256.0);
    int64_t pushed = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (pushed < total) {
        // pace at the writer's drain rate — the invariant under test
        // is "writer throughput ≥ realtime need" (0.29 MB/s here);
        // the ring bridges bursts, not infinite producer speed
        while (e.rt().ring().fillPct() > 60)
            std::this_thread::sleep_for(
                std::chrono::microseconds(200));
        e.rt().pushPostMaster(blk.l.data(), blk.r.data(), 8192);
        pushed += 8192;
        if ((pushed & 0xFFFFFF) == 0) e.tick();   // periodic housekeeping
    }
    e.tick();
    REQUIRE(e.stop());
    const double secs =
        std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count();
    // integrity: every pushed frame landed across file parts
    int files = 0; int64_t frames = 0; int64_t bytes = 0;
    for (auto& ent : fs::directory_iterator(dir)) {
        if (ent.path().extension() != ".wav") continue;
        ++files; int64_t fr;
        REQUIRE(rec::WavEncoder::validate(ent.path().string(),
                                          nullptr, nullptr, &fr));
        frames += fr; bytes += (int64_t)fs::file_size(ent);
    }
    CHECK(frames == total);
    CHECK(bytes >= total * 6 + 44);         // 6 B/frame stereo 24-bit
    CHECK(files >= 2);                      // auto-split at ~3.8 GB
    const auto m = e.meter();
    CHECK(m.droppedFrames == 0);            // writer kept up
    CHECK(m.frames == total);
    INFO("stress: " << secs << " s wall, "
         << (double)bytes / 1e9 << " GB, parts=" << files);
    CHECK(secs < 300.0);                    // generous CI bound
}
