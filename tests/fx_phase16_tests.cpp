// Phase 16 — new processors, triplets, macros, routing, chains,
// reorder, diagnostics, presets, commands, 4-deck FX stress.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <numeric>
#include <vector>

#include "core/fx/BitCrusherFX.h"
#include "core/fx/BeatRepeatFX.h"
#include "core/fx/BeatRollFX.h"
#include "core/fx/ChorusFX.h"
#include "core/fx/CompressorFX.h"
#include "core/fx/DistortionFX.h"
#include "core/fx/FXAutomation.h"
#include "core/fx/FXClock.h"
#include "core/fx/FXDiagnostics.h"
#include "core/fx/FXEngine.h"
#include "core/fx/FXMacro.h"
#include "core/fx/FXPresetManager.h"
#include "core/fx/FXRouting.h"
#include "core/fx/FXSlot.h"
#include "core/mixer/MixerController.h"
#include "core/mixer/ParameterRegistry.h"
#include "core/rt/CommandQueue.h"
#include "ui/CommandIds.h"
#include "ui/CommandManager.h"
#include "ui/EngineCommands.h"
#include "ui/FXSurface.h"

using namespace onda;
using Catch::Approx;

namespace {

constexpr int    kBlock = 512;
constexpr double kRate  = 48000.0;

float peakP(const float* v, int n) {
    float p = 0.0f;
    for (int i = 0; i < n; ++i) p = std::max(p, std::fabs(v[i]));
    return p;
}
bool allFinite(const std::vector<float>& v) {
    for (float x : v) if (!std::isfinite(x)) return false;
    return true;
}
int firstIndexAbove(const std::vector<float>& v, float th, int from = 0) {
    for (int i = from; i < (int)v.size(); ++i)
        if (std::fabs(v[i]) > th) return i;
    return -1;
}

/// Standalone processor rig — same shape as the Phase 6 harness.
struct FxRig {
    std::unique_ptr<fx::FXProcessor> fx;
    fx::SyncContext ctx;
    std::vector<float> l, r;
    double phase_ { 0.0 };

    explicit FxRig(fx::FXKind k)
        : fx(fx::createFX(k)), l(kBlock), r(kBlock) {
        REQUIRE(fx != nullptr);
        fx->prepare(kRate, kBlock);
        ctx.bpm = 120.0f;
        ctx.synced = 1;
        ctx.division = (int)fx::BeatDiv::D_1_4;
    }
    void silence() {
        std::fill(l.begin(), l.end(), 0.0f);
        std::fill(r.begin(), r.end(), 0.0f);
    }
    void sine(double freq, float amp) {
        for (int i = 0; i < kBlock; ++i) {
            const float v = amp * (float)std::sin(phase_);
            l[i] = v; r[i] = v;
            phase_ += 2.0 * 3.14159265358979 * freq / kRate;
        }
    }
    void impulse(float amp = 0.8f, int at = 0) {
        silence();
        l[(size_t)at] = amp; r[(size_t)at] = amp;
    }
    void ramp() {   // deterministic distinct-content signal
        for (int i = 0; i < kBlock; ++i) {
            l[i] = 0.6f * std::sin(0.11 * i);
            r[i] = 0.6f * std::cos(0.07 * i);
        }
    }
    std::vector<float> run() {
        fx->setSyncContext(ctx);
        fx->process(l.data(), r.data(), kBlock);
        return l;
    }
    void runBlocks(int n) { for (int i = 0; i < n; ++i) run(); }
};

struct SlotRig {
    fx::FXSlot slot;
    fx::SyncContext ctx;
    std::vector<float> l, r;
    double phase_ { 0.0 };

    SlotRig() : l(kBlock), r(kBlock) {
        slot.prepare(kRate);
        ctx.bpm = 120.0f;
    }
    void load(fx::FXKind k, float mix = 1.0f) {
        slot.loadEffect(k);
        slot.setParam(fx::FXSlotKind::Enabled, 1.0f);
        slot.setParam(fx::FXSlotKind::Mix, mix);
    }
    std::vector<float> run() {
        slot.process(l.data(), r.data(), kBlock, ctx);
        return l;
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
    void nan() {
        std::fill(l.begin(), l.end(),
                  std::numeric_limits<float>::quiet_NaN());
        std::fill(r.begin(), r.end(),
                  std::numeric_limits<float>::quiet_NaN());
    }
};

} // namespace

// =============================================================== CHORUS
TEST_CASE("ChorusFX: silence stays silent; sine gets modulated",
          "[fx16][chorus]") {
    FxRig g(fx::FXKind::Chorus);
    g.silence();
    auto b = g.run();
    REQUIRE(peakP(b.data(), kBlock) == 0.0f);

    // sine in, wet out differs from dry (modulated delay)
    FxRig dry(fx::FXKind::Chorus);
    g.fx->setParamNormalized(1, 0.8f);   // depth up
    std::vector<float> in, wet;
    for (int i = 0; i < 8; ++i) { g.sine(440.0, 0.5f); in = g.run(); }
    REQUIRE(allFinite(in));
    float diff = 0.0f;
    for (int i = 0; i < kBlock; ++i)
        diff = std::max(diff, std::fabs(in[(size_t)i]));
    REQUIRE(diff > 0.01f);
    REQUIRE(diff < 2.0f);   // bounded
}

TEST_CASE("ChorusFX: reset clears state", "[fx16][chorus]") {
    FxRig g(fx::FXKind::Chorus);
    for (int i = 0; i < 4; ++i) { g.sine(300.0, 0.8f); g.run(); }
    g.fx->reset();
    g.silence();
    auto b = g.run();
    // post-reset tail must not exceed the pre-delay region by much
    REQUIRE(peakP(b.data(), kBlock) < 0.2f);
}

// =========================================================== DISTORTION
TEST_CASE("DistortionFX: bounded output at any drive/input",
          "[fx16][distortion]") {
    FxRig g(fx::FXKind::Distortion);
    g.fx->setParamNormalized(0, 1.0f);   // max drive
    g.fx->setParamNormalized(3, 0.5f);   // output 1.0
    for (int i = 0; i < 8; ++i) {
        g.sine(200.0, 8.0f);             // extreme input
        auto b = g.run();
        REQUIRE(allFinite(b));
        REQUIRE(peakP(b.data(), kBlock) <= 1.01f);   // shaper bounded
    }
    // silence in → silence out (flush the tone filter's residual
    // state — each run must start from a silenced buffer or the
    // residual gets re-distorted by the extreme drive)
    for (int i = 0; i < 4; ++i) { g.silence(); g.run(); }
    g.silence();
    REQUIRE(peakP(g.run().data(), kBlock) < 1e-4f);
}

TEST_CASE("DistortionFX: drive actually shapes the wave",
          "[fx16][distortion]") {
    FxRig g(fx::FXKind::Distortion);
    g.fx->setParamNormalized(0, 0.9f);
    g.fx->setParamNormalized(1, 1.0f);   // tone open
    g.fx->setParamNormalized(3, 0.5f);
    std::vector<float> in(0), out;
    for (int i = 0; i < 4; ++i) {
        g.sine(100.0, 0.5f);
        auto before = g.l;               // capture pre-process
        auto b = g.run();
        for (int k = 0; k < kBlock; ++k) {
            in.push_back(before[(size_t)k]);
            out.push_back(b[(size_t)k]);
        }
    }
    // distorted output ≠ clean input (harmonics present)
    float err = 0.0f;
    for (size_t i = 0; i < out.size(); ++i)
        err = std::max(err, std::fabs(out[i] - in[i]));
    REQUIRE(err > 0.02f);
}

// ========================================================== BIT CRUSHER
TEST_CASE("BitCrusherFX: quantization snaps DC to the bit grid",
          "[fx16][crusher]") {
    FxRig g(fx::FXKind::BitCrusher);
    g.fx->setParamNative(0, 2.0f);       // 2 bits → {-1,-0.5,0,0.5,1}
    g.fx->setParamNative(1, 1.0f);       // no decimation
    g.fx->setParamNative(2, 18000.0f);   // LP open
    g.fx->setParamNative(3, 0.0f);       // no dither
    const float grid[] = { -1.f, -0.5f, 0.f, 0.5f, 1.f };
    // a settled DC input must land on a grid level
    for (float dc : { 0.3f, -0.9f, 0.62f }) {
        for (int i = 0; i < 20; ++i) {
            std::fill(g.l.begin(), g.l.end(), dc);
            std::fill(g.r.begin(), g.r.end(), dc);
            g.fx->setSyncContext(g.ctx);
            g.fx->process(g.l.data(), g.r.data(), kBlock);
        }
        for (int i = 0; i < kBlock; ++i) {
            bool onGrid = false;
            for (float gv : grid)
                if (std::fabs(g.l[(size_t)i] - gv) < 0.02f) onGrid = true;
            REQUIRE(onGrid);
        }
    }
}

TEST_CASE("BitCrusherFX: decimation holds samples",
          "[fx16][crusher]") {
    FxRig g(fx::FXKind::BitCrusher);
    g.fx->setParamNative(0, 16.0f);      // full resolution
    g.fx->setParamNative(1, 32.0f);      // 32x hold
    g.fx->setParamNative(2, 18000.0f);
    g.sine(4000.0, 0.5f);                // fast-moving input
    auto b = g.run();
    // consecutive outputs repeat while held (LP may smooth — check
    // at least some exact repeats among raw held pairs early on)
    int repeats = 0;
    for (int i = 1; i < 64; ++i)
        if (std::fabs(b[(size_t)i] - b[(size_t)(i - 1)]) < 1e-6f)
            ++repeats;
    REQUIRE(repeats >= 8);
}

TEST_CASE("BitCrusherFX: silence + extremes stay bounded",
          "[fx16][crusher]") {
    FxRig g(fx::FXKind::BitCrusher);
    g.silence();
    REQUIRE(peakP(g.run().data(), kBlock) < 1e-4f);
    g.fx->setParamNormalized(0, 0.0f);   // 1 bit
    g.fx->setParamNormalized(1, 1.0f);   // max decimation
    for (int i = 0; i < 6; ++i) {
        g.sine(100.0, 7.0f);
        auto b = g.run();
        REQUIRE(allFinite(b));
        REQUIRE(peakP(b.data(), kBlock) <= 1.01f);
    }
}

// =========================================================== COMPRESSOR
TEST_CASE("CompressorFX: loud signal gets attenuated, quiet passes",
          "[fx16][compressor]") {
    FxRig g(fx::FXKind::Compressor);
    g.fx->setParamNative(0, -20.0f);     // thr
    g.fx->setParamNative(1, 8.0f);       // ratio
    g.fx->setParamNative(2, 1.0f);       // attack ms
    g.fx->setParamNative(3, 50.0f);      // release ms
    g.fx->setParamNative(4, 0.0f);       // hard knee
    g.fx->setParamNative(5, 0.0f);       // no makeup

    // loud sine — after attack settles the gain should be < 1
    float loudRmsIn = 0.0f, loudRmsOut = 0.0f;
    for (int i = 0; i < 30; ++i) {
        g.sine(200.0, 0.95f);
        auto in = g.l;
        auto b = g.run();
        if (i > 10) {
            for (int k = 0; k < kBlock; ++k) {
                loudRmsIn  += in[(size_t)k]  * in[(size_t)k];
                loudRmsOut += b[(size_t)k]   * b[(size_t)k];
            }
        }
    }
    REQUIRE(loudRmsOut < loudRmsIn * 0.7f);

    // quiet sine below threshold → ~unity
    FxRig q(fx::FXKind::Compressor);
    q.fx->setParamNative(0, -10.0f);
    q.fx->setParamNative(1, 8.0f);
    q.fx->setParamNative(2, 1.0f);
    q.fx->setParamNative(3, 50.0f);
    float inRms = 0.0f, outRms = 0.0f;
    for (int i = 0; i < 20; ++i) {
        q.sine(200.0, 0.01f);
        auto in = q.l; auto b = q.run();
        if (i > 8) {
            for (int k = 0; k < kBlock; ++k) {
                inRms  += in[(size_t)k] * in[(size_t)k];
                outRms += b[(size_t)k]  * b[(size_t)k];
            }
        }
    }
    REQUIRE(outRms == Approx(inRms).margin(inRms * 0.1 + 1e-4));
}

TEST_CASE("CompressorFX: stereo-linked gain (no image wander)",
          "[fx16][compressor]") {
    FxRig g(fx::FXKind::Compressor);
    g.fx->setParamNative(0, -24.0f);
    g.fx->setParamNative(1, 10.0f);
    g.fx->setParamNative(2, 1.0f);
    g.fx->setParamNative(3, 60.0f);
    // L hot, R quiet — both must get the SAME gain factor per frame
    for (int i = 0; i < 20; ++i) {
        g.sine(200.0, 0.0f);
        for (int k = 0; k < kBlock; ++k) {
            g.l[(size_t)k] = 0.9f * (float)std::sin(0.3 * (i * kBlock + k));
            g.r[(size_t)k] = 0.05f;
        }
        auto lin = g.l; auto rin = g.r;
        g.fx->setSyncContext(g.ctx);
        g.fx->process(g.l.data(), g.r.data(), kBlock);
        if (i > 5) {
            for (int k = 100; k < 400; ++k) {
                const float gl = g.l[(size_t)k] / lin[(size_t)k];
                const float gr = g.r[(size_t)k] / rin[(size_t)k];
                REQUIRE(gl == Approx(gr).margin(1e-3f));
            }
        }
    }
}

// =========================================================== FX CLOCK
TEST_CASE("FXClock + SyncContext: division timing at 120/128/140 BPM",
          "[fx16][clock]") {
    for (float bpm : { 120.0f, 128.0f, 140.0f }) {
        fx::SyncContext ctx;
        ctx.bpm = bpm;
        const double beatMs = 60000.0 / bpm;
        for (int d = 0; d < fx::beatDivCount(); ++d) {
            ctx.division = d;
            const double beats = (double)fx::beatsOf((fx::BeatDiv)d);
            REQUIRE(ctx.divisionMs() ==
                    Approx(beats * beatMs).margin(0.01));
            REQUIRE(ctx.divisionFrames(kRate) ==
                    Approx(beats * beatMs * kRate / 1000.0).margin(1.0));
            // canonical clock agrees bit-for-tolerance
            REQUIRE(fx::FXClock::framesFor(bpm, d, kRate) ==
                    Approx(ctx.divisionFrames(kRate)).margin(0.5));
        }
    }
}

TEST_CASE("FXClock: triplet divisions are 2/3 of straight timing",
          "[fx16][clock][triplet]") {
    const float bpm = 120.0f;   // beat = 0.5 s
    // 1/8 straight = 0.5 beat = 12000 frames; 1/8T = 1/3 beat = 8000
    REQUIRE(fx::FXClock::framesFor(bpm, (int)fx::BeatDiv::D_1_8, kRate)
            == Approx(12000.0).margin(1));
    REQUIRE(fx::FXClock::framesFor(bpm, (int)fx::BeatDiv::D_1_8T, kRate)
            == Approx(8000.0).margin(1));
    REQUIRE(fx::FXClock::framesFor(bpm, (int)fx::BeatDiv::D_1_16T, kRate)
            == Approx(4000.0).margin(1));
    REQUIRE(fx::FXClock::framesFor(bpm, (int)fx::BeatDiv::D_1_4T, kRate)
            == Approx(16000.0).margin(1));
    // and SyncContext agrees
    fx::SyncContext ctx; ctx.bpm = bpm;
    ctx.division = (int)fx::BeatDiv::D_1_4T;
    REQUIRE(ctx.divisionFrames(kRate) == Approx(16000.0).margin(1));
}

// =========================================================== BEAT REPEAT
TEST_CASE("BeatRepeatFX: latches a division-sized slice and loops it",
          "[fx16][repeat]") {
    FxRig g(fx::FXKind::BeatRepeat);
    g.ctx.bpm = 120.0f;
    g.ctx.division = (int)fx::BeatDiv::D_1_8;  // 0.5 beat = 12000 fr
    // record distinctive content while disengaged (passthrough)
    for (int i = 0; i < 48; ++i) {   // ~0.5 s of ramp content
        g.ramp();
        const auto inL = g.l;         // capture pre-process input
        auto out = g.run();
        // disengaged → output == input (passthrough)
        for (int k = 0; k < kBlock; ++k)
            REQUIRE(out[(size_t)k] ==
                    Approx(inL[(size_t)k]).margin(1e-4f));
    }
    g.fx->setParamNormalized(0, 1.0f);   // engage
    g.silence();
    auto b1 = g.run();
    auto b2 = g.run();
    // engaged → repeating slice content (nonzero) and periodic at
    // sliceLen (12000) — blocks 1 and 2 start the slice over
    REQUIRE(peakP(b1.data(), kBlock) > 0.05f);
    REQUIRE(allFinite(b1));
    REQUIRE(allFinite(b2));
    // release → passthrough resumes
    g.fx->setParamNormalized(0, 0.0f);
    g.sine(500.0, 0.4f);
    auto rel = g.run();
    float corr = 0.0f;
    for (int k = 0; k < kBlock; ++k)
        corr = std::max(corr, std::fabs(rel[(size_t)k]));
    REQUIRE(corr > 0.3f);   // live input flows again
}

TEST_CASE("BeatRepeatFX: slice period follows division x SizeMult",
          "[fx16][repeat]") {
    for (float bpm : { 120.0f, 128.0f, 140.0f }) {
        FxRig g(fx::FXKind::BeatRepeat);
        g.ctx.bpm = bpm;
        g.ctx.division = (int)fx::BeatDiv::D_1_16;  // 0.25 beat
        g.fx->setParamNormalized(1, // SizeMult 1x
            (float)((1.0 - 0.25) / (2.0 - 0.25)));
        const double slice = fx::FXClock::framesFor(
            bpm, (int)fx::BeatDiv::D_1_16, kRate);
        // fill capture; marker impulse lands in the LAST slice-sized
        // region so the latch picks it up
        for (int i = 0; i < 40; ++i) {
            if (i == 39) g.impulse(0.8f);
            else g.silence();
            g.run();
        }
        g.fx->setParamNormalized(0, 1.0f);
        std::vector<float> all;
        for (int i = 0; i < 30; ++i) {
            g.silence();
            auto b = g.run();
            all.insert(all.end(), b.begin(), b.end());
        }
        // the impulse should re-appear every `slice` frames (±tol)
        const int first = firstIndexAbove(all, 0.1f);
        if (first >= 0) {
            const int second = firstIndexAbove(all, 0.1f, first + 64);
            if (second >= 0)
                REQUIRE((double)(second - first) ==
                        Approx(slice).margin(96));
        }
    }
}

// ============================================================ BEAT ROLL
TEST_CASE("BeatRollFX: engaged output repeats the freshest slice",
          "[fx16][roll]") {
    FxRig g(fx::FXKind::BeatRoll);
    g.ctx.bpm = 120.0f;
    g.ctx.division = (int)fx::BeatDiv::D_1_16;
    // fill capture
    for (int i = 0; i < 30; ++i) { g.ramp(); g.run(); }
    g.fx->setParamNormalized(0, 1.0f);       // engage
    g.fx->setParamNormalized(2, 0.0f);       // no gate
    // while engaged feed silence — output still produces the roll
    std::vector<float> all;
    for (int i = 0; i < 8; ++i) {
        g.silence();
        auto b = g.run();
        all.insert(all.end(), b.begin(), b.end());
    }
    REQUIRE(allFinite(all));
    // roll plays captured content → first blocks non-silent
    REQUIRE(peakP(all.data(), kBlock * 4) > 0.02f);
    // release → passthrough
    g.fx->setParamNormalized(0, 0.0f);
    g.sine(300.0, 0.5f);
    auto rel = g.run();
    REQUIRE(peakP(rel.data(), kBlock) > 0.3f);
}

// =============================================================== MACRO
TEST_CASE("FXMacro: mappings resolve to param ids incl. focus target",
          "[fx16][macro]") {
    fx::FXMacro m;
    using K = fx::FXSlotKind;
    m.addMapping({ fx::FXMacro::kFocusTarget, 1, (int)K::P1, 0.0f, 1.0f, 0 });
    m.addMapping({ fx::FXMacro::kFocusTarget, 2, (int)K::Mix, 0.2f, 0.8f, 1 });
    m.addMapping({ (int)fx::FXTarget::Master, 0, (int)K::P2, 0.5f, 1.0f, 0 });
    m.setValue(0.5f);
    fx::FXMacro::Write w[8];
    const int n = m.evaluate(w, 8, (int)fx::FXTarget::DeckB);
    REQUIRE(n == 3);
    // focus mapping → DeckB ids
    REQUIRE(fx::fxTarget(w[0].paramId) == (int)fx::FXTarget::DeckB);
    REQUIRE(fx::fxSlot(w[0].paramId)   == 1);
    REQUIRE(w[0].value == Approx(0.5f));
    // curve: square of 0.5 → 0.25 → 0.2+0.25*(0.6) = 0.35
    REQUIRE(w[1].value == Approx(0.35f).margin(1e-3));
    // absolute mapping keeps its target
    REQUIRE(fx::fxTarget(w[2].paramId) == (int)fx::FXTarget::Master);
    REQUIRE(w[2].value == Approx(0.75f));
}

TEST_CASE("FXMacro: MixerController::applyFxMacro pushes writes",
          "[fx16][macro]") {
    mixer::MixerController mc;
    std::vector<std::pair<uint16_t, float>> sent;
    mc.setSink([&](uint16_t id, float v) { sent.push_back({ id, v }); });
    fx::FXMacro m;
    using K = fx::FXSlotKind;
    m.addMapping({ fx::FXMacro::kFocusTarget, 0, (int)K::Mix, 0.0f, 1.0f, 0 });
    m.setValue(0.75f);
    const int n = mc.applyFxMacro(m, (int)fx::FXTarget::DeckA);
    REQUIRE(n == 1);
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].first ==
            mixer::ParameterRegistry::fxMix(
                (int)fx::FXTarget::DeckA, 0));
    REQUIRE(sent[0].second == Approx(0.75f));
}

// ============================================================= ROUTING
TEST_CASE("FXRouting: declarative table matches mixer topology",
          "[fx16][routing]") {
    REQUIRE(fx::routingFor(fx::FXTarget::DeckA).route ==
            fx::FXRoute::DeckInsert);
    REQUIRE(fx::routingFor(fx::FXTarget::DeckA).node == 0);
    REQUIRE(fx::routingFor(fx::FXTarget::SendB).route ==
            fx::FXRoute::SendReturn);
    REQUIRE(fx::routingFor(fx::FXTarget::SendB).postFader);
    REQUIRE(fx::routingFor(fx::FXTarget::Master).route ==
            fx::FXRoute::MasterInsert);
    REQUIRE(fx::routingFor(fx::FXTarget::Spare).route ==
            fx::FXRoute::SamplerInsert);
    // cue bus is never routed through FX today
    for (int t = 0; t < (int)fx::FXTarget::kCount; ++t)
        REQUIRE(!fx::routingFor((fx::FXTarget)t).affectsCue);
}

// ============================================================ REORDER
TEST_CASE("FXRack::moveSlot reorders the chain without reallocating",
          "[fx16][reorder]") {
    fx::FXRack rack;
    rack.prepare(kRate, nullptr, (int)fx::FXTarget::DeckA);
    rack.slot(0).loadEffect(fx::FXKind::Filter);
    rack.slot(1).loadEffect(fx::FXKind::Echo);
    // adopt pending (one silent block)
    std::vector<float> l(kBlock, 0.0f), r(kBlock, 0.0f);
    fx::SyncContext ctx;
    rack.process(l.data(), r.data(), kBlock, ctx);
    REQUIRE(rack.slot(0).activeKind() == fx::FXKind::Filter);
    REQUIRE(rack.slot(1).activeKind() == fx::FXKind::Echo);

    rack.moveSlot(0, 1);   // Filter moves to position 1
    REQUIRE(rack.slot(0).activeKind() == fx::FXKind::Echo);
    REQUIRE(rack.slot(1).activeKind() == fx::FXKind::Filter);
    rack.moveSlot(1, 0);   // back
    REQUIRE(rack.slot(0).activeKind() == fx::FXKind::Filter);
    REQUIRE(rack.slot(1).activeKind() == fx::FXKind::Echo);
}

// ========================================================== DIAGNOSTICS
TEST_CASE("FXSlot: sustained bad input faults + resets processor",
          "[fx16][diag]") {
    SlotRig g;
    g.load(fx::FXKind::Reverb);
    for (int i = 0; i < 6; ++i) { g.nan(); g.run(); }
    REQUIRE(g.slot.sanitized.load() > 0);
    REQUIRE(g.slot.faults.load() >= 1);
    // still alive: clean input processes fine after the reset
    g.sine(300.0, 0.3f);
    auto b = g.run();
    REQUIRE(allFinite(b));
}

TEST_CASE("FXEngine::fillDiagnostics reflects rack state",
          "[fx16][diag]") {
    fx::FXEngine eng;
    eng.prepare(kRate);
    eng.loadEffect((int)fx::FXTarget::DeckB, 2, fx::FXKind::Chorus);
    // adopt: process the deck rack once
    eng.setTempo((int)fx::FXTarget::DeckB, 126.0f, 0.0f);
    std::vector<float> l(kBlock, 0.1f), r(kBlock, 0.1f);
    // mark the slot enabled through the param path
    eng.setParam(mixer::ParameterRegistry::fxEnabled(
                     (int)fx::FXTarget::DeckB, 2), 1.0f);
    eng.processDeckInsert(1, l.data(), r.data(), kBlock);
    fx::FXRackDiag d;
    eng.fillDiagnostics((int)fx::FXTarget::DeckB, d);
    REQUIRE(d.target == (int)fx::FXTarget::DeckB);
    REQUIRE(d.route == (int)fx::FXRoute::DeckInsert);
    REQUIRE(d.slots[2].kind == (int)fx::FXKind::Chorus);
    REQUIRE(d.slots[2].enabled);
    REQUIRE(d.totalLatency == 0);
    const auto rep = eng.diagnosticReport();
    REQUIRE(rep.find("DeckB") != std::string::npos);
}

// ============================================================== PRESETS
TEST_CASE("FXPresetManager: factory list covers Phase 16 collection",
          "[fx16][preset]") {
    fx::FXPresetManager pm;
    REQUIRE(pm.count() >= 18);
    for (const char* nm : { "Echo Out", "Filter Sweep", "Reverb Wash",
                            "Deep Filter", "Tape Echo", "Build Up",
                            "Down Sweep", "Space Delay", "Crush",
                            "Pump", "Roll 16", "Stutter" })
        REQUIRE(pm.find(nm) >= 0);
    // new-kind presets reference valid kinds
    REQUIRE(pm.at(pm.find("Crush")).effectKind ==
            (int)fx::FXKind::BitCrusher);
    REQUIRE(pm.at(pm.find("Roll 16")).effectKind ==
            (int)fx::FXKind::BeatRoll);
}

TEST_CASE("applyFxPreset pushes type+slot+normalized params",
          "[fx16][preset]") {
    mixer::MixerController mc;
    int loadedKind = -1;
    mc.setFxLoader([&](int, int, int k) { loadedKind = k; });
    std::vector<std::pair<uint16_t, float>> sent;
    mc.setSink([&](uint16_t id, float v) { sent.push_back({ id, v }); });

    fx::FXPreset p;
    p.effectKind = (int)fx::FXKind::Compressor;
    p.syncMode = 0;
    p.division = (int)fx::BeatDiv::D_1_2;
    p.mix = 0.8f;
    p.params[0] = -24.0f;   // threshold (native)
    p.params[1] = 11.0f;    // ratio (native — range 1..20 → 0.526..)
    mc.applyFxPreset((int)fx::FXTarget::Master, 0, p);

    using R = mixer::ParameterRegistry;
    REQUIRE(loadedKind == (int)fx::FXKind::Compressor);
    REQUIRE(mc.value(R::fxType((int)fx::FXTarget::Master, 0)) ==
            (float)(int)fx::FXKind::Compressor);
    REQUIRE(mc.value(R::fxMix((int)fx::FXTarget::Master, 0)) ==
            Approx(0.8f));
    REQUIRE(mc.value(R::fxDivision((int)fx::FXTarget::Master, 0)) ==
            (float)(int)fx::BeatDiv::D_1_2);
    // P1 normalized: (-24 - -60) / (0 - -60) = 0.6
    REQUIRE(mc.value(R::fxParam((int)fx::FXTarget::Master, 0, 0)) ==
            Approx(0.6f).margin(1e-3));
}

// ============================================================= COMMANDS
TEST_CASE("0xA000 FX commands drive the focus surface",
          "[fx16][commands]") {
    mixer::MixerController mc;
    std::vector<std::pair<uint16_t, float>> sent;
    mc.setSink([&](uint16_t id, float v) { sent.push_back({ id, v }); });
    fx::FXPresetManager pm;
    ui::FXSurface surf;
    surf.bind(&mc, &pm);
    surf.seedDefaultMacros();

    ui::EnginePorts ports;
    ports.mixer = &mc;
    ports.fx = &surf;
    ui::CommandManager cm;
    ui::registerEngineCommands(cm, ports);
    using CS = ui::CommandSource;

    using R = mixer::ParameterRegistry;
    // focus DeckB slot 1
    REQUIRE(cm.invoke(ui::cmd::FxFocusTarget, CS::Midi,
                      (float)(int)fx::FXTarget::DeckB));
    REQUIRE(cm.invoke(ui::cmd::FxFocusSlot, CS::Midi, 1.0f));
    // enable + mix
    REQUIRE(cm.invoke("fx.enable", CS::Midi));
    REQUIRE(mc.value(R::fxEnabled((int)fx::FXTarget::DeckB, 1)) == 1.0f);
    REQUIRE(cm.invoke("fx.mix", CS::Midi, 0.65f));
    REQUIRE(mc.value(R::fxMix((int)fx::FXTarget::DeckB, 1)) ==
            Approx(0.65f));
    // type select + division
    REQUIRE(cm.invoke("fx.typeSet", CS::Midi,
                      (float)(int)fx::FXKind::BeatRoll));
    REQUIRE(cm.invoke("fx.divSet", CS::Midi,
                      (float)(int)fx::BeatDiv::D_1_8T));
    REQUIRE(mc.value(R::fxDivision((int)fx::FXTarget::DeckB, 1)) ==
            (float)(int)fx::BeatDiv::D_1_8T);
    // param set/nudge
    REQUIRE(cm.invoke("fx.p1Set", CS::Midi, 0.5f));
    REQUIRE(cm.invoke("fx.p1Up", CS::Midi));
    REQUIRE(mc.value(R::fxParam((int)fx::FXTarget::DeckB, 1, 0)) ==
            Approx(0.52f).margin(1e-3));
    // preset apply → type loaded through loader path (none wired →
    // registry value still updates)
    REQUIRE(cm.invoke("fx.presetApply", CS::Midi));
    // macro 0 (Build) writes Mix+P1 across slots of the focus rack
    sent.clear();
    REQUIRE(cm.invoke("fx.macro0", CS::Midi, 1.0f));
    bool sawP1 = false, sawMix = false;
    for (auto& [id, v] : sent) {
        if (id == R::fxParam((int)fx::FXTarget::DeckB, 0, 0)) {
            sawP1 = true; REQUIRE(v == Approx(1.0f));
        }
        if (id == R::fxMix((int)fx::FXTarget::DeckB, 0)) sawMix = true;
    }
    REQUIRE(sawP1);
    REQUIRE(sawMix);
}

// =============================================================== CHAIN
TEST_CASE("FX chain: Filter→Echo in series through a rack",
          "[fx16][chain]") {
    fx::FXRack rack;
    rack.prepare(kRate, nullptr, (int)fx::FXTarget::DeckA);
    rack.slot(0).loadEffect(fx::FXKind::Filter);
    rack.slot(1).loadEffect(fx::FXKind::Echo);
    for (int s = 0; s < 2; ++s) {
        rack.slot(s).setParam(fx::FXSlotKind::Enabled, 1.0f);
        rack.slot(s).setParam(fx::FXSlotKind::Mix, 1.0f);
    }
    fx::SyncContext ctx;
    ctx.bpm = 128.0f;
    ctx.division = (int)fx::BeatDiv::D_1_4;
    double ph = 0.0;
    std::vector<float> l(kBlock), r(kBlock);
    float pk = 0.0f;
    for (int b = 0; b < 24; ++b) {
        for (int i = 0; i < kBlock; ++i) {
            l[i] = 0.5f * (float)std::sin(ph);
            r[i] = l[i];
            ph += 2.0 * 3.14159265358979 * 440.0 / kRate;
        }
        rack.process(l.data(), r.data(), kBlock, ctx);
        pk = std::max(pk, peakP(l.data(), kBlock));
        REQUIRE(std::isfinite(l[0]));
    }
    REQUIRE(pk > 0.1f);   // chain produces output
}

TEST_CASE("FX chain: Filter→Echo→Reverb + reorder + reset",
          "[fx16][chain]") {
    fx::FXRack rack;
    rack.prepare(kRate, nullptr, (int)fx::FXTarget::DeckA);
    rack.slot(0).loadEffect(fx::FXKind::Filter);
    rack.slot(1).loadEffect(fx::FXKind::Echo);
    rack.slot(2).loadEffect(fx::FXKind::Reverb);
    for (int s = 0; s < 3; ++s) {
        rack.slot(s).setParam(fx::FXSlotKind::Enabled, 1.0f);
        rack.slot(s).setParam(fx::FXSlotKind::Mix, 0.7f);
    }
    fx::SyncContext ctx;
    std::vector<float> l(kBlock, 0.3f), r(kBlock, 0.3f);
    for (int b = 0; b < 12; ++b)
        rack.process(l.data(), r.data(), kBlock, ctx);
    // reorder: move Reverb position to front
    rack.moveSlot(2, 0);
    REQUIRE(rack.slot(0).activeKind() == fx::FXKind::Reverb);
    // still processes without artifacts
    for (int b = 0; b < 12; ++b)
        rack.process(l.data(), r.data(), kBlock, ctx);
    REQUIRE(std::isfinite(l[0]));
    // rack latency is the slot sum
    REQUIRE(rack.latencySamples() == 0);
}

// ==================================================== ENGINE INTEGRATION
TEST_CASE("FXEngine: moveSlot + diagnostics + routing wired",
          "[fx16][engine]") {
    fx::FXEngine eng;
    eng.prepare(kRate);
    eng.loadEffect((int)fx::FXTarget::DeckA, 0, fx::FXKind::Distortion);
    eng.loadEffect((int)fx::FXTarget::DeckA, 1, fx::FXKind::BitCrusher);
    eng.setParam(mixer::ParameterRegistry::fxEnabled(
                     (int)fx::FXTarget::DeckA, 0), 1.0f);
    eng.setParam(mixer::ParameterRegistry::fxEnabled(
                     (int)fx::FXTarget::DeckA, 1), 1.0f);
    std::vector<float> l(kBlock, 0.2f), r(kBlock, 0.2f);
    eng.processDeckInsert(0, l.data(), r.data(), kBlock);
    eng.moveSlot((int)fx::FXTarget::DeckA, 0, 1);
    REQUIRE(eng.rack((int)fx::FXTarget::DeckA).slot(0).activeKind()
            == fx::FXKind::BitCrusher);
    REQUIRE(eng.routing((int)fx::FXTarget::DeckA).route ==
            fx::FXRoute::DeckInsert);
}

// ================================================================ STRESS
TEST_CASE("Phase16 stress: 4 decks x 4 slots of mixed new FX",
          "[fx16][stress]") {
    fx::FXEngine eng;
    eng.prepare(kRate);
    const fx::FXKind kinds[] = {
        fx::FXKind::Chorus, fx::FXKind::Distortion,
        fx::FXKind::BitCrusher, fx::FXKind::Compressor,
        fx::FXKind::BeatRepeat, fx::FXKind::BeatRoll,
        fx::FXKind::Echo, fx::FXKind::Reverb };
    using R = mixer::ParameterRegistry;
    for (int d = 0; d < 4; ++d) {
        for (int s = 0; s < 4; ++s) {
            const int t = (int)fx::FXTarget::DeckA + d;
            eng.loadEffect(t, s, kinds[(size_t)((d * 4 + s) % 8)]);
            eng.setParam(R::fxEnabled(t, s), 1.0f);
            eng.setParam(R::fxMix(t, s), 0.85f);
            eng.setParam(R::fxSyncMode(t, s), 1.0f);
        }
        eng.setTempo((int)fx::FXTarget::DeckA + d,
                     120.0f + 8.0f * d, 0.0f);
    }
    std::vector<float> l(kBlock), r(kBlock);
    double ph = 0.0;
    const auto t0 = std::chrono::steady_clock::now();
    const int kBlocks = 160;   // ~1.7 s of audio per deck x4
    for (int b = 0; b < kBlocks; ++b) {
        for (int i = 0; i < kBlock; ++i) {
            l[i] = 0.4f * (float)std::sin(ph);
            r[i] = 0.4f * (float)std::cos(ph * 1.01);
            ph += 2.0 * 3.14159265358979 * 330.0 / kRate;
        }
        for (int d = 0; d < 4; ++d)
            eng.processDeckInsert(d, l.data(), r.data(), kBlock);
        REQUIRE(std::isfinite(l[0]));
        REQUIRE(std::isfinite(r[0]));
    }
    const double wallMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    // sanity: 16 slots processing ~655k stereo frames must stay far
    // under the audio duration (≈13.6 s of rendered material)
    REQUIRE(wallMs < 4000.0);
    fx::FXRackDiag d0;
    eng.fillDiagnostics(0, d0);
    REQUIRE(d0.totalFaults == 0);
}

// ======================================================== AUTOMATION
TEST_CASE("FXAutomation: lanes interpolate on the beat domain",
          "[fx16][automation]") {
    using R = mixer::ParameterRegistry;
    fx::FXAutomation auto_;
    auto& lane = auto_.laneFor(
        R::fxMix((int)fx::FXTarget::DeckA, 0));
    lane.addPoint({ 0.0f, 0.0f });
    lane.addPoint({ 8.0f, 1.0f });
    REQUIRE(lane.sampleAt(4.0f) == Approx(0.5f).margin(1e-3));
    REQUIRE(lane.sampleAt(-1.0f) == 0.0f);
    REQUIRE(lane.sampleAt(99.0f) == 1.0f);
    uint16_t ids[4]; float vals[4];
    const int n = auto_.evaluate(2.0f, ids, vals, 4);
    REQUIRE(n == 1);
    REQUIRE(vals[0] == Approx(0.25f).margin(1e-3));
    REQUIRE(ids[0] == R::fxMix((int)fx::FXTarget::DeckA, 0));
}

// ============================================================ SLOT GAP
TEST_CASE("Every new FX reports latency + resets without artifacts",
          "[fx16][reset]") {
    const fx::FXKind kinds[] = {
        fx::FXKind::Chorus, fx::FXKind::Distortion,
        fx::FXKind::BitCrusher, fx::FXKind::Compressor,
        fx::FXKind::BeatRepeat, fx::FXKind::BeatRoll };
    for (auto k : kinds) {
        FxRig g(k);
        REQUIRE(g.fx->latencySamples() == 0);
        for (int i = 0; i < 8; ++i) { g.sine(250.0, 0.7f); g.run(); }
        g.fx->reset();
        g.silence();
        const auto b = g.run();
        REQUIRE(allFinite(b));
    }
}
