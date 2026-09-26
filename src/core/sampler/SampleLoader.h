#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "../decode/IDecoder.h"
#include "../engine/TrackLoader.h"   // DecoderFactory
#include "SamplerTypes.h"

namespace onda::sampler {

/// Async sample decoding (task §3/§4/§26). One worker thread, FIFO
/// job queue; decoded SampleData + meta are handed to a completion
/// callback on the CALLER's chosen thread (the engine drains results
/// — it owns publish-to-RT). Never touches the audio thread.
///
/// Analysis per load: duration, sample rate, channels, peak, coarse
/// peak waveform (SampleData::wave). BPM is a caller-supplied hint —
/// heavy tempo analysis stays in the analysis engine (task §4).
class SampleLoader {
public:
    struct Job {
        int         slot     = -1;
        int64_t     requestId = 0;
        std::string path;
        double      bpmHint  = 0.0;
    };
    struct Result {
        Job    job;
        std::shared_ptr<const SampleData> data;   // null on failure
        std::string error;
    };
    using DoneFn = std::function<void(Result&&)>;

    explicit SampleLoader(DecoderFactory factory, double hostRate = 48000.0)
        : factory_(std::move(factory)), rate_(hostRate) {}
    ~SampleLoader() { stop(); }

    void start(DoneFn done) {
        done_ = std::move(done);
        if (running_.exchange(true)) return;
        worker_ = std::thread([this] { run(); });
    }
    void stop() {
        if (!running_.exchange(false)) return;
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    /// app thread: enqueue a decode (drops stale requests per slot).
    int64_t load(int slot, const std::string& path,
                 double bpmHint = 0.0) {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto it = q_.begin(); it != q_.end();)
            it = (it->slot == slot) ? q_.erase(it) : std::next(it);
        const int64_t id = ++reqSeq_;
        q_.push_back({ slot, id, path, bpmHint });
        cv_.notify_one();
        return id;
    }
    void cancelAll() {
        std::lock_guard<std::mutex> lk(mu_);
        q_.clear();
    }
    bool busy() const {
        std::lock_guard<std::mutex> lk(mu_);
        return !q_.empty() || working_;
    }

private:
    void run() {
        for (;;) {
            Job j;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [&] { return !q_.empty() || !running_; });
                if (!running_) return;
                j = q_.front(); q_.pop_front();
                working_ = true;
            }
            Result r; r.job = j;
            decode(j, r);
            {
                std::lock_guard<std::mutex> lk(mu_);
                working_ = false;
            }
            if (done_) done_(std::move(r));
        }
    }

    void decode(const Job& j, Result& r) {
        auto dec = factory_ ? factory_() : nullptr;
        if (!dec) { r.error = "no decoder"; return; }
        if (!dec->open(j.path, rate_)) { r.error = "open failed"; return; }
        const StreamInfo si = dec->info();
        auto data = std::make_shared<SampleData>();
        data->sampleRate = rate_;
        data->channels   = si.channels > 0 ? si.channels : 2;
        data->bpm        = j.bpmHint;

        const size_t cap = si.totalFrames > 0
                         ? (size_t)si.totalFrames : (size_t)rate_ * 30;
        auto l = std::shared_ptr<float[]>(new float[cap]);
        auto rr = std::shared_ptr<float[]>(new float[cap]);
        std::vector<float> inter(8192 * 2);
        size_t n = 0;
        float peak = 0.0f;
        for (;;) {
            const size_t got = dec->readChunk(inter.data(),
                                              inter.size() / 2);
            if (got == 0) break;
            if (n + got > cap) break;             // guard: oversized
            for (size_t i = 0; i < got; ++i) {
                const float a = inter[i * 2], b = inter[i * 2 + 1];
                l[n + i] = a; rr[n + i] = b;
                const float m = std::max(std::fabs(a), std::fabs(b));
                if (m > peak) peak = m;
            }
            n += got;
            if (dec->atEnd()) break;
        }
        dec->close();
        if (n == 0) { r.error = "empty decode"; return; }
        data->l = std::const_pointer_cast<const float[]>(std::move(l));
        data->r = std::const_pointer_cast<const float[]>(std::move(rr));
        data->frames = (int64_t)n;
        data->peak   = peak;
        // coarse peak waveform for previews
        const int wc = (int)std::min<int64_t>(
            SampleData::kWaveN, std::max<int64_t>(1, n / 256));
        for (int w = 0; w < wc; ++w) {
            const int64_t a0 = (int64_t)w * n / wc;
            const int64_t a1 = std::max<int64_t>(
                a0 + 1, (int64_t)(w + 1) * (int64_t)n / wc);
            float m = 0.0f;
            for (int64_t i = a0; i < a1; i += 4)
                m = std::max(m, std::max(std::fabs(data->l[i]),
                                         std::fabs(data->r[i])));
            data->wave[w] = m;
        }
        data->waveCount = wc;
        r.data = std::move(data);
    }

    DecoderFactory      factory_;
    double              rate_;
    DoneFn              done_;
    std::thread         worker_;
    mutable std::mutex  mu_;
    std::condition_variable cv_;
    std::deque<Job>     q_;
    bool                working_ = false;
    std::atomic<bool>   running_{ false };
    int64_t             reqSeq_ = 0;
};

} // namespace onda::sampler
