// Phase 33 — FX deltas: parameter curves (§37), automation modes
// (§40), FXQuality tiers (§55), the BeatClock consultable view (§33).
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "core/fx/FXAutomation.h"
#include "core/fx/FXClock.h"
#include "core/fx/FXEngine.h"
#include "core/fx/FXFactory.h"
#include "core/fx/FXProcessor.h"
#include "core/fx/ReverbFX.h"
#include "core/fx/FilterFX.h"

using namespace onda;
using namespace onda::fx;
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

FXParamInfo infoWithCurve(FXParamCurve c) {
    FXParamInfo i { "P", 20.0f, 20000.0f, 100.0f, "Hz", 0.0f,
                    FXParamType::Float, c };
    return i;
}

} // namespace

// ==================================================== §37 curves
TEST_CASE("P33: FXParamCurve maps normalized->native per law",
          "[fx33][curve]") {
    using B = FXBase<1>;
    const auto lin = infoWithCurve(FXParamCurve::Linear);
    const auto log = infoWithCurve(FXParamCurve::Log);
    const auto exp = infoWithCurve(FXParamCurve::Exp);
    const auto cus = infoWithCurve(FXParamCurve::Custom);

    // endpoints exact on every curve
    for (const auto* d : { &lin, &log, &exp, &cus }) {
        REQUIRE(B::curveToNative(*d, 0.0f) == Approx(d->minV));
        REQUIRE(B::curveToNative(*d, 1.0f) == Approx(d->maxV));
    }
    // midpoints diverge as designed
    REQUIRE(B::curveToNative(lin, 0.5f) == Approx(10010.0f));
    REQUIRE(B::curveToNative(log, 0.5f)
            == Approx(632.456f).margin(0.01f));   // geometric mean
    REQUIRE(B::curveToNative(exp, 0.5f)
            == Approx(20.0f + 19980.0f * 0.25f));
    REQUIRE(B::curveToNative(cus, 0.5f)
            == Approx(10010.0f));                  // Linear fallback

    // log degenerate range (min <= 0) falls back to linear
    FXParamInfo bad { "P", -5.0f, 5.0f, 0.0f, "", 0.0f,
                      FXParamType::Float, FXParamCurve::Log };
    REQUIRE(B::curveToNative(bad, 0.5f) == Approx(0.0f));

    // round-trips are exact and monotone
    for (const auto* d : { &lin, &log, &exp }) {
        float prev = -1.0f;
        for (int s = 0; s <= 100; ++s) {
            const float v = s / 100.0f;
            const float x = B::curveToNative(*d, v);
            REQUIRE(x >= prev - 1e-6f);            // monotonic
            REQUIRE(B::curveToNorm(*d, x) == Approx(v).margin(1e-4f));
            prev = x;
        }
    }
}

TEST_CASE("P33: real processors expose Log curves on Hz params",
          "[fx33][curve]") {
    // every FX param declared in Hz maps logarithmically — equal knob
    // travel per octave instead of collapsing resolution into the top
    int hzLog = 0;
    for (int k = 1; k < (int)FXKind::kCount; ++k) {
        auto p = createFX((FXKind)k);
        REQUIRE(p != nullptr);
        for (int i = 0; i < p->paramCount(); ++i) {
            const auto info = p->paramInfo(i);
            if (info.unit != nullptr && std::strcmp(info.unit, "Hz") == 0
                && info.minV > 0.0f) {
                REQUIRE(info.curve == FXParamCurve::Log);
                ++hzLog;
            }
        }
    }
    REQUIRE(hzLog >= 10);   // Cutoff/Tone/PostLP/Rate/Color/Center...
}

TEST_CASE("P33: FilterFX cutoff uses the Log law end-to-end",
          "[fx33][curve]") {
    FilterFX f;
    f.prepare(kRate, kBlock);
    f.setParamNormalized(0, 0.5f);
    REQUIRE(f.paramNative(0) == Approx(632.456f).margin(0.5f));
    // native -> normalized inverts through the same law
    f.setParamNative(0, 5000.0f);
    REQUIRE(f.paramNormalized(0) == Approx(0.7995f).margin(1e-3f));
}

// =============================================== §40 auto modes
TEST_CASE("P33: FXAutomationMode gates lane evaluation",
          "[fx33][automation]") {
    FXAutomation a;
    auto& lane = a.laneFor(0x1234);
    lane.addPoint({ 0.0f, 0.25f });
    lane.addPoint({ 4.0f, 0.75f });

    uint16_t ids[4]; float vals[4];
    lane.mode = FXAutomationMode::Manual;
    REQUIRE(a.evaluate(2.0f, ids, vals, 4) == 1);
    REQUIRE(vals[0] == Approx(0.5f));

    lane.mode = FXAutomationMode::Off;
    REQUIRE(a.evaluate(2.0f, ids, vals, 4) == 0);

    // capture policies keep evaluating (playback keeps the lane)
    for (FXAutomationMode m : { FXAutomationMode::Latch,
                                FXAutomationMode::Touch,
                                FXAutomationMode::Write }) {
        lane.mode = m;
        REQUIRE(a.evaluate(2.0f, ids, vals, 4) == 1);
    }
    lane.mode = FXAutomationMode::Manual;
    REQUIRE((int)FXAutomationMode::Off == 0);
}

// ==================================================== §55 quality
TEST_CASE("P33: ReverbFX quality tier changes tap count, stays valid",
          "[fx33][quality]") {
    ReverbFX rev;
    rev.prepare(kRate, kBlock);
    rev.setParamNative(0, 0.8f);   // size
    rev.setParamNative(1, 0.9f);   // decay
    rev.setParamNative(4, 0.9f);   // width
    REQUIRE(rev.quality() == FXQuality::Medium);

    std::vector<float> l(kBlock), r(kBlock);
    auto feedSine = [&](float f) {
        for (int i = 0; i < kBlock; ++i) {
            l[(size_t)i] = r[(size_t)i] =
                0.4f * (float)std::sin(2.0 * 3.14159 * f * i / kRate);
        }
    };
    for (FXQuality q : { FXQuality::Low, FXQuality::Medium,
                        FXQuality::High }) {
        rev.setQuality(q);
        REQUIRE(rev.quality() == q);
        rev.reset();
        for (int b = 0; b < 40; ++b) {
            feedSine(220.0f + b * 13.0f);
            rev.process(l.data(), r.data(), kBlock);
            REQUIRE(allFinite(l.data(), kBlock));
            REQUIRE(allFinite(r.data(), kBlock));
        }
        REQUIRE(peakAbs(l.data(), kBlock) > 0.001f);  // wet tail exists
    }
    // Low still produces a reverb tail (not silence)
    rev.setQuality(FXQuality::Low);
    rev.reset();
    feedSine(330.0f);
    for (int b = 0; b < 30; ++b) rev.process(l.data(), r.data(), kBlock);
    std::fill(l.begin(), l.end(), 0.0f);
    std::fill(r.begin(), r.end(), 0.0f);
    for (int b = 0; b < 30; ++b) rev.process(l.data(), r.data(), kBlock);
    REQUIRE(peakAbs(l.data(), kBlock) > 0.0005f); // decaying tail
}

TEST_CASE("P33: FXSlot/FXEngine forward quality to live + pending",
          "[fx33][quality]") {
    FXEngine eng;
    eng.prepare(kRate);
    // live processor on master slot 0
    eng.loadEffect((int)FXTarget::Master, 0, FXKind::Reverb);
    float l[16]{}, r[16]{};
    SyncContext ctx;
    for (int b = 0; b < 4; ++b)
        eng.rack((int)FXTarget::Master).process(l, r, 16, ctx);
    eng.setQuality((int)FXTarget::Master, 0, FXQuality::Low);
    const auto* p = eng.rack((int)FXTarget::Master).slot(0).effect();
    REQUIRE(p != nullptr);
    REQUIRE(p->quality() == FXQuality::Low);

    // pending swap inherits the slot quality at adoption
    eng.loadEffect((int)FXTarget::Master, 0, FXKind::Reverb);
    for (int b = 0; b < 4; ++b)
        eng.rack((int)FXTarget::Master).process(l, r, 16, ctx);
    const auto* p2 = eng.rack((int)FXTarget::Master).slot(0).effect();
    REQUIRE(p2 != nullptr);
    REQUIRE(p2->quality() == FXQuality::Low);
    eng.collectTrash();
}

// ==================================================== §33 clock
TEST_CASE("P33: BeatClock view mirrors FXClock exactly",
          "[fx33][clock]") {
    SyncContext ctx;
    ctx.bpm = 128.0f; ctx.beatPhase = 0.5f;
    ctx.division = (int)BeatDiv::D_1_2; ctx.synced = 1;
    const auto c = BeatClock::from(ctx, kRate);
    REQUIRE(c.bpm == Approx(128.0f));
    REQUIRE(c.beatPhase == Approx(0.5f));
    REQUIRE(c.synced == 1);
    REQUIRE(c.div() == BeatDiv::D_1_2);
    REQUIRE(c.divBeats() == Approx(2.0f));
    REQUIRE(c.divFrames()
            == Approx(FXClock::framesFor(128.0f, (int)BeatDiv::D_1_2,
                                         kRate)));
    REQUIRE(c.divMs() == Approx(937.5f));        // 2 beats @128
    REQUIRE(c.barPhase() == Approx(0.25f));      // 0.5/2 inside beat

    // degenerate bpm clamps like FXClock
    SyncContext z; z.bpm = 0.0f; z.division = (int)BeatDiv::D_1_4;
    const auto cz = BeatClock::from(z, kRate);
    REQUIRE(cz.divFrames()
            == Approx(FXClock::framesFor(120.0f,
                                         (int)BeatDiv::D_1_4, kRate)));
}
