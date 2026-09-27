#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "../analysis/BeatGrid.h"
#include "../audio/TrackAudio.h"
#include "../fx/FXSlot.h"
#include "../model/DeckStats.h"

namespace onda {

class ITimeStretcher;

/// Phase 21: POD view of a deck's stem buffers for the loader→RT
/// handoff. The owner (StemsEngine) builds this from whatever
/// container it uses and keeps the backing alive until the deck
/// retires the pointer — same protocol as tracks/grids. The deck
/// only ever sees raw interleaved-stereo pointers sharing the
/// track's timeline (same numFrames, frame 0 aligned).
struct DeckStemBuffers {
    static constexpr int kMaxStems = 9;   // stems::StemType capacity
    const float* samples[kMaxStems] {};   // nullptr = stem absent
    int64_t      frames { 0 };
    uint32_t     mask   { 0 };            // bit per present stem
};

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

    // ---- brake (RT, Phase 28 §3) ----
    /// Vinyl brake: `on` engages a smoothstep spin-down that ends with
    /// the deck paused; `off` spins back up and resumes playback. The
    /// ramp is block-counted on the RT timeline — no timers involved.
    void setBrake(bool on, float sec) noexcept;
    bool brakeActive() const noexcept { return brakeDir_ != 0; }

    // ---- slip (RT) ----
    /// Slip: the musical shadow position keeps advancing while the user
    /// performs temporary actions (roll, hot cue hold, scratch). On
    /// endSlip the deck jumps to where the track "would have been".
    void beginSlip() noexcept;
    void endSlip() noexcept;
    /// Phase 20 §17: permanent moves under slip (hot cue, beat jump)
    /// re-anchor the shadow at the new position so slip-off doesn't
    /// jump back to the pre-action timeline.
    void reanchorSlip() noexcept;
    bool slipActive() const noexcept { return slipActive_; }
    /// §22 — SlipReturnPosition echo for the control side (sec, -1 off).
    double slipReturnSec() const noexcept {
        return slipActive_ ? slipShadow_ / deviceRate_ : -1.0; }

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

    // ---- stems (RT, Phase 21) ----
    /// 0 = original track, 1 = stem mix. With no set adopted the deck
    /// always renders the original (§25 fallback is unconditional).
    void setStemMode(int mode) noexcept;
    /// Resolved per-stem params. `stem` indexes stems::StemType.
    /// Mute wins over solo; if any stem is soloed, non-solo stems are
    /// silent (resolution mirrors StemMixer's documented semantics).
    void setStemParam(int stem, float gain, float pan,
                      bool mute, bool solo) noexcept;
    bool stemsActive() const noexcept {
        return stems_ != nullptr && stemMode_ == 1; }

    // ---- per-stem FX (Phase 25, §47) ----
    /// One FXSlot per stem kind (stems::StemType order: Vocals, Drums,
    /// Bass, Instruments). A stem whose slot is enabled/busy is staged
    /// out of the inline stem mix, processed as a block, then summed —
    /// the slot's own dry/wet handles the blend. Param space: FX ids
    /// with target=deck, slot=4+stem (see FXTypes.h).
    static constexpr int kStemFx = 4;
    /// Borrowed slot access for FXEngine::bindStemSlots — the deck
    /// keeps ownership; params reach it through the FX id space.
    fx::FXSlot* stemFx() noexcept { return &stemFx_[0]; }
    /// Per-block musical context for the stem slots: `ctx` carries the
    /// deck's own tempo, `masterCtx` the sync-master deck (a stem slot
    /// selecting MASTER tempo follows it — §22). RT only.
    void setStemFxContext(const fx::SyncContext& ctx,
                          const fx::SyncContext& masterCtx) noexcept {
        stemCtx_ = ctx;
        stemMasterCtx_ = masterCtx;
    }

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

    /// Phase 21: stem-buffer handoff — identical protocol. The
    /// StemsEngine owns the backing StemSet and publishes a POD view;
    /// retired views are released by the owner on its poll.
    std::atomic<const DeckStemBuffers*> pendingStems_{nullptr};
    std::atomic<const DeckStemBuffers*> retiredStems_{nullptr};

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
    void   advanceBrake(int n) noexcept;
    double brakeFactor() const noexcept;

    // ---- stems (RT helpers, Phase 21) ----
    void adoptPendingStems() noexcept;
    /// Mix all present stems at source position `pos` — cubic read +
    /// smoothed gain + pan per stem. Only called when stemsActive().
    /// A stem routed through its FX slot is written to that slot's
    /// input stage at index `outIdx` instead of joining the mix; the
    /// staged block is processed and summed by applyStemFx().
    void stemsMixAt(double pos, float& l, float& r, int outIdx) noexcept;
    /// True while stem `s`'s FX slot should intercept its render.
    bool stemTakesFx(int s) const noexcept {
        return s >= 0 && s < kStemFx
               && (stemFx_[(size_t)s].enabled() || stemFx_[(size_t)s].busy());
    }
    /// Zero the input stages of every FX'd stem (pre-mix, per block).
    void clearStemFxStage(int n) noexcept;
    /// Process staged stem blocks and sum into (l, r) — stride=2 for
    /// the interleaved stretcher stage (§37 keeps FX pre-stretch so the
    /// stretched output stays time-coherent).
    void applyStemFx(float* l, float* r, int n, int stride) noexcept;
    bool anyStemFx() const noexcept;
    void recomputeStemMasks() noexcept;      // after param change
    void publishStemStats() noexcept;

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

    // ---- brake (Phase 28) ----
    int      brakeDir_    { 0 };            // 0 off, +1 spin-down, -1 spin-up
    int64_t  brakeT_      { 0 };            // frames into the ramp
    int64_t  brakeFrames_ { 26400 };        // 0.55 s @48k default
    bool     brakeStopped_{ false };        // latch: halt came from brake

    // ---- stems (RT-owned, Phase 21) ----
    const DeckStemBuffers* stems_ { nullptr }; // borrowed, owned upstream
    int  stemMode_   { 0 };                    // 0 original, 1 stems
    bool stemAnySolo_{ false };
    struct StemRtParam { float gain { 1.0f }, pan { 0.0f };
                         bool mute { false }, solo { false }; };
    struct StemRt { float gL { 1.0f }, gR { 1.0f };   // resolved target
                    float smL { 1.0f }, smR { 1.0f }; // smoothed state
                    StemRtParam p; };
    std::array<StemRt, DeckStemBuffers::kMaxStems> stem_{};
    std::vector<float> stemStage_;             // stretcher feed staging

    // ---- per-stem FX (Phase 25) ----
    fx::FXSlot      stemFx_[kStemFx];
    std::vector<float> stemFxInL_[kStemFx], stemFxInR_[kStemFx];
    fx::SyncContext stemCtx_{}, stemMasterCtx_{};

    std::unique_ptr<ITimeStretcher> stretcher_;
    std::vector<float> scratch_;            // stretcher pull buffer
};

} // namespace onda
