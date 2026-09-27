# Performance Engine (Phase 35)

Advanced musical time/rhythm manipulation for live performance:
loops, beat jump, beat roll / loop roll, slip, quantize, cue jump —
one deterministic façade over the deck engines.

## Architecture

```
ControllerEngine / UI / FLX4 / keyboard / MIDI learn
  → PadLayer → PadModeManager / CommandManager
  → PerformanceEngine                    (control thread, never RT)
  → PerformanceScheduler                 (priority + coalescing)
  → rt::CommandQueue                     (bounded POD commands)
  → DeckEngine ×4                        (audio thread)
  → MixerEngine / FXEngine / SamplerEngine
```

`PerformanceEngine` owns per-deck `DeckCtx`: grid controller, cues,
saved loops, quantize config, `SlipEngine` (refcounted), `ReverseEngine`,
`LoopRollEngine`, action table — and translates musical intent into
`rt::Command`s. No DB / file I/O / locks / allocation on the audio path.

## Sub-engines (`src/core/performance/`)

| file | role |
|---|---|
| `QuantizeEngine` | grid snap: `snap` (Nearest), `nextBoundary` (Next), `previousBoundary` / `boundaryAtOrBefore` (Phase 35 §6) |
| `SmartLoopEngine` | pure loop math: `beatLoop`, `resizeBeats`, `halve`, `double_`, `move`, `clampOut`, `boundedMove` (§56) |
| `BeatJumpEngine` | jump target — phase-preserving raw, beat-aligned quantized |
| `LoopRollEngine` | temporary-loop state: arm / live-resize / release |
| `SlipEngine` | refcounted slip sources: UserMode / Roll / Momentary |
| `CueEngine` + `CueJumpEngine` | cue storage + the §52-53 named jump-target seam (beat re-resolve → snap) |
| `BeatgridController` | per-deck frame↔beat authority, deterministic fallback without a grid |
| `LoopSizeController` | canonical size tables (loop 1/32…32, roll 1/32…1+, jump 1…64) |
| `PerformanceScheduler` | command ordering: SlipEnd→SlipBegin→LoopExit→Seek→LoopArm→Transport→State, per-class coalescing |
| `PerformanceActions` | `PerfAction` + `ActionState` lifecycle + resolver priority table |
| `PerformanceParams` | §46 registry — `loop.size`, `loop.active`, `loop.quantize`, `beatJump.size`, `slip.active`, `quantize.mode`, `beatRoll.size` |
| `PerformanceState` | spec-shaped snapshots: `LoopState`, `SlipState`, `PerformanceClock`, `PerfCmd`, `PerformanceCommand` |

## Clock (§2)

`PerformanceEngine::perfClock()` returns `PerformanceClock` — a
repack of `masterClockView().sample` (`sync::ClockSample`: bpm,
beatPos, bar, phase, beatsPerBar). One session clock, fed by the
master deck (or an external `ClockSource`); no parallel BPM source.

## Quantize (§3-6)

- Resolutions: Off (flag), 1/32…1/2 subdivisions, 1 beat, Bar,
  **2/4/8 beats** (Phase 35, appended to `Quantize`), Custom.
- Targets: hot cues (`c->quantized` per-cue opt-out), loop in/out,
  beat jump, loop roll, cue jumps, seeks.
- Modes: Nearest (`snap`), Next (`nextBoundary`), Previous
  (`previousBoundary`), at-or-before (`boundaryAtOrBefore`).
- Quantize = **destination snapping**, not "wait for the beat" —
  the seek executes immediately on a grid-aligned frame.

## Loops (§7-20)

- `loopBeats`/`loopBeatsIdx` auto-loop over the size table;
  quantized start anchors at-or-before the grid.
- `loopIn`/`loopOut` manual loop with armed-state tracking;
  `loopOut < loopIn` → deterministic `BadRange` rejection.
- `loopHalve`/`loopDouble`/`loopMove` preserve the in-anchor and
  musical phase through the grid (tempo-map safe).
- `reloop` re-enters the last range; `exitLoop` never jumps —
  playback continues through the out point.
- `loopState(deck)` → `LoopState{active,start,end,length,
  quantized,beatLength,mode}` (mode: 0 auto, 1 manual armed,
  2 roll active).

## Slip (§21-27)

Refcounted: `UserMode` toggle, `Roll` (loop roll), `Momentary` —
the shadow runs until the last ref releases. The RT deck keeps a
virtual timeline (`DeckStats::slipShadowSec`); releasing slip
resumes exactly where the track "would have been".
`slipState(deck)` → `SlipState{active, originalFrame, virtualSec,
slipStartMs}` — anchors are recorded on every `SlipBegin` edge
(Phase 35 §23). Cue triggers and beat jumps under slip **re-anchor**
the shadow (`SlipReanchor`) — a permanent move, never a surprise
rewind. Scratch-under-slip is the documented Phase-36 seam.

## Beat jump (§28-34)

`beatJump(deck, ±beats)` — any size including fractions
(1/32…64 via `kJumpSizes` presets + arbitrary doubles). Without
quantize the beat phase is preserved; with quantize the landing
snaps to the deck quantum. During a loop: the seek repositions, the
armed range survives (re-entry on return). During slip: shadow
re-anchored (§34). Clamped into `[0, trackEnd)` (Phase 35 §56).

## Beat roll / loop roll (§35-40)

`loopRoll(deck, beats)` — temporary loop that holds a `SlipEngine::Roll`
ref: the displayed position repeats the region while the shadow
keeps advancing; release (`loopRollEnd` / `exitLoop`) resumes the
musical timeline. A second arm **resizes** the running roll
(resolver `Decision::Resize`); rolling during scratch is denied.
Permanent loop vs roll state is strictly separated
(`LoopRollEngine` vs `lastLoopIn/Out`).

## End of track (§56)

Every positioning path clamps to `trackEndFrame` =
`durationSec × fileSampleRate`, now published by `DeckEngine` at
track adoption (covers async loader **and** direct `pendingTrack_`
handoffs). Auto loops clamp `out`; `in` past the end rejects
`BadRange`. Beat jumps clamp to `end-1`. `boundedMove` keeps the
whole loop inside `[0, end)` preserving length — evaluated on the
beat axis so negative beats clamp before frame conversion.

## Safety (§55/§69)

- `out <= in` → `PerfError::BadRange`; no track → `NoTrack`;
  no grid → `NoGrid`; empty cue → `EmptySlot`; disabled → `Disabled`;
  full scheduler → `SchedulerFull`. `lastError(deck)` surfaces it.
- `detachDeck` clears grid, cues, loops, slip refs, roll, reverse,
  jog, transport, cue point, action table, slip anchors — no
  incompatible state survives a track/deck change.
- Loop/jump math is tempo-map safe (resolves through `BeatGrid`,
  never assumes constant BPM → tempo changes stay musical, §58).

## Controller integration (§43-45)

Every action is reachable from pads (`PadModeManager` resolves
Loop/BeatJump/Sampler/HotCue/… modes), `CommandManager` ids, MIDI
learn (`MappingEngine`) and the FLX4 map — all funnel into the same
façade methods; hardware never emits `rt::Command` directly.

## Limitations

- `kJumpSizes` presets start at 1 beat — fractional jumps work via
  `beatJump(deck, double)`; extending the preset table would shift
  persisted `jumpSizeIdx`.
- `previousBoundary` is exposed at engine level; no façade consumer
  yet (future quantize-mode UI).
- Scratch + slip DSP is Phase 36 scope — the slip ref seam
  (`SlipEngine::Momentary`) is already in place.
- `LoopState::active` is the RT `loopOn` echo — one render block of
  latency behind `SetLoopOn` (normal echo semantics).
- Saved loops are stored per-track via `trackId`; the live range is
  session-only until `saveLoop` is invoked.
