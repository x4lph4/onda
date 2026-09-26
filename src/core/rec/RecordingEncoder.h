#pragma once

#include <cstdio>
#include <cstring>
#include <string>

#include "core/rec/RecordingTypes.h"

namespace onda::rec {

/// PCM WAV encoder — RIFF/WAVE with a placeholder header patched on
/// finalize(). Writes run on the writer thread only. 16/24-bit use
/// format tag 1 (PCM), 32-bit float tag 3 (IEEE float). Data beyond
/// the classic 4 GiB RIFF limit is the caller's concern (the engine
/// auto-splits before reaching it — see kMaxWavData).
class WavEncoder {
public:
    /// 3.8 GiB data ceiling — safe margin below the 4 GiB RIFF limit.
    static constexpr int64_t kMaxWavData = 3800LL * 1000 * 1000;

    bool open(const std::string& path, double rate, int ch,
              BitDepth depth) {
        rate_ = rate; ch_ = ch > 0 ? ch : 2; depth_ = depth;
        bytesPerFrame_ = ch_ * bytesPerSample(depth);
        f_ = std::fopen(path.c_str(), "wb");
        if (!f_) return false;
        writeHeader(0);
        return true;
    }

    /// Appends n interleaved float frames (converted to bit depth).
    /// Returns false on write error (disk full etc.) — ferror set.
    bool writeFrames(const float* inter, size_t n) {
        if (!f_ || n == 0) return f_ != nullptr;
        const int bps = bytesPerSample(depth_);
        const size_t outBytes = n * (size_t)bytesPerFrame_;
        scratch_.resize(outBytes);        // amortised — one thread
        unsigned char* dst = scratch_.data();
        const float* src = inter;
        for (size_t i = 0; i < n * (size_t)ch_; ++i) {
            float v = src[i];
            if (v >  1.0f) v =  1.0f;
            if (v < -1.0f) v = -1.0f;
            // 24-bit scale — +1.0 clamps at 8388607 (0x7FFFFF), not
            // 8388608 which would sign-wrap to full-scale negative
            int32_t q = (int32_t)(v * 8388608.0f);
            if (q >  8388607)  q =  8388607;
            if (q < -8388608)  q = -8388608;
            if (depth_ == BitDepth::Pcm16) {
                const int16_t s = (int16_t)(v * 32767.0f);
                *dst++ = (unsigned char)(s & 0xFF);
                *dst++ = (unsigned char)((s >> 8) & 0xFF);
            } else if (depth_ == BitDepth::Pcm24) {
                *dst++ = (unsigned char)(q & 0xFF);
                *dst++ = (unsigned char)((q >> 8) & 0xFF);
                *dst++ = (unsigned char)((q >> 16) & 0xFF);
            } else {                                    // 32f
                std::memcpy(dst, &v, 4); dst += 4;
            }
        }
        const size_t got = std::fwrite(scratch_.data(), 1,
                                       outBytes, f_);
        dataBytes_ += (int64_t)got;
        return got == outBytes;
    }

    /// Patches RIFF/data sizes and closes. Safe to call once.
    void finalize() {
        if (!f_) return;
        std::fflush(f_);
        writeHeader(dataBytes_);
        std::fclose(f_);
        f_ = nullptr;
    }
    bool open_()    const { return f_ != nullptr; }
    int64_t bytes() const { return dataBytes_; }
    int64_t frames() const {
        return bytesPerFrame_ ? dataBytes_ / bytesPerFrame_ : 0; }
    int bytesPerFrame() const { return bytesPerFrame_; }
    /// True once data reaches the split threshold.
    bool needsSplit() const { return dataBytes_ >= kMaxWavData; }

    /// Reads back a WAV header for validation. Returns false when the
    /// file is missing, too small, or has a corrupt RIFF/WAVE prefix.
    static bool validate(const std::string& path, double* rateOut = nullptr,
                         int* chOut = nullptr, int64_t* framesOut = nullptr,
                         int* bitsOut = nullptr) {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        unsigned char h[44];
        const bool ok = std::fread(h, 1, 44, f) == 44
                     && std::memcmp(h, "RIFF", 4) == 0
                     && std::memcmp(h + 8, "WAVE", 4) == 0;
        if (!ok) { std::fclose(f); return false; }
        const int ch  = h[22] | (h[23] << 8);
        const int rate= h[24] | (h[25] << 8) | (h[26] << 16)
                      | (h[27] << 24);
        const uint16_t bits = h[34] | (h[35] << 8);
        const uint32_t data = (uint32_t)h[40] | ((uint32_t)h[41] << 8)
                            | ((uint32_t)h[42] << 16)
                            | ((uint32_t)h[43] << 24);
        const int bpf = ch * ((int)bits / 8);
        if (rateOut)   *rateOut   = (double)rate;
        if (chOut)     *chOut     = ch;
        if (bitsOut)   *bitsOut   = (int)bits;
        if (framesOut) *framesOut = bpf ? (int64_t)data / bpf : 0;
        std::fclose(f);
        return rate > 0 && ch > 0 && bpf > 0;
    }

    /// Re-patches ONLY the RIFF/data size fields of an interrupted
    /// file in place — fmt/rate/depth bytes are left untouched (the
    /// writer already laid them down correctly at open).
    static bool patchHeader(const std::string& path,
                            int64_t dataBytes) {
        FILE* f = std::fopen(path.c_str(), "r+b");
        if (!f) return false;
        const uint32_t data32 = (uint32_t)(dataBytes & 0xFFFFFFFFu);
        const uint32_t riff   = 36 + data32;
        unsigned char b[4];
        auto put = [&](uint32_t v) {
            b[0] = (unsigned char)(v & 0xFF);
            b[1] = (unsigned char)((v >> 8) & 0xFF);
            b[2] = (unsigned char)((v >> 16) & 0xFF);
            b[3] = (unsigned char)(v >> 24);
        };
        put(riff);
        std::fseek(f, 4, SEEK_SET);
        const bool a = std::fwrite(b, 1, 4, f) == 4;
        put(data32);
        std::fseek(f, 40, SEEK_SET);
        const bool bOk = std::fwrite(b, 1, 4, f) == 4;
        std::fclose(f);
        return a && bOk;
    }

private:
    /// Writes the 44-byte header at position 0 and restores the file
    /// pointer to end-of-data (append position).
    void writeHeader(int64_t dataBytes) {
        const int bps   = bytesPerSample(depth_);
        const int bits  = bps * 8;
        const int tag   = depth_ == BitDepth::F32 ? 3 : 1;
        const uint32_t data32 = (uint32_t)(dataBytes & 0xFFFFFFFFu);
        const uint32_t riff   = 36 + data32;
        const uint32_t byteRate = (uint32_t)(rate_ * ch_ * bps);
        const uint16_t blockAlign = (uint16_t)(ch_ * bps);
        unsigned char h[44] = {0};
        std::memcpy(h,      "RIFF", 4); put32(h + 4,  riff);
        std::memcpy(h + 8,  "WAVE", 4);
        std::memcpy(h + 12, "fmt ", 4); put32(h + 16, 16);
        put16(h + 20, (uint16_t)tag);
        put16(h + 22, (uint16_t)ch_);
        put32(h + 24, (uint32_t)rate_);
        put32(h + 28, byteRate);
        put16(h + 32, blockAlign);
        put16(h + 34, (uint16_t)bits);
        std::memcpy(h + 36, "data", 4); put32(h + 40, data32);
        std::fseek(f_, 0, SEEK_SET);
        std::fwrite(h, 1, 44, f_);
        std::fseek(f_, 0, SEEK_END);
    }
    static void put16(unsigned char* p, uint16_t v) {
        p[0] = (unsigned char)(v & 0xFF); p[1] = (unsigned char)(v >> 8); }
    static void put32(unsigned char* p, uint32_t v) {
        p[0] = (unsigned char)(v & 0xFF); p[1] = (unsigned char)((v >> 8) & 0xFF);
        p[2] = (unsigned char)((v >> 16) & 0xFF);
        p[3] = (unsigned char)(v >> 24);
    }

    FILE*   f_ = nullptr;
    double  rate_ = 0;
    int     ch_ = 2;
    BitDepth depth_ = BitDepth::Pcm24;
    int     bytesPerFrame_ = 6;
    int64_t dataBytes_ = 0;
    std::vector<unsigned char> scratch_;
};

} // namespace onda::rec
