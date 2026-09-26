# ADR 0019 — Waveform performance view

Phase 19. Status: implemented.

## Context

Phases 8–18 already ship a working `WaveformView` (detail + overview
strips, `WaveformMapping` math, click/drag seek, wheel zoom) and a
`DeckView` covering transport, sync, loops, pads, meters. Phase 19
asks for the professional version of the same surface: the full
renderer architecture named in the spec, a real viewport/zoom model,
smoothed-but-honest playhead, grid editing gestures, cue/loop pointer
interaction, deck-state and time-mode displays, 4-deck layout, HiDPI,
diagnostics, and long-track performance — with the hard constraint
that the audio thread must never feel any of this.

## Decisions

1. **Pure render-plan engine, JUCE-free, in `onda_ui`.** All visual
   math lives in `src/ui/WaveformViewEngine.h`: a `WaveformFrame`
   (columns, beat markers, cue flags, loop rects, bar band, playhead
   px, badges) built from plain inputs. The JUCE component only blits
   it. This keeps the entire spec surface unit-testable headlessly —
   the project rule that `onda_ui` links no JUCE is preserved and the
   "visual regression" tests assert the deterministic geometry model
   rather than raster bytes.

2. **Raster cache keyed on the full dependency set.** Columns + loop
   fills + bar band + beat ticks rasterize into a `juce::Image` whose
   FNV key covers viewport, size, waveform identity, style, grid-edit
   flag and a data revision bumped on every `setX` call. Cheap live
   layers (cue flags, playhead, jump flashes, badges) redraw every
   frame on top. Result: `WaveformData::query` runs only when the
   answer would actually change.

3. **`WaveformViewport` is an alias, not a new class.** The spec's
   viewport semantics (visible start/end, zoom, scroll, center,
   px/sec, clamping) are exactly what `WaveformMapping` has provided
   and tested since Phase 8 — one implementation, no duplication.

4. **Playhead position is deck truth; smoothing is cosmetic.**
   `PlayheadSmoother` dead-reckons between 30 Hz snapshot ticks and
   converges at 30/s; position discontinuities > 0.4 s are jump
   *events* (shown as decaying flash markers, not waveform edits).
   Paused state draws the exact position. Nothing ever writes back.

5. **Interactions are values (`WaveCmd`), routed by the host.** The
   component performs zero musical logic: press/drag/release produce
   `Seek/ScrubSeek/CueTrigger/CueSet/CueDelete/LoopSet/GridNudgeMs/
   GridSetFirstBeat/ScrollDetail/ZoomAt` which `AppShell::routeWaveCmd`
   maps to `rt::` commands, `PerformanceEngine` calls, or the existing
   `analysisEdit` port. Cue deletion is two-strike armed (right-click
   arms, second confirms) — no modal dialog, no accidents.

6. **Grid editing reuses the Phase-14 surface.** `AnaSetFirstBeat`,
   `NudgeGrid`, `DoubleTempo`, `HalfTempo`, `SetDownbeat`, `ResetGrid`,
   `Reanalyze` already exist and persist user overrides; grid-edit
   mode only adds pointer gestures on top of them.

7. **View-state commands get their own action space.** `0x0080+`
   inside the per-deck range (`WaveZoomIn/Out/Fit`, `WaveAutoScroll`,
   `WaveGridEditTgl`, `DeckTimeMode`) routed through one
   `EnginePorts::viewAction` port to the shell — presentation state,
   no engine involvement, keyboard/MIDI-addressable like everything
   else.

8. **4-deck is real but visibility-gated.** All four DeckView/
   WaveformView pairs exist up front; `LayoutSpec::deckMode` computes
   `waves[4]/decks[4]` (A C | mixer | D B), and the master deck-mode
   toggle flips visibility live. Two-deck geometry is unchanged.

9. **Honest spectral seam.** The pyramid stores min/max/RMS — no band
   data — so the optional "spectral" style is a crest-factor
   (peak/RMS) tint, explicitly not advertised as frequency content.
   `crest` travels in the frame plan so a future true-band level can
   drive the same code path.

## Consequences

- 19 new test cases / ~5 k assertions, all headless; the app compiles
  unchanged apart from the rewired view internals.
- Frame-build cost is O(width × aggregate-depth), independent of
  track length — measured on synthetic 5 min/30 min/2 h pyramids.
- Two follow-ups documented as seams: true spectral bands (needs a
  new pyramid level) and phrase markers (needs a detector); the draw
  paths already exist.
