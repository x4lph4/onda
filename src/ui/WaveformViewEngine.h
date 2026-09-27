#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "VisualizationState.h"
#include "WaveformMapping.h"
#include "core/analysis/BeatGrid.h"
#include "core/analysis/WaveformData.h"
#include "core/model/DeckStats.h"
#include "ui/UiState.h"

namespace onda::ui {

/// Phase 19+31 — professional waveform/deck visualization engine.
///
/// Pure, JUCE-free model that turns analysis data (WaveformData +
/// BeatGrid + DeckSnapshot + markers) into a render plan a
/// FrameRenderer backend (JUCE raster today) draws. Split per the
/// phase architecture:
///
///   WaveformViewEngine
///   ├── WaveformViewport   (sec↔px transform — alias of the proven
///   │                      WaveformMapping; one implementation kept)
///   ├── WaveformZoom       (anchor policy: cursor / playhead / center)
///   ├── WaveformAutoScroll (Off / Center / Smart)
///   ├── PlayheadSmoother   (visual-only interpolation + jump detect)
///   ├── WaveformRenderer   (buildColumns — amplitude columns)
///   ├── BeatGridRenderer / BeatMarkerRenderer (buildMarkers, bar nos)
///   ├── CueRenderer        (buildCueFlags — hot/memory/primary types,
///   │                      §33 collision tiers, §47-48 index)
///   ├── LoopRenderer       (buildLoopRects — active + saved regions,
///   │                      prefix-max left-edge index)
///   ├── VisualizationState (vis — §2 layer gates + §45 perf degrade)
///   ├── WaveformInteraction (hit tests + drags → WaveCmd, §42;
///   │                      §53 SeekBegin/Scrub/Commit/Cancel session)
///   ├── WaveformCache      (cache keys — view stores the image)
///   └── WaveformDiagnostics (counters/timings the shell may poll)
///
/// Everything here is plain values — no colors, no JUCE. The view maps
/// semantic fields (crest, kind, tier, severity) to theme colors, so
/// tests assert geometry/data and the paint layer stays thin.

// ==========================================================================
// Viewport — the spec name for the mapping type
// ==========================================================================
using WaveformViewport = WaveformMapping;

// ==========================================================================
// WaveformZoom — anchored zoom policy (§33-34)
// ==========================================================================
enum class ZoomAnchor : int { Cursor = 0, Playhead, Center };

struct WaveformZoom {
    /// Multiplicative step per wheel notch / zoom command.
    double step = 1.25;
    /// Default detail zoom (px/sec) — reset target.
    double defaultPxPerSec = 160.0;

    double anchorSec(const WaveformViewport& v, ZoomAnchor a,
                     double cursorPx, double playheadSec) const {
        switch (a) {
            case ZoomAnchor::Playhead: return playheadSec;
            case ZoomAnchor::Center:   return v.scrollSec
                                            + v.visibleSec() * 0.5;
            case ZoomAnchor::Cursor:
            default:                   return v.pxToSec(cursorPx);
        }
    }
    /// factor > 1 zooms in, < 1 out — anchor stays put (§34).
    void apply(WaveformViewport& v, double factor, ZoomAnchor a,
               double cursorPx, double playheadSec) const {
        v.zoomAt(factor, anchorSec(v, a, cursorPx, playheadSec));
    }
    void in (WaveformViewport& v, ZoomAnchor a, double px,
             double ph) const { apply(v, step, a, px, ph); }
    void out(WaveformViewport& v, ZoomAnchor a, double px,
             double ph) const { apply(v, 1.0 / step, a, px, ph); }
    void reset(WaveformViewport& v, double playheadSec) const {
        v.pxPerSec = defaultPxPerSec;
        v.ensureVisible(playheadSec, 0.25);
    }
};

// ==========================================================================
// WaveformAutoScroll — how the detail view follows the playhead (§35)
// ==========================================================================
enum class AutoScrollMode : int { Off = 0, Center = 1, Smart = 2 };

struct WaveformAutoScroll {
    /// Ease rate for Center mode (1/s) — smooth, never instant.
    double ease = 8.0;

    /// Move the viewport for this tick. Playing only — paused/seeking
    /// never drags the view. Center eases toward playhead-centred;
    /// Smart reveals the playhead only when it nears the margin.
    void apply(WaveformViewport& v, double posSec, bool playing,
               AutoScrollMode mode, double dtSec) const {
        if (!playing || mode == AutoScrollMode::Off) return;
        if (mode == AutoScrollMode::Center) {
            const double target =
                std::clamp(posSec - v.visibleSec() * 0.5, 0.0,
                           std::max(0.0,
                                    v.durationSec - v.visibleSec()));
            const double k = 1.0 - std::exp(-ease * dtSec);
            v.scrollTo(v.scrollSec + (target - v.scrollSec) * k);
        } else { // Smart
            v.ensureVisible(posSec, 0.35);
        }
    }
};

// ==========================================================================
// PlayheadSmoother — visual interpolation (§10-11) + beat-jump detect
// ==========================================================================
/// The REAL position always comes from the deck snapshot. This helper
/// only computes the pixel the UI draws: during playback it advances
/// between ticks and gently catches up to the reported position, so a
/// 30 Hz feed looks continuous. It never writes back to any engine.
/// A position discontinuity is reported as a jump event (seek, beat
/// jump, loop exit) — the view flashes a marker but the waveform
/// history is untouched (§22).
struct PlayheadSmoother {
    double drawSec       = 0.0;
    bool   primed        = false;
    double jumpSec       = 0.40;   // |Δ| beyond this ⇒ jump, not drift
    double catchUpRate   = 30.0;   // 1/s — ~full converge per 30Hz tick
    int    jumps         = 0;
    double lastJumpPosSec = 0.0;   // where the last jump landed

    /// rate = playback rate estimate (effBpm/bpm or 1.0 when unknown).
    double update(double posSec, bool playing, double dtSec,
                  double rate = 1.0) {
        if (dtSec < 0.0) dtSec = 0.0;
        const double predicted = primed
            ? drawSec + dtSec * rate : posSec;
        if (!primed || std::abs(posSec - predicted) > jumpSec) {
            if (primed) { ++jumps; lastJumpPosSec = posSec; }
            drawSec = posSec;
            primed = true;
            return drawSec;
        }
        if (playing) {
            drawSec = predicted;
            // converge: kill small lag/lead without visible stepping
            const double k =
                std::min(1.0, dtSec * catchUpRate);
            drawSec += (posSec - drawSec) * k;
        } else {
            drawSec = posSec;           // paused/seeking: exact
        }
        return drawSec;
    }
    void reset() { primed = false; jumps = 0; }
};

// ==========================================================================
// Render plan — what the JUCE layer draws (§43: blit, don't rebuild)
// ==========================================================================

/// One pixel column of waveform, normalized 0..1 amplitudes.
/// `crest` = peak/RMS ratio folded to 0..1 — drives the optional
/// spectral-style tint (§9, honest: derived from amplitude data only).
struct WaveColumn {
    float env   = 0.0f;   // peak envelope |min|max| 0..1
    float rms   = 0.0f;   // RMS 0..1
    float crest = 0.0f;   // transient-ness 0..1
};

/// Beat/bar/downbeat tick in detail px. kind: 0 beat, 1 bar,
/// 2 downbeat. `barNo` ≥ 0 ⇒ the view may print the number (§13-14).
/// `current` marks the beat/bar under the playhead (§29 emphasis).
struct BeatMarkerPx {
    float px    = 0.0f;
    int   kind  = 0;
    int   barNo = -1;
    bool  current  = false;
    bool  gridEdit = false;   // emphasized while grid-edit mode is on
};

struct CueFlagPx {
    float px    = 0.0f;
    int   slot  = -1;         // 0..7 hot-cue slot, -1 = unslotted
    int   type  = 0;          // 0 hot, 1 memory, 2 loop-cue, 3 primary
    uint32_t argb = 0;
    char  label[16] = {};
    bool  armedDelete = false; // armed for confirm-delete (§19)
    /// §33 — label collision tier: flags closer than the label width
    /// stack their labels on alternating rows instead of overlapping.
    /// The marker itself never moves (positions are the truth).
    int   tier = 0;
};

struct LoopRectPx {
    float x0 = 0.0f, x1 = 0.0f;
    bool  active = false;
    bool  saved  = false;     // saved loop vs the live deck loop
    int   id     = -1;
};

/// Future phrase markers (§30) — architecture seam, empty today.
struct PhraseMarker { double sec = 0.0; int index = -1; };

/// State chip the view draws right-aligned on the detail strip.
/// severity: 0 info, 1 accent, 2 warn, 3 danger.
struct StateBadge { char text[28] = {}; int severity = 0; };

/// Active-bar highlight band (beat-phrase emphasis, §29).
struct BarBandPx { float x0 = 0.0f, x1 = 0.0f; };

struct WaveformFrame {
    // detail strip
    std::vector<WaveColumn>  cols;
    std::vector<BeatMarkerPx> beats;
    std::vector<CueFlagPx>   cues;
    std::vector<LoopRectPx>  loops;
    std::vector<PhraseMarker> phrases;   // seam (§30)
    BarBandPx barBand;
    bool      hasBarBand = false;
    double    playheadPx  = 0.0;        // smoothed draw position
    /// §53 — drag-seek ghost: while a wave drag is active the frame
    /// carries the scrub position so the view can draw a preview
    /// playhead without touching the real one. -1 = no preview.
    double    previewPx   = -1.0;
    int       levelUsed   = -1;         // 0 overview / 1 mid / 2 detail
    int64_t   f0 = 0, f1 = 0;           // frames queried (diagnostics)
    bool      hasWave = false, hasGrid = false;
    std::vector<StateBadge> badges;

    void clear() {
        cols.clear(); beats.clear(); cues.clear(); loops.clear();
        phrases.clear(); badges.clear();
        hasBarBand = false; playheadPx = 0.0; previewPx = -1.0;
        levelUsed = -1; hasWave = hasGrid = false;
    }
};

// ==========================================================================
// WaveformCache — key for the raster image the view keeps (§43)
// ==========================================================================
/// FNV-1a over everything that affects the rasterized waveform strip:
/// viewport geometry, data identity, style and pixel scale. When the
/// key matches, the view blits its cached image instead of re-running
/// WaveformData::query + column math.
struct WaveformCache {
    static uint64_t hashMix(uint64_t h, double d) {
        uint64_t u;
        std::memcpy(&u, &d, sizeof u);
        h ^= u; h *= 1099511628211ULL;
        return h;
    }
    static uint64_t key(const WaveformViewport& v, int wPx,
                        const void* wfPtr, int64_t wfFrames,
                        int style, double scale) {
        uint64_t h = 1469598103934665603ULL;
        h = hashMix(h, v.scrollSec);
        h = hashMix(h, v.pxPerSec);
        h = hashMix(h, (double)wPx);
        h = hashMix(h, (double)(uintptr_t)wfPtr);
        h = hashMix(h, (double)wfFrames);
        h = hashMix(h, (double)style);
        h = hashMix(h, scale);
        return h;
    }
};

// ==========================================================================
// WaveformInteraction — hit tests + drags → commands (§42)
// ==========================================================================

/// Zones a press can land on, in priority order (top → bottom).
enum class HitZone : int {
    None = 0, CueFlag, LoopIn, LoopOut, LoopBody, Wave, OverviewWindow
};

struct WaveHit {
    HitZone zone   = HitZone::None;
    int     index  = -1;   // cue flag index or loop index in the frame
    double  sec    = 0.0;  // sec under the cursor
};

/// What the view should do — translated to engine calls by the host
/// (AppShell), never performed inside the component (§41-42).
enum class WaveCmdKind : int {
    None = 0,
    Seek,            // a = sec              — click/drag seek (§31)
    ScrubSeek,       // a = sec              — rapid scrub (§36)
    CueTrigger,      // slot                 — click a cue flag (§19)
    CueSet,          // slot, a = sec        — set/move cue at sec
    CueDelete,       // slot                 — confirmed delete
    CueArmDelete,    // slot                 — first strike arms it
    LoopSet,         // a = inSec, b = outSec — new/edited loop (§21)
    GridNudgeMs,     // a = delta ms         — grid drag (§17)
    GridSetFirstBeat,// a = sec of click     — set beat 0 (§16)
    ZoomAt,          // a = factor, b = px   — wheel zoom (§33-34)
    ScrollDetail,    // a = delta sec        — overview pan
    // §53 — a wave drag is a seek transaction: Begin when the press
    // lands, live ScrubSeek while dragging, Commit on release with the
    // final position, Cancel when the host/user aborts (Esc, right-
    // click mid-drag). Hosts can bracket scrub state (e.g. slip-arms).
    SeekBegin,
    SeekCommit,      // a = final sec
    SeekCancel
};

struct WaveCmd { WaveCmdKind kind = WaveCmdKind::None;
                 int slot = -1; double a = 0.0, b = 0.0; };

/// Drag state machine — header-friendly, fully testable.
struct WaveformInteraction {
    bool   gridEdit = false;

    // arm-to-delete: right-click on a cue flag arms it; a second within
    // the window confirms. Prevents accidental cue loss (§19).
    double armWindowMs = 2500.0;
    int    armedSlot   = -1;
    double armedAtMs   = -1.0;

    // active drag
    HitZone dragZone    = HitZone::None;
    int     dragIndex   = -1;
    double  dragLastPx  = 0.0;
    double  dragStartSec = 0.0;
    bool    dragged     = false;
    /// §53 — a wave scrub is a transaction: SeekBegin on first move,
    /// live ScrubSeek while held, SeekCommit on release, SeekCancel
    /// on abort. Click-without-drag never opens a session.
    bool    seekSession_ = false;
    double  seekPreviewSec = -1.0;   // ghost position during scrub

    /// Hit-test a press on the detail strip. `cueTopPx` is the height
    /// of the flag zone; `edgePx` the grab tolerance on loop edges.
    static WaveHit hitTest(double px, double py,
                           const WaveformFrame& f,
                           const WaveformViewport& v,
                           double cueTopPx = 14.0,
                           double edgePx   = 6.0) {
        WaveHit h;
        h.sec = v.pxToSec(px);
        // cue flags live in the top strip — highest priority
        if (py <= cueTopPx) {
            double best = cueTopPx;
            for (size_t i = 0; i < f.cues.size(); ++i) {
                const double d = std::abs(f.cues[i].px - px);
                if (d <= 8.0 && d < best) {
                    best = d; h.zone = HitZone::CueFlag;
                    h.index = (int)i;
                }
            }
            if (h.zone != HitZone::None) return h;
        }
        // loop edges then body
        for (size_t i = 0; i < f.loops.size(); ++i) {
            const auto& L = f.loops[i];
            if (std::abs(L.x0 - px) <= edgePx) {
                h.zone = HitZone::LoopIn;  h.index = (int)i; return h;
            }
            if (std::abs(L.x1 - px) <= edgePx) {
                h.zone = HitZone::LoopOut; h.index = (int)i; return h;
            }
            if (px > L.x0 && px < L.x1) {
                h.zone = HitZone::LoopBody; h.index = (int)i;
                return h;
            }
        }
        h.zone = HitZone::Wave;
        return h;
    }

    /// Overview strip press: inside the detail-viewport window ⇒ pan,
    /// outside ⇒ seek.
    static WaveHit hitTestOverview(double px,
                                   const WaveformViewport& detail,
                                   const WaveformViewport& over) {
        WaveHit h;
        h.sec = over.pxToSec(px);
        const double v0 = over.secToPx(detail.scrollSec);
        const double v1 = over.secToPx(detail.endSec());
        h.zone = (px >= v0 && px <= v1) ? HitZone::OverviewWindow
                                        : HitZone::Wave;
        return h;
    }

    /// Press → first command (or drag setup).
    WaveCmd press(const WaveHit& h, const WaveformFrame& f,
                bool rightBtn, double nowMs) {
        WaveCmd c;
        switch (h.zone) {
            case HitZone::CueFlag: {
                const auto& cf = f.cues[(size_t)h.index];
                if (rightBtn) {
                    if (armedSlot == cf.slot &&
                        nowMs - armedAtMs <= armWindowMs) {
                        armedSlot = -1;
                        c.kind = WaveCmdKind::CueDelete;
                        c.slot = cf.slot;
                        return c;
                    }
                    armedSlot = cf.slot; armedAtMs = nowMs;
                    c.kind = WaveCmdKind::CueArmDelete; c.slot = cf.slot;
                    return c;
                }
                dragZone = HitZone::CueFlag; dragIndex = h.index;
                dragLastPx = h.sec; dragged = false;
                return c;                    // decide on release
            }
            case HitZone::LoopIn:
            case HitZone::LoopOut:
                dragZone = h.zone; dragIndex = h.index;
                dragLastPx = h.sec;
                return c;                    // live drags follow
            case HitZone::LoopBody:
                dragZone = h.zone; dragIndex = h.index;
                dragLastPx = h.sec; dragStartSec = h.sec;
                return c;
            case HitZone::OverviewWindow:
                dragZone = h.zone; dragLastPx = h.sec;
                return c;
            case HitZone::Wave:
            default:
                dragZone = HitZone::Wave; dragLastPx = h.sec;
                if (gridEdit) {
                    c.kind = WaveCmdKind::GridSetFirstBeat;
                    c.a = h.sec;
                    return c;
                }
                c.kind = WaveCmdKind::Seek; c.a = h.sec;
                return c;
        }
    }

    /// Drag → live command (grid nudge / seek / loop edges).
    WaveCmd drag(double px, const WaveformViewport& v) {
        WaveCmd c;
        const double sec = v.pxToSec(px);
        switch (dragZone) {
            case HitZone::CueFlag:
            case HitZone::LoopIn:
            case HitZone::LoopOut:
            case HitZone::LoopBody:
                dragged = true; dragLastPx = sec; return c;  // at release
            case HitZone::OverviewWindow:
                c.kind = WaveCmdKind::ScrollDetail;
                c.a = -(sec - dragLastPx);   // content follows finger
                dragLastPx = sec;
                return c;
            case HitZone::Wave:
            default:
                if (gridEdit) {
                    c.kind = WaveCmdKind::GridNudgeMs;
                    c.a = (sec - dragLastPx) * 1000.0;
                    dragLastPx = sec;
                    return c;
                }
                dragged = true;
                seekPreviewSec = sec;
                if (!seekSession_) {
                    // first move of the scrub — the session opens;
                    // the position arrives on the next ScrubSeek.
                    seekSession_ = true;
                    dragLastPx = sec;
                    c.kind = WaveCmdKind::SeekBegin;
                    c.a = sec;
                    return c;
                }
                c.kind = WaveCmdKind::ScrubSeek; c.a = sec;
                dragLastPx = sec;
                return c;
        }
    }

    /// Release → deferred command for flagged drags.
    WaveCmd release(const WaveformFrame& f, bool rightBtn,
                    const WaveformViewport& v) {
        WaveCmd c;
        const HitZone z = dragZone; const int i = dragIndex;
        dragZone = HitZone::None; dragIndex = -1;
        if (z == HitZone::CueFlag && i >= 0 && i < (int)f.cues.size()) {
            const auto& cf = f.cues[(size_t)i];
            if (!dragged && !rightBtn) {
                c.kind = WaveCmdKind::CueTrigger; c.slot = cf.slot;
            } else if (dragged) {
                c.kind = WaveCmdKind::CueSet; c.slot = cf.slot;
                c.a = std::clamp(dragLastPx, 0.0, v.durationSec);
            }
        } else if ((z == HitZone::LoopIn || z == HitZone::LoopOut) &&
                   i >= 0 && i < (int)f.loops.size()) {
            const auto& L = f.loops[(size_t)i];
            const double in  = z == HitZone::LoopIn
                ? dragLastPx : v.pxToSec(L.x0);
            const double out = z == HitZone::LoopOut
                ? dragLastPx : v.pxToSec(L.x1);
            c.kind = WaveCmdKind::LoopSet;
            c.a = std::clamp(std::min(in, out), 0.0, v.durationSec);
            c.b = std::clamp(std::max(in, out), 0.0, v.durationSec);
        } else if (z == HitZone::LoopBody && i >= 0 &&
                   i < (int)f.loops.size()) {
            const auto& L = f.loops[(size_t)i];
            const double len  = (L.x1 - L.x0) / v.pxPerSec;
            const double in   = std::clamp(
                v.pxToSec(L.x0) + (dragLastPx - dragStartSec),
                0.0, std::max(0.0, v.durationSec - len));
            c.kind = WaveCmdKind::LoopSet; c.a = in; c.b = in + len;
        }
        if (seekSession_) {                 // §53 — scrub commit
            seekSession_ = false;
            c.kind = WaveCmdKind::SeekCommit;
            c.a = std::clamp(seekPreviewSec >= 0.0 ? seekPreviewSec
                                                 : dragLastPx,
                             0.0, v.durationSec);
            seekPreviewSec = -1.0;
        }
        dragged = false;
        return c;
    }

    /// §53 — abort an in-flight seek session (Esc, focus loss, second
    /// button mid-drag). Hosts decide whether to snap back or stay;
    /// the interaction only reports the abort.
    WaveCmd cancelSeek() {
        WaveCmd c;
        if (seekSession_) {
            seekSession_ = false;
            seekPreviewSec = -1.0;
            dragZone = HitZone::None; dragIndex = -1;
            dragged = false;
            c.kind = WaveCmdKind::SeekCancel;
        }
        return c;
    }

    /// Disarm the pending delete (any other action / timeout clears).
    void disarm(double nowMs) {
        if (armedSlot >= 0 && nowMs - armedAtMs > armWindowMs)
            armedSlot = -1;
    }
};

// ==========================================================================
// WaveformDiagnostics — observable counters the shell polls (§ diag)
// ==========================================================================
struct WaveformDiagnostics {
    // last frame built
    int     columns = 0, markers = 0, cues = 0, loops = 0;
    int     level = -1;                    // pyramid level used
    int64_t spanFrames = 0;
    double  buildMs = 0.0;                 // frame-build time (ms)
    // cumulative
    uint64_t queries = 0, cacheHits = 0, cacheMisses = 0;
    uint64_t staleDrops = 0;               // updates skipped (no change)
    double   scaleFactor = 1.0;            // HiDPI scale seen (§44)
    int      visibleDecks = 2;
    char     warning[96] = {};             // last perf/anomaly warning

    void warn(const char* w) {
        std::snprintf(warning, sizeof warning, "%s", w);
    }
};

// ==========================================================================
// WaveformViewEngine — the orchestrator
// ==========================================================================
/// Owns the per-deck view state and produces WaveformFrames. All data
/// inputs are non-owning pointers re-pointed by the host per track —
/// the engine never decodes audio and never touches RT state.
class WaveformViewEngine {
public:
    // ---- configuration ----
    WaveformZoom       zoom;
    WaveformAutoScroll autoScroll;
    AutoScrollMode     autoScrollMode = AutoScrollMode::Smart;
    PlayheadSmoother   playhead;
    WaveformInteraction interact;
    WaveformDiagnostics diag;
    VisualizationState vis;              // §2 layer toggles + §45 mode
    int                style = 0;        // 0 clean, 1 spectral-tint
    /// Marker density limits (§13): min px between labels / markers.
    double barLabelMinPx = 36.0;
    double markerMinPx   = 6.0;
    /// §33 — min px between cue-flag labels on the same tier; closer
    /// flags stack their labels instead of overlapping.
    double cueLabelMinPx = 44.0;
    /// Marker revision — the view bumps it whenever `cues`/`loops`
    /// change so the visible-range index is rebuilt lazily (§47-48).
    uint32_t markersRev = 0;

    // ---- inputs (re-pointed by the view per snapshot/track) ----
    const WaveformData* wf   = nullptr;
    const BeatGrid*     grid = nullptr;
    const DeckSnapshot* snap = nullptr;
    std::vector<CueMarker>  cues;
    std::vector<LoopRegion> loops;
    std::vector<PhraseMarker> phrases;

    WaveformViewport detail;             // zoomable strip
    WaveformViewport overview;           // whole-track nav strip

    /// Called every UI tick: auto-scroll + smoothed playhead.
    /// dtSec = real elapsed seconds since last call.
    void tick(double dtSec) {
        const bool playing =
            snap && snap->state == (int)DeckState::Playing;
        if (snap && snap->durationSec > 0.0) {
            autoScroll.apply(detail, snap->positionSec, playing,
                             autoScrollMode, dtSec);
            const double rate =
                snap->bpm > 0.0 && snap->effBpm > 0.0
                    ? snap->effBpm / snap->bpm : 1.0;
            playhead.update(snap->positionSec, playing, dtSec, rate);
        }
        if (playhead.jumps > lastJumpCount_) {
            lastJumpCount_ = playhead.jumps;
            ++jumpEvents_;
        }
    }

    /// Build the detail-strip frame. Pure w.r.t. inputs — writes only
    /// to `out` (reused; no allocation churn after warm-up).
    void buildDetail(WaveformFrame& out) {
        const auto t0 = std::chrono::steady_clock::now();
        out.clear();
        const int n = std::max(0, detail.widthPx);
        if (snap) diag.visibleDecks = 2;   // host may overwrite

        const bool haveWave = wf && !wf->empty() &&
                              wf->sampleRate > 0.0 &&
                              snap && snap->durationSec > 0.0;
        out.hasWave = haveWave;
        out.hasGrid = grid && !grid->empty();

        if (haveWave && n > 0) {
            // ---- WaveformRenderer: columns (§2-3: pyramid select) ----
            detail.frameRange(out.f0, out.f1, wf->sampleRate);
            out.levelUsed = wf->queryLevel(out.f0, out.f1, n);
            binsBuf_.clear();
            wf->query(out.f0, out.f1, n, binsBuf_, 0);
            ++diag.queries;
            buildColumns(binsBuf_, out);
        }

        // ---- BeatGridRenderer / BeatMarkerRenderer (§12-14) ----
        if (out.hasGrid && wf && wf->sampleRate > 0.0 &&
            vis.showBeatgrid)
            buildMarkers(out);

        // ---- CueRenderer (§18, §2 flags) ----
        if (vis.showCues) buildCueFlags(out);

        // ---- LoopRenderer (§20) ----
        if (vis.showLoops) buildLoopRects(out);

        // ---- phrase seam (§30) ----
        if (vis.showPhrases)
            for (auto& pm : phrases) {
                const double px = detail.secToPx(pm.sec);
                if (px >= -8.0 && px <= n + 8.0)
                    out.phrases.push_back(pm);
            }

        // ---- playhead (§10-11): smoothed draw position + §53 ghost --
        out.playheadPx = detail.secToPx(playhead.drawSec);
        if (interact.seekSession_)
            out.previewPx = detail.secToPx(interact.seekPreviewSec);

        // ---- state badges (§38-40) ----
        buildBadges(out);

        diag.columns = (int)out.cols.size();
        diag.markers = (int)out.beats.size();
        diag.cues    = (int)out.cues.size();
        diag.loops   = (int)out.loops.size();
        diag.level   = out.levelUsed;
        diag.spanFrames = out.f1 - out.f0;
        diag.buildMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        if (diag.buildMs > 8.0) diag.warn("frame build > 8ms");
    }

    /// Build the overview-strip frame (whole track, no grid markers —
    /// cues + loops only, §7).
    void buildOverview(WaveformFrame& out) {
        out.clear();
        const int n = std::max(0, overview.widthPx);
        const bool haveWave = wf && !wf->empty() &&
                              wf->sampleRate > 0.0 &&
                              snap && snap->durationSec > 0.0;
        out.hasWave = haveWave;
        out.hasGrid = grid && !grid->empty();
        if (haveWave && n > 0) {
            overview.frameRange(out.f0, out.f1, wf->sampleRate);
            out.levelUsed = wf->queryLevel(out.f0, out.f1, n);
            ovBins_.clear();
            wf->query(out.f0, out.f1, n, ovBins_, 0);
            ++diag.queries;
            buildColumns(ovBins_, out);
        }
        if (vis.showCues) buildCueFlagsFor(overview, out);
        if (vis.showLoops) buildLoopRectsFor(overview, out);
        out.playheadPx = overview.secToPx(
            snap ? snap->positionSec : 0.0);
    }

    int jumpEvents() const { return jumpEvents_; }

private:
    // ---- WaveformRenderer ----
    void buildColumns(const std::vector<WaveformBin>& bins,
                      WaveformFrame& out) {
        out.cols.resize(bins.size());
        for (size_t i = 0; i < bins.size(); ++i) {
            const WaveformBin& b = bins[i];
            const float env = std::max(std::abs(b.min),
                                       std::abs(b.max));
            const float rms = std::abs(b.rms);
            // crest factor → 0..1 transient-ness. ~1.0 = flat/sustained
            // (rms≈peak), high = spiky transient. Fold at ~4×.
            const float crest = rms > 1e-6f
                ? std::min(1.0f, (env / rms - 1.0f) / 3.0f)
                : 0.0f;
            out.cols[i] = { env, rms, crest };
        }
    }

    // ---- BeatGridRenderer / BeatMarkerRenderer ----
    void buildMarkers(WaveformFrame& out) {
        // gate marker detail by zoom so low zooms stay readable (§13)
        const double secPerBeat =
            60.0 / std::max(1.0, snap ? snap->bpm : 120.0);
        const double beatPx = detail.pxPerSec * secPerBeat;
        if (beatPx < markerMinPx) return;      // too dense — omit beats

        markersBuf_.clear();
        collectGridMarkers(detail, wf->sampleRate,
            [&](int64_t f) { return grid->frameToBeat(f); },
            [&](double b)  { return grid->beatToFrame(b); },
            [&](double b)  { return grid->isDownbeat(b); },
            markersBuf_);
        const int bpb = std::max(1, grid->beatsPerBar);
        const double playBeat =
            grid->frameToBeat((int64_t)(
                playhead.drawSec * wf->sampleRate));
        const bool labelBars = vis.showBarNumbers && !vis.perfMode &&
            beatPx * bpb >= barLabelMinPx;    // room for numbers
        double lastLabelPx = -1e9;
        for (const GridMarker& gm : markersBuf_) {
            if (gm.px < -4.0 || gm.px > detail.widthPx + 4.0) continue;
            if (vis.perfMode && gm.kind == 0) continue;  // §45: bars only
            BeatMarkerPx m;
            m.px = (float)gm.px;
            m.kind = gm.kind;
            m.gridEdit = interact.gridEdit;
            if (gm.kind >= 1) {
                const int bar = gm.index / bpb;
                if (labelBars && gm.px - lastLabelPx >= barLabelMinPx) {
                    m.barNo = bar + 1;
                    lastLabelPx = gm.px;
                }
                // current-bar band: from this bar line to the next
                if (playBeat >= gm.index && playBeat < gm.index + bpb) {
                    const double x2 = detail.secToPx(
                        grid->beatToFrame(gm.index + bpb) /
                        wf->sampleRate);
                    out.barBand = { (float)gm.px, (float)x2 };
                    out.hasBarBand = x2 > gm.px;
                }
            }
            m.current = playBeat >= gm.index &&
                        playBeat < gm.index + 1.0;
            out.beats.push_back(m);
        }
    }

    // ---- CueRenderer ----
    /// §47-48 — visible-window lookup instead of a full scan. The
    /// sec-sorted index is rebuilt lazily when `markersRev` changes;
    /// each frame binary-searches the window, so hundreds of markers
    /// on a long track cost O(log n + visible), never O(n).
    void ensureCueIndex() {
        // rebuild triggers: explicit markersRev bump (the view's
        // contract for in-place edits) or a vector swap that changed
        // size/backing pointer — cheap guards so callers that assign
        // `cues`/`loops` wholesale never see a stale index.
        if (markersRev == idxRev_ &&
            cueIdx_.size() == cues.size() &&
            loopIdx_.size() == loops.size() &&
            idxCuesData_ == cues.data() &&
            idxLoopsData_ == loops.data())
            return;
        idxRev_ = markersRev;
        idxCuesData_ = cues.data();
        idxLoopsData_ = loops.data();
        cueIdx_.clear();
        cueIdx_.reserve(cues.size());
        for (size_t i = 0; i < cues.size(); ++i)
            cueIdx_.push_back(&cues[i]);
        std::sort(cueIdx_.begin(), cueIdx_.end(),
                  [](const CueMarker* a, const CueMarker* b) {
                      return a->sec < b->sec;
                  });
        loopIdx_.clear();
        loopIdx_.reserve(loops.size());
        for (size_t i = 0; i < loops.size(); ++i)
            loopIdx_.push_back(&loops[i]);
        std::sort(loopIdx_.begin(), loopIdx_.end(),
                  [](const LoopRegion* a, const LoopRegion* b) {
                      return a->inSec < b->inSec;
                  });
        // prefix-max outSec: a loop may start long before the window
        // yet still cross its left edge; loopMaxOut_[i] bounds how
        // far back any reaching loop can live (§48).
        loopMaxOut_.assign(loopIdx_.size(), 0.0);
        for (size_t i = 0; i < loopIdx_.size(); ++i)
            loopMaxOut_[i] = std::max(i ? loopMaxOut_[i - 1] : 0.0,
                                      loopIdx_[i]->outSec);
    }

    bool cueVisible(int type) const {
        switch (type) {
            case 1:  return vis.showMemoryCues;
            case 3:  return vis.showPrimaryCue;
            case 0:
            default: return vis.showHotCues;
        }
    }

    void buildCueFlagsFor(const WaveformViewport& v,
                          WaveformFrame& out) {
        ensureCueIndex();
        // visible window → [lo, hi) in the sorted index
        const double s0 = v.pxToSec(-10.0);
        const double s1 = v.pxToSec(v.widthPx + 10.0);
        auto lo = std::lower_bound(cueIdx_.begin(), cueIdx_.end(), s0,
            [](const CueMarker* m, double s) { return m->sec < s; });
        auto hi = std::upper_bound(lo, cueIdx_.end(), s1,
            [](double s, const CueMarker* m) { return s < m->sec; });
        // §33 — collision tiers: labels closer than cueLabelMinPx on
        // the same tier step down a row. Tiers 0-2; beyond that the
        // label is suppressed (marker still drawn — §32 legibility).
        double lastTierPx[3] = { -1e9, -1e9, -1e9 };
        for (; lo != hi; ++lo) {
            const CueMarker& cm = **lo;
            if (!cueVisible(cm.type)) continue;
            const double px = v.secToPx(cm.sec);
            CueFlagPx f;
            f.px = (float)px;
            f.slot = cm.slot;
            f.type = cm.type;
            f.argb = cm.argb;
            for (int t = 0; t < 3; ++t) {
                if (px - lastTierPx[t] >= cueLabelMinPx) {
                    f.tier = t;
                    lastTierPx[t] = px;
                    break;
                }
                f.tier = t + 1;
            }
            if (f.tier >= 3) f.label[0] = '\0';   // crowd: hide label
            else std::snprintf(f.label, sizeof f.label, "%s",
                               cm.label.c_str());
            f.armedDelete = interact.armedSlot == cm.slot &&
                            cm.slot >= 0;
            out.cues.push_back(f);
        }
    }
    void buildCueFlags(WaveformFrame& out) {
        buildCueFlagsFor(detail, out);
    }

    // ---- LoopRenderer ----
    void buildLoopRectsFor(const WaveformViewport& v,
                           WaveformFrame& out) {
        ensureCueIndex();
        const double s0 = v.pxToSec(-4.0);
        const double s1 = v.pxToSec(v.widthPx + 4.0);
        // binary-search the window start, then walk back over every
        // earlier loop that could still reach it (prefix-max outSec).
        size_t i = (size_t)(
            std::lower_bound(loopIdx_.begin(), loopIdx_.end(), s0,
                [](const LoopRegion* l, double s) {
                    return l->inSec < s;
                }) - loopIdx_.begin());
        while (i > 0 && loopMaxOut_[i - 1] >= s0) --i;
        for (; i < loopIdx_.size(); ++i) {
            const LoopRegion& lr = *loopIdx_[i];
            if (lr.inSec > s1) break;
            const double x0 = v.secToPx(lr.inSec);
            const double x1 = v.secToPx(lr.outSec);
            if (x1 < -4.0) continue;
            LoopRectPx r;
            r.x0 = (float)std::max(-4.0, x0);
            r.x1 = (float)std::min(v.widthPx + 4.0, x1);
            r.active = lr.active;
            r.saved  = lr.saved;
            r.id     = lr.id;
            out.loops.push_back(r);
        }
    }
    void buildLoopRects(WaveformFrame& out) {
        buildLoopRectsFor(detail, out);
    }

    // ---- deck-state badges (§38-40): pure values, view styles them --
    void buildBadges(WaveformFrame& out) {
        if (!snap) return;
        const auto st = (DeckState)snap->state;
        const auto chip = [&](const char* t, int sev) {
            StateBadge b{}; b.severity = sev;
            std::snprintf(b.text, sizeof b.text, "%s", t);
            out.badges.push_back(b);
        };
        switch (st) {
            case DeckState::Empty:   chip("EMPTY", 0); break;
            case DeckState::Loading: chip("DECODING", 2); break;
            case DeckState::Error:   chip("ERROR", 3); break;
            case DeckState::Ready:   chip("READY", 0); break;
            case DeckState::Paused:  chip("PAUSED", 0); break;
            case DeckState::Seeking: chip("SEEK", 1); break;
            default: break;
        }
        if (st != DeckState::Empty && st != DeckState::Error &&
            st != DeckState::Loading) {
            if (!snap->analyzed && !snap->path.empty())
                chip("ANALYZING", 2);
            else if (snap->analyzed && !out.hasGrid)
                chip("NO GRID", 2);
            if (interact.gridEdit) chip("GRID EDIT", 1);
            if (snap->isMaster)    chip("MASTER", 1);
            else if (snap->syncMode != 0 && snap->syncConf > 0.0f &&
                     snap->syncConf < 0.45f)
                chip("SYNC UNCERTAIN", 2);
            if (snap->streamEnded) chip("STREAM END", 2);
        }
    }

    // reusable buffers — stable across frames
    std::vector<WaveformBin> binsBuf_, ovBins_;
    std::vector<GridMarker>  markersBuf_;
    // §47-48 — sorted visible-window index (rebuilt on markersRev)
    std::vector<const CueMarker*>  cueIdx_;
    std::vector<const LoopRegion*> loopIdx_;
    std::vector<double>            loopMaxOut_;
    const CueMarker*  idxCuesData_  = nullptr;
    const LoopRegion* idxLoopsData_ = nullptr;
    uint32_t idxRev_ = 0xFFFFFFFFu;   // forces first build
    int lastJumpCount_ = 0;
    int jumpEvents_    = 0;
};

} // namespace onda::ui
