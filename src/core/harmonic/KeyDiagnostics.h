#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

#include "HarmonicTypes.h"

namespace onda::harmonic {

/// ------------------------------------------------------------------
/// §31/§32 — tonal stability model over one track's segments:
/// dominant key, segment count, modulation markers, mean stability.
/// ------------------------------------------------------------------
struct TonalProfile {
    MusicalKey dominant;                 // longest/most-confident run
    float      stability     { 0.0f };   // 1.0 = single key throughout
    int        segmentCount  { 0 };
    int        modulations   { 0 };      // key changes between segments
    int        unknownSegments { 0 };    // windows too weak to name
    bool       modulating    { false };  // >1 distinct confident key
    std::string summary;                 // "stable 8A" / "modulating 8A→9A"

    /// Build from a segment list (device-rate frames + sampleRate for
    /// weighting by duration).
    static TonalProfile fromSegments(const std::vector<TonalSegment>& segs,
                                     double /*sampleRate*/) {
        TonalProfile p;
        p.segmentCount = (int)segs.size();
        if (segs.empty()) { p.summary = "no data"; return p; }

        // Weight each confident key by window length; dominant wins.
        int64_t w[24] = {}, total = 0; int lastKey = -2; int distinct = 0;
        for (const auto& s : segs) {
            const int64_t len = std::max<int64_t>(1, s.endFrame - s.startFrame);
            total += len;
            if (!s.key.valid()) { p.unknownSegments++; lastKey = -2; continue; }
            w[s.key.index()] += len;
            if (lastKey == -2) distinct++;
            else if (s.key.index() != lastKey) { distinct++; p.modulations++; }
            lastKey = s.key.index();
        }
        int best = -1; int64_t bw = 0;
        for (int i = 0; i < 24; ++i) if (w[i] > bw) { bw = w[i]; best = i; }
        if (best < 0) { p.summary = "undetermined"; return p; }
        p.dominant = MusicalKey::fromIndex(best, (float)bw / (float)total);
        p.stability = (float)bw / (float)total;
        p.modulating = p.modulations > 0;
        p.summary = p.modulating ? "modulating" : "stable";
        return p;
    }
};

/// ------------------------------------------------------------------
/// Engine diagnostics — counters + timing for ops/debug UI. Worker
/// threads write; UI reads. Atomic counters, no locks on hot path.
/// ------------------------------------------------------------------
class KeyDiagnostics {
public:
    struct Snapshot {
        uint64_t analysesStarted   { 0 };
        uint64_t analysesCompleted { 0 };
        uint64_t analysesFailed    { 0 };
        uint64_t analysesCancelled { 0 };
        uint64_t cacheHits         { 0 };
        uint64_t cacheMisses       { 0 };
        uint64_t lowConfidence     { 0 }; // results < 0.25
        uint64_t ambiguousResults  { 0 };
        double   lastAnalyzeMs     { 0.0 };
        double   avgAnalyzeMs      { 0.0 };
        int      workers           { 0 };
        std::string lastError;
    };

    void onStart()              { started_++; }
    void onDone(bool ok, bool cancelled, bool lowConf, bool ambiguous,
                double ms) {
        if (cancelled) cancelled_.fetch_add(1);
        else if (ok) done_.fetch_add(1);
        else failed_.fetch_add(1);
        if (ok && lowConf)   lowConf_.fetch_add(1);
        if (ok && ambiguous) ambig_.fetch_add(1);
        lastMs_.store(ms);
        // running average
        const double prev = avgMs_.load();
        const uint64_t n = done_.load();
        avgMs_.store(n > 0 ? prev + (ms - prev) / (double)n : ms);
    }
    void onCache(bool hit)      { hit ? hits_++ : misses_++; }
    void onError(const std::string& e) {
        std::lock_guard<std::mutex> lk(mu_);
        lastError_ = e;
    }
    void setWorkers(int w)      { workers_.store(w); }

    Snapshot snapshot() const {
        Snapshot s;
        s.analysesStarted   = started_.load();
        s.analysesCompleted = done_.load();
        s.analysesFailed    = failed_.load();
        s.analysesCancelled = cancelled_.load();
        s.cacheHits         = hits_.load();
        s.cacheMisses       = misses_.load();
        s.lowConfidence     = lowConf_.load();
        s.ambiguousResults  = ambig_.load();
        s.lastAnalyzeMs     = lastMs_.load();
        s.avgAnalyzeMs      = avgMs_.load();
        s.workers           = workers_.load();
        { std::lock_guard<std::mutex> lk(mu_); s.lastError = lastError_; }
        return s;
    }

private:
    std::atomic<uint64_t> started_{0}, done_{0}, failed_{0}, cancelled_{0};
    std::atomic<uint64_t> hits_{0}, misses_{0}, lowConf_{0}, ambig_{0};
    std::atomic<double>   lastMs_{0.0}, avgMs_{0.0};
    std::atomic<int>      workers_{0};
    mutable std::mutex    mu_;
    std::string           lastError_;
};

} // namespace onda::harmonic
