#pragma once

#include <cstdint>

#include "PerformanceTypes.h"

namespace onda {

/// Unified per-deck performance snapshot (Phase 28 §2). One POD
/// aggregate combining durable musical state (cues, loops, quantize
/// config) with transient runtime echoes (playing, slip shadow, roll)
/// so UI, LEDs, diagnostics and tests read a single struct instead of
/// querying a dozen places. Built by PerformanceEngine::deckState()
/// from DeckStats (RT echoes) + DeckCtx (control-side truth).
struct DeckPerfState {
    int      deck          { -1 };
    int      trackState    { 0 };     // DeckState
    bool     playing       { false };
    bool     paused        { false };
    int64_t  frame         { 0 };     // displayed transport position
    double   positionSec   { 0.0 };
    double   durationSec   { 0.0 };

    // ---- tempo / pitch / key ----
    float    pitchPct      { 0.0f };
    float    pitchRange    { 8.0f };
    float    bendPct       { 0.0f };
    bool     keyLock       { false };
    int      keyIndex      { -1 };    // musical key (wired from track meta)
    int      keyEffectiveIndex { -1 };

    // ---- performance modes ----
    bool     quantize      { false };
    Quantize quantum       { Quantize::Beat1 };
    double   quantumBeats  { 1.0 };
    bool     slipMode      { false }; // user slip toggle
    bool     slipActive    { false }; // any slip ref held
    double   slipReturnSec { -1.0 };  // §22 — shadow resume point, -1 off
    bool     reverse       { false };
    bool     brake         { false };
    SyncMode syncMode      { SyncMode::Off };
    int      syncStatus    { 0 };     // SyncStatus
    bool     isMaster      { false };

    // ---- cue ----
    int64_t  cuePointFrame { -1 };    // primary cue point (frame)
    double   cuePointBeat  { -1.0 };  // its musical position when known
    int      cueBehavior   { 0 };     // CueBehavior
    bool     cuePreview    { false }; // hold-preview sounding
    int      hotCueMask    { 0 };     // bit per occupied hot/loop pad slot
    int      memoryCueMask { 0 };     // bit per memory cue
    int      activeHotCue  { -1 };    // last triggered hot cue slot
    int      activeMemCue  { -1 };    // last triggered memory cue slot

    // ---- loop ----
    bool     loopOn        { false };
    int64_t  loopInFrame   { -1 };
    int64_t  loopOutFrame  { -1 };
    double   loopInSec     { 0.0 };
    double   loopOutSec    { 0.0 };
    double   loopSizeBeats { 0.0 };   // selected auto-loop size
    bool     loopInArmed   { false }; // manual in-point waiting for out
    bool     rollActive    { false };
    double   rollBeats     { 0.0 };

    // ---- beatgrid ----
    bool     hasGrid       { false };
    double   beatPos       { -1.0 };
    double   barPos        { 0.0 };
    double   bpm           { 0.0 };
    double   effBpm        { 0.0 };
    double   targetBpm     { 0.0 };

    // ---- actions (§42-44) ----
    bool     scratching    { false };
    int      lastError     { 0 };     // PerfError
};

} // namespace onda
