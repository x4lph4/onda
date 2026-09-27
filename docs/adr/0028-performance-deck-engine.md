# ADR 0028 — Performance deck engine completion (unified transport, cue behaviors, action resolver, per-deck facade)

## Status

Accepted — Phase 28.

## Context

Phases 4/9/10/15/20/21/25 built most deck performance behavior:
`PerformanceEngine` already owned per-deck beatgrid, cues, loops,
quantize, slip, reverse and sync context, `DeckEngine` handled
transport/pitch/key-lock/loops/slip shadow, `PerformanceScheduler`
ordered commands deterministically and `CueEngine`/`PerformanceStore`
covered storage + persistence.

Audit against the professional-performance spec found these real
gaps: transport commands emitted from ~5 places (no single entry
point, no brake), state scattered across `DeckStats` + `DeckCtx`
(no unified snapshot), a bare `int64` cue point (no musical metadata,
single hard-coded hold behavior), no snap-on-create for hot cues,
loop in/out snapped to a hardcoded 1.0 beat, no named engine
surfaces (roll/key-lock/pitch/jog/beatgrid), no action model with
states/priorities, no slip-return echo, jog bypassing the
performance layer, missing prefs persistence, and missing logical
control ids (slip/restart/hot-cue/roll/brake/pitch-reset).

## Decisions

1. **One transport entry point.** `PerformanceEngine::transport()`
   translates `Transport` (Play/Pause/Toggle/Stop/CuePress/
   CueRelease/Restart/Brake±) into rt commands or delegates
   (cueButton, instantRestart). UI `EngineCommands` was migrated so
   no caller emits `rt::DeckTransport` directly; `panic` uses the
   same path. `TransportEngine` (non-RT, per-deck) owns brake edge
   logic so `Brake`/`BrakeOn`/`BrakeOff` degenerate correctly.
2. **Brake is a turntable start/stop latch.** RT-side smoothstep
   ramp multiplies `resolvedRate`; when spin-down halts the deck it
   latches `brakeStopped_` so a later release spins back up and
   resumes — the stop was *caused by* the brake. Explicit transport
   (`setPlaying`/`stop`) clears the latch. `stats.brake` echoes the
   whole latch; `poll()` reconciles the non-RT flag from the RT
   truth (the ramp can finish on its own).
3. **Snapshots are read-side, not a second truth.** `DeckPerfState`
   is a POD aggregate built on demand by `deckState()` from
   `DeckStats` (RT echoes) + `DeckCtx` (control truth). No state is
   duplicated into it — UI/LEDs/diagnostics read one struct.
4. **Cue point gains musical metadata; behaviors are policy.**
   `PrimaryCuePoint{frame,beat,createdAt}` replaces the bare frame;
   `CueBehavior::{Hold,ToStart,ToLastCue}` selects what the cue
   button does while paused and whether cue actions re-anchor the
   point. All behaviors share the preview/return path — the
   difference is *what the point is*, not how cueing sounds.
5. **Snap happens on write, never on read.** Hot/loop cues snap to
   the deck quantum at `setCue` time (quantize ON + grid attached);
   memory cues keep authored frames. Loop in/out snap at
   resolution-time to the *configured* quantum — a `1.0` constant
   became `quantizeBeatsFor(deck)` (`Custom` included).
6. **Actions are explicit, resolvable, timestamped.** A per-deck
   `PerformanceAction` table tracks `PerfAction` ×
   `ActionState(Idle→Armed→Active→Releasing→Completed)`; completed
   retires to Idle so `actionState()` answers "is this gesture
   running". `PerformanceActionResolver` is a pure decision table
   (roll-resize, scratch↔roll exclusion); the facade performs the
   side effects. Timestamps use an injected clock — bookkeeping,
   never timing.
7. **Jog policy lives in the performance layer.** `MainComponent`
   and controller jog ticks route through `perf.jog()` →
   `JogEngine::tick` decides Seek/PitchBend/Scratch/Browse →
   rt commands (or none, for Browse). This removed the old path
   where UI pushed `rt::DeckSeek`/`SetPitchBend` directly — scratch
   vs. roll exclusion can now deny at the resolver instead of
   corrupting state downstream. `DeckJogMode` numerically matches
   the controller layer's `JogMode` so hardware wiring is a cast.
8. **Named engines are non-RT bookkeeping.** `LoopRollEngine`,
   `KeyLockEngine`, `PitchEngine`, `BeatgridController` live in
   `DeckCtx` — they own policy state (roll arm/size, lock flag+algo,
   range, grid pointer+trackId) while the RT work stays in
   `DeckEngine`. `PerformanceDeckEngine` is a value-type handle over
   one deck's ctx — the §1 architecture without duplicating logic.
9. **Custom quantum is config, not hardware.** `Quantize::Custom`
   snaps via `quantizeBeats(Custom, bpb, customBeats)` but is
   excluded from `cycleQuantize()` — the 7 hardware resolutions stay
   the button cycle (regression caught by `[sync]` tests).
10. **Prefs persist durable settings only.** Quantize on/off,
    quantum, custom beats, slip, key lock, pitch range and cue
    behavior persist per deck through `PerformanceStore` kv keys and
    restore on `setStore`. Transient action state is never stored.

## Consequences

- UI/MIDI/HID/automation share every action — new input paths need
  only call the facade.
- `DeckStats` grew `slipShadowSec` + `brake` echoes; `poll()` gained
  brake reconcile + loop-action lifecycle edges.
- `rt::Command` gained `DeckBrake`; scheduler ordering unchanged
  (brake rides the transport bucket).
- The unified transport changed command delivery for
  `EngineCommands`-driven playback: tests that fake `EnginePorts`
  must wire `perf.setCommandSink` (exactly like the app does) —
  the sink is now the single emit path.
- Roll/scratch exclusion is a documented policy (`ActionRejected`),
  not silent corruption — future actions extend the resolver table.
- Brake resume-after-full-stop is deliberate: matches turntable
  start/stop UX; a plain `Play` after brake-stop starts instantly
  (latch cleared by `setPlaying`).

## Deferred / limits

- `HotCueMode::JumpCue` when stopped is wired through
  `triggerCue`'s stopped path; pad-level "cue audition" semantics
  stay with `CueBehavior`.
- Key-lock quality remains bound to `WsolaStretcher`; the algo slot
  (`setKeyLockAlgo`) is reserved for future stretchers.
- `BeatgridController` owns grid access for the perf layer; the RT
  side still receives grids via pending/retired atomic handoff
  (unchanged, RT-safe).
