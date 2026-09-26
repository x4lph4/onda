#include "core/rec/RecordingEngine.h"

#include <filesystem>

namespace onda::rec {

RecordingEngine::RecordingEngine(RecordingRepository* repo)
    : repo_(repo), manager_(repo) {}
RecordingEngine::~RecordingEngine() { shutdown(); }

// ---- state machine -------------------------------------------------

bool RecordingEngine::canTransit(State from, State to) noexcept {
    using S = State;
    switch (from) {
        case S::Idle:        return to == S::Armed || to == S::Recording
                                  || to == S::Failed;
        case S::Armed:       return to == S::Recording || to == S::Idle
                                  || to == S::Failed;
        case S::Recording:   return to == S::Paused || to == S::Stopping
                                  || to == S::Failed;
        case S::Paused:      return to == S::Recording || to == S::Stopping
                                  || to == S::Failed;
        case S::Stopping:    return to == S::Finalizing || to == S::Failed;
        case S::Finalizing:  return to == S::Completed || to == S::Failed;
        case S::Completed:
        case S::Failed:
        case S::Recoverable: return to == S::Idle || to == S::Armed;
    }
    return false;
}

bool RecordingEngine::transit(State to) {
    const State from = state_.load();
    if (!canTransit(from, to)) return false;
    state_.store(to);
    if (repo_ && sessionRowId_ >= 0)
        repo_->setState(sessionRowId_, to);
    return true;
}

void RecordingEngine::notify(const std::string& t,
                             const std::string& d, int lvl) {
    if (ports_.notify) ports_.notify(t, d, lvl);
}

void RecordingEngine::fail(const std::string& why) {
    err_ = why;
    rt_.arm(false);
    transit(State::Failed);                    // valid from any live state
    journal_.log("error", why);
    if (repo_ && sessionRowId_ >= 0) {
        repo_->setState(sessionRowId_, State::Failed);
        repo_->event(sessionRowId_, unixNow(), "error", why);
    }
    notify("Recording failed", why, 2);
}

// ---- configuration -------------------------------------------------

void RecordingEngine::setOutputDir(const std::string& dir) {
    outDir_ = dir;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
}

// ---- transport -----------------------------------------------------

bool RecordingEngine::arm() {
    const State st = state_.load();
    if (st != State::Idle && st != State::Completed
        && st != State::Failed) return false;
    if (st != State::Idle) state_.store(State::Idle);
    if (outDir_.empty()) {
        fail("no recording directory configured");
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(outDir_, ec)
        || !std::filesystem::is_directory(outDir_, ec)) {
        fail("recording directory does not exist");
        return false;
    }
    // disk check — require ~10 min of headroom at the session format
    const double rate = ports_.sampleRate ? ports_.sampleRate() : 48000.0;
    const int64_t need = estimateBytes(rate > 0 ? rate : 48000.0,
                                       depth_, 2, 600.0);
    const int64_t free_ = diskFreeBytes(outDir_);
    if (free_ > 0 && free_ < need) {
        fail("insufficient disk space for recording");
        return false;
    }
    rt_.resetMeters();
    err_.clear();
    return transit(State::Armed);
}

bool RecordingEngine::start(Source src) {
    const State st = state_.load();
    if (st == State::Idle || st == State::Completed
        || st == State::Failed) {
        if (!arm()) return false;
    } else if (st != State::Armed && st != State::Paused) {
        return false;
    }
    if (st == State::Paused) return resume();   // start() == resume there

    const double rate = ports_.sampleRate ? ports_.sampleRate() : 0.0;
    if (rate <= 0.0) { fail("audio engine sample rate unavailable");
                       return false; }
    source_ = src;
    cur_ = SessionInfo{};
    cur_.id = -1;
    cur_.source = src; cur_.sampleRate = rate;
    cur_.bitDepth = depth_; cur_.channels = 2;
    cur_.state = State::Recording;
    if (cur_.startTs == 0) cur_.startTs = unixNow();
    cur_.name = "Session";
    cur_.filePath = makeWavPath(outDir_, cur_.name, 0);

    if (!journal_.open(cur_.filePath)) {
        fail("cannot create recording journal"); return false; }
    journal_.log("started",
                 "rate=" + std::to_string((int)rate)
                 + " depth=" + std::to_string(bytesPerSample(depth_) * 8)
                 + " src=" + std::to_string((int)src));

    if (repo_) {
        SessionInfo row = cur_; row.state = State::Recording;
        sessionRowId_ = repo_->beginSession(row,
            std::to_string(cur_.startTs) + "-" +
            std::to_string((int64_t)(uintptr_t)this & 0xFFFF));
        cur_.id = sessionRowId_;
        repo_->event(sessionRowId_, cur_.startTs, "start",
                     cur_.filePath);
    }
    if (!writer_.start(cur_.filePath, rate, depth_, 2,
                       rt_.ring(), journal_)) {
        fail("cannot open recording file"); journal_.close();
        return false;
    }
    pausedTotal_ = 0; pauseStart_ = 0;
    for (auto& p : deckWasPlaying_) p = 0;
    for (auto& s : deckLastPath_) s.clear();
    rt_.arm(true);
    if (state_.load() == State::Armed) transit(State::Recording);
    else state_.store(State::Recording);      // re-start after pause path
    notify("Recording started", cur_.filePath, 0);
    return true;
}

bool RecordingEngine::pause() {
    if (state_.load() != State::Recording) return false;
    rt_.arm(false);                            // frames simply stop
    pauseStart_ = unixNow();
    journal_.log("paused");
    if (repo_) repo_->event(sessionRowId_, pauseStart_, "pause");
    return transit(State::Paused);
}

bool RecordingEngine::resume() {
    if (state_.load() != State::Paused) return false;
    if (pauseStart_) pausedTotal_ += unixNow() - pauseStart_;
    pauseStart_ = 0;
    journal_.log("resumed");
    if (repo_) repo_->event(sessionRowId_, unixNow(), "resume");
    rt_.arm(true);
    return transit(State::Recording);
}

bool RecordingEngine::stop() {
    const State st = state_.load();
    if (st == State::Armed) {                  // disarm
        journal_.close();
        return transit(State::Idle);
    }
    if (st != State::Recording && st != State::Paused
        && st != State::Stopping) return false;
    if (st != State::Stopping && !transit(State::Stopping)) return false;
    rt_.arm(false);
    if (pauseStart_) pausedTotal_ += unixNow() - pauseStart_;
    pauseStart_ = 0;
    journal_.log("stopped");
    closeDeckRows(unixNow());                  // flush in-flight plays
    if (repo_) repo_->event(sessionRowId_, unixNow(), "stop");
    writer_.stop();                            // drain+finalize+join
    finalize();
    return true;
}

bool RecordingEngine::split() {
    if (state_.load() != State::Recording) return false;
    const std::string next =
        makeWavPath(outDir_, cur_.name, ++cur_.part);
    writer_.requestSplit(next);
    if (repo_) {
        repo_->event(sessionRowId_, unixNow(), "split", next);
        repo_->setState(sessionRowId_, state_.load());
    }
    notify("Recording split", next, 0);
    return true;
}

bool RecordingEngine::toggle() {
    const State st = state_.load();
    if (st == State::Recording || st == State::Paused) return stop();
    if (st != State::Idle && st != State::Completed
        && st != State::Failed) return false;
    // Quick Record: device up? disk ok? → session → rolling.
    if (ports_.audioRunning && !ports_.audioRunning()) {
        notify("Recording unavailable", "audio device not running", 2);
        return false;
    }
    return start(source_);
}

void RecordingEngine::shutdown() {
    const State st = state_.load();
    if (st == State::Recording || st == State::Paused
        || st == State::Armed || st == State::Stopping)
        stop();
    writer_.stop();
    journal_.close();
}

void RecordingEngine::simulateCrashForTest() {
    // Mimic an interrupted write: stop feeding frames and abandon the
    // file handle without finalize — journal keeps no completion line.
    rt_.arm(false);
    writer_.stop();          // writer finalizes normally — the *journal*
    journal_.close();        // then strip the completion marker below
    state_.store(State::Idle);
    // rewrite journal without "finalize_completed" to emulate crash
    std::vector<std::string> lines;
    { FILE* f = std::fopen(journal_.path().c_str(), "rb");
      if (f) { char l[512];
               while (std::fgets(l, sizeof l, f)) lines.push_back(l);
               std::fclose(f); } }
    FILE* f = std::fopen(journal_.path().c_str(), "wb");
    if (!f) return;
    for (auto& l : lines)
        if (l.find("finalize_completed") == std::string::npos)
            std::fputs(l.c_str(), f);
    std::fclose(f);
    // and truncate the header's data-size so it looks unfinished
    WavEncoder::patchHeader(cur_.filePath, 0);
}

// ---- finalize ------------------------------------------------------

void RecordingEngine::finalize() {
    if (state_.load() == State::Stopping)
        transit(State::Finalizing);
    cur_.endTs = unixNow();
    const auto parts = writer_.parts();
    int64_t totalFrames = 0, totalBytes = 0;
    for (const auto& p : parts) {
        totalFrames += p.frames; totalBytes += p.fileBytes;
        if (repo_) repo_->addFile(sessionRowId_, p);
    }
    cur_.frames = totalFrames;
    cur_.fileBytes = totalBytes;
    cur_.durationSec = cur_.sampleRate > 0
        ? (double)totalFrames / cur_.sampleRate : 0.0;
    const Meter m = rt_.meter(cur_.sampleRate);
    cur_.peakLevel = std::max<float>(m.maxPeakL, m.maxPeakR);
    cur_.clipCount = m.clipCount;
    // point session at the first part (or only file)
    if (!parts.empty()) cur_.filePath = parts.front().filePath;
    // validate the main file
    double vr = 0; int vc = 0; int64_t vf = 0;
    const bool valid = !parts.empty()
        && WavEncoder::validate(cur_.filePath, &vr, &vc, &vf);
    cur_.state = valid ? State::Completed : State::Failed;
    // metadata sidecar — never mutates the audio file
    if (valid) {
        const std::vector<TrackPlay> tr =
            repo_ && sessionRowId_ >= 0 ? repo_->tracks(sessionRowId_)
                                        : std::vector<TrackPlay>{};
        export_::writeFile(cur_.filePath + ".meta.json",
            export_::sessionJson(cur_, tr, appVersion_));
    }
    journal_.close();
    if (repo_ && sessionRowId_ >= 0) {
        repo_->endSession(sessionRowId_, cur_);
        repo_->event(sessionRowId_, cur_.endTs,
                     valid ? "completed" : "failed");
    }
    transit(valid ? State::Completed : State::Failed);
    if (valid)
        notify("Recording complete", cur_.filePath, 3);
    else
        notify("Recording failed", "file validation failed", 2);
}

// ---- tick ----------------------------------------------------------

void RecordingEngine::tick() {
    const State st = state_.load();
    if (st == State::Recording || st == State::Paused) {
        // writer health / auto-split on 3.8 GB / disk space poll
        if (writer_.failed()) { fail(writer_.error()); return; }
        if (st == State::Recording && writer_.sizeSplitWanted())
            split();
        const int64_t now = unixNow();
        if (now != lastDiskPoll_) {
            lastDiskPoll_ = now;
            diag_ = writer_.snapshot(diskFreeBytes(outDir_),
                                     rt_.meter(cur_.sampleRate)
                                         .droppedFrames,
                cur_.sampleRate * 2 * bytesPerSample(depth_) / 1e6);
            if (diag_.diskWarn)
                notify("Disk slow",
                       "write throughput below recording need", 1);
            if (diag_.diskFreeBytes > 0
                && diag_.diskFreeBytes
                   < estimateBytes(cur_.sampleRate, depth_, 2, 60.0))
                notify("Disk almost full",
                       "less than ~1 minute of recording space", 1);
        }
        // clip notification edge
        const Meter m = rt_.meter(cur_.sampleRate);
        if (m.clipCount != lastClipCount_) {
            lastClipCount_ = m.clipCount;
            notify("Clipping detected",
                   "peak " + std::to_string(m.clipPeak), 1);
        }
        ClipEvent ce;
        while (rt_.popClip(ce)) {
            if (repo_)
                repo_->event(sessionRowId_, unixNow(), "clip",
                    "ch" + std::to_string(ce.channel));
        }
        pollDecks();
    }
}

void RecordingEngine::pollDecks() {
    if (!ports_.deckPlaying || !ports_.deckPath) return;
    const int64_t now = unixNow();
    for (int d = 0; d < ports_.numDecks && d < 8; ++d) {
        const int playing = ports_.deckPlaying(d);
        if (playing < 0) continue;
        const std::string path = playing ? ports_.deckPath(d) : "";
        const bool was = deckWasPlaying_[d] != 0;
        if (playing && (!was || path != deckLastPath_[d])) {
            if (was && deckPlayRow_[d] >= 0) {   // track swap mid-play
                const double secs =
                    (double)(now - deckPlayStart_[d]);
                if (repo_) repo_->updateTrackPlayed(deckPlayRow_[d], secs);
                if (ports_.recordPlay) {
                    TrackPlay t = ports_.trackMeta
                        ? ports_.trackMeta(d, deckLastPath_[d])
                        : TrackPlay{};
                    ports_.recordPlay(t.trackId, d, deckPlayStart_[d],
                                      secs);
                }
            }
            TrackPlay t = ports_.trackMeta
                ? ports_.trackMeta(d, path) : TrackPlay{};
            t.ts = now; t.deck = d; t.path = path;
            deckPlayRow_[d] = repo_ ? repo_->addTrack(sessionRowId_, t)
                                    : -1;
            deckPlayStart_[d] = now;
            deckLastPath_[d] = path;
            deckWasPlaying_[d] = 1;
            if (repo_) repo_->event(sessionRowId_, now,
                                    "track_start", t.title);
        } else if (!playing && was) {
            const double secs = (double)(now - deckPlayStart_[d]);
            if (repo_ && deckPlayRow_[d] >= 0)
                repo_->updateTrackPlayed(deckPlayRow_[d], secs);
            if (ports_.recordPlay) {
                TrackPlay t = ports_.trackMeta
                    ? ports_.trackMeta(d, deckLastPath_[d])
                    : TrackPlay{};
                ports_.recordPlay(t.trackId, d, deckPlayStart_[d], secs);
            }
            if (repo_) repo_->event(sessionRowId_, now,
                                    "track_stop", deckLastPath_[d]);
            deckWasPlaying_[d] = 0;
        }
    }
}

/// Close any deck rows still open when the session ends — the setlist
/// row gets its played_sec and HistoryManager gets the real duration.
void RecordingEngine::closeDeckRows(int64_t now) {
    for (int d = 0; d < 8; ++d) {
        if (!deckWasPlaying_[d]) continue;
        const double secs = (double)(now - deckPlayStart_[d]);
        if (repo_ && deckPlayRow_[d] >= 0)
            repo_->updateTrackPlayed(deckPlayRow_[d], secs);
        if (ports_.recordPlay) {
            TrackPlay t = ports_.trackMeta
                ? ports_.trackMeta(d, deckLastPath_[d]) : TrackPlay{};
            ports_.recordPlay(t.trackId, d, deckPlayStart_[d], secs);
        }
        deckWasPlaying_[d] = 0;
    }
}

// ---- state + telemetry ----------------------------------------------

SessionInfo RecordingEngine::session() const {
    SessionInfo s = cur_;
    s.state = state_.load();
    if ((s.state == State::Recording || s.state == State::Paused)
        && cur_.sampleRate > 0) {
        s.durationSec =
            (double)rt_.meter(cur_.sampleRate).frames / cur_.sampleRate;
        s.fileBytes = writer_.encBytes();
    }
    return s;
}
Meter RecordingEngine::meter() const {
    return rt_.meter(cur_.sampleRate > 0 ? cur_.sampleRate : 48000.0);
}

// ---- recovery --------------------------------------------------------

std::vector<RecordingRecovery::Item> RecordingEngine::scanRecovery() {
    // 1) DB sessions abandoned mid-flight → mark Recoverable
    if (repo_) {
        for (auto& s : repo_->interrupted()) {
            repo_->setState(s.id, State::Recoverable);
            repo_->recoveryLog(s.id, "marked_recoverable", s.filePath);
        }
    }
    // 2) journal sidecars without finalize_completed
    return outDir_.empty() ? std::vector<RecordingRecovery::Item>{}
                           : RecordingRecovery::scan(outDir_);
}

bool RecordingEngine::recover(RecordingRecovery::Item& it) {
    // detect bytes-per-frame from the file's own header — the session
    // being recovered may predate the current engine configuration
    int bpf = cur_.channels * bytesPerSample(cur_.bitDepth);
    { double r = 0; int ch = 0, bits = 0;
      if (WavEncoder::validate(it.wavPath, &r, &ch, nullptr, &bits)
          && ch > 0 && bits > 0)
          bpf = ch * (bits / 8); }
    const bool ok = RecordingRecovery::recover(
        it, bpf > 0 ? bpf : 6);
    if (ok && repo_) {
        // bind to a session row when one references this file
        for (auto& s : repo_->sessions(500))
            if (s.filePath == it.wavPath) {
                repo_->setState(s.id, State::Completed);
                repo_->recoveryLog(s.id, "recovered", it.wavPath);
                break;
            }
        repo_->recoveryLog(-1, "recovered", it.wavPath);
    }
    if (ok) notify("Recording recovered", it.wavPath, 3);
    return ok;
}

bool RecordingEngine::discardRecovery(
    const RecordingRecovery::Item& it) {
    const bool ok = RecordingRecovery::discard(it);
    if (ok && repo_) repo_->recoveryLog(-1, "discarded", it.wavPath);
    return ok;
}

} // namespace onda::rec
