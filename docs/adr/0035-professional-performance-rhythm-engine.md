# ADR 0035 — Professional performance rhythm engine (quantize modes, multi-beat quanta, spec snapshots, CueJumpEngine, perf params, end-of-track clamps)

## Status

Accepted — Phase 35.

## Context

Phases 20 and 28 built the performance deck engine
(`docs/adr/0020-performance-engine.md`,
`docs/adr/0028-performance-deck-engine.md`): `PerformanceEngine`
façade over `DeckCtx`, `QuantizeEngine`/`SmartLoopEngine`/
`BeatJumpEngine`/`SlipEngine`/`LoopRollEngine`/`CueEngine`,
`BeatgridController`, `LoopSizeController`, `PerformanceScheduler`
(priority + coalescing), action state machine with resolver,
saved loops, master clock view, persistence prefs, pad/mode
resolution and the FLX4 mapping path.

The Phase-35 audit found the remaining spec gaps:

- **§6** — quantize offered Nearest + Next only; no Previous mode.
- **§3** — `Quantize` stopped at Bar/Custom; no 2/4/8-beat quanta.
- **§8/§23/§2/§41-42** — the spec's named PODs (`LoopState`,
  `SlipState`, `PerformanceClock`, `PerformanceCommand`/`PerfCmd`)
  had no representation even though the underlying data existed.
- **§52-53** — cue-jump math lived inside `CueEngine::trigger`;
  no named seam for arbitrary cue jumps.
- **§46** — mixer `ParameterRegistry` is channel-scoped; the
  performance parameter table (loop.size, slip.active, …) didn't
  exist anywhere.
- **§56** — no end-of-track clamping: beat jumps and loop outs
  could target frames past the track end (RT clamps the seek, but
  domain state recorded unclamped ranges) — and `trackEndFrame`
  couldn't even work because `durationSec`/`fileSampleRate` were
  only published by the async `TrackLoader`, never at track
  adoption.

## Decision

- `QuantizeEngine::previousBoundary` (strict `ceil−1`) and
  `boundaryAtOrBefore` (non-strict floor) complete the
  Nearest/Next/Previous mode set — same pure grid math, no clock.
- `Quantize::Beat2/Beat4/Beat8` appended **after** `Custom`
  (indices 8/9/10) so persisted resolution ints stay valid;
  `cycleQuantize` still cycles the pad-facing subset.
- `PerformanceState.h` defines the spec-shaped views —
  `LoopState` (built from `lastLoopIn/Out` + `loopOn` echo +
  grid beat math + the new `loopQuantized` flag funneled through
  `storeLoopRange`), `SlipState` (needs activation anchors →
  `slipEntryFrame`/`slipStartMs` recorded on every `SlipBegin`
  edge), `PerformanceClock` (repack of `ClockSample`),
  `PerfCmd`/`PerformanceCommand` vocabulary mirroring the façade.
- `CueJumpEngine` is the named pure seam (beat re-resolve →
  quantum snap) — `CueEngine::trigger` already implements the same
  math for stored cues; the seam covers arbitrary cue jumps and
  stays unit-testable.
- `PerformanceParams.h` is the §46 registry — name-keyed defs with
  min/max/step + `perfParamId(deck, param)` (16 ids per deck) and
  `sanitizePerfParam`, deliberately parallel to the mixer registry
  pattern rather than extending it across scopes.
- **End-of-track**: `trackEndFrame(deck)` =
  `durationSec × fileSampleRate`; applied to `beatJump`
  (clamp to end−1), `loopBeats`/`loopOut`/`loopRoll` (`clampOut`,
  `in` past end → `BadRange`), `loopMove` (`boundedMove` —
  beat-axis clamp so negative beats can't corrupt through
  `beatToFrame(<0)` → −1). **DeckEngine now publishes
  `durationSec`/`fileSampleRate` at `adoptPendingTrack`** — every
  track handoff exposes duration, not just the async loader path
  (same values the loader already wrote; harmless rewrite).

## Consequences

- All new enum/table additions are append-only; no persisted
  value changes meaning.
- `boundedMove` preserves loop length at both track ends; a move
  that can't fit anywhere returns the input loop (deterministic).
- `previousBoundary`/`boundaryAtOrBefore` are engine-level seams —
  no façade consumer until a quantize-mode UI needs them.
- `PerformanceCommand`/`PerfCmd` is a vocabulary for controllers
  and tests; dispatch stays in the façade methods (no parallel
  command path).
- `adoptPendingTrack` publishing stats slightly widens the stats
  write surface on the audio thread — two relaxed atomic stores
  once per track swap, bounded and lock-free.
- `docs/performance/performance-engine.md` documents the subsystem;
  `[perf35]` adds 13 cases / 85 assertions.
