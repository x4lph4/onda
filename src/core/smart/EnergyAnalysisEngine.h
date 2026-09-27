#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "../analysis/AnalysisTypes.h"
#include "../analysis/WaveformData.h"
#include "../library/MiniJson.h"
#include "SmartLibraryTypes.h"

namespace onda::smart {

using library::MiniJson;

/// Phase 23 §13-18 — derives the energy descriptor from data the
/// pipeline already measures: integrated LUFS + RMS + loudness range
/// for the scalar, the waveform overview RMS pyramid for the
/// over-time curve. No additional DSP pass is needed and nothing
/// runs on the audio thread (analysis workers own this).
///
/// The scalar is a documented heuristic, not objective truth:
///   e = 0.55 * norm(lufs, -40..0) + 0.30 * norm(rms, 0..0.5)
///     + 0.15 * norm(lra, 0..20)
/// clamped to 0..1. Descriptive only (§13).
class EnergyAnalysisEngine {
public:
    static constexpr int kAlgorithmVersion = 1;

    // ---- scalar descriptor (§13) --------------------------------
    static float energyOf(float lufsI, float rms, float lra,
                          const EnergyLevels& lv = {}) {
        if (lufsI <= -69.0f && rms <= 0.0f) return -1.0f;  // unknown
        auto norm = [](float v, float lo, float hi) {
            return std::clamp((v - lo) / (hi - lo), 0.0f, 1.0f);
        };
        const float e = 0.55f * norm(lufsI, -40.0f, 0.0f)
                      + 0.30f * norm(rms,    0.0f,  0.5f)
                      + 0.15f * norm(lra,    0.0f, 20.0f);
        (void)lv;
        return std::clamp(e, 0.0f, 1.0f);
    }

    // ---- over-time curve (§14) ----------------------------------
    /// Overview pyramid RMS → normalized curve points. `maxPoints`
    /// bounds the stored JSON size (downsamples by max-pooling).
    static EnergyCurve curveOf(const WaveformData& wf,
                               int maxPoints = 96) {
        EnergyCurve out;
        if (wf.overview.empty() || wf.numFrames <= 0
            || wf.sampleRate <= 0.0 || wf.channels <= 0)
            return out;
        const int bins = (int)wf.overview.size() / wf.channels;
        if (bins <= 0) return out;

        // per-bin mono RMS (mean across channels) + track max
        std::vector<float> vals((size_t)bins, 0.0f);
        float mx = 1e-6f;
        for (int b = 0; b < bins; ++b) {
            float acc = 0.0f;
            for (int ch = 0; ch < wf.channels; ++ch)
                acc += wf.overview[(size_t)b * wf.channels + ch].rms;
            vals[(size_t)b] = acc / (float)wf.channels;
            mx = std::max(mx, vals[(size_t)b]);
        }
        const double binSec =
            (double)wf.numFrames / wf.sampleRate / (double)bins;
        const int mp = std::max(1, maxPoints);
        const int step = std::max(1, (bins + mp - 1) / mp);
        for (int b = 0; b < bins; b += step) {
            float v = 0.0f;
            const int e = std::min(bins, b + step);
            for (int i = b; i < e; ++i) v = std::max(v, vals[(size_t)i]);
            out.push_back({ b * binSec, e * binSec,
                            std::clamp(v / mx, 0.0f, 1.0f) });
        }
        return out;
    }

    // ---- transitions (§16) --------------------------------------
    /// Points where the curve changes by more than `minDelta`
    /// between adjacent points; confidence ∝ |delta| (sustained,
    /// sharp changes score highest).
    static std::vector<EnergyTransition> transitionsOf(
        const EnergyCurve& c, float minDelta = 0.25f) {
        std::vector<EnergyTransition> out;
        for (size_t i = 1; i < c.size(); ++i) {
            const float d = c[i].value - c[i - 1].value;
            if (std::fabs(d) < minDelta) continue;
            out.push_back({ c[i - 1].start, c[i].end, d,
                            std::clamp(std::fabs(d) * 1.6f, 0.0f, 1.0f) });
        }
        return out;
    }

    // ---- structure model (§17-18) -------------------------------
    /// Conservative section detection: only Intro/Outro are claimed,
    /// and only when the leading/trailing energy is clearly low for
    /// a sustained span. Everything else is Unknown or omitted —
    /// no invented drops/choruses (§17).
    static std::vector<StructureSegment> structureOf(
        const EnergyCurve& c, double durationSec) {
        std::vector<StructureSegment> out;
        if (c.size() < 8 || durationSec <= 0.0) return out;

        auto spanAvg = [&](size_t a, size_t b) {
            float s = 0.0f;
            for (size_t i = a; i < b && i < c.size(); ++i)
                s += c[i].value;
            return s / (float)std::max<size_t>(1, std::min(b, c.size()) - a);
        };
        const float trackAvg = spanAvg(0, c.size());
        const float introAvg = spanAvg(0, c.size() / 10 + 1);
        const float outroAvg =
            spanAvg(c.size() - c.size() / 10 - 1, c.size());

        const double span = durationSec / (double)c.size();
        size_t iEnd = 0;
        while (iEnd < c.size() / 4 && c[iEnd].value < trackAvg * 0.7f)
            ++iEnd;
        if (iEnd >= 2 && introAvg < trackAvg * 0.75f)
            out.push_back({ 0.0, iEnd * span,
                            StructureSegment::Kind::Intro,
                            std::clamp(1.0f - introAvg / trackAvg, 0.f, 1.f),
                            false });

        size_t oStart = c.size();
        while (oStart > c.size() * 3 / 4
               && c[oStart - 1].value < trackAvg * 0.7f) --oStart;
        if (oStart <= c.size() - 2 && outroAvg < trackAvg * 0.75f)
            out.push_back({ oStart * span, durationSec,
                            StructureSegment::Kind::Outro,
                            std::clamp(1.0f - outroAvg / trackAvg, 0.f, 1.f),
                            false });
        return out;
    }

    // ---- bundle assembly ----------------------------------------
    static EnergyResult analyze(const WaveformData& wf,
                                const LoudnessResult& loud,
                                double durationSec,
                                const EnergyLevels& lv = {}) {
        EnergyResult r;
        r.algorithmVersion = kAlgorithmVersion;
        r.energy   = energyOf(loud.integratedLufs, loud.rms,
                              loud.loudnessRange, lv);
        r.level    = lv.levelFor(r.energy);
        r.curve    = curveOf(wf);
        r.transitions = transitionsOf(r.curve);
        r.structure   = structureOf(r.curve, durationSec);
        r.confidence  = loud.valid ? 0.9f
                        : r.curve.empty() ? 0.0f : 0.5f;
        return r;
    }

    // ---- JSON codecs (analysis row columns) ---------------------
    static std::string curveJson(const EnergyCurve& c) {
        MiniJson a = MiniJson::array();
        for (const auto& p : c) {
            MiniJson pt = MiniJson::array();
            pt.push(p.start); pt.push(p.end); pt.push((double)p.value);
            a.push(std::move(pt));
        }
        return a.dump();
    }
    static std::string transitionsJson(
        const std::vector<EnergyTransition>& t) {
        MiniJson a = MiniJson::array();
        for (const auto& p : t) {
            MiniJson pt = MiniJson::array();
            pt.push(p.start); pt.push(p.end);
            pt.push((double)p.delta); pt.push((double)p.confidence);
            a.push(std::move(pt));
        }
        return a.dump();
    }
    static std::string structureJson(
        const std::vector<StructureSegment>& s) {
        MiniJson a = MiniJson::array();
        for (const auto& p : s) {
            MiniJson o = MiniJson::object();
            o.set("s", p.start); o.set("e", p.end);
            o.set("k", StructureSegment::name(p.kind));
            o.set("c", (double)p.confidence);
            if (p.userDefined) o.set("u", true);
            a.push(std::move(o));
        }
        return a.dump();
    }

    static EnergyCurve parseCurve(const std::string& j) {
        EnergyCurve out;
        bool ok = false;
        const MiniJson a = MiniJson::parse(j, ok);
        if (!ok) return out;
        for (const auto& pt : a.arr()) {
            const auto& v = pt.arr();
            if (v.size() < 3) continue;
            out.push_back({ v[0].num(), v[1].num(), (float)v[2].num() });
        }
        return out;
    }
    static std::vector<EnergyTransition> parseTransitions(
        const std::string& j) {
        std::vector<EnergyTransition> out;
        bool ok = false;
        const MiniJson a = MiniJson::parse(j, ok);
        if (!ok) return out;
        for (const auto& pt : a.arr()) {
            const auto& v = pt.arr();
            if (v.size() < 4) continue;
            out.push_back({ v[0].num(), v[1].num(),
                            (float)v[2].num(), (float)v[3].num() });
        }
        return out;
    }
    static std::vector<StructureSegment> parseStructure(
        const std::string& j) {
        std::vector<StructureSegment> out;
        bool ok = false;
        const MiniJson a = MiniJson::parse(j, ok);
        if (!ok) return out;
        for (const auto& o : a.arr()) {
            StructureSegment s;
            s.start = o.at("s").num(); s.end = o.at("e").num();
            s.kind  = StructureSegment::kindFor(o.at("k").str());
            s.confidence  = (float)o.at("c").num();
            s.userDefined = o.at("u").boolean();
            out.push_back(s);
        }
        return out;
    }
};

} // namespace onda::smart
