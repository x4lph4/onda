# Phase 31 — Deck visualization engine

The professional deck display: waveform + beatgrid + playhead + cues
+ loops + zoom + viewport + time/BPM/key readouts — built as a pure
model that emits a backend-neutral render plan, with a thin JUCE
raster/input shell on top. Phase 19 built the core; this phase
formalizes the remaining surfaces (state, renderer seam, collision,
virtualization, seek transaction, primary cue, perf mode, time
format).

## Architecture

```
WaveformView (JUCE, src/app/ui/WaveformView.h)
└── WaveformViewEngine (pure C++, src/ui/WaveformViewEngine.h)
    ├── WaveformViewport      sec↔px transform (WaveformMapping)
    ├── WaveformZoom          anchor policy: cursor/playhead/center
    ├── WaveformAutoScroll    Off / Center / Smart  →  static /
    │                         centered / scrolling waveform modes
    ├── PlayheadSmoother      visual-only interpolation + jumps
    ├── WaveformFrame         render plan consumed by FrameRenderer
    ├── WaveformCache         FNV-1a raster keys → image reuse
    ├── WaveformInteraction   hit-tests + drags → WaveCmd
    ├── VisualizationState    per-layer visibility + perfMode (§2)
    └── WaveformDiagnostics   build counters/timings

FrameRenderer (seam, src/ui/GraphicsRenderer.h)
├── WaveformView::paint*    JUCE software raster (shipping backend)
└── NullFrameRenderer       headless/test backend

TimeDisplay (src/ui/TimeDisplay.h) — M:SS / M:SS.ss / H:MM:SS
```

Conceptual roles from the spec map onto the plan builders:
`WaveformRenderer`→`buildColumns`, `BeatGridRenderer`/`BeatMarker`
→`buildMarkers`, `PlayheadRenderer`→`playheadPx`+`PlayheadSmoother`,
`CueRenderer`/`HotCue`/`MemoryCue`→`buildCueFlagsFor` (types 0/1/3),
`LoopRenderer`→`buildLoopRectsFor`, `SelectionRenderer`→loop rects +
cue drags, `TimeDisplayRenderer`→`TimeDisplay`,
`DeckOverlayRenderer`→`buildBadges`, `ZoomController`→`WaveformZoom`,
`ViewportController`→`WaveformViewport`+`WaveformAutoScroll`,
`VisualizationState`→`engine_.vis`.

## VisualizationState (§2, §45)

`engine_.vis` gates each layer **at build time** — a hidden layer
costs zero geometry, not just zero paint:

| flag               | gates                                    |
|--------------------|------------------------------------------|
| `showBeatgrid`     | beat/bar/downbeat ticks + bar band       |
| `showBarNumbers`   | bar labels                               |
| `showCues`         | master gate for all cue flags            |
| `showHotCues`      | type-0 flags                             |
| `showMemoryCues`   | type-1 flags                             |
| `showPrimaryCue`   | type-3 flag (transport cue point)        |
| `showLoops`        | active + saved loop regions              |
| `showRms`          | inner RMS trace on columns (paint-side)  |
| `showPhrases`      | §30 seam — off until a detector ships    |
| `perfMode`         | §45 degrade (below)                      |

`perfMode` sheds secondary detail while keeping the geometry a DJ
reads mid-scratch: plain beat ticks and bar numbers are dropped
(bars/downbeats stay), the RMS trace is skipped at paint, phrase
markers are suppressed. Columns, loops, cues and the playhead are
never degraded. Hosts flip it on sustained `diag.buildMs` or a
dedicated performance layout (`WaveformView::setPerfMode`).

## Waveform source & levels (§3-5)

Unchanged contract: the renderer reads **only** `WaveformData`
(min/max/RMS pyramid from `MusicAnalysisEngine`); audio is never
re-decoded for drawing. `query()` picks the cheapest adequate level
(overview 4,096 bins / mid 1,024-fr / detail 256-fr) for the bin
budget; `frame.levelUsed` + `diag.level` record the choice. Columns
are produced once per raster-cache miss into a `juce::Image` —
never thousands of graphics objects per frame.

## GraphicsRenderer seam (§6, §7, §46)

`FrameRenderer` (src/ui/GraphicsRenderer.h) is the backend-neutral
seam over `WaveformFrame`: one `drawFrame(frame, ctx)` entry plus
capability hints (`nativeBatching()` for §46 adaptive detail,
`maxFps()` for §7 refresh targeting — 60 today, 120-capable without
assuming every system supports it). `ctx` is backend-defined
(`juce::Graphics*` for the raster path). `WaveformView`'s
paint/paintOverview pair is the shipping implementation;
`NullFrameRenderer` drives the pipeline headlessly in tests. No
concrete GPU API is assumed.

## Playhead, modes & seeking (§8-14, §19, §53-54)

- Source of truth stays `DeckSnapshot::positionSec` (audio/deck
  timeline). `PlayheadSmoother` interpolates the *drawn* position
  only; jumps (>0.4 s) snap + count, never write back.
- Modes map to `AutoScrollMode`: `Off` = static waveform (§13),
  `Center` = centered playhead with eased follow (§12), `Smart` =
  edge-triggered scrolling (§14, default).
- **Seek transaction (§53)**: a wave press still emits `Seek`
  immediately (click semantics preserved). The first real drag opens
  a session: `SeekBegin` → `ScrubSeek`* → `SeekCommit{finalSec}` on
  release, or `SeekCancel` via `interact.cancelSeek()` /
  `WaveformView::cancelSeekDrag()` (Esc / focus loss). While the
  session is open the frame carries `previewPx` — the view draws a
  45%-alpha ghost playhead there; the real playhead is untouched.
  All commands route through the host's `onSeek`/`onWaveCmd` →
  `rt::DeckSeek`; the view never touches `AudioEngine` internals
  (§19, §54 — no extra audio preview path).

## Beatgrid (§20-24)

Unchanged: `collectGridMarkers` emits beat/bar/downbeat ticks with
density gating, bar numbers above `barLabelMinPx`, current-beat flag
and the active `barBand`; `beatsPerBar` comes from the grid (no 4/4
assumption). Grid-edit mode flags markers and adds click=first-beat /
drag=nudge-ms gestures through `analysisEdit`.

## Cues — primary, hot, memory (§25-28, §32-33)

- `CueMarker.type`: 0 hot / 1 memory / 3 **primary** (new §25).
  `AppShell` emits the transport cue point (`perf_->cuePoint`) as a
  type-3 marker with slot −1 — excluded from hit-test/delete-arm.
  The view draws it as a filled **diamond** in `p.text`, never a pad
  colour, so it can't be confused with a hot cue. Memory cues stay
  hollow triangles (non-colour distinction, §28).
- **Collision tiers (§33)**: visible flags are emitted in
  ascending-px order; a label that would land within `cueLabelMinPx`
  (44 px) of a label on the same tier steps down a row (tiers 0-2).
  Beyond tier 2 the label is suppressed — the marker line never
  moves, positions are the truth. The view offsets the label by
  `tier × 11 px`.
- Marker anchors are timeline seconds; px is computed per frame —
  zoom/pan never desyncs them (§27).

## Loops (§29-31)

`LoopRectPx{x0,x1,active,saved,id}` — active region highlighted,
saved loops dimmed, edges draggable, body drag preserves length,
emitted as `LoopSet` → `rt::SetLoop`+`SetLoopOn`. `LoopEngine` stays
the source of truth; the view repaints on the next snapshot (§31).

## Marker virtualization (§21, §47-48)

`ensureCueIndex()` maintains sec-sorted pointer indexes into `cues`
and `loops`, rebuilt lazily when `markersRev` changes (the view bumps
it in `setMarkers`) or the vectors' size/backing pointer changes
(direct assignment also stays correct). Per frame the window is
binary-searched — **O(log n + visible)**, never O(n):

- Cues: `lower_bound`/`upper_bound` on `sec` — markers outside the
  viewport are never touched (§21 for beats applies to all markers).
- Loops: variable length means `inSec` alone can't bound the left
  edge — `loopMaxOut_` (prefix-max of `outSec`) bounds how far back a
  reaching loop can live, so a 0→200 s mega-loop still renders
  correctly while the scan stays bounded.

## Time / BPM / tempo / key (§37-42)

- `TimeDisplay` (pure, src/ui/TimeDisplay.h): `format` → `M:SS` under
  an hour, `H:MM:SS` at/over (auto, §38); `formatCs` → `M:SS.ss`
  sub-hour for scratch precision; `formatMode` → Elapsed/Remaining/
  Total with the conventional `-` sign and 0-clamp; `tag` →
  `EL/REM/TOT`. `DeckView`'s time row is now a thin call into it.
- BPM shows `effBpm` (or `eff→target` under sync pull); tempo% rides
  the pitch display; key renders `keyIndex` via the notation pref.
- Deck status (`PLAYING/PAUSED/CUE/LOOP/SYNC/…`) comes through the
  badge builder — all text, never colour-only.

## Interaction → command map (additions in bold)

| gesture                | WaveCmd           | host route                 |
|------------------------|-------------------|----------------------------|
| click wave             | `Seek`            | `rt::DeckSeek`             |
| first wave drag        | **`SeekBegin`**   | session bracket (host)     |
| drag wave              | `ScrubSeek`       | `rt::DeckSeek`             |
| release after scrub    | **`SeekCommit`**  | `rt::DeckSeek` (final sec) |
| cancel drag (Esc,…)    | **`SeekCancel`**  | host abort policy          |

Rest unchanged (CueTrigger/CueSet/CueDelete/LoopSet/Grid*/ScrollDetail
/ZoomAt) — see the Phase 19 doc.

## Controller / DDJ-FLX4 integration (§55-56)

Nothing new to wire: the chain is already `jog/pad/button →
ControllerEngine → Perf/Deck engines → snapshot → 30 Hz shell feed →
frame`. A DDJ-FLX4 hot-cue pad moves `PerformanceEngine`'s cue list,
which AppShell republishes into `setMarkers` next tick — the marker
appears because engine state changed, never because the view polled
hardware (§55). Loop controls likewise mutate loop regions through
the engine (§56).

## Threading & realtime (§57-58)

- Snapshot data flows audio → `DeckStats` atomics → 30 Hz shell →
  engine pointers; the frame builder reads immutable data, no locks.
- `setWaveform/setGrid/setMarkers` only re-point + bump revisions;
  index/raster rebuilds are lazy on the UI thread.
- The renderer never blocks `AudioEngine`; the UI never touches
  database/analysis/file I/O in the paint path.

## Testing

`tests/waveform_phase31_tests.cpp` — 17 cases `[ui31]`: TimeDisplay
formats/modes/edges; state-flag gating (master + per-type); perfMode
shedding; collision tiers + crowd suppression; cue/loop virtualization
windows incl. left-edge overlap; the full seek transaction (begin,
ghost px, commit, cancel, click-stays-click); static mode; renderer
seam; 120-min build benchmark; 500-cue × 1 h virtualized benchmark;
400-step zoom stress; 60-snap seek stress; 4-deck shared-data frames.

## Performance (measured, this box)

- 120-min track: 100 detail frames ≪ 800 ms total (≪10 ms/frame at
  120 Hz budget).
- 1 h track × 500 cues: 100 frames over a roaming window ≪ 500 ms —
  the index keeps per-frame work at ~1-4 visible markers.

## Limitations

- `previewPx` ghost is presentation-only; a host that wants
  "snap-back on cancel" implements it from `SeekBegin`'s stored sec.
- Tier 3+ labels are hidden, not grouped — a count chip ("+3") is a
  possible later refinement.
- Spectral style is still the crest-factor tint from Phase 19;
  `WaveformData::bands` (Phase 30) can drive a true band tint later —
  the `style` slot is the seam.
- Audible scrub preview (§54) is deliberately unimplemented — the
  transaction events give the audio layer everything it needs when a
  dedicated scrub voice is designed.
- GPU backend: the seam exists; a batched implementation is future
  work. The raster path meets the 60/120 Hz budget today.
