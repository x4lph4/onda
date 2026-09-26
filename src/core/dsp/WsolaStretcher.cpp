#include "WsolaStretcher.h"

#include "Biquad.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace onda {

void WsolaStretcher::prepare(double sampleRate, size_t /*maxBlock*/) {
    sr_ = sampleRate;
    // Window ~43 ms, power of two.
    w_ = 2048;
    while (w_ > 512 && w_ > (size_t)(sr_ * 0.043)) w_ >>= 1;
    hs_ = w_ / 2;
    delta_ = w_ / 8;

    inCap_  = w_ * 8;
    outCap_ = w_ * 8;
    inBuf_.assign(inCap_ * 2, 0.0f);
    outBuf_.assign(outCap_ * 2, 0.0f);
    refBuf_.assign(hs_ * 2, 0.0f);

    win_.resize(w_);
    for (size_t i = 0; i < w_; ++i) // Hann
        win_[i] = 0.5f - 0.5f * std::cos(2.0 * kPi * (double)i / (double)(w_ - 1));

    setRate(1.0);
    reset();
}

void WsolaStretcher::reset() {
    std::fill(inBuf_.begin(), inBuf_.end(), 0.0f);
    std::fill(outBuf_.begin(), outBuf_.end(), 0.0f);
    inBase_ = 0; inCount_ = 0; prevStart_ = -1;
    outWriteAbs_ = outReadAbs_ = outFinalAbs_ = anchorAbs_ = 0;
    inputEnded_ = tailDone_ = false;
}

void WsolaStretcher::setRate(double r) {
    rate_ = r;
    ha_ = r * (double)hs_;
}

float WsolaStretcher::inSample(int64_t absFrame, int ch) const noexcept {
    return inBuf_[((size_t)(absFrame % (int64_t)inCap_)) * 2 + (size_t)ch];
}

void WsolaStretcher::discardInputBefore(int64_t absFrame) noexcept {
    if (absFrame <= inBase_) return;
    const int64_t drop = std::min<int64_t>(absFrame - inBase_, (int64_t)inCount_);
    inBase_ += drop;
    inCount_ -= (size_t)drop;
}

float WsolaStretcher::corr(int64_t refStart, int64_t candStart) noexcept {
    float acc = 0.0f;
    for (size_t i = 0; i < hs_; ++i) {
        const float r = inSample(refStart + (int64_t)i, 0) + inSample(refStart + (int64_t)i, 1);
        const float c = inSample(candStart + (int64_t)i, 0) + inSample(candStart + (int64_t)i, 1);
        acc += r * c;
    }
    return acc;
}

int64_t WsolaStretcher::bestAlign(int64_t nominal, int64_t searchFrom, int64_t availEnd) noexcept {
    // Reference: tail of the previous grain still overlapping the new one.
    const int64_t refStart = prevStart_ + (int64_t)hs_; // length hs_ == W - Hs
    int64_t lo = std::max(nominal - (int64_t)delta_, searchFrom);
    int64_t hi = std::min(nominal + (int64_t)delta_, availEnd - (int64_t)w_);
    if (hi <= lo) return lo;

    int64_t best = lo;
    float bestC = corr(refStart, lo);
    for (int64_t c = lo + 1; c <= hi; ++c) {
        const float v = corr(refStart, c);
        if (v > bestC) { bestC = v; best = c; }
    }
    return best;
}

size_t WsolaStretcher::push(const float* in, size_t frames) {
    const size_t freeSpace = inCap_ - inCount_;
    const size_t n = std::min(frames, freeSpace);
    for (size_t i = 0; i < n; ++i) {
        const int64_t pos = inBase_ + (int64_t)inCount_ + (int64_t)i;
        const size_t idx = ((size_t)(pos % (int64_t)inCap_)) * 2;
        inBuf_[idx]     = in[i * 2];
        inBuf_[idx + 1] = in[i * 2 + 1];
    }
    inCount_ += n;
    produceGrains();
    return n;
}

void WsolaStretcher::produceGrains() {
    while (true) {
        int64_t nominal = prevStart_ < 0
            ? inBase_
            : prevStart_ + (int64_t)llround(ha_);
        const int64_t availEnd = inBase_ + (int64_t)inCount_;
        int64_t needed = nominal + (int64_t)delta_ + (int64_t)w_;

        if (availEnd < needed) {
            if (!inputEnded_ || tailDone_) return;
            if (availEnd - inBase_ < (int64_t)w_) { tailDone_ = true; return; }
            nominal = inBase_; // last flush grain
        }

        const int64_t align = prevStart_ < 0
            ? nominal
            : bestAlign(nominal, inBase_, availEnd);

        // Overlap-add the grain into the output ring.
        const int64_t base = outWriteAbs_;
        for (size_t i = 0; i < w_; ++i) {
            const size_t o = ((size_t)((base + (int64_t)i) % (int64_t)outCap_)) * 2;
            const float wn = win_[i];
            outBuf_[o]     += wn * inSample(align + (int64_t)i, 0);
            outBuf_[o + 1] += wn * inSample(align + (int64_t)i, 1);
        }
        outWriteAbs_ += (int64_t)hs_;
        outFinalAbs_  = outWriteAbs_; // everything before this is finalized
        prevStart_    = align;
        anchorAbs_    = align + (int64_t)hs_;

        // Input before `align` can never be referenced again.
        discardInputBefore(align);
    }
}

size_t WsolaStretcher::pull(float* out, size_t frames) {
    const size_t avail = (size_t)std::max<int64_t>(0, outFinalAbs_ - outReadAbs_);
    const size_t n = std::min(frames, avail);
    for (size_t i = 0; i < n; ++i) {
        const size_t idx = ((size_t)((outReadAbs_ + (int64_t)i) % (int64_t)outCap_)) * 2;
        out[i * 2]     = outBuf_[idx];
        out[i * 2 + 1] = outBuf_[idx + 1];
        // Clear the slot after reading: a future grain will OLA-add into it
        // once the write cursor wraps around the ring.
        outBuf_[idx] = outBuf_[idx + 1] = 0.0f;
    }
    outReadAbs_ += (int64_t)n;
    return n;
}

} // namespace onda
