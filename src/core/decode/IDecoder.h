#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace onda {

struct StreamInfo {
    double      sampleRate  { 0.0 };  // file's native rate
    int         channels    { 0 };
    int64_t     totalFrames { -1 };   // in file frames; -1 = unknown
    std::string codecName;
    std::string title;
    std::string artist;
    std::string album;
};

/// Decodes an audio file into interleaved stereo float at a requested
/// output sample rate. Lives entirely on a worker thread — never RT.
class IDecoder {
public:
    virtual ~IDecoder() = default;

    virtual bool open(const std::string& path, double targetSampleRate) = 0;
    virtual StreamInfo info() const = 0;

    /// Decodes up to maxFrames into interleaved stereo float.
    /// Returns frames produced; 0 means EOF (or nothing right now).
    virtual size_t readChunk(float* interleavedOut, size_t maxFrames) = 0;

    /// Frame-accurate-ish seek in the *file's* timeline. Best-effort.
    virtual bool seekToFrame(int64_t frame) = 0;

    virtual bool atEnd() const = 0;
    virtual void close() = 0;
};

} // namespace onda
