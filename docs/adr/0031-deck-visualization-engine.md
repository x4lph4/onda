# ADR 0031 — Deck visualization engine (visualization state, renderer seam, marker collision + virtualization, seek transaction, primary cue, perf mode, time display)

## Status

Accepted — Phase 31.

## Context

Phase 19 built a mature waveform/deck display: `WaveformViewEngine`
(pure model → `WaveformFrame` render plan), `WaveformView` (JUCE
raster + input), `WaveformMapping` viewport/zoom, `PlayheadSmoother`,
marker + loop + badge geometry, cached raster images, 30 Hz snapshot
feed, four-deck layout. Audit against the deck-visualization spec
found the remaining real gaps:

- No explicit layer-visibility/perf state — spec §2 wants a
  `VisualizationState` (grid/cue/loop/hot/memory toggles), §45 a
  degrade mode.
- No renderer abstraction — spec §6/§7/§46 want a
  `GraphicsRenderer`-style seam so a batched/GPU backend can replace
  the JUCE raster without touching the model.
- Cue labels overlapped freely (§33 collision rules missing).
- Cue/loop emitters scanned the whole vector per frame (§47-48
  viewport index missing) — O(n) on 500-marker sets.
- Drag-seek had no transaction semantics: no begin/commit/cancel and
  no preview position (§53).
- The transport's primary cue never reached the waveform (§25) —
  only hot/memory/loop cues were emitted by `AppShell`.
- `DeckView` formatted time inline (`M:SS.ss` only); spec §37-38 wants
  a dedicated `TimeDisplayRenderer` with `M:SS`/`H:MM:SS` auto-format.

## Decision

**Extend the proven model/shell split instead of building a parallel
renderer hierarchy.** The spec's component tree maps 1:1 onto the
existing plan builders (`buildColumns` = WaveformRenderer,
`buildMarkers` = BeatGridRenderer, etc.); creating sibling classes
would duplicate the frame contract. Concrete additions:

1. **`VisualizationState`** (`src/ui/VisualizationState.h`) — pure
   flag struct on the engine (`vis`), consulted at build time so a
   hidden layer costs zero geometry. `perfMode` sheds beat ticks,
   bar numbers, RMS trace and phrases while keeping structure.
2. **`FrameRenderer` seam** (`src/ui/GraphicsRenderer.h`) —
   `drawFrame(WaveformFrame&, void* ctx)` + capability hints
   (`nativeBatching`, `maxFps` for 60→120 Hz). `WaveformView` stays
   the shipping raster backend; `NullFrameRenderer` serves tests.
   No GPU API assumed.
3. **Collision tiers** — `CueFlagPx.tier`: visible flags in px order,
   labels closer than `cueLabelMinPx` (44 px) on a tier step down a
   row (0-2); tier 3+ suppresses the label. Marker positions never
   move — only labels stack.
4. **Virtualized markers** — `ensureCueIndex()` keeps sec-sorted
   pointer indexes, rebuilt lazily on `markersRev` (bumped by
   `setMarkers`) or a vector size/backing-pointer change (direct
   assignment stays correct — Phase 19 tests encode that contract).
   Per frame: binary-search the window → O(log n + visible).
   `loopMaxOut_` prefix-max bounds how far back a long loop can reach
   the window's left edge — correctness for arbitrary-length loops.
5. **Seek transaction** — wave press still emits `Seek` (click
   preserved); first drag opens a session: `SeekBegin` → `ScrubSeek`*
   → `SeekCommit{finalSec}` or `SeekCancel` (`interact.cancelSeek()`,
   `WaveformView::cancelSeekDrag()`). `WaveformFrame.previewPx`
   carries the ghost position — the view draws a translucent preview
   playhead; the real one is untouched. The Phase 19 interaction test
   was updated to the new protocol (deliberate contract change).
6. **Primary cue** — `CueMarker.type` 3: `AppShell` emits
   `perf_->cuePoint(d)` as a `-1`-slot marker (out of hit-test/arm);
   the view draws a filled diamond in the text colour — distinct
   shape, never a pad colour.
7. **`TimeDisplay`** (`src/ui/TimeDisplay.h`) — pure formatter:
   `M:SS` < 1 h, `H:MM:SS` ≥ 1 h, `M:SS.ss` variant for sub-hour
   precision, `formatMode` for Elapsed/Remaining(`-`)/Total + `EL/
   REM/TOT` tags. `DeckView` now calls it instead of an inline lambda.

## Consequences

- All new state/commands ride the existing frame/command buses — no
  engine, audio, or RT changes. `SeekCommit` reuses the `onSeek` fast
  path; Begin/Cancel only reach `onWaveCmd` so hosts can bracket the
  transaction (e.g. store the pre-drag position for snap-back).
- Per-frame marker cost on 500-cue sets drops to ~1-4 emitted flags;
  120-min builds stay ≪10 ms/frame (measured).
- `VisualizationState`/`perfMode` give hosts one switch for
  performance layouts and adaptive degrade — free toggle, no repaint
  bookkeeping beyond `dataRev`.
- GPU path: any backend implementing `drawFrame` consumes the same
  plan; batching hints already reported through `nativeBatching()`.
- Deferred: tier-3 label grouping ("+n" chip), spectral-band tint
  (`WaveformData::bands` seam exists), audible scrub preview (the
  transaction events are the hook when a scrub voice is designed).
