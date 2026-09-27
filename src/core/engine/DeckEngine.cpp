#include "DeckEngine.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../audio/AudioFormat.h"
#include "../dsp/Resample.h"
#include "ITimeStretcher.h"

namespace onda {

static constexpr int64_t kFeedChunk = 512; // source frames per stretcher feed

DeckEngine::DeckEngine() : scratch_(AudioFormat::kMaxBlockFrames * 2, 0.0f) {}

void DeckEngine::bind(DeckStats* stats) noexcept { stats_ = stats; }

void DeckEngine::setDeviceRate(double rate) noexcept {
    deviceRate_ = rate;
}

void DeckEngine::setStretcher(std::unique_ptr<ITimeStretcher> s) noexcept {
    stretcher_ = std::move(s);
}

void DeckEngine::prepareDsp(double rate) noexcept {
    if (stretcher_) stretcher_->prepare(rate, AudioFormat::kMaxBlockFrames);
    stemStage_.assign((size_t)kFeedChunk * 2, 0.0f); // non-RT: prealloc
    for (int s = 0; s < kStemFx; ++s) {              // Phase 25 stem FX
        stemFx_[s].prepare(rate);
        stemFxInL_[s].assign(AudioFormat::kMaxBlockFrames, 0.0f);
        stemFxInR_[s].assign(AudioFormat::kMaxBlockFrames, 0.0f);
    }
}

// ============================ track lifecycle ============================

void DeckEngine::adoptPendingTrack() noexcept {
    const TrackAudio* pending = pendingTrack_.exchange(nullptr,
                                                       std::memory_order_acq_rel);
    if (pending == nullptr)
        return;

    adoptPendingGrid(); // a new track may already have a grid waiting
    adoptPendingStems(); // and possibly a cached stem set (Phase 21)

    // Retire the previous track for the loader to free.
    if (track_ != nullptr)
        retiredTrack_.store(track_, std::memory_order_release);

    track_        = pending;
    pos_          = 0.0;
    dispPos_      = 0.0;
    trackEnded_   = false;
    stretcherFeedEnded_ = false;
    fade_         = 0.0f; // ramp in
    if (stretcher_) { stretcher_->reset(); feedBase_ = 0; pushed_ = 0; }
    // Transport stays paused on load (DJ convention).
    playing_ = false;
}

void DeckEngine::unloadTrack() noexcept {
    if (track_ != nullptr) {
        retiredTrack_.store(track_, std::memory_order_release);
        track_ = nullptr;
    }
    if (grid_ != nullptr) {
        retiredGrid_.store(grid_, std::memory_order_release);
        grid_ = nullptr;
    }
    if (stems_ != nullptr) {   // stems belong to the track (Phase 21)
        retiredStems_.store(stems_, std::memory_order_release);
        stems_ = nullptr;
    }
    playing_     = false;
    trackEnded_  = false;
    pos_         = 0.0;
    slipActive_  = false;
    syncMode_    = 0;
    syncMulSm_   = 1.0;
    blockCorr_   = 0.0;
    if (stats_) stats_->state.store((int)DeckState::Empty);
}

void DeckEngine::adoptPendingGrid() noexcept {
    const BeatGrid* g = pendingGrid_.exchange(nullptr,
                                              std::memory_order_acq_rel);
    if (g == nullptr)
        return;
    if (grid_ != nullptr)
        retiredGrid_.store(grid_, std::memory_order_release);
    grid_ = g;
}

// ============================== stems (Phase 21) =========================
//
// Stems render at `pos_` through the SAME read head as the track —
// loops, slip, sync tempo and key-lock all apply identically to every
// stem (§36-38) because there is no separate stem clock. The original
// track buffer stays live: stemMode_=0 or a missing set renders it
// untouched (§25).

void DeckEngine::adoptPendingStems() noexcept {
    const DeckStemBuffers* s = pendingStems_.exchange(
        nullptr, std::memory_order_acq_rel);
    if (s == nullptr)
        return;
    if (stems_ != nullptr)
        retiredStems_.store(stems_, std::memory_order_release);
    stems_ = s;
    recomputeStemMasks();
    publishStemStats();
}

void DeckEngine::setStemMode(int mode) noexcept {
    stemMode_ = mode == 1 ? 1 : 0;
    publishStemStats();
}

void DeckEngine::setStemParam(int stem, float gain, float pan,
                              bool mute, bool solo) noexcept {
    if (stem < 0 || stem >= DeckStemBuffers::kMaxStems) return;
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 2.0f) gain = 2.0f;
    if (pan  < -1.0f) pan = -1.0f;
    if (pan  > 1.0f)  pan = 1.0f;
    // store resolved intent; targets recomputed below for all stems
    // (solo state is global — one stem's solo affects every target)
    stem_[stem].p = { gain, pan, mute, solo };
    recomputeStemMasks();
    publishStemStats();
}

void DeckEngine::recomputeStemMasks() noexcept {
    stemAnySolo_ = false;
    for (int i = 0; i < DeckStemBuffers::kMaxStems; ++i)
        stemAnySolo_ |= stem_[i].p.solo;
    for (int i = 0; i < DeckStemBuffers::kMaxStems; ++i) {
        const auto& p = stem_[i].p;
        const bool audible =
            stems_ != nullptr && (stems_->mask & (1u << i)) != 0 &&
            !p.mute && !(stemAnySolo_ && !p.solo);
        const double t = ((double)p.pan + 1.0) * 0.7853981633974483;
        stem_[i].gL = audible ? (float)(p.gain * std::cos(t)) : 0.0f;
        stem_[i].gR = audible ? (float)(p.gain * std::sin(t)) : 0.0f;
    }
}

void DeckEngine::stemsMixAt(double pos, float& l, float& r,
                            int outIdx) noexcept {
    constexpr float kSmooth = 0.001f;   // same one-pole as smGain_
    l = 0.0f; r = 0.0f;
    for (int i = 0; i < DeckStemBuffers::kMaxStems; ++i) {
        const float* s = stems_->samples[i];
        if (s == nullptr) continue;
        float sl, sr;
        readCubicStereo(s, stems_->frames, pos, sl, sr);
        StemRt& t = stem_[i];
        t.smL += kSmooth * (t.gL - t.smL);
        t.smR += kSmooth * (t.gR - t.smR);
        sl *= t.smL; sr *= t.smR;
        // Phase 25: a stem with an active FX slot renders into that
        // slot's input stage instead of the mix — the slot's own
        // dry/wet blend produces its contribution afterwards.
        if (stemTakesFx(i)) {
            stemFxInL_[(size_t)i][(size_t)outIdx] = sl;
            stemFxInR_[(size_t)i][(size_t)outIdx] = sr;
        } else {
            l += sl; r += sr;
        }
    }
}

bool DeckEngine::anyStemFx() const noexcept {
    for (int s = 0; s < kStemFx; ++s)
        if (stemTakesFx(s)) return true;
    return false;
}

void DeckEngine::clearStemFxStage(int n) noexcept {
    for (int s = 0; s < kStemFx; ++s) {
        if (!stemTakesFx(s)) continue;
        std::memset(stemFxInL_[s].data(), 0, (size_t)n * sizeof(float));
        std::memset(stemFxInR_[s].data(), 0, (size_t)n * sizeof(float));
    }
}

void DeckEngine::applyStemFx(float* l, float* r, int n,
                             int stride) noexcept {
    for (int s = 0; s < kStemFx; ++s) {
        if (!stemTakesFx(s)) continue;
        float* il = stemFxInL_[s].data();
        float* ir = stemFxInR_[s].data();
        stemFx_[s].process(il, ir, n, stemCtx_, &stemMasterCtx_);
        for (int i = 0; i < n; ++i) {
            l[(size_t)i * (size_t)stride] += il[i];
            r[(size_t)i * (size_t)stride] += ir[i];
        }
    }
}

void DeckEngine::publishStemStats() noexcept {
    if (!stats_) return;
    stats_->stemMode.store(stems_ != nullptr ? stemMode_ : 0);
    uint32_t m = 0;
    if (stemsActive())
        for (int i = 0; i < DeckStemBuffers::kMaxStems; ++i)
            if (stem_[i].gL != 0.0f || stem_[i].gR != 0.0f) m |= (1u << i);
    stats_->stemsAudible.store(m);
}

// ================================ transport ============================

void DeckEngine::setPlaying(bool playing) noexcept {
    if (playing && (track_ == nullptr || pastEnd()))
        playing = false;
    playing_ = playing;
    if (stats_ && track_ != nullptr)
        stats_->state.store(playing ? (int)DeckState::Playing
                                    : (int)DeckState::Paused);
}

void DeckEngine::stop() noexcept {
    playing_ = false;
    pos_ = 0.0;
    dispPos_ = 0.0;
    trackEnded_ = false;
    if (stretcher_) { stretcher_->reset(); feedBase_ = 0; pushed_ = 0; }
    stretcherFeedEnded_ = false;
    fade_ = 0.0f;
    if (stats_ && track_ != nullptr)
        stats_->state.store((int)DeckState::Paused);
}

void DeckEngine::seekSec(double sec) noexcept {
    if (track_ == nullptr) return;
    seekFrame((int64_t)(sec * deviceRate_));
}

void DeckEngine::seekFrame(int64_t frame) noexcept {
    if (track_ == nullptr) return;
    pos_ = (double)clampPos((double)frame);
    dispPos_ = pos_;
    trackEnded_ = false;
    fade_ = 0.0f;
    if (stretcher_) {
        stretcher_->reset();
        feedBase_ = (int64_t)pos_;
        pushed_ = 0;
        stretcherFeedEnded_ = false;
    }
    seeking_ = true;
    if (stats_ && track_ != nullptr)
        stats_->state.store((int)DeckState::Seeking);
}

void DeckEngine::beginSlip() noexcept {
    if (track_ == nullptr || slipActive_) return;
    slipActive_ = true;
    slipShadow_ = displayedPos();
}

void DeckEngine::endSlip() noexcept {
    if (!slipActive_) return;
    slipActive_ = false;
    // Recover where the track "would have been" — faded seek.
    const int64_t target = (int64_t)std::clamp(slipShadow_, 0.0,
                                               track_ ? (double)track_->numFrames
                                                      : 0.0);
    pos_ = (double)target;
    dispPos_ = pos_;
    fade_ = 0.0f;
    if (stretcher_) {
        stretcher_->reset();
        feedBase_ = (int64_t)pos_;
        pushed_ = 0;
        stretcherFeedEnded_ = false;
    }
}

void DeckEngine::reanchorSlip() noexcept {
    // Phase 20 §17: a permanent move under slip (hot cue, beat jump)
    // re-anchors the shadow at the new position — slip-off later does
    // NOT jump back to the pre-cue timeline.
    if (slipActive_) slipShadow_ = displayedPos();
}

void DeckEngine::advanceSlip(int produced) noexcept {
    if (!slipActive_ || track_ == nullptr) return;
    slipShadow_ += std::fabs(resolvedRate()) * (double)produced;
    if (slipShadow_ > (double)track_->numFrames)
        slipShadow_ = (double)track_->numFrames;
}

void DeckEngine::setReverse(bool on) noexcept {
    if (on == reverse_) return;
    reverse_ = on;
    fade_ = 0.0f; // direction flip — soften the edge
}

// ================================ pitch ================================

void DeckEngine::setPitchPercent(float pct) noexcept {
    pitchPct_ = std::clamp(pct, -pitchRange_, pitchRange_);
}

void DeckEngine::setPitchRange(float pct) noexcept {
    pitchRange_ = std::clamp(pct, 1.0f, 100.0f);
    setPitchPercent(pitchPct_); // re-clamp inside new range
}

void DeckEngine::setKeyLock(bool on) noexcept {
    if (on == keylock_) return;
    if (!on) {
        // Resume varispeed at the musical-transport position so the
        // playhead continues seamlessly (one deterministic pipeline-latency
        // discontinuity, masked by the fade ramp).
        if (track_ != nullptr)
            pos_ = (double)clampPos(dispPos_);
    } else if (stretcher_) {
        stretcher_->reset();
        stretcher_->setRate(std::fabs(resolvedRate()));
        feedBase_ = (int64_t)pos_;
        pushed_ = 0;
        dispPos_ = pos_;
        stretcherFeedEnded_ = false;
    }
    keylock_ = on;
    fade_ = 0.0f;
}

// ================================ loop ================================

void DeckEngine::setLoop(double inSec, double outSec) noexcept {
    if (track_ == nullptr) return;
    setLoopFrames((int64_t)(inSec * deviceRate_),
                  (int64_t)(outSec * deviceRate_));
}

void DeckEngine::setLoopFrames(int64_t inFrame, int64_t outFrame) noexcept {
    if (track_ == nullptr) return;
    const int64_t in  = clampPos((double)inFrame);
    const int64_t out = clampPos((double)outFrame);
    if (out <= in) { loopOn_ = false; return; }
    loopIn_ = in; loopOut_ = out; loopOn_ = true;
}

void DeckEngine::setLoopEnabled(bool on) noexcept {
    loopOn_ = on && loopOut_ > loopIn_;
}

void DeckEngine::setLoopSizeSec(double sizeSec) noexcept {
    if (track_ == nullptr || sizeSec <= 0.0) return;
    loopOut_ = std::min<int64_t>(loopIn_ + (int64_t)(sizeSec * deviceRate_),
                                 track_->numFrames);
}

// ================================ render ===============================

// ================================ sync =================================

void DeckEngine::setSyncMode(int m) noexcept {
    syncMode_ = std::clamp(m, 0, 3);
    if (syncMode_ == 0) {
        blockCorr_    = 0.0;
        syncErrFrames_ = 0.0;
        syncLimited_  = false;
    }
}

void DeckEngine::applySyncCorrection(double masterEffBpm,
                                     double masterPhaseAfter,
                                     int blockFrames) noexcept {
    if (syncMode_ == 0 || grid_ == nullptr || track_ == nullptr ||
        !playing_ || masterEffBpm <= 0.0) {
        blockCorr_ = 0.0;
        return;
    }
    const double pos = displayedPos();
    const double ownBpm = grid_->bpmAt(grid_->frameToBeat((int64_t)pos));
    if (ownBpm <= 0.0) { blockCorr_ = 0.0; return; }

    // Tempo: the synced deck's total rate becomes masterBpm/ownBpm —
    // the user's pitch fader is ignored while synced. The multiplier is
    // slewed per block so BPM changes are smooth (no clicks).
    const double lo = 1.0 - (double)pitchRange_ / 100.0;
    const double hi = 1.0 + (double)pitchRange_ / 100.0;
    const double target = masterEffBpm / ownBpm;
    syncLimited_ = target < lo || target > hi;
    const double tgt = std::clamp(target, lo, hi);
    syncMulSm_ += std::clamp(tgt - syncMulSm_, -0.002, 0.002);

    blockCorr_ = 0.0;
    if (syncMode_ < 2)
        return;

    // Phase: compare the beat phase both decks will reach at the END of
    // this block so correction is independent of deck render order.
    const double ownBeatAfter = grid_->frameToBeat(
        (int64_t)(pos + syncMulSm_ * blockFrames));
    double err = masterPhaseAfter - ownBeatAfter;
    err -= std::floor(err + 0.5); // wrap to [-0.5, 0.5]

    const double fpb = deviceRate_ * 60.0 / ownBpm;
    const double errFrames = err * fpb;
    syncErrFrames_ = errFrames;

    const double aErr = std::fabs(errFrames);
    if (aErr < 8.0) { // ~0.17 ms deadband — no jitter
        blockCorr_ = 0.0;
        return;
    }
    if (aErr > fpb * 0.35) {
        // Way off (post-seek, sync just enabled, ...): one faded snap
        // instead of a long, audible chase.
        nudgeFrames(errFrames);
        return;
    }
    // Bounded chase: correction spread across this block, max ~5% rate.
    const double cap = blockFrames * 0.05 * std::fabs(syncMulSm_);
    blockCorr_ = std::clamp(errFrames, -cap, +cap) / (double)blockFrames;
}

void DeckEngine::nudgeFrames(double frames) noexcept {
    pos_     = (double)clampPos(pos_ + frames);
    dispPos_ = pos_;
    fade_    = 0.0f;
    if (stretcher_) {
        stretcher_->reset();
        feedBase_ = (int64_t)pos_;
        pushed_ = 0;
        stretcherFeedEnded_ = false;
    }
}

double DeckEngine::effBpm() const noexcept {
    if (grid_ == nullptr || track_ == nullptr) return 0.0;
    const double bpm = grid_->bpmAt(grid_->frameToBeat((int64_t)displayedPos()));
    return bpm * std::fabs(resolvedRate());
}

double DeckEngine::phaseAfter(int blockFrames) const noexcept {
    if (grid_ == nullptr) return 0.0;
    const double r = std::fabs(resolvedRate());
    const double beat = grid_->frameToBeat(
        (int64_t)(displayedPos() + r * blockFrames));
    return beat - std::floor(beat);
}

double DeckEngine::resolvedRate() const noexcept {
    // Synced decks ignore the user pitch fader: the slewed sync
    // multiplier IS the rate (bend still applies on top, plus the
    // per-block phase correction injected by applySyncCorrection).
    const double base = (syncMode_ >= 1 ? syncMulSm_
                                        : 1.0 + (double)pitchPct_ / 100.0);
    const double r = base * (1.0 + (double)bendPct_ / 100.0) + blockCorr_;
    return reverse_ ? -r : r;
}

int64_t DeckEngine::framesToBoundary() const noexcept {
    if (track_ == nullptr) return 0;
    if (reverse_)
        return (int64_t)pos_ - (loopOn_ ? loopIn_ : 0);
    return (loopOn_ ? loopOut_ : track_->numFrames) - (int64_t)pos_;
}

void DeckEngine::wrapLoop() noexcept {
    const int64_t len = loopOut_ - loopIn_;
    if (len <= 0) return;
    if (reverse_) pos_ += (double)len;
    else          pos_ -= (double)len;
    fade_ = 0.0f; // soften the wrap edge (aligned loops stay seamless)
}

bool DeckEngine::pastEnd() const noexcept {
    if (track_ == nullptr) return true;
    return reverse_ ? pos_ <= 0.0 : pos_ >= (double)track_->numFrames;
}

int64_t DeckEngine::clampPos(double p) const noexcept {
    return (int64_t)std::clamp(p, 0.0, (double)track_->numFrames);
}

double DeckEngine::displayedPos() const noexcept {
    // In key-lock mode the transport clock is the truth (the feed head
    // runs ahead inside the stretcher and is not the sounding position).
    return keylock_ && !reverse_ && stretcher_ ? dispPos_ : pos_;
}

void DeckEngine::advanceDisp(int produced) noexcept {
    // Musical clock: the stretcher emits |rate| input-worth of music per
    // output frame, so the transport advances at the consumption rate.
    dispPos_ += std::fabs(resolvedRate()) * (double)produced;
    if (loopOn_ && loopOut_ > loopIn_) {
        const double len = (double)(loopOut_ - loopIn_);
        if (dispPos_ > (double)loopOut_)
            dispPos_ = (double)loopIn_ + std::fmod(dispPos_ - (double)loopIn_, len);
    }
    if (track_ != nullptr && dispPos_ > (double)track_->numFrames)
        dispPos_ = (double)track_->numFrames;
}

void DeckEngine::renderPcm(float* l, float* r, int n) noexcept {
    adoptPendingTrack();
    adoptPendingGrid();
    adoptPendingStems();   // sets may arrive mid-playback (Phase 21)

    if (!playing_ || track_ == nullptr) {
        std::memset(l, 0, (size_t)n * sizeof(float));
        std::memset(r, 0, (size_t)n * sizeof(float));
        blockCorr_ = 0.0;
        publishStats();
        return;
    }

    if (keylock_ && !reverse_ && stretcher_)
        renderStretch(l, r, n);
    else
        renderVarispeed(l, r, n);

    blockCorr_ = 0.0; // one-shot: consumed this block
    applyPostChain(l, r, n);
    publishStats();
    seeking_ = false; // transient state: cleared after one published frame
}

void DeckEngine::renderVarispeed(float* l, float* r, int n) noexcept {
    const float* s = track_->samples.data();
    const int64_t frames = track_->numFrames;
    int produced = 0;
    const bool stemFx = stemsActive() && anyStemFx();
    if (stemFx) clearStemFxStage(n);

    while (produced < n) {
        const double rate = resolvedRate();
        if (rate == 0.0 || pastEnd()) break;

        const int64_t toBoundary = framesToBoundary();
        if (toBoundary <= 0) {
            if (loopOn_) { wrapLoop(); continue; }
            break; // end of track
        }

        const int step = (int)std::min<int64_t>(
            (int64_t)(toBoundary / std::fabs(rate)) + 1, n - produced);

        for (int i = 0; i < step; ++i) {
            float sl, sr;
            if (stemsActive())                      // Phase 21 stems
                stemsMixAt(pos_, sl, sr, produced + i);
            else
                readCubicStereo(s, frames, pos_, sl, sr);
            const float f = fade_ < 1.0f ? (fade_ = std::min(1.0f, fade_ + 1.0f/256.0f)) : 1.0f;
            l[produced + i] = sl * f;
            r[produced + i] = sr * f;
            pos_ += rate;
        }
        produced += step;

        if (loopOn_ && framesToBoundary() <= 0)
            wrapLoop();
    }
    if (stemFx) applyStemFx(l, r, produced, 1);
    std::memset(l + produced, 0, (size_t)(n - produced) * sizeof(float));
    std::memset(r + produced, 0, (size_t)(n - produced) * sizeof(float));
    advanceSlip(produced);

    if (pastEnd() && playing_) {
        playing_ = false;
        trackEnded_ = true;
        if (stats_) stats_->state.store((int)DeckState::Paused);
    }
}

void DeckEngine::renderStretch(float* l, float* r, int n) noexcept {
    const int64_t frames = track_->numFrames;
    const float* s = track_->samples.data();

    // Keep the stretcher rate in sync with resolved pitch.
    const double want = std::fabs(resolvedRate());
    if (stretcher_->rate() != want)
        stretcher_->setRate(want);

    int produced = 0;
    while (produced < n) {
        const size_t got = stretcher_->pull(scratch_.data(), (size_t)(n - produced));
        for (size_t i = 0; i < got; ++i) {
            const float f = fade_ < 1.0f ? (fade_ = std::min(1.0f, fade_ + 1.0f/256.0f)) : 1.0f;
            l[produced + (int)i] = scratch_[i * 2]     * f;
            r[produced + (int)i] = scratch_[i * 2 + 1] * f;
        }
        produced += (int)got;
        if (produced >= n) break;

        // Feed more source.
        if (pos_ >= (double)frames) {
            stretcher_->finishInput();
            const size_t more = stretcher_->pull(scratch_.data(), (size_t)(n - produced));
            for (size_t i = 0; i < more; ++i) {
                l[produced + (int)i] = scratch_[i * 2];
                r[produced + (int)i] = scratch_[i * 2 + 1];
            }
            produced += (int)more;
            break;
        }
        // Push one chunk, honouring the loop wrap.
        int64_t chunk = kFeedChunk;
        const int64_t toBoundary = framesToBoundary();
        chunk = std::min<int64_t>(chunk, std::max<int64_t>(toBoundary, 0));
        if (chunk <= 0 && loopOn_) { wrapLoop(); continue; }
        if (chunk <= 0) {
            pos_ = (double)frames; // at end — will finishInput above
            continue;
        }
        const int64_t ipos = clampPos(pos_);
        const float* feed = s + ipos * 2;
        if (stemsActive() && (size_t)chunk * 2 <= stemStage_.size()) {
            // Phase 21: stage the stem mix — the stretcher is
            // content-agnostic, so the same WSOLA path stretches all
            // stems identically (§37).
            const bool stemFx = anyStemFx();
            if (stemFx) clearStemFxStage((int)chunk);
            for (int64_t i = 0; i < chunk; ++i)
                stemsMixAt((double)(ipos + i),
                           stemStage_[(size_t)i * 2],
                           stemStage_[(size_t)i * 2 + 1],
                           (int)i);
            // Phase 25: stem FX runs on the staged (source-domain)
            // block so the stretcher still sees coherent audio.
            if (stemFx)
                applyStemFx(stemStage_.data(), stemStage_.data() + 1,
                            (int)chunk, 2);
            feed = stemStage_.data();
        }
        pushed_ += stretcher_->push(feed, (size_t)chunk);
        pos_ += (double)chunk;
        if (loopOn_ && framesToBoundary() <= 0) {
            wrapLoop();
            // WSOLA is content-agnostic; the stream continues from loopIn.
        }
    }
    std::memset(l + produced, 0, (size_t)(n - produced) * sizeof(float));
    std::memset(r + produced, 0, (size_t)(n - produced) * sizeof(float));

    advanceDisp(produced);
    advanceSlip(produced);

    if (produced == 0 && pos_ >= (double)frames && playing_) {
        playing_ = false;
        trackEnded_ = true;
        if (stats_) stats_->state.store((int)DeckState::Paused);
    }
}

void DeckEngine::applyPostChain(float* l, float* r, int n) noexcept {
    // Source-level output: track gain + play/pause mute smoothing.
    // Channel trim/EQ/filter/fader live in the mixer's ChannelStrip.
    const float target = playing_ ? trackGain_ : 0.0f;
    constexpr float kSmooth = 0.001f;
    float peakL = 0.0f, peakR = 0.0f;
    for (int i = 0; i < n; ++i) {
        smGain_ += kSmooth * (target - smGain_);
        const float gl = l[i] * smGain_, gr = r[i] * smGain_;
        l[i] = gl; r[i] = gr;
        peakL = std::max(peakL, std::fabs(gl));
        peakR = std::max(peakR, std::fabs(gr));
    }
    if (stats_) {
        stats_->levelL.store(peakL, std::memory_order_relaxed);
        stats_->levelR.store(peakR, std::memory_order_relaxed);
    }
}

void DeckEngine::publishStats() noexcept {
    if (stats_ == nullptr) return;
    const int64_t disp = (int64_t)std::clamp(displayedPos(), 0.0,
                                             track_ ? (double)track_->numFrames : 0.0);
    stats_->framesRead.store(disp, std::memory_order_relaxed);
    stats_->positionSec.store(disp / deviceRate_, std::memory_order_relaxed);
    stats_->streamEnded.store(trackEnded_, std::memory_order_relaxed);
    stats_->pitchPct.store(pitchPct_, std::memory_order_relaxed);
    stats_->pitchRange.store(pitchRange_, std::memory_order_relaxed);
    stats_->bendPct.store(bendPct_, std::memory_order_relaxed);
    stats_->quantize.store(quantize_, std::memory_order_relaxed);
    stats_->keylock.store(keylock_, std::memory_order_relaxed);
    stats_->reverse.store(reverse_, std::memory_order_relaxed);
    stats_->loopOn.store(loopOn_, std::memory_order_relaxed);
    stats_->loopInSec.store(loopIn_ / deviceRate_, std::memory_order_relaxed);
    stats_->loopOutSec.store(loopOut_ / deviceRate_, std::memory_order_relaxed);
    // ---- performance echoes ----
    stats_->slip.store(slipActive_, std::memory_order_relaxed);
    stats_->syncMode.store(syncMode_, std::memory_order_relaxed);
    stats_->syncErrMs.store((float)(syncErrFrames_ / deviceRate_ * 1000.0),
                            std::memory_order_relaxed);
    stats_->syncLimited.store(syncLimited_, std::memory_order_relaxed);
    if (grid_ != nullptr && track_ != nullptr) {
        const double beat = grid_->frameToBeat(disp);
        const double gbpm = grid_->bpmAt(beat);
        stats_->bpm.store(gbpm, std::memory_order_relaxed);
        stats_->anchorSec.store(grid_->firstBeatFrame / grid_->rate(),
                                std::memory_order_relaxed);
        stats_->beatPos.store(beat, std::memory_order_relaxed);
        stats_->barPos.store((double)grid_->barOf(beat),
                             std::memory_order_relaxed);
        stats_->effBpm.store(gbpm * std::fabs(resolvedRate()),
                             std::memory_order_relaxed);
    } else {
        stats_->bpm.store(0.0, std::memory_order_relaxed);
        stats_->beatPos.store(-1.0, std::memory_order_relaxed);
        stats_->barPos.store(0.0, std::memory_order_relaxed);
        stats_->effBpm.store(0.0, std::memory_order_relaxed);
    }
    if (seeking_)
        stats_->state.store((int)DeckState::Seeking, std::memory_order_relaxed);
    else if (track_ != nullptr)
        stats_->state.store(playing_ ? (int)DeckState::Playing
                                     : (int)DeckState::Paused, std::memory_order_relaxed);
    publishStemStats();   // stemMode/stemsAudible echoes (Phase 21)
}

} // namespace onda
