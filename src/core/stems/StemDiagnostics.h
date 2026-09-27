#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace onda::stems {

/// ------------------------------------------------------------------
/// §39/§44/§52 — observability for the stems pipeline. Atomics so the
/// scheduler workers, cache and control thread can all publish
/// without locks. Snapshot is a plain struct for tests/UI.
/// ------------------------------------------------------------------
class StemDiagnostics {
public:
    std::atomic<uint64_t> jobsQueued{0}, jobsStarted{0}, jobsDone{0};
    std::atomic<uint64_t> jobsFailed{0}, jobsCancelled{0};
    std::atomic<uint64_t> decodeFailures{0}, modelFailures{0};
    std::atomic<uint64_t> cacheHits{0}, cacheMisses{0}, cacheWrites{0};
    std::atomic<uint64_t> cacheWriteFailures{0}, cacheReadFailures{0};
    std::atomic<uint64_t> bytesWritten{0}, bytesRead{0};
    std::atomic<uint64_t> paramChanges{0}, modeChanges{0}, rejects{0};
    /// last separation timing (ms) — §39 latency tracking
    std::atomic<double>   lastSeparateMs{0.0}, lastLoadMs{0.0};
    /// RT-side render accounting — published by DeckEngine each block
    /// when stems contribute; masked per-stem rendered.
    std::atomic<uint64_t> deckStemBlocks[4] { {}, {}, {}, {} };

    struct Snapshot {
        uint64_t jobsQueued, jobsStarted, jobsDone, jobsFailed,
                 jobsCancelled, decodeFailures, modelFailures,
                 cacheHits, cacheMisses, cacheWrites,
                 cacheWriteFailures, cacheReadFailures,
                 bytesWritten, bytesRead, paramChanges, modeChanges,
                 rejects, deckBlocks;
        double lastSeparateMs, lastLoadMs;
    };
    Snapshot snapshot() const {
        Snapshot s;
        s.jobsQueued = jobsQueued; s.jobsStarted = jobsStarted;
        s.jobsDone = jobsDone; s.jobsFailed = jobsFailed;
        s.jobsCancelled = jobsCancelled;
        s.decodeFailures = decodeFailures;
        s.modelFailures = modelFailures;
        s.cacheHits = cacheHits; s.cacheMisses = cacheMisses;
        s.cacheWrites = cacheWrites;
        s.cacheWriteFailures = cacheWriteFailures;
        s.cacheReadFailures = cacheReadFailures;
        s.bytesWritten = bytesWritten; s.bytesRead = bytesRead;
        s.paramChanges = paramChanges; s.modeChanges = modeChanges;
        s.rejects = rejects;
        s.lastSeparateMs = lastSeparateMs; s.lastLoadMs = lastLoadMs;
        s.deckBlocks = deckStemBlocks[0] + deckStemBlocks[1]
                     + deckStemBlocks[2] + deckStemBlocks[3];
        return s;
    }

    std::string text() const {
        const Snapshot s = snapshot();
        char b[512];
        std::snprintf(b, sizeof(b),
            "stems: jobs %llu/%llu ok %llu fail %llu canc %llu | "
            "cache hit %llu miss %llu wr %llu | "
            "io %llu/%llu B | sep %.1fms load %.1fms | "
            "params %llu mode %llu rej %llu | rtblocks %llu",
            (unsigned long long)s.jobsStarted,
            (unsigned long long)s.jobsQueued,
            (unsigned long long)s.jobsDone,
            (unsigned long long)s.jobsFailed,
            (unsigned long long)s.jobsCancelled,
            (unsigned long long)s.cacheHits,
            (unsigned long long)s.cacheMisses,
            (unsigned long long)s.cacheWrites,
            (unsigned long long)s.bytesRead,
            (unsigned long long)s.bytesWritten,
            s.lastSeparateMs, s.lastLoadMs,
            (unsigned long long)s.paramChanges,
            (unsigned long long)s.modeChanges,
            (unsigned long long)s.rejects,
            (unsigned long long)s.deckBlocks);
        return b;
    }
};

/// Per-deck stem state container (§3) — what the panel/pads read.
/// Non-RT: updated by StemsEngine on the control thread, published
/// to UI via UiSnapshot; the RT deck only sees resolved params.
class StemManager {
public:
    void reset() {
        for (int i = 0; i < kStemMax; ++i) {
            stems_[i] = Stem{};
            stems_[i].id = i;
            stems_[i].type = (StemType)i;
        }
        presentMask_ = 0; availability_ = StemAvailability::Unavailable;
        mode_ = 0; jobId_ = -1;
    }

    /// Reflect a loaded set: fills presence + duration per stem.
    void onSetLoaded(const StemSet& s) {
        presentMask_ = s.mask;
        int have = 0;
        for (int i = 0; i < kStemMax; ++i) {
            auto& st = stems_[i];
            st.present = (s.mask & (1u << i)) != 0;
            st.duration = st.present ? s.numFrames : 0;
            st.separationStatus = st.present ? (int)StemCompState::Done
                                             : (int)StemCompState::None;
            st.cacheStatus = st.present ? (int)StemCompState::Done
                                        : (int)StemCompState::None;
            if (st.present) ++have;
        }
        availability_ = have == 0 ? StemAvailability::Unavailable
                      : have >= kStemCount ? StemAvailability::Ready
                                           : StemAvailability::Partial;
    }
    void onCleared() {
        for (auto& st : stems_) st.present = false;
        presentMask_ = 0; availability_ = StemAvailability::Unavailable;
    }

    Stem& stem(int i)             { return stems_[(size_t)i]; }
    const Stem& stem(int i) const { return stems_[(size_t)i]; }
    uint32_t presentMask() const  { return presentMask_; }
    StemAvailability availability() const { return availability_; }
    void setAvailability(StemAvailability a) { availability_ = a; }
    int  mode() const      { return mode_; }
    void setMode(int m)    { mode_ = m; }
    int64_t jobId() const  { return jobId_; }
    void setJobId(int64_t id) { jobId_ = id; }

private:
    std::array<Stem, kStemMax> stems_ {};
    uint32_t presentMask_ { 0 };
    StemAvailability availability_ { StemAvailability::Unavailable };
    int mode_ { 0 };            // 0 original, 1 stems
    int64_t jobId_ { -1 };      // separation job attached to this deck
};

} // namespace onda::stems
