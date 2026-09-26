#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace onda::sampler {

/// Recording foundation (task §23). Captures interleaved float audio
/// pushed from an engine tap into a lock-free-ish ring on the RT
/// thread; a control thread drains it to a 32-bit-float WAV file in a
/// temp location. NO source is armed by default — master/cue/mic
/// taps are wired explicitly by the host when a safe source exists.
///
/// Ring is SPSC: audio thread pushes, drain thread pops. Bounded;
/// overrun drops oldest (recorded as diagnostics, never blocks RT).
class SampleRecorder {
public:
    enum class Source : int { None = 0, Master, Cue, External, Mic };

    bool open(const std::string& tempWavPath, double sampleRate) {
        close();
        f_ = std::fopen(tempWavPath.c_str(), "wb");
        if (!f_) return false;
        rate_ = sampleRate;
        frames_ = 0;
        writeHeader(0);          // placeholder, patched on close
        std::fseek(f_, 0, SEEK_END);  // data starts after the header
        return true;
    }
    bool isOpen() const { return f_ != nullptr; }
    Source source() const { return source_; }
    void setSource(Source s) { source_ = s; }   // explicit, off-RT

    /// RT tap: capture interleaved stereo. Bounded, never blocks.
    void push(const float* interleaved, int frames) noexcept {
        if (!armed_.load(std::memory_order_relaxed)) return;
        const int cap = (int)ring_.size() / 2;
        int w = wpos_.load(std::memory_order_relaxed);
        const int r = rpos_.load(std::memory_order_acquire);
        for (int i = 0; i < frames; ++i) {
            const int next = (w + 1) % cap;
            if (next == r) { dropped_.fetch_add(frames - i,
                              std::memory_order_relaxed); return; }
            ring_[(size_t)w * 2]     = interleaved[i * 2];
            ring_[(size_t)w * 2 + 1] = interleaved[i * 2 + 1];
            w = next;
        }
        wpos_.store(w, std::memory_order_release);
    }
    void arm(bool on) { armed_.store(on, std::memory_order_relaxed); }
    bool armed() const { return armed_.load(std::memory_order_relaxed); }

    /// Control thread: drain available audio to file. Returns frames.
    int drainToFile() {
        if (!f_) return 0;
        const int cap = (int)ring_.size() / 2;
        int r = rpos_.load(std::memory_order_relaxed);
        const int w = wpos_.load(std::memory_order_acquire);
        int wrote = 0;
        while (r != w) {
            const float lr[2] = { ring_[(size_t)r * 2],
                                  ring_[(size_t)r * 2 + 1] };
            std::fwrite(lr, sizeof(float), 2, f_);
            r = (r + 1) % cap;
            ++wrote;
        }
        rpos_.store(r, std::memory_order_release);
        frames_ += wrote;
        return wrote;
    }

    /// Control thread: flush, patch header, close. Safe to call once.
    void close() {
        if (!f_) return;
        drainToFile();
        writeHeader(frames_);
        std::fclose(f_);
        f_ = nullptr;
        armed_.store(false, std::memory_order_relaxed);
    }

    int64_t framesWritten() const { return frames_; }
    int64_t framesDropped() const {
        return dropped_.load(std::memory_order_relaxed);
    }

private:
    void writeHeader(int64_t dataFrames) {
        if (!f_) return;
        const long cur = std::ftell(f_);
        std::fseek(f_, 0, SEEK_SET);
        const uint32_t dataBytes = (uint32_t)(dataFrames * 2 * 4);
        const uint32_t fmtBytes = 16;
        const uint32_t riffSize = 4 + (8 + fmtBytes) + (8 + dataBytes);
        auto u32 = [](uint32_t v, FILE* f) {
            uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8),
                             (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
            std::fwrite(b, 1, 4, f);
        };
        auto u16 = [](uint16_t v, FILE* f) {
            uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
            std::fwrite(b, 1, 2, f);
        };
        std::fwrite("RIFF", 1, 4, f_); u32(riffSize, f_);
        std::fwrite("WAVE", 1, 4, f_);
        std::fwrite("fmt ", 1, 4, f_); u32(fmtBytes, f_);
        u16(3, f_);                        // IEEE float
        u16(2, f_);                        // stereo
        u32((uint32_t)rate_, f_);
        u32((uint32_t)(rate_ * 2 * 4), f_);
        u16(8, f_); u16(32, f_);
        std::fwrite("data", 1, 4, f_); u32(dataBytes, f_);
        std::fseek(f_, cur, SEEK_SET);
    }

    FILE*  f_ = nullptr;
    double rate_ = 48000.0;
    int64_t frames_ = 0;
    Source source_ = Source::None;

    // 2 seconds of stereo @48k ring — bounded, preallocated
    std::vector<float> ring_ =
        std::vector<float>((size_t)(48000 * 2 * 2));
    std::atomic<int>     wpos_{0}, rpos_{0};
    std::atomic<int64_t> dropped_{0};
    std::atomic<bool>    armed_{false};
};

} // namespace onda::sampler
