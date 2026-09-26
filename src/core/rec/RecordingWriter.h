#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/rec/RecordingBuffer.h"
#include "core/rec/RecordingEncoder.h"
#include "core/rec/RecordingSession.h"

namespace onda::rec {

/// Background writer: drains the SPSC ring into a WavEncoder in fixed
/// chunks, journals progress, measures write latency/throughput, and
/// exposes health via atomics. The only owner of FILE*/disk IO.
class RecordingWriter {
public:
    RecordingWriter() = default;
    ~RecordingWriter() { stop(); }
    RecordingWriter(const RecordingWriter&) = delete;

    /// Starts the writer thread on `wavPath`. The journal is opened by
    /// the caller (so recovery metadata exists before frames flow).
    bool start(const std::string& wavPath, double rate, BitDepth depth,
               int channels, SpscRing& ring, Journal& journal) {
        if (th_.joinable()) return false;
        ring_ = &ring; journal_ = &journal;
        enc_ = std::make_unique<WavEncoder>();
        if (!enc_->open(wavPath, rate, channels, depth)) {
            enc_.reset(); return false;
        }
        wavPath_ = wavPath; rate_ = rate;
        depth_ = depth; channels_ = channels;
        stopFlag_.store(false);
        done_.store(false); failed_.store(false);
        splitFlag_.store(false);
        th_ = std::thread([this] { run(); });
        return true;
    }

    /// Signals the loop to drain the ring, finalize the file, and exit.
    /// Joins the thread. Idempotent.
    void stop() {
        if (!th_.joinable()) return;
        stopFlag_.store(true);
        th_.join();
    }

    /// Requested by the engine (session split / size cap): writer
    /// finalizes the current encoder and opens `nextPath` without
    /// draining the ring — no frame gap.
    void requestSplit(const std::string& nextPath) {
        std::lock_guard<std::mutex> g(pathMx_);
        pendingSplitPath_ = nextPath;
        splitFlag_.store(true);
    }

    // ---- telemetry (any thread) ----
    const Diagnostics& diag() const { return diagSnap_; }
    bool done()    const { return done_.load(); }
    bool failed()  const { return failed_.load(); }
    std::string    error() const {
        std::lock_guard<std::mutex> g(errMx_); return err_; }
    /// Completed file parts (filled as splits finalize).
    std::vector<SessionInfo> parts() {
        std::lock_guard<std::mutex> g(partsMx_); return parts_; }

private:
    void run() {
        std::vector<float> staging(8192 * 2);     // 8192-frame chunks
        auto t0 = std::chrono::steady_clock::now();
        int64_t bytes0 = 0;
        int chunkN = 0;
        for (;;) {
            const bool stop = stopFlag_.load(std::memory_order_acquire);
            const size_t n = ring_->pop(staging.data(), 8192);
            if (n > 0) {
                const auto w0 = std::chrono::steady_clock::now();
                const bool ok = enc_->writeFrames(staging.data(), n);
                const double ms =
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - w0).count();
                writeMs_.store(ms);
                if (ms > maxWriteMs_.load()) maxWriteMs_.store(ms);
                if (!ok) {
                    std::lock_guard<std::mutex> g(errMx_);
                    if (err_.empty()) err_ = "disk write failed";
                    failed_.store(true); diskErr_.store(true);
                    break;                       // leave file as-is → recovery
                }
            }
            // split request — swap file under our own thread
            if (splitFlag_.load(std::memory_order_acquire)) {
                splitFlag_.store(false);
                std::string next;
                { std::lock_guard<std::mutex> g(pathMx_);
                  next = pendingSplitPath_; }
                if (!next.empty() && enc_) {
                    enc_->finalize();
                    { std::lock_guard<std::mutex> g(partsMx_);
                      parts_.push_back({});        // engine fills details
                      parts_.back().filePath = wavPath_;
                      parts_.back().fileBytes = enc_->bytes() + 44;
                      parts_.back().frames    = enc_->frames(); }
                    auto e2 = std::make_unique<WavEncoder>();
                    if (e2->open(next, rate_, channels_, depth_)) {
                        journal_->log("split", next);
                        enc_ = std::move(e2); wavPath_ = next;
                    } else {
                        std::lock_guard<std::mutex> g(errMx_);
                        err_ = "split file open failed";
                        failed_.store(true); diskErr_.store(true);
                        break;
                    }
                }
            }
            if (enc_ && enc_->needsSplit())
                sizeSplit_.store(true);           // engine polls + requests
            if (stop && n == 0) break;            // drained
            if (n == 0 && !stop)
                std::this_thread::sleep_for(
                    std::chrono::microseconds(500));
            // rolling throughput + journal heartbeat each ~1s
            if (++chunkN >= 64) {
                chunkN = 0;
                const auto t1 = std::chrono::steady_clock::now();
                const double dt =
                    std::chrono::duration<double>(t1 - t0).count();
                const int64_t b = enc_ ? enc_->bytes() : 0;
                mbps_.store(dt > 0 ? (double)(b - bytes0) / dt / 1e6
                                   : 0.0);
                bytes0 = b; t0 = t1;
                bytesWritten_.store(b);
                fill_.store(ring_->fillPct());
                journal_->log("chunk",
                              "bytes=" + std::to_string(b));
            }
        }
        if (enc_) {
            journal_->log("finalize_started");
            enc_->finalize();
            { std::lock_guard<std::mutex> g(partsMx_);
              parts_.push_back({});
              parts_.back().filePath = wavPath_;
              parts_.back().fileBytes = enc_->bytes() + 44;
              parts_.back().frames    = enc_->frames(); }
            if (!failed_.load())
                journal_->log("finalize_completed");
        }
        done_.store(true);
    }

    SpscRing* ring_ = nullptr;
    Journal*  journal_ = nullptr;
    std::unique_ptr<WavEncoder> enc_;
    std::string wavPath_;
    double rate_ = 0.0;
    BitDepth depth_ = BitDepth::Pcm24;
    int channels_ = 2;
    std::thread th_;
    std::atomic<bool> stopFlag_{false}, done_{false}, failed_{false};
    std::atomic<bool> diskErr_{false}, splitFlag_{false};
    std::atomic<bool> sizeSplit_{false};
    std::atomic<double> writeMs_{0}, maxWriteMs_{0}, mbps_{0};
    std::atomic<int64_t> bytesWritten_{0};
    std::atomic<int> fill_{0};
    std::string pendingSplitPath_;  std::mutex pathMx_;
    mutable std::mutex errMx_;      std::string err_;
    std::mutex partsMx_;            std::vector<SessionInfo> parts_;
    Diagnostics diagSnap_;

public:
    /// Snapshot of the atomic stats — rebuilt by the engine's tick.
    Diagnostics snapshot(int64_t diskFree, int64_t dropped,
                         double needMBps) const {
        Diagnostics d;
        d.writeMs  = writeMs_.load();  d.maxWriteMs = maxWriteMs_.load();
        d.diskMBps = mbps_.load();     d.bytesWritten = bytesWritten_.load();
        d.fillPct  = fill_.load();     d.droppedFrames = dropped;
        d.diskFreeBytes = diskFree;    d.diskError = diskErr_.load();
        d.diskWarn = needMBps > 0.0 && d.diskMBps > 0.0
                  && d.diskMBps < needMBps * 0.9;
        return d;
    }
    bool sizeSplitWanted() { return sizeSplit_.exchange(false); }
    int64_t encBytes() const { return bytesWritten_.load(); }
};

} // namespace onda::rec
