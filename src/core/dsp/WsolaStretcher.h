#pragma once

#include <cstdint>
#include <vector>

#include "../engine/ITimeStretcher.h"

namespace onda {

/// WSOLA time-stretcher (waveform similarity overlap-add).
///
/// Consumes source frames faster/slower than it emits output, preserving
/// pitch. Single-threaded by design (the deck's RT thread both pushes and
/// pulls); every buffer is preallocated in prepare() — zero allocation,
/// zero locks on the audio path.
///
/// Grain model: synthesis hop Hs = W/2; each grain consumes ~rate*Hs of
/// input; the grain start is aligned within +-W/8 by correlation against
/// the tail of the previous input frame.
class WsolaStretcher : public ITimeStretcher {
public:
    void prepare(double sampleRate, size_t maxBlockFrames) override;
    void reset() override;
    void setRate(double rate) override;
    double rate() const override { return rate_; }

    size_t push(const float* in, size_t frames) override;
    size_t pull(float* out, size_t frames) override;
    void finishInput() override { inputEnded_ = true; }

    int64_t anchorPos() const override { return anchorAbs_; }
    int     latency()  const override { return (int)w_; }

private:
    void produceGrains();
    int64_t bestAlign(int64_t nominal, int64_t searchFrom, int64_t availEnd) noexcept;
    float corr(int64_t refStart, int64_t candStart) noexcept;
    float inSample(int64_t absFrame, int ch) const noexcept;
    void  discardInputBefore(int64_t absFrame) noexcept;

    // ---- params ----
    size_t w_     { 2048 };   // grain window
    size_t hs_    { 1024 };   // synthesis hop
    size_t delta_ { 256 };    // search radius
    double rate_  { 1.0 };
    double ha_    { 1024.0 }; // analysis hop = rate * hs
    double sr_    { 48000.0 };

    // ---- circular buffers, indexed by ABSOLUTE frame index % cap ----
    std::vector<float> inBuf_, outBuf_, win_, refBuf_;
    size_t inCap_  { 0 };
    size_t outCap_ { 0 };

    // ---- stream cursors ----
    int64_t inBase_       { 0 };   // absolute index of inBuf slot 0
    size_t  inCount_      { 0 };   // frames present in inBuf
    int64_t prevStart_    { -1 };  // absolute start of last grain
    int64_t outWriteAbs_  { 0 };   // next output index to receive OLA
    int64_t outReadAbs_   { 0 };   // next output index to emit
    int64_t outFinalAbs_  { 0 };   // output finalized up to here
    int64_t anchorAbs_    { 0 };   // sounding position (input index)

    bool inputEnded_ { false };
    bool tailDone_   { false };
};

} // namespace onda
