#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace onda::stems {

/// ------------------------------------------------------------------
/// Stem types (§2). VOCALS/DRUMS/BASS/INSTRUMENTS are the implemented
/// set — the remaining slots are reserved enum values so saved state,
/// pad maps and cache keys stay stable when richer models arrive.
/// ------------------------------------------------------------------
enum class StemType : int {
    Vocals      = 0,
    Drums       = 1,
    Bass        = 2,
    Instruments = 3,
    // ---- reserved for richer models (§2) ----
    Other  = 4,
    Piano  = 5,
    Guitar = 6,
    Synth  = 7,
    Fx     = 8,
    Count
};

constexpr int kStemCount = 4;                 // implemented stems
constexpr int kStemMax   = (int)StemType::Count; // model capacity

inline const char* stemTypeName(StemType t) {
    switch (t) {
        case StemType::Vocals:      return "VOC";
        case StemType::Drums:       return "DRUM";
        case StemType::Bass:        return "BASS";
        case StemType::Instruments: return "INST";
        case StemType::Other:       return "OTHR";
        case StemType::Piano:       return "PIAN";
        case StemType::Guitar:      return "GUIT";
        case StemType::Synth:       return "SYN";
        case StemType::Fx:          return "FX";
        default:                    return "?";
    }
}
inline int stemIndex(StemType t)   { return (int)t; }
inline uint32_t stemBit(StemType t){ return 1u << (int)t; }

/// Quality tiers (§8). Selects model configuration — filter order for
/// the reference band-split model, worker count and GPU usage for
/// future learned models.
enum class StemQuality : int { Fast = 0, Balanced = 1, HighQuality = 2 };
inline const char* stemQualityName(StemQuality q) {
    switch (q) {
        case StemQuality::Fast:        return "fast";
        case StemQuality::Balanced:    return "balanced";
        case StemQuality::HighQuality: return "high";
        default:                       return "?";
    }
}

/// GPU policy (§46): Off forces CPU, Auto uses an accelerator when the
/// provider reports one AND the model supports it, On requests it
/// (still falls back to CPU when unavailable — never blocks).
enum class GpuMode : int { Off = 0, Auto = 1, On = 2 };

/// Job pipeline phases (§4/§43) — surfaced to the UI verbatim.
enum class StemJobPhase : int {
    Queued = 0, Analyzing, Separating, Validating, Caching,
    Ready, Failed, Cancelled
};
inline const char* stemPhaseName(StemJobPhase p) {
    switch (p) {
        case StemJobPhase::Queued:     return "QUEUED";
        case StemJobPhase::Analyzing:  return "ANALYZING";
        case StemJobPhase::Separating: return "SEPARATING";
        case StemJobPhase::Validating: return "VALIDATING";
        case StemJobPhase::Caching:    return "CACHING";
        case StemJobPhase::Ready:      return "READY";
        case StemJobPhase::Failed:     return "FAILED";
        case StemJobPhase::Cancelled:  return "CANCELLED";
        default:                       return "?";
    }
}

/// Per-deck stem availability (§25/§32). `Original` = play the track
/// as-is; separation is never a precondition for playback.
enum class StemAvailability : int {
    Unavailable = 0,   // no cached set, nothing running
    Queued,            // separation scheduled
    Separating,        // running
    Loading,           // set exists; buffers being loaded for the deck
    Ready,             // all requested stems present
    Partial            // some stems present (§26)
};
inline const char* stemAvailName(StemAvailability a) {
    switch (a) {
        case StemAvailability::Unavailable: return "—";
        case StemAvailability::Queued:      return "QUEUED";
        case StemAvailability::Separating:  return "ANALYZING";
        case StemAvailability::Loading:     return "LOADING";
        case StemAvailability::Ready:       return "READY";
        case StemAvailability::Partial:     return "PARTIAL";
        default:                            return "?";
    }
}

/// Explicit failure reasons — a failed model NEVER stops playback
/// (§41); the deck falls back to the original track and the panel
/// shows STEMS UNAVAILABLE.
enum class StemError : int {
    Ok = 0, NoDeck, NoTrack, DecodeFailed, ModelLoadFailed,
    ModelRunFailed, InvalidAudio, CacheWriteFailed, CacheReadFailed,
    Cancelled, Busy, DiskFull, BadKey
};

/// Component-level status (§3) — analysis/separation/waveform/cache
/// are tracked separately so the UI can show honest partial state.
enum class StemCompState : int { None = 0, Pending, Done, Failed };

/// One stem's full control+status state (§3).
struct Stem {
    int     id           { -1 };
    StemType type        { StemType::Vocals };
    bool    present      { false };  // buffer exists in the loaded set
    bool    enabled      { true };   // participates when stems mode on
    bool    muted        { false };
    bool    solo         { false };
    float   gain         { 1.0f };   // linear 0..2
    float   pan          { 0.0f };   // -1..+1
    int64_t position     { 0 };      // source frames (aligned to track)
    int64_t duration     { 0 };      // source frames
    int     analysisStatus   { 0 };  // StemCompState
    int     separationStatus { 0 };
    int     waveformStatus   { 0 };
    int     cacheStatus      { 0 };
};

/// ------------------------------------------------------------------
/// Internal audio format (§10): interleaved float32 stereo at the
/// DEVICE rate — identical to TrackAudio so stem buffers drop into the
/// deck render path without resampling. All stems share one timeline
/// with the source track (§11): same numFrames, frame 0 aligned.
/// ------------------------------------------------------------------
struct StemSet {
    double    rate      { 0.0 };      // device rate of `samples`
    int       channels  { 2 };
    int64_t   numFrames { 0 };
    /// Presence mask — bit per StemType. Partial sets are legal (§26):
    /// a model may legitimately produce fewer than kStemCount stems.
    uint32_t  mask      { 0 };
    /// Interleaved stereo buffers, one per present stem.
    std::array<std::vector<float>, kStemMax> samples;

    const float* ptr(StemType t) const noexcept {
        const int i = stemIndex(t);
        return (mask & (1u << i)) ? samples[(size_t)i].data() : nullptr;
    }
    float* ptr(StemType t) noexcept {
        const int i = stemIndex(t);
        return (mask & (1u << i)) ? samples[(size_t)i].data() : nullptr;
    }
    bool has(StemType t) const noexcept { return ptr(t) != nullptr; }
    size_t bytes() const {
        size_t n = 0;
        for (auto& s : samples) n += s.size() * sizeof(float);
        return n;
    }
};

/// Identity of a cached set (§27). `key` is derived from ALL fields —
/// changing any invalidates the cache entry (§28).
struct StemSetMeta {
    int64_t     trackId     { 0 };
    uint64_t    contentHash { 0 };     // track identity (relocation-safe)
    std::string modelId;
    std::string modelVersion;
    int         quality   { (int)StemQuality::Balanced };
    double      rate      { 0.0 };
    int         channels  { 2 };
    int64_t     frames    { 0 };
    uint32_t    mask      { 0 };
    int64_t     createdAt { 0 };
    std::string key;                   // hex cache key / dir name
};

/// Separation job descriptor + live status.
struct StemJob {
    int64_t    id          { 0 };
    int64_t    trackId     { 0 };
    uint64_t   contentHash { 0 };
    std::string path;                 // source file (decoded by worker)
    int        quality    { (int)StemQuality::Balanced };
    int        priority   { 0 };
    double     targetRate { 0.0 };
    int        kind       { 0 };      // 0 separate, 1 load-from-cache
    std::string modelId, modelVersion, cacheKey;  // load-job fields
    StemJobPhase phase    { StemJobPhase::Queued };
    float      progress   { 0.0f };
    std::string error;
};

/// Engine configuration (§45/§46).
struct StemConfig {
    std::string cacheDir;             // <dbDir>/stems
    int     workers       { 1 };      // separation threads (CPU §45)
    int     gpuMode       { (int)GpuMode::Auto };
    int     defaultQuality{ (int)StemQuality::Balanced };
    bool    autoSeparate  { false };  // separate on deck load when absent
    int     cpuLimitPct   { 75 };     // gate: above → workers yield
};

} // namespace onda::stems
