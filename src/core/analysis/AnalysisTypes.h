#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace onda {

/// Bumped whenever any analyser's output meaning changes; cached
/// analyses with a different version report OUTDATED.
/// v2: loudness/transients/component-status/needs-review columns.
inline constexpr int kAnalysisVersion = 2;

enum class AnalysisState : int {
    NotAnalyzed = 0,
    Analyzing,
    Analyzed,
    Failed,
    Outdated
};

/// Queue priority (§4): deck-load → High; manual import → Normal;
/// bulk library scan → Low; Realtime reserved for pre-play deadlines.
enum class AnalysisPriority : int {
    Low = 0, Normal = 1, High = 2, Realtime = 3
};

/// §31 — simple time signature; denominator reserved (most content
/// is x/4) but never hard-coded as the only possibility.
struct TimeSignature {
    int numerator   { 4 };
    int denominator { 4 };
};

/// Per-component pipeline stages (§26-27): each reports its own
/// state + progress so a key failure doesn't sink a good waveform.
/// Phase 30 — Spectral (§43) and Energy (§44) appended at the END so
/// stored comp_status bit positions stay stable.
enum class AnalysisComponent : int {
    Decode = 0, Waveform, Bpm, BeatGrid, Key, Transients, Loudness,
    Spectral, Energy,
    Count
};
inline constexpr int kNumComponents =
    (int)AnalysisComponent::Count;
struct ComponentStatus {
    int   state { 0 };      // AnalysisState
    float progress { 0.0f }; // 0..1
};

/// §51 — bit over AnalysisComponent selecting which stages a job
/// runs. `All` keeps the default full pipeline; deck-load requests
/// only what playback needs first (§52) while a follow-up LOW job
/// finishes the rest (§53).
namespace AnalysisMask {
    inline constexpr uint32_t bit(AnalysisComponent c) {
        return 1u << (int)c;
    }
    inline constexpr uint32_t Decode    = bit(AnalysisComponent::Decode);
    inline constexpr uint32_t Waveform  = bit(AnalysisComponent::Waveform);
    inline constexpr uint32_t Bpm       = bit(AnalysisComponent::Bpm);
    inline constexpr uint32_t BeatGrid  = bit(AnalysisComponent::BeatGrid);
    inline constexpr uint32_t Key       = bit(AnalysisComponent::Key);
    inline constexpr uint32_t Transients= bit(AnalysisComponent::Transients);
    inline constexpr uint32_t Loudness  = bit(AnalysisComponent::Loudness);
    inline constexpr uint32_t Spectral  = bit(AnalysisComponent::Spectral);
    inline constexpr uint32_t Energy    = bit(AnalysisComponent::Energy);
    /// Everything except the mandatory decode.
    inline constexpr uint32_t All = Waveform | Bpm | BeatGrid | Key |
        Transients | Loudness | Spectral | Energy;
    /// §52 — what a deck needs before it can perform.
    inline constexpr uint32_t Essential = Bpm | BeatGrid | Waveform;
}

/// §4 — external job lifecycle. Richer than AnalysisState (which
/// describes a stored result): a job can wait in queue and can be
/// cancelled mid-flight.
enum class JobState : int {
    Queued = 0, Decoding, Analyzing, Caching,
    Completed, Failed, Cancelled
};
inline const char* jobStateText(int s) {
    switch ((JobState)s) {
        case JobState::Queued:    return "queued";
        case JobState::Decoding:  return "decoding";
        case JobState::Analyzing: return "analyzing";
        case JobState::Caching:   return "caching";
        case JobState::Completed: return "completed";
        case JobState::Failed:    return "failed";
        case JobState::Cancelled: return "cancelled";
        default:                  return "?";
    }
}

/// §3 — the public descriptor of one analysis job.
struct AnalysisJob {
    int64_t     jobId { 0 };
    int64_t     trackId { -1 };        // -1 when path-only enqueue
    std::string path;
    int         priority { (int)AnalysisPriority::Normal };
    uint32_t    requestedAnalyses { AnalysisMask::All };
    int         state { (int)JobState::Queued };
    float       progress { 0.0f };
    int         attempts { 0 };
    std::string error;
};

/// A detected transient — reused by sampler/slicer/beat-repeat UIs.
struct TransientEvent {
    int64_t frame { 0 };
    float   strength { 0.0f };  // onset strength 0..1-ish
};

/// BS.1770-flavoured loudness (§21). Informational only — the audio
/// file is never normalised or altered.
struct LoudnessResult {
    float  peak { 0.0f };       // max |sample| (linear)
    float  rms { 0.0f };        // overall RMS (linear)
    float  integratedLufs { -70.0f }; // K-weighted, gated
    float  loudnessRange { 0.0f };    // LRA (p10–p95, LU)
    /// §45 — inter-sample peak estimate (4× linear oversampling —
    /// honest approximation, documented: not a polyphase true-peak
    /// meter but catches clipped-looking intersample overs).
    float  truePeak { 0.0f };
    bool   valid { false };
};

/// §43 — spectral summary. Band energies are fractions of total
/// energy (sum ≈ 1), centroid in Hz. Bands also live per-overview-
/// bin in WaveformData for colored rendering (§42).
struct SpectralResult {
    float  low { 0.0f };        // < 250 Hz fraction
    float  mid { 0.0f };        // 250 Hz – 4 kHz fraction
    float  high { 0.0f };       // > 4 kHz fraction
    float  centroidHz { -1.0f };// -1 when undetermined
    float  confidence { 0.0f }; // spectral spread reliability 0..1
};

/// §44 — normalized energy score (0..1) + how trustworthy it is.
struct EnergyResult {
    float  score { -1.0f };     // -1 = unknown
    float  confidence { 0.0f };
};

struct BpmCandidate {
    double bpm { 0.0 };
    float  score { 0.0f };  // normalized 0..1 relative to best
};

struct TempoSegment {
    int64_t startFrame { 0 };  // device-rate frame where this tempo starts
    int64_t startBeat  { 0 };  // beat index at which the change applies
    double  bpm        { 0.0 };
};

struct BpmResult {
    double bpm { 0.0 };
    float  confidence { 0.0f };
    float  stability { 0.0f };          // 1 = steady, 0 = drifty
    bool   variable { false };          // tempo map has >1 segment
    std::vector<BpmCandidate> candidates;
    std::vector<TempoSegment> tempoMap; // empty => constant tempo
    int64_t firstOnsetFrame { -1 };     // earliest detected onset
};

struct KeyResult {
    int    key { -1 };        // 0..23: 0-11 major (C..B), 12-23 minor (Am..G#m)
    float  confidence { 0.0f };
    std::string standard;     // e.g. "F#m", "A"
    std::string camelot;      // e.g. "11A", "8B"
};

struct SilenceRegion {
    int64_t startFrame { 0 };
    int64_t endFrame   { 0 };
};

/// One fully-populated analysis for a track (all fields at device rate).
struct AnalysisResult {
    AnalysisState state { AnalysisState::NotAnalyzed };
    std::string   error;

    int64_t numFrames    { 0 };
    double  sampleRate   { 0.0 };
    double  durationSec  { 0.0 };

    BpmResult bpm;
    KeyResult key;

    int64_t firstSoundFrame { -1 };   // leading silence
    int64_t lastSoundFrame  { -1 };   // trailing silence
    std::vector<SilenceRegion> silences;

    // Phase 14 — component-level results
    LoudnessResult loudness;
    // Phase 30 — §43/§44
    SpectralResult spectral;
    EnergyResult   energy;
    std::vector<TransientEvent> transients;
    ComponentStatus components[kNumComponents];
    bool needsReview { false };              // BeatGridValidator flag
    std::vector<std::string> reviewReasons;  // human-readable why

    // BeatGrid lives in its own header (editable object); engine fills
    // an initial one from the bpm/onsets.
};

/// User analysis-edit actions (UI/command 0x9000 range → service).
enum class AnalysisEdit : int {
    SetFirstBeat = 0,   // value = frame index (usually playhead)
    NudgeGrid,          // value = delta milliseconds
    ChangeBpm,          // value = new grid BPM (rescales grid)
    DoubleTempo,        // value unused — grid BPM ×2
    HalfTempo,          // value unused — grid BPM ÷2
    SetDownbeat,        // value = beat index that becomes bar-beat 1
    SetBeatsPerBar,     // value = beats per bar
    ResetGrid,          // drop the user override → detected grid
    SetKey,             // value = key index (0–23, -1 clears)
    SetBpmOverride,     // value = BPM override only (-1 clears)
    Reanalyze,          // requeue analysis at high priority
    TapBpm,             // §56 — record one tap timestamp; ≥2 taps
                        // sets a BPM override (value unused)
};

} // namespace onda
