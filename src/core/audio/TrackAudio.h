#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace onda {

/// A fully-decoded track living in RAM (interleaved stereo float at the
/// device sample rate). Produced once by TrackLoader, then IMMUTABLE.
///
/// Ownership: the loader thread owns every instance. The RT thread only
/// borrows raw pointers via the pending/retired slot protocol and never
/// deletes. Once published, the object is read-only — safe to share
/// between RT readers without locks.
struct TrackAudio {
    std::string path;
    std::string title;
    std::string artist;

    double  fileSampleRate { 0.0 };  // rate of the source file
    double  deviceRate     { 0.0 };  // rate of `samples` (== engine rate)
    int     channels       { 2 };
    int64_t numFrames      { 0 };    // frames in `samples` (device rate)

    std::vector<float> samples;      // interleaved stereo, deviceRate

    size_t bytes() const        { return samples.size() * sizeof(float); }
    double durationSec() const  { return deviceRate > 0.0 ? numFrames / deviceRate : 0.0; }
};

} // namespace onda
