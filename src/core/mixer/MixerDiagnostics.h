#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#include "MixerTypes.h"

namespace onda::mixer {

/// §40-41 — copyable mixer state snapshot for diagnostics and tests.
/// Filled by MixerEngine::snapshot() from atomic meters + RT state;
/// read on any thread. All fields are POD — no allocation, no locks.
struct MixerChannelDiag {
    bool  active     { false };
    int   sourceType { (int)ChSourceType::Silence };
    int   sourceIndex{ 0 };
    float gain       { 1.0f };
    float lowDb      { 0.0f }, midDb { 0.0f }, highDb { 0.0f };
    float filterPos  { 0.0f }, filterRes { 0.707f };
    int   filterType { 0 }, eqMode { 0 };
    float pan        { 0.0f };
    float fader      { 1.0f };
    float sendA      { 0.0f }, sendB { 0.0f };
    int   xfAssign   { (int)XfAssign::Thru };
    bool  cue { false }, mute { false }, solo { false },
          phase { false };
    float peakL { 0.0f }, peakR { 0.0f };
    float rmsL  { 0.0f }, rmsR  { 0.0f };
    float holdL { 0.0f }, holdR { 0.0f };
    uint64_t clips { 0 };
    int   latencySamples { 0 };       // insert-rack reported latency
};

struct MixerBusDiag {
    float xfPos { 0.0f };
    int   xfCurve { 0 };
    float masterGain { 1.0f };
    bool  masterMute { false };
    bool  limiterOn  { false };
    float limiterGain { 1.0f };       // current gain reduction
    float masterPeakL { 0.0f }, masterPeakR { 0.0f };
    float masterRmsL  { 0.0f }, masterRmsR  { 0.0f };
    uint64_t masterClips { 0 };
    float cueVol { 0.8f }, cueMix { 0.5f };
    bool  masterCue { false };
    float cuePeakL { 0.0f }, cuePeakR { 0.0f };
    int   soloCount { 0 }, cueCount { 0 };
    int   deckMode { 4 };
    int   activeChannels { 0 };
    int   masterLatencySamples { 0 }; // master FX rack latency
    float boothGain { 1.0f };
    float boothPeakL { 0.0f }, boothPeakR { 0.0f };
    uint64_t boothClips { 0 };
    bool  recorderAttached { false };
    bool  headphoneMono { false };
    // §40-41 output guards (mapped pairs: master / cue / booth)
    float masterDc { 0.0f }, cueDc { 0.0f }, boothDc { 0.0f };
    uint64_t outSanitized { 0 };   // NaN/Inf/over-clamp samples fixed
    uint64_t outOverFull  { 0 };   // |x| > 1.0 samples detected
};

/// Human-readable report — same style as fx::diagnosticReport.
/// Built on the control thread only (std::string allocates).
inline std::string mixerReport(const MixerBusDiag& d,
                               const MixerChannelDiag* ch,
                               int nCh) {
    std::string s;
    s.reserve(1400);
    auto num = [](float v) { char b[32]; std::snprintf(b, 32, "%.2f", (double)v); return std::string(b); };
    s += "ONDA MIXER DIAGNOSTICS\n";
    s += "mode=" + std::to_string(d.deckMode) + "deck  active="
       + std::to_string(d.activeChannels)
       + "  xf=" + num(d.xfPos) + " curve=" + std::to_string(d.xfCurve)
       + "  master=" + num(d.masterGain) + (d.masterMute ? " MUTE" : "")
       + (d.limiterOn ? "  LIM(gr=" + num(d.limiterGain) + ")" : "")
       + "  mPeak=" + num(d.masterPeakL) + "/" + num(d.masterPeakR)
       + "  mClips=" + std::to_string(d.masterClips)
       + "  lat=" + std::to_string(d.masterLatencySamples) + "smp\n";
    s += "cue: vol=" + num(d.cueVol) + " mix=" + num(d.cueMix)
       + (d.masterCue ? " MASTER-CUE" : "")
       + "  pfl=" + std::to_string(d.cueCount)
       + " solo=" + std::to_string(d.soloCount)
       + "  cuePeak=" + num(d.cuePeakL) + "/" + num(d.cuePeakR) + "\n";
    s += "booth: gain=" + num(d.boothGain)
       + "  peak=" + num(d.boothPeakL) + "/" + num(d.boothPeakR)
       + "  clips=" + std::to_string(d.boothClips)
       + (d.recorderAttached ? "  REC" : "") + "\n";
    s += "outGuard: dc=" + num(d.masterDc) + "/" + num(d.cueDc)
       + "/" + num(d.boothDc)
       + "  sanitized=" + std::to_string(d.outSanitized)
       + "  overFull=" + std::to_string(d.outOverFull)
       + (d.headphoneMono ? "  PHONES-MONO" : "") + "\n";
    for (int i = 0; i < nCh; ++i) {
        const auto& c = ch[i];
        if (!c.active) continue;
        s += "ch" + std::to_string(i) + " src=" + std::to_string(c.sourceType)
           + ":" + std::to_string(c.sourceIndex)
           + " g=" + num(c.gain)
           + " eq[" + num(c.lowDb) + "/" + num(c.midDb) + "/" + num(c.highDb) + "]"
           + " flt=" + num(c.filterPos) + " q=" + num(c.filterRes)
           + " pan=" + num(c.pan)
           + (c.eqMode == 1 ? " ISO" : "")
           + " f=" + num(c.fader)
           + (c.cue ? " CUE" : "") + (c.mute ? " MUTE" : "")
           + (c.solo ? " SOLO" : "") + (c.phase ? " Ø" : "")
           + " pk=" + num(c.peakL) + " rms=" + num(c.rmsL)
           + " clips=" + std::to_string(c.clips)
           + " lat=" + std::to_string(c.latencySamples) + "\n";
    }
    return s;
}

} // namespace onda::mixer
