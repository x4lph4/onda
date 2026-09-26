#pragma once

#include <cstddef>
#include <cstdint>

namespace onda {

/// Time-stretch backend behind key lock. Implementations must be
/// real-time safe: no allocation/locks inside push/pull (all buffers
/// preallocated in prepare()).
///
/// Model: the deck PUSHES consecutive source frames and PULLS output
/// frames. rate > 1 consumes input faster than output (tempo up, pitch
/// preserved).
class ITimeStretcher {
public:
    virtual ~ITimeStretcher() = default;

    virtual void prepare(double sampleRate, size_t maxBlockFrames) = 0;
    virtual void reset() = 0;
    virtual void setRate(double rate) = 0;
    virtual double rate() const = 0;

    /// Append interleaved stereo input. Returns frames appended (impls
    /// consume everything given; buffering internally).
    virtual size_t push(const float* in, size_t frames) = 0;

    /// Produce interleaved stereo output. Returns frames produced.
    virtual size_t pull(float* out, size_t frames) = 0;

    /// No more input will arrive — drain the tail.
    virtual void finishInput() = 0;

    /// Absolute source index of the current output anchor — used for
    /// position reporting (approximate sounding position).
    virtual int64_t anchorPos() const = 0;

    /// Frames of latency between input consumption and output.
    virtual int latency() const = 0;
};

} // namespace onda
