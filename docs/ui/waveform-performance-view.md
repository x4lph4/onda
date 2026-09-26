# Phase 19 — Waveform performance view

The deck waveform/deck display stack: how analysis data becomes
pixels, how the playhead stays glued to real audio, and how every
interaction becomes a command rather than view logic.

## Architecture

```
WaveformView (JUCE, src/app/ui/WaveformView.h)
└── WaveformViewEngine (pure C++, src/ui/WaveformViewEngine.h)
    ├── WaveformViewport     sec↔px transform (alias of WaveformMapping)
    ├── WaveformZoom         anchor policy: cursor / playhead / center
    ├── WaveformAutoScroll   Off / Center / Smart follow modes
    ├── PlayheadSmoother     visual interpolation + jump detection
    ├── WaveformFrame        render plan (columns, markers, cues,
    │                        loops, bar band, playhead px, badges)
    ├── WaveformCache        FNV-1a raster keys → image reuse
    ├── WaveformInteraction  hit-tests + drags → WaveCmd
    └── WaveformDiagnostics  build counters/timings
```

The split is total: `WaveformViewEngine` contains **no JUCE** and is
fully unit-tested (`tests/waveform_phase19_tests.cpp`). `WaveformView`
owns the engine, calls `buildDetail()/buildOverview()`, caches the
raster in a `juce::Image`, and only translates theme colors + input
events.

## Data source and waveform levels

Rendering reads **only** `WaveformData` (min/max/RMS pyramid produced
by `MusicAnalysisEngine`). The file is never re-decoded for drawing.

`WaveformData::query(startFrame, endFrame, maxBins, out)` picks the
cheapest pyramid level that can fill the bin budget:

| level    | spacing        | used when requested span yields ≤ 4×maxBins bins |
|----------|----------------|-------------------------------------------------|
| overview | 4,096 total    | always available                                |
| mid      | 1,024 frames   | ~21 ms @ 48 k                                   |
| detail   | 256 frames     | ~5.3 ms @ 48 k                                  |

`WaveformData::queryLevel()` (added in Phase 19) reports which level a
query would use — the engine records it in `frame.levelUsed` and
`diag.level` for diagnostics.

## Frame plan (what the view draws)

`WaveformViewEngine::buildDetail(out)` fills a reused `WaveformFrame`:

- `cols` — one normalized `WaveColumn{env, rms, crest}` per pixel
  column. `crest` = `(peak/rms − 1)/3` folded to 0..1 — a transient-ness
  measure derived from real amplitude data.
- `beats` — `BeatMarkerPx{px, kind, barNo, current, gridEdit}`:
  kind 0 beat / 1 bar-start / 2 downbeat (every 4th bar). Density is
  gated: markers are skipped entirely below `markerMinPx` px/beat, and
  bar numbers appear only when a bar is ≥ `barLabelMinPx` px wide.
- `barBand` — px rect of the bar containing the playhead (drawn as a
  translucent highlight behind the waveform).
- `cues` — `CueFlagPx{px, slot, type, argb, label, armedDelete}`;
  type 0 hot / 1 memory / 2 loop-cue.
- `loops` — `LoopRectPx{x0,x1,active,saved,id}` clamped to the view.
- `playheadPx` — the *smoothed* draw position (see below).
- `badges` — state chips: `EMPTY/DECODING/ERROR/READY/PAUSED/SEEK`,
  `ANALYZING`, `NO GRID`, `GRID EDIT`, `MASTER`, `SYNC UNCERTAIN`,
  `STREAM END`. Severity only; the theme picks colors.
- `phrases` — seam for future phrase markers (§30): the draw path
  exists, no detector emits them yet.

`buildOverview(out)` emits the same column/cue/loop sets across the
whole track, minus grid markers, with the *real* (unsmoothed) playhead.

## Raster cache

Drawing the columns is the expensive step — so `WaveformView` renders
columns + loop fills + bar band + beat ticks into a `juce::Image` keyed
by `WaveformCache::key(viewport, widthPx, wfPtr, wfFrames, variant,
scale)`. `variant` folds in style, grid-edit flag, and `dataRev_` (a
counter bumped by every `setWaveform/setGrid/setMarkers/setPhrases`
call). Any viewport/data/style change produces a different key → one
rebuild; unchanged keys → `drawImageTransformed` blit. Cue flags,
playhead, jump flashes, and badges are drawn live on top (cheap).

HiDPI (§44): the raster is allocated at
`getApproximateScaleFactorForComponent()` × logical size and drawn
down-scaled, so lines stay crisp on Retina/HiDPI instead of being
1 logical px = 1 physical px.

## Viewport, zoom, auto-scroll

- `WaveformViewport` = `WaveformMapping` (proven since Phase 8):
  `scrollSec`, `pxPerSec`, `widthPx`, `durationSec`; `secToPx`,
  `pxToSec`, `frameRange`, `seekFromPx`; clamps `[0, duration]`.
- `WaveformZoom` — `in/out/apply` with `ZoomAnchor::{Cursor,Playhead,
  Center}`; the anchor second stays put (§34). `reset()` restores the
  default 160 px/s and reveals the playhead. `fitAll()` maps to the
  mapping's own fit.
- `WaveformAutoScroll` — `apply(view, posSec, playing, mode, dt)`:
  - `Off`: never moves.
  - `Center`: exponential ease toward playhead-centred view.
  - `Smart`: `ensureVisible(0.35)` — only scrolls when the playhead
    approaches a margin (default).
- Wheel: `deltaY` → cursor-anchored zoom; `deltaX`/shift → scroll.

## Playhead — real audio position, visual-only smoothing

`DeckSnapshot.positionSec` (from `DeckStats` atomics) is the source of
truth; the 30 Hz UI timer is only a delivery mechanism (§10).
`PlayheadSmoother` computes `drawSec`:

- playing: dead-reckon forward at `effBpm/bpm` rate between ticks and
  converge exponentially (`catchUpRate` 30/s ≈ full converge per tick);
- paused/seeking: exact position;
- `|Δ| > 0.4 s` in any state → a *jump event* (seek, beat jump, loop
  exit): position snaps, `jumps` counter increments, `lastJumpPosSec`
  recorded. `WaveformView` flashes a fading marker at the landing
  point; the waveform raster is untouched (§22).

Smoothing **only** changes what is drawn — it never writes to any
engine state.

## Beatgrid display and editing

Markers come from `collectGridMarkers` + the live `BeatGrid` pointer
(user-edited grids republished through `PerformanceEngine::attachGrid`
flow straight into the view — the waveform raster itself is grid-free,
so `dataRev` invalidation redraws markers instantly, §15).

Grid-edit mode (`WaveGridEditTgl` command / `setGridEdit`):

- **click** on the wave → `WaveCmd::GridSetFirstBeat` → AppShell →
  `analysisEdit(SetFirstBeat, frameAtClick)`.
- **horizontal drag** → `WaveCmd::GridNudgeMs` → `analysisEdit
  (NudgeGrid, ms)`.
- Markers get the `gridEdit` emphasis flag; a `GRID EDIT` badge shows.
- BPM ×2/÷2, downbeat set, reset, reanalyze already exist as
  `AnaBpmDouble/Half/SetDownbeat/GridReset/Reanalyze` commands
  (Phase 14) — the mode just adds pointer interaction.

## Cues and loops

- Cue flags: colored triangle + label at the flag's top; memory cues
  drawn hollow (non-color distinction, §47). Types survive to the view
  through `CueMarker{slot,type}`.
- **Click** flag → `CueTrigger` (perf engine trigger); **drag** →
  `CueSet{slot,sec}` at release; **right-click** → `CueArmDelete`
  (flag shows ×, second right-click within 2.5 s → `CueDelete`) —
  arm-to-delete, no accidental loss (§19).
- Loops: active region filled behind the waveform with edge lines;
  saved loops dimmer. **Drag an edge** → `LoopSet{in,out}` at release;
  **drag the body** → loop moves preserving length; emitted as
  `rt::SetLoop` + `SetLoopOn`.
- Quantize/beatgrid honor is the engine's job — the view emits raw
  seconds and `PerformanceEngine`/deck loop code snaps when quantize
  is on (§21).

## Interaction → command map (§42)

| gesture                        | WaveCmd            | host route                        |
|--------------------------------|--------------------|-----------------------------------|
| click wave                     | `Seek`             | `rt::DeckSeek`                    |
| drag wave                      | `ScrubSeek`        | `rt::DeckSeek` (scrub seam, §36)  |
| click cue flag                 | `CueTrigger`       | `perf->triggerCue`                |
| drag cue flag                  | `CueSet`           | `perf->setCue(slot,frame)`        |
| right-click flag ×2            | `CueDelete`        | `perf->deleteCue`                 |
| drag loop edge/body            | `LoopSet`          | `rt::SetLoop` + `SetLoopOn`       |
| click (grid-edit)              | `GridSetFirstBeat` | `analysisEdit`                    |
| drag (grid-edit)               | `GridNudgeMs`      | `analysisEdit`                    |
| drag inside overview window    | `ScrollDetail`     | viewport pan                      |
| wheel                          | `ZoomAt`           | viewport zoom                     |

## Deck performance display

`DeckView` additions:

- **Time mode** (§24): ELAPSED → REMAINING → TOTAL; click the time row
  or `deck.N.timeMode` command cycles; a small `EL/REM/TOT` tag marks
  the mode (non-color cue).
- **BPM** (§25-26): `eff→target` chip when sync pulls the deck away
  from grid BPM; plain `effBpm` otherwise.
- **Key** (§27): `keyIndex` from the bridge → `KeyNotationConverter`
  in the chosen notation (Standard/Camelot/OpenKey pref, Camelot
  default); `keyText` fallback.
- **Artwork** (§37): `setArtwork` thumbnail left of the title; AppShell
  loads `lib_->artworkPath(trackId)` once per track (cached).
- State badges: `LOADING`, `ERROR`, `PAUSED`, `READY`, `SEEK`, `PFL`,
  `MASTER`, `SYNC UNCERTAIN`, `SYNC LIMITED`, `ANALYSIS PENDING` —
  all text, never color-only (§47).

## Commands added

Deck-scoped view actions (`forDeck`, routed to `ports_.viewAction`):

`WaveZoomIn/Out/Fit` (0x0080-82), `WaveAutoScroll` (0x0083, cycles
Off→Center→Smart), `WaveGridEditTgl` (0x0084), `DeckTimeMode` (0x0085).

## Layout — 2/4 decks, responsive

`LayoutSpec` gained `prefs.deckMode` + `ShellLayout::waves[4]/decks[4]`.
2-deck mode is unchanged. In 4-deck mode the wave strip splits in four
equal columns and the deck row orders `A C | mixer | D B`. AppShell
creates all four `DeckView`/`WaveformView` pairs up front and toggles
visibility from `master.deckMode` (the `MixDeckModeTgl` command flips
it live). Collapse order and minimums are unchanged — no critical info
is lost before the floor sizes hit.

## Diagnostics

`WaveformDiagnostics` (per view, on the engine): columns/markers/cues/
loops rendered, pyramid level, frame span, `buildMs` of the last frame,
`queries`, `cacheHits/Misses`, `staleDrops`, `scaleFactor`,
`visibleDecks`, last `warning`. Read via `WaveformView::diag()` —
no audio-thread cost (all UI side).

## Testing

19 new test cases in `tests/waveform_phase19_tests.cpp`, all on the
pure model: pyramid level selection, zoom anchors/clamps, auto-scroll
modes, playhead smoothing/jump detection, frame contents (columns,
crest, markers + bar numbers + gating, current beat/bar band, cue
flags + armed delete, loop clamping, badges), overview frame, the full
interaction state machine, cache-key sensitivity, jump events, 4-deck
layout, diagnostics counters — plus 5 min/30 min/2 h frame-build
benchmarks and a 4-deck stress loop.

## Limitations

- `Spectral` style is a **crest-factor tint** (peak/RMS), not true
  frequency-band data — the analysis pyramid stores min/max/RMS only.
  True low/mid/high bands would need a spectral pyramid level; the
  `style` slot + `crest` channel are the seam.
- Phrase markers: data path exists, no detector emits them yet (§30).
- ScrubSeek currently maps to the same `DeckSeek` command — a
  dedicated scrub audio path (audible scrubbing) is a later phase.
- Cue rename/recolor routes exist in `PerformanceEngine::updateCue`
  but have no waveform gesture yet (pads/context menu later).
- Visual regression is asserted structurally on the deterministic
  frame model (pixel-space geometry + marker sets), not PNG diffs —
  the test binary has no JUCE graphics link.
