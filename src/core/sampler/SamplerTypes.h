#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace onda::sampler {

// ---- playback semantics (task §5) ----
enum class PlaybackMode : int {
    OneShot = 0,   // play once, then stop
    Loop,          // repeat while active
    Hold,          // play while pad held (release stops)
    Gate,          // pad press toggles play/stop
    // Phase 34 §9 — appended so persisted mode values stay valid.
    Retrigger,     // every press starts a NEW voice — no same-slot
                   // choke (true polyphonic retrigger)
};

// ---- trigger quantization (task §12) ----
enum class QuantizeMode : int {
    Off    = 0,    // fire immediately
    Q_1_4  = 1,    // next 1/4 note boundary
    Q_1_8  = 2,    // next 1/8
    Q_1_16 = 3,    // next 1/16
    Q_1_32 = 4,    // next 1/32
    Bar    = 5,    // next bar
    // Phase 34 §20 — appended so persisted mode values stay valid.
    Q_1_2  = 6,    // next 1/2 note (2 beats)
    Q_2Bar = 7,    // next 2-bar boundary
    Q_4Bar = 8,    // next 4-bar boundary
};
inline double quantizeBeats(QuantizeMode m) {
    switch (m) {
        case QuantizeMode::Q_1_4:  return 1.0;
        case QuantizeMode::Q_1_8:  return 0.5;
        case QuantizeMode::Q_1_16: return 0.25;
        case QuantizeMode::Q_1_32: return 0.125;
        case QuantizeMode::Bar:    return 4.0;
        case QuantizeMode::Q_1_2:  return 2.0;
        case QuantizeMode::Q_2Bar: return 8.0;
        case QuantizeMode::Q_4Bar: return 16.0;
        default:                   return 0.0;
    }
}

// ---- voice exhaustion policy (task §7) ----
enum class StealPolicy : int {
    Oldest = 0,      // steal oldest active voice
    Quietest,        // steal lowest current gain voice
    SameSample,      // prefer stealing a voice of the same sample
    Deny,            // refuse the new trigger
};

// ---- observable slot state (task §20/§26) ----
enum class SlotState : int {
    Empty = 0, Loading, Ready, Queued,
    Playing, Looping, Error, Disabled,
};

// ---- slot parameter kinds (rt::SampleSlotParam.kind) ----
enum SlotParamKind : uint8_t {
    SP_Volume = 0,   // 0..1  (post-gain fader)
    SP_Pan,          // -1..+1
    SP_Gain,         // 0..2  (trim/normalization gain)
    SP_Pitch,        // semitones -48..+48
    SP_Mode,         // PlaybackMode
    SP_LoopOn,       // 0/1 (LOOP-mode only matters when playing)
    SP_SyncOn,       // 0/1
    SP_Quantize,     // QuantizeMode
    SP_Mute,         // 0/1
    SP_Solo,         // 0/1
    SP_Filter,       // -1..+1 (per-slot tilt filter, Phase 34 §32)
    SP_Attack,       // ms 0..500   (§28 envelope; 0 = default)
    SP_Release,      // ms 0..2000  (§28 envelope; 0 = default)
    SP_TrimStart,    // frames >= 0 (§59-61 non-destructive trim)
    SP_TrimEnd,      // frames >= 0; 0/-1 = sample end
    SP_LoopIn,       // frames >= 0; -1 = trim start
    SP_LoopOut,      // frames >= 0; -1 = trim end
};

// ---- decoded sample (immutable, produced by SampleLoader) ----
/// Planar stereo float at the HOST sample rate (loader resamples).
/// Ownership: shared_ptr<const SampleData> handed to the RT engine
/// through the lock-free mailbox (SamplerRt::publishSlot).
struct SampleData {
    std::shared_ptr<const float[]> l, r;  // planar, `frames` long
    int64_t  frames    = 0;
    double   sampleRate = 48000.0;
    int      channels  = 2;      // source channels (1 = mono, 2+ = stereo)
    float    peak      = 0.0f;
    double   bpm       = 0.0;    // detected (>0 when confident)
    // coarse peak-pairs waveform for UI preview (up to kWaveN)
    static constexpr int kWaveN = 512;
    float    wave[kWaveN] = {};
    int      waveCount = 0;
};

/// Per-slot domain state — written by the app thread, mirrored to the
/// RT engine through rt::SampleSlotParam commands. `state` is written
/// by RT echoes (atomic) so UI/hardware read one source of truth.
struct SampleSlot {
    int         slot     = -1;
    int64_t     sampleId = -1;     // DB row / cache key (-1 = none)
    std::string path;
    std::string name;
    uint32_t    colorArgb = 0xFF4A90D9;  // default accent
    float       volume   = 1.0f;   // 0..1 fader
    float       pan      = 0.0f;   // -1..+1
    float       gain     = 1.0f;   // 0..2 trim / normalization
    float       pitch    = 0.0f;   // semitones
    PlaybackMode mode    = PlaybackMode::OneShot;
    QuantizeMode quant   = QuantizeMode::Off;
    bool        syncOn   = false;
    bool        loopOn   = false;
    bool        mute     = false;
    bool        solo     = false;
    float       filterPos = 0.0f;  // per-slot tilt filter -1..+1
    // Phase 34 §28 — per-slot envelope overrides; 0 = engine default
    // (~0.2 ms attack / ~21 ms release anti-click ramps).
    float       attackMs  = 0.0f;  // 0..500
    float       releaseMs = 0.0f;  // 0..2000
    // Phase 34 §59-61 — non-destructive playback window, in decoded
    // frames. -1 = unset (startF→0, endF→sample end, loopIn→startF,
    // loopOut→endF). Metadata only — the file/data is never touched.
    double      startF  = -1.0;
    double      endF    = -1.0;
    double      loopIn  = -1.0;
    double      loopOut = -1.0;
    std::atomic<int> missing{ 0 };  // §65 — file gone, keep assignment
    std::atomic<int> state{ (int)SlotState::Empty };  // SlotState
    std::atomic<int64_t> durationFrames{ 0 };
    std::atomic<double>  bpm{ 0.0 };

    SampleSlot() = default;
    /// Value-copy: atomics snapshot-load (atomics are not copyable).
    SampleSlot(const SampleSlot& o)
        : slot(o.slot), sampleId(o.sampleId), path(o.path),
          name(o.name), colorArgb(o.colorArgb), volume(o.volume),
          pan(o.pan), gain(o.gain), pitch(o.pitch), mode(o.mode),
          quant(o.quant), syncOn(o.syncOn), loopOn(o.loopOn),
          mute(o.mute), solo(o.solo), filterPos(o.filterPos),
          attackMs(o.attackMs), releaseMs(o.releaseMs),
          startF(o.startF), endF(o.endF),
          loopIn(o.loopIn), loopOut(o.loopOut) {
        missing.store(o.missing.load());
        state.store(o.state.load());
        durationFrames.store(o.durationFrames.load());
        bpm.store(o.bpm.load());
    }
    SampleSlot& operator=(const SampleSlot& o) {
        if (this == &o) return *this;
        slot = o.slot; sampleId = o.sampleId; path = o.path;
        name = o.name; colorArgb = o.colorArgb; volume = o.volume;
        pan = o.pan; gain = o.gain; pitch = o.pitch; mode = o.mode;
        quant = o.quant; syncOn = o.syncOn; loopOn = o.loopOn;
        mute = o.mute; solo = o.solo; filterPos = o.filterPos;
        attackMs = o.attackMs; releaseMs = o.releaseMs;
        startF = o.startF; endF = o.endF;
        loopIn = o.loopIn; loopOut = o.loopOut;
        missing.store(o.missing.load());
        state.store(o.state.load());
        durationFrames.store(o.durationFrames.load());
        bpm.store(o.bpm.load());
        return *this;
    }
    // no move ops — declared copy ops suppress them; moves fall back
    // to the snapshot copy (atomics are not movable)
};

} // namespace onda::sampler
