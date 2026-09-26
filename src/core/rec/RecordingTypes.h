#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace onda::rec {

/// Recording sources. Master is the only live tap today; the rest are
/// reserved so the session/state/command model can express them.
enum class Source : int {
    Master   = 0,
    Channel  = 1,   // per-channel tap (future)
    Cue      = 2,   // headphone cue bus (future)
    External = 3,   // device external input (future)
    Mic      = 4,   // talkover / mic (future)
    Sampler  = 5,   // sampler bus (future)
};

/// Session state machine. Transitions are enforced — see
/// RecordingEngine::canTransit. COMPLETED/FAILED/RECOVERABLE are
/// terminal for a given file; a new run needs a new session.
enum class State : int {
    Idle        = 0,
    Armed       = 1,
    Recording   = 2,
    Paused      = 3,
    Stopping    = 4,
    Finalizing  = 5,
    Completed   = 6,
    Failed      = 7,
    Recoverable = 8,
};

enum class BitDepth : int { Pcm16 = 0, Pcm24 = 1, F32 = 2 };
inline int bytesPerSample(BitDepth d) noexcept {
    return d == BitDepth::Pcm16 ? 2 : d == BitDepth::Pcm24 ? 3 : 4;
}

/// Clip event drained by the app thread — bounded queue.
struct ClipEvent {
    int64_t  frame   = 0;   // recording-time frame index
    int      channel = 0;   // 0/1
    float    peak    = 0.0f;
};

/// Plain-value meter snapshot for UI/test (built app-side, no JUCE).
struct Meter {
    float   peakL    = 0.0f,  peakR    = 0.0f;   // recent block peaks
    float   maxPeakL = 0.0f,  maxPeakR = 0.0f;   // session peaks
    float   rmsL     = 0.0f,  rmsR     = 0.0f;   // running RMS
    int64_t frames   = 0;                       // frames pushed
    double  durationSec = 0.0;
    uint32_t clipCount = 0;
    float   clipPeak   = 0.0f;                  // loudest clipped sample
    int64_t droppedFrames = 0;                  // ring overflow losses
    int     fillPct  = 0;                       // ring occupancy %
};

/// Value describing one finished/in-progress session (mirror of the
/// recording_sessions row + transient fields).
struct SessionInfo {
    int64_t     id        = -1;
    std::string name;
    int64_t     startTs   = 0, endTs = 0;       // unix seconds
    Source      source    = Source::Master;
    double      sampleRate = 0.0;
    BitDepth    bitDepth  = BitDepth::Pcm24;
    int         channels  = 2;
    int         part      = 0;                  // split part index
    std::string filePath;
    std::string format    = "wav";
    int64_t     fileBytes = 0;
    int64_t     frames    = 0;
    double      durationSec = 0.0;
    float       peakLevel = 0.0f;
    uint32_t    clipCount = 0;
    State       state     = State::Idle;
};

/// One "track played" row of the session setlist.
struct TrackPlay {
    int64_t ts        = 0;   // play start, unix seconds
    int     deck      = 0;
    int64_t trackId   = -1;  // library tracks.id, -1 when unknown
    std::string path;
    std::string title;
    std::string artist;
    double  bpm       = 0.0;
    std::string key;
    double  playedSec = 0.0;
};

/// Writer thread health, drained by tick().
struct Diagnostics {
    int64_t droppedFrames = 0;
    int     fillPct       = 0;
    double  writeMs       = 0.0;   // last write() cost
    double  maxWriteMs    = 0.0;   // worst chunk write
    double  diskMBps      = 0.0;   // rolling throughput
    int64_t diskFreeBytes = 0;
    int64_t bytesWritten  = 0;
    bool    diskError     = false;
    bool    diskWarn      = false; // throughput below need
};

} // namespace onda::rec
