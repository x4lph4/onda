#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace onda::ui {

/// Plain-value UI snapshots — views render ONLY from these structs.
/// EngineBridge builds them from the engines' atomic stats so a view
/// never touches DeckStats/MasterStats or calls into RT objects.
/// Tests fabricate snapshots directly; widgets are pure functions of
/// (snapshot, theme, bounds).

struct DeckSnapshot {
    int    state        = 0;    // DeckState
    double durationSec  = 0.0;
    double positionSec  = 0.0;
    float  loadProgress = 0.0f;
    float  levelL = 0.0f, levelR = 0.0f;
    float  pitchPct = 0.0f, pitchRange = 8.0f, bendPct = 0.0f;
    bool   keylock = false, reverse = false, quantize = false;
    bool   slip = false;
    bool   loopOn = false;
    double loopInSec = 0.0, loopOutSec = 0.0;
    double bpm = 0.0, effBpm = 0.0, beatPos = -1.0, barPos = 0.0;
    int    syncMode = 0;                 // SyncMode
    float  syncErrMs = 0.0f;
    bool   syncLimited = false;
    // Phase 15 — sync control-plane state (from PerformanceEngine)
    int    syncStatus = 0;               // SyncStatus
    float  syncConf = 0.0f;              // 0..1 — grid + phase stability
    bool   isMaster = false;
    bool   syncLock = true;              // sync survives track swap
    double targetBpm = 0.0;
    double phase = 0.0;                  // frac beat 0..1 (BeatIndicator)
    int    beatsPerBar = 4;
    bool   pfl = false;
    bool   streamEnded = false;
    // Phase 21 — stems (availability mirrors stems::StemAvailability;
    // stemFlags per pad: bit0 present,1 muted,2 solo,3 loading)
    int      stemsAvail = 0;
    int      stemMode = 0;                    // 0 original, 1 stems
    uint32_t stemsAudible = 0;               // bit per rendered stem
    std::array<uint8_t, 8> stemFlags {};
    std::array<float, 4>   stemGain { 1.0f, 1.0f, 1.0f, 1.0f };
    // meta (filled by bridge from track/library, not atomics)
    std::string path, title, artist, keyText;
    int    keyIndex = -1;               // 0..23 — notation chosen by view
    // Phase 22 — tonal detail (§28): detected confidence, ambiguity,
    // mode, and effective/shifted key when key-shift is active.
    float  keyConfidence = 0.0f;
    bool   keyAmbiguous = false;
    bool   keyIsMinor = false;
    int    keyEffectiveIndex = -1;      // -1 = same as keyIndex/unknown
    int    keyShiftSemis = 0;
    int64_t trackId = -1;
    bool    analyzed = false;
};

struct ChannelSnapshot {
    float gain = 1.0f, low = 0.0f, mid = 0.0f, high = 0.0f;
    float filter = 0.0f, fader = 1.0f;
    float sendA = 0.0f, sendB = 0.0f;
    bool  cue = false, mute = false, solo = false;
    int   xfAssign = 1;                  // XfAssign: 0 A, 1 Thru, 2 B
    float levelL = 0.0f, levelR = 0.0f;
    float holdL = 0.0f,  holdR = 0.0f;
    bool  clip = false;
};

struct MasterSnapshot {
    float gain = 1.0f, cueVol = 1.0f, cueMix = 0.0f;
    float crossfader = 0.0f;
    int   xfCurve = 0;
    bool  limiterOn = true, masterMute = false;
    float peakL = 0.0f, peakR = 0.0f, rmsL = 0.0f, rmsR = 0.0f;
    float holdL = 0.0f,  holdR = 0.0f;
    float limiterGain = 1.0f;
    bool  clip = false;
    bool  cueAvailable = false;
    bool  masterCue = false;    // Phase 17: master → phones cue tap
    int   deckMode = 4;         // Phase 17: visible deck channels
    double masterBpm = 0.0;
    int   masterDeck = -1;
};

struct FxSlotSnapshot {
    int   kind = 0;            // FXKind
    bool  enabled = false;
    float mix = 0.0f;
    bool  synced = false;
    int   division = 0;
    float params[8] = {};      // native param values (P1..P8)
    bool  busy = false;
    float cpuUs = 0.0f;
};

/// Waveform overlay markers (plain values — views map them to px).
/// CueMarker: slot = hot-cue slot 0..7 (-1 unslotted), type mirrors
/// CueType (0 hot / 1 memory / 2 loop) so views distinguish marker
/// shapes without including perf headers (Phase 19 §18).
struct CueMarker { double sec = 0.0; uint32_t argb = 0;
                   std::string label; int slot = -1; int type = 0; };
/// LoopRegion: id = saved-loop slot when known; saved distinguishes a
/// stored loop marker from the live deck loop (Phase 19 §20).
struct LoopRegion { double inSec = 0.0, outSec = 0.0;
                    bool active = false; bool saved = false;
                    int id = -1; };

struct AudioStatus {
    std::string deviceName;
    double sampleRate = 0.0;
    int    bufferSize = 0;
    float  cpuLoad = 0.0f;
    float  maxCallbackMs = 0.0f;
    uint64_t xruns = 0, clips = 0;
    bool   running = false;
    double inputLatencySec = 0.0, outputLatencySec = 0.0;
    float  uiFps = 0.0f;       // measured UI update rate (Hz)
    float  uiFrameMs = 0.0f;   // measured snapshot/feed time (ms)
    // Phase 13 — device engine layer
    std::string backend;       // "WASAPI", "CoreAudio", "ASIO", ...
    double   estLatencyMs = 0.0;   // estimated round-trip ms
    int      recovery = 0;         // adevice::HealthState ordinal
    uint64_t underruns = 0;        // over-budget callbacks
    uint64_t deviceErrors = 0;     // backend-reported device errors
};

/// Recording strip snapshot (Phase 12) — plain values only; `state`
/// mirrors rec::State so the view doesn't include the engine header.
struct RecordingSnapshot {
    int    state       = 0;    // rec::State ordinal
    double elapsedSec  = 0.0;
    float  peakL = 0.0f, peakR = 0.0f;
    float  rmsL  = 0.0f, rmsR  = 0.0f;
    float  maxPeak = 0.0f;
    uint32_t clips = 0;
    int64_t droppedFrames = 0;
    int    fillPct = 0;
    int64_t diskFreeBytes = 0;
    bool   diskWarn = false;
    bool   diskError = false;
    std::string fileName;
};

struct UiSnapshot {
    DeckSnapshot    deck[4];
    ChannelSnapshot channel[4];
    MasterSnapshot  master;
    AudioStatus     audio;
    RecordingSnapshot rec;
    FxSlotSnapshot  fxDeck[4][4];        // [deck][slot]
    FxSlotSnapshot  fxSend[2][4];
    FxSlotSnapshot  fxMaster[4];
    float           fxReturn[2] = { 1.0f, 1.0f };
    uint32_t        frame = 0;           // snapshot generation counter
};

} // namespace onda::ui
