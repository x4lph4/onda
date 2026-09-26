#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/rec/RecordingBuffer.h"
#include "core/rec/RecordingManager.h"
#include "core/rec/RecordingRecovery.h"
#include "core/rec/RecordingRepository.h"
#include "core/rec/RecordingSession.h"
#include "core/rec/RecordingTypes.h"
#include "core/rec/RecordingWriter.h"

namespace onda::rec {

/// Professional session-recording façade. Owns the RT tap (fed by
/// AudioEngine post-master), the writer thread, the journal, the
/// session state machine, setlist tracking, and recovery hooks.
///
/// Threading: control methods + tick() run on the app thread; only
/// RecorderRt::pushPostMaster touches the audio thread.
class RecordingEngine {
public:
    /// Wiring to the rest of the app — all optional in tests.
    struct Ports {
        /// toast: (title, detail, level) level:0 info 1 warn 2 error 3 ok
        std::function<void(std::string,std::string,int)> notify;
        /// deck playing state: 1 playing, 0 not, -1 no such deck
        std::function<int(int)>                        deckPlaying;
        std::function<std::string(int)>                deckPath;
        /// fill TrackPlay meta (title/artist/bpm/key/trackId) from library
        std::function<TrackPlay(int,const std::string&)> trackMeta;
        /// HistoryManager::recordPlay passthrough (track end)
        std::function<void(int64_t,int,int64_t,double)> recordPlay;
        /// real engine sample rate (recording never resamples)
        std::function<double()>                        sampleRate;
        /// audio device running check for Quick Record
        std::function<bool()>                          audioRunning;
        int numDecks = 4;
    };

    explicit RecordingEngine(RecordingRepository* repo = nullptr);
    ~RecordingEngine();

    void setPorts(Ports p) { ports_ = std::move(p); }
    void setAppVersion(std::string v) { appVersion_ = std::move(v); }

    // ---- configuration (app thread, pre-arm) ----
    void        setOutputDir(const std::string& dir);
    std::string outputDir() const { return outDir_; }
    void        setBitDepth(BitDepth d) { depth_ = d; }
    BitDepth    bitDepth() const { return depth_; }

    // ---- transport (app thread / commands) ----
    bool arm();
    bool start(Source src = Source::Master);   // arm+start when Idle
    bool pause();
    bool resume();
    bool stop();                               // finalize → Completed
    bool split();                              // seamless part N+1
    bool toggle();                             // Quick Record
    /// Clean shutdown hook — stops an active recording properly.
    void shutdown();

    /// App-thread housekeeping, ~10-30 Hz: writer health, auto-split,
    /// clip/disk notifications, deck-play edge → session_tracks.
    void tick();

    // ---- state + telemetry (any thread) ----
    State       state() const { return state_.load(); }
    SessionInfo session() const;
    Meter       meter() const;
    Diagnostics diag() const { return diag_; }
    RecorderRt& rt() { return rt_; }           // attach to AudioEngine

    // ---- session management / recovery ----
    RecordingManager& manager() { return manager_; }
    /// At boot: mark DB-interrupted sessions Recoverable + return
    /// journal-scan items for UI (Recover/Discard/Inspect).
    std::vector<RecordingRecovery::Item> scanRecovery();
    bool recover(RecordingRecovery::Item& it);
    bool discardRecovery(const RecordingRecovery::Item& it);
    /// Failure-injection seam for tests: pretend the writer died and
    /// the process crashed without finalize.
    void simulateCrashForTest();

    /// Valid-transition table — exposed for tests/UI.
    static bool canTransit(State from, State to) noexcept;

private:
    bool transit(State to);
    void fail(const std::string& why);
    void finalize();                           // Stopping→Finalizing→Completed
    void pollDecks();
    void closeDeckRows(int64_t now);           // flush in-flight plays
    void notify(const std::string& t, const std::string& d, int lvl);

    RecordingRepository* repo_ = nullptr;
    RecordingManager     manager_;
    Ports                ports_;
    RecorderRt           rt_;
    RecordingWriter      writer_;
    Journal              journal_;

    std::atomic<State>   state_{State::Idle};
    SessionInfo          cur_;                  // app-thread only
    std::string          outDir_;
    BitDepth             depth_ = BitDepth::Pcm24;
    Source               source_ = Source::Master;
    std::string          appVersion_ = "dev";
    std::string          err_;
    int64_t              sessionRowId_ = -1;

    // setlist edge tracking
    std::array<int,      8> deckWasPlaying_{};
    std::array<int64_t,  8> deckPlayStart_{};
    std::array<int64_t,  8> deckPlayRow_{};
    std::array<std::string,8> deckLastPath_{};

    Diagnostics          diag_;
    uint32_t             lastClipCount_ = 0;
    int64_t              lastDiskPoll_  = 0;
    int64_t              pauseStart_    = 0;
    int64_t              pausedTotal_   = 0;
};

} // namespace onda::rec
