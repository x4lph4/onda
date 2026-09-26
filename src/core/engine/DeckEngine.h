#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "../analysis/BeatGrid.h"
#include "../audio/TrackAudio.h"
#include "../model/DeckStats.h"

namespace onda {

class ITimeStretcher;

/// One fully independent deck. Everything inside is owned by the RT audio
/// thread except the two handoff slots, which implement the lock-free
/// track-swap protocol with the loader thread:
///
///   loader  -- pendingTrack_ (exchange) -->  RT adopts at block top
///   RT      -- retiredTrack_ (store)   -->  loader frees it
///
/// Commands from UI arrive via the engine's command queue and are applied
/// by calling the public setters *on the RT thread*.
///
/// Processing order per block:
///   source read (varispeed cubic | WSOLA key-lock) -> fade
///   -> trackGain * play/pause mute smoothing -> level meter
///   -> [mixer ChannelStrip applies trim, EQ, filter, fader,
///       crossfader assignment, cue tap and master routing]
class DeckEngine {
public:
    DeckEngine();

    // ---- setup (non-RT) ----
    void bind(DeckStats* stats) noexcept;
    void setDeviceRate(double rate) noexcept;
    void setStretcher(std::unique_ptr<ITimeStretcher> s) noexcept;
    void prepareDsp(double rate) noexcept;

    // ---- track lifecycle (RT) ----
    void adoptPendingTrack() noexcept;   // called at the top of every render
    void unloadTrack() noexcept;

    // ---- transport (RT setters) ----
    void setPlaying(bool playing) noexcept;
    void stop() noexcept;                       // pause + back to start
    void seekSec(double sec) noexcept;
    void seekFrame(int64_t frame) noexcept;
    void setReverse(bool on) noexcept;
    bool isPlaying() const noexcept { return playing_; }

    // ---- slip (RT) ----
    /// Slip: the musical shadow position keeps advancing while the user
    /// performs temporary actions (roll, hot cue hold, scratch). On
    /// endSlip the deck jumps to where the track "would have been".
    void beginSlip() noexcept;
    void endSlip() noexcept;
    bool slipActive() const noexcept { return slipActive_; }

    // ---- sync (RT) ----
    /// mode: 0 off, 1 bpm, 2 beat, 3 phrase (2/3 handled identically on
    /// RT: continuous phase lock; phrase chooses snap targets upstream).
    void setSyncMode(int m) noexcept;
    /// Per-block call from SyncRtCore *before* renderPcm: computes the
    /// tempo multiplier slew and a bounded phase correction.
    void applySyncCorrection(double masterEffBpm, double masterPhaseAfter,
                             int blockFrames) noexcept;
    /// Master-side helpers for SyncRtCore.
    bool   hasGrid() const noexcept { return grid_ != nullptr; }
    double effBpm() const noexcept;             // grid bpm * |rate|
    double phaseAfter(int blockFrames) const noexcept; // predicted beat phase 0..1

    // ---- pitch (RT setters) ----
    void setTrackGain(float g) noexcept { trackGain_ = g; }
    void setPitchPercent(float pct) noexcept;
    void setPitchRange(float pct) noexcept;
    void setPitchBend(float pct) noexcept { bendPct_ = pct; }
    void setKeyLock(bool on) noexcept;
    void setQuantize(bool on) noexcept { quantize_ = on; }

    // ---- loop (RT setters) ----
    void setLoop(double inSec, double outSec) noexcept;
    void setLoopFrames(int64_t inFrame, int64_t outFrame) noexcept;
    void setLoopEnabled(bool on) noexcept;
    void setLoopSizeSec(double sizeSec) noexcept;
    void exitLoop() noexcept { loopOn_ = false; }

    /// Stats slot the deck was bound with — the mixer ChannelStrip
    /// re-uses it to publish channel meters (pre-fader level).
    DeckStats* stats() const noexcept { return stats_; }

    /// RT. Writes this deck's stereo, source-level audio (post track
    /// gain and mute smoothing, pre channel-strip processing).
    void renderPcm(float* l, float* r, int n) noexcept;

    // ---- loader <-> RT handoff slots ----
    std::atomic<const TrackAudio*> pendingTrack_{nullptr};
    std::atomic<const TrackAudio*> retiredTrack_{nullptr};

    /// BeatGrid handoff (same pattern as tracks): the performance layer
    /// keeps the shared_ptr alive; the RT thread borrows the raw pointer.
    /// Retired grids are published here for the owner to release.
    std::atomic<const BeatGrid*>  pendingGrid_{nullptr};
    std::atomic<const BeatGrid*>  retiredGrid_{nullptr};

private:
    // render paths
    void renderVarispeed(float* l, float* r, int n) noexcept;
    void renderStretch(float* l, float* r, int n) noexcept;
    void applyPostChain(float* l, float* r, int n) noexcept;

    // transport helpers
    double resolvedRate() const noexcept;   // signed consumption rate
    void   adoptPendingGrid() noexcept;
    void   nudgeFrames(double frames) noexcept; // sync phase snap (keeps playing)
    int64_t framesToBoundary() const noexcept;
    void wrapLoop() noexcept;
    bool pastEnd() const noexcept;
    void bumpFade(float amount = 1.0f / 256.0f) noexcept;
    void publishStats() noexcept;
    double displayedPos() const noexcept;   // position shown to UI
    void advanceDisp(int produced) noexcept; // keylock transport clock
    int64_t clampPos(double p) const noexcept;
    void   advanceSlip(int produced) noexcept;

    DeckStats* stats_ { nullptr };
    double deviceRate_ { 48000.0 };

    // ============ RT-owned state ============
    const TrackAudio* track_ { nullptr };
    double pos_        { 0.0 };   // source read head (device-rate frames)
    double dispPos_    { 0.0 };   // musical transport clock (UI/sync truth)
    int64_t feedBase_  { 0 };     // track index when the stretcher was reset
    int64_t pushed_    { 0 };     // frames fed to the stretcher since reset
    bool   playing_    { false };
    bool   seeking_    { false }; // transient, cleared next block
    bool   reverse_    { false };
    bool   trackEnded_ { false };

    float trackGain_ { 1.0f };
    float smGain_    { 1.0f };              // smoothed gain/mute
    float fade_      { 1.0f };              // click-free ramp 0..1

    float pitchPct_  { 0.0f };              // [-pitchRange_, +pitchRange_]
    float pitchRange_{ 8.0f };              // 8 / 16 / 50 etc.
    float bendPct_   { 0.0f };              // momentary nudge
    bool  keylock_   { false };
    bool  quantize_  { false };
    bool  stretcherFeedEnded_ { false };

    bool    loopOn_  { false };
    int64_t loopIn_  { 0 };
    int64_t loopOut_ { 0 };

    // ---- performance state ----
    const BeatGrid* grid_ { nullptr };      // borrowed, owned upstream
    int    syncMode_     { 0 };
    double syncMulSm_    { 1.0 };           // slewed sync tempo multiplier
    double blockCorr_    { 0.0 };           // one-shot frames/frame offset
    double syncErrFrames_{ 0.0 };
    bool   syncLimited_  { false };
    bool   slipActive_   { false };
    double slipShadow_   { 0.0 };           // where the track "would be"

    std::unique_ptr<ITimeStretcher> stretcher_;
    std::vector<float> scratch_;            // stretcher pull buffer
};

} // namespace onda
