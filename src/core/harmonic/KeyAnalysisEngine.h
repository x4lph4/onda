#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "../analysis/MusicAnalysisEngine.h"  // DecoderFactory
#include "../decode/IDecoder.h"
#include "../data/Database.h"
#include "HarmonicTypes.h"
#include "KeyAnalysisCache.h"
#include "KeyAnalysisProvider.h"
#include "KeyDiagnostics.h"

namespace onda::harmonic {

/// ------------------------------------------------------------------
/// §1/§5/§33/§34 — key-analysis coordinator. Owns the detector (via
/// provider), the cache, diagnostics and ONE background worker for
/// standalone key (re)analysis — e.g. HIGH_QUALITY re-runs with the
/// segment pass, or re-analysis after an algorithm upgrade.
///
/// - Worker thread only; nothing here runs on the audio callback.
/// - CPU gate: `setPlaybackActive(true)` parks the worker during
///   playback (§34 — AudioEngine > UI > background analysis).
/// - Jobs are cancellable; cancel is cooperative.
/// - Full-track analysis still flows through MusicAnalysisEngine —
///   this engine is for key-only passes and library refresh.
/// ------------------------------------------------------------------
class KeyAnalysisEngine {
public:
    struct Job {
        int64_t trackId     { -1 };
        std::string path;
        int         quality { (int)KeyQuality::Balanced };
        std::shared_ptr<std::atomic<bool>> cancel;
        uint64_t    contentHash { 0 };   // identity for cache rows
    };
    using DoneCb = std::function<void(int64_t trackId,
                                      const KeyAnalysisResult& r)>;

    explicit KeyAnalysisEngine(Database* db = nullptr) : db_(db) {}

    ~KeyAnalysisEngine() { stopWorker(); }

    void setDatabase(Database& db) { db_ = &db; }
    void setDecoderFactory(DecoderFactory f) { decFactory_ = std::move(f); }
    void setDoneCallback(DoneCb cb) {
        std::lock_guard<std::mutex> lk(mu_); done_ = std::move(cb);
    }
    /// §34 — during playback the worker sleeps between jobs.
    void setPlaybackActive(bool v) { playbackActive_.store(v); cv_.notify_all(); }

    KeyDiagnostics::Snapshot diagnostics() const { return diag_.snapshot(); }

    /// Queue a key-only analysis pass (async, worker thread).
    /// `decoder` supplies audio; without one the job fails cleanly.
    void request(Job j) {
        if (!j.cancel) j.cancel = std::make_shared<std::atomic<bool>>(false);
        {
            std::lock_guard<std::mutex> lk(mu_);
            jobs_.push_back(std::move(j));
        }
        ensureWorker();
        cv_.notify_one();
    }

    /// Cancel every queued/running job.
    void cancelAll() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (auto& j : jobs_) if (j.cancel) j.cancel->store(true);
        }
        cancelCurrent_.store(true);
        cv_.notify_all();
    }

    /// Synchronous analysis of a decoded buffer (tests + queue reuse).
    KeyAnalysisResult analyzeBuffer(const float* samples,
                                    int64_t numFrames, int channels,
                                    double sampleRate,
                                    const KeyAnalysisConfig& cfg,
                                    const std::atomic<bool>* cancel = nullptr) {
        auto det = provider_.createDetector(cfg);
        KeyAnalysisResult r;
        diag_.onStart();
        const auto t0 = std::chrono::steady_clock::now();
        if (cancel && cancel->load()) {
            diag_.onDone(false, true, false, false, 0.0);
            return r;
        }
        try {
            r = det->analyze(samples, numFrames, channels, sampleRate, cfg);
        } catch (...) {
            diag_.onError("detector threw");
            diag_.onDone(false, false, false, false, 0.0);
            return {};
        }
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        diag_.onDone(true, false,
                     r.primary.confidence < 0.25f, r.ambiguous, ms);
        return r;
    }

    /// Decode a file (48 kHz stereo reference) and analyze it.
    KeyAnalysisResult analyzeFile(const std::string& path,
                                  const KeyAnalysisConfig& cfg,
                                  const std::atomic<bool>* cancel = nullptr) {
        KeyAnalysisResult r;
        auto dec = decFactory_ ? decFactory_() : nullptr;
        if (!dec || !dec->open(path, 48000.0)) {
            diag_.onStart();
            diag_.onError("cannot decode: " + path);
            diag_.onDone(false, false, false, false, 0.0);
            return r;
        }
        std::vector<float> pcm, chunk(8192 * 2);
        while (true) {
            if (cancel && cancel->load()) {
                dec->close();
                diag_.onStart(); diag_.onDone(false, true, false, false, 0.0);
                return r;
            }
            const size_t got = dec->readChunk(chunk.data(), 8192);
            if (got == 0) break;
            pcm.insert(pcm.end(), chunk.begin(),
                       chunk.begin() + (ptrdiff_t)(got * 2));
        }
        dec->close();
        if (pcm.empty()) {
            diag_.onStart(); diag_.onError("empty decode: " + path);
            diag_.onDone(false, false, false, false, 0.0);
            return r;
        }
        return analyzeBuffer(pcm.data(), (int64_t)pcm.size() / 2, 2,
                             48000.0, cfg, cancel);
    }

    /// Cache access (valid only when a Database is bound).
    std::optional<KeyAnalysisResult> loadCached(
        int64_t trackId, const std::string& algo,
        const std::string& version) {
        if (!db_) return std::nullopt;
        auto r = KeyAnalysisCache(*db_).load(trackId);
        diag_.onCache(r && KeyAnalysisCache::valid(*r, algo, version));
        if (r && KeyAnalysisCache::valid(*r, algo, version)) return r;
        return std::nullopt;
    }
    void saveResult(int64_t trackId, const KeyAnalysisResult& r) {
        if (db_) KeyAnalysisCache(*db_).save(trackId, r);
    }

private:
    void ensureWorker() {
        if (worker_.joinable()) return;
        diag_.setWorkers(1);
        worker_ = std::thread([this] { workerMain(); });
    }
    void stopWorker() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stopping_ = true;
        }
        cancelCurrent_.store(true);
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }
    void workerMain() {
        while (true) {
            Job j;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [&] {
                    return stopping_ ||
                           (!jobs_.empty() && !playbackActive_.load());
                });
                if (stopping_) return;
                j = std::move(jobs_.front());
                jobs_.pop_front();
            }
            cancelCurrent_.store(false);
            auto cfg = provider_.configFor(j.quality);
            KeyAnalysisResult r = analyzeFile(
                j.path, cfg, j.cancel ? j.cancel.get() : &cancelCurrent_);
            if (r.primary.valid() || r.analyzedAt > 0) {
                if (db_ && j.trackId >= 0)
                    KeyAnalysisCache(*db_).save(j.trackId, r);
                DoneCb cb;
                { std::lock_guard<std::mutex> lk(mu_); cb = done_; }
                if (cb) cb(j.trackId, r);
            }
        }
    }

    Database*               db_ { nullptr };
    KeyAnalysisProvider     provider_;
    KeyDiagnostics          diag_;
    DecoderFactory          decFactory_;
    DoneCb                  done_;
    std::atomic<bool>       playbackActive_ { false };
    std::atomic<bool>       cancelCurrent_ { false };
    std::atomic<bool>       stopping_ { false };

    std::mutex              mu_;
    std::condition_variable cv_;
    std::deque<Job>         jobs_;
    std::thread             worker_;
};

} // namespace onda::harmonic
