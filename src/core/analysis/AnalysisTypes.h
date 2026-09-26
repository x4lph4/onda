#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace onda {

/// Bumped whenever any analyser's output meaning changes; cached
/// analyses with a different version report OUTDATED.
inline constexpr int kAnalysisVersion = 1;

enum class AnalysisState : int {
    NotAnalyzed = 0,
    Analyzing,
    Analyzed,
    Failed,
    Outdated
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

    // BeatGrid lives in its own header (editable object); engine fills
    // an initial one from the bpm/onsets.
};

} // namespace onda
