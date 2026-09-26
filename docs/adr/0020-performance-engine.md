# ADR 0020 — Professional performance engine

Phase 20. Status: implemented.

## Context

Phases 4/15 built the core performance machinery: `PerformanceEngine`
with per-deck grid/context, `CueEngine`, `QuantizeEngine`,
`BeatJumpEngine`, `SmartLoopEngine`, slip shadow + reverse in
`DeckEngine`, pad modes in `PadModeManager`, cue persistence in
`PerformanceStore`. Phase 20 demands the complete professional
surface — named sub-engines, richer cue metadata, loop rolls, cue
button semantics, undo, deterministic ordering when actions collide,
diagnostics, and persistence of the new fields — without duplicating
transport (`DeckEngine`), sync (`SyncEngine`), or input routing
(`CommandManager`).

## Decisions

1. **Extend, don't fork.** The requested architecture exists almost
   entirely — Phase 20 adds the *missing named components* around it:
   `HotCueManager`/`MemoryCueManager` (typed facades over the shared
   `CueEngine` storage — never a second cue table),
   `LoopSizeController` (canonical size tables), `SlipEngine`,
   `ReverseEngine`, `PerformanceScheduler`,
   `PerformanceDiagnostics`. `DeckEngine`/`SyncEngine`/`CommandManager`
   keep their jobs unchanged.

2. **Every action posts through `PerformanceScheduler`.** Commands
   queue with a documented priority (SlipEnd → SlipBegin → LoopExit →
   Seek → LoopArm → Transport → State) and coalesce per deck+class so
   UI/MIDI arrival order can never decide what the audio thread does.
   Bounded 64-entry pending list; overflow reports
   `PerfError::SchedulerFull` — the audio thread is untouched by
   scheduler internals (it only ever sees the emitted rt commands).

3. **Slip is reference-counted (`SlipEngine`).** `UserMode`, `Roll`,
   `Momentary` each hold a ref; `SlipBegin`/`SlipEnd` emit only on
   active-state *edges*. A roll released under user-armed slip keeps
   the shadow — a boolean flag cannot express that layering.
   `DeckEngine` gained `SlipReanchor`/`reanchorSlip()`: permanent
   navigation under slip (hot cue, beat jump, instant restart) posts
   reanchor **after** its seek so the shadow adopts the new position —
   the Phase-20 semantic that hot-cue-under-slip is a *move*, not a
   temporary action (loop roll / momentary reverse remain temporary
   and snap back).

4. **Cue model v6.** `Cue` gains `kind` (Instant/Cue), `enabled`,
   `quantized`, `comment`; `HotCueMode` (JumpPlay/JumpCue) defines
   stopped-deck trigger behavior; `CuePhase` models device-agnostic
   CUE press/release; `PerfError` + `ActionRejected` make every
   rejection deterministic and reportable. Schema migrates to v6 —
   fields round-trip through `PerformanceStore`, keyed by `track_id`
   (verified-relink keeps identity across file moves).

5. **Cue point ≠ hot cue.** `cuePoint` is a session-only temporary
   marker; `cueButton(Press|Release)` implements the classic
   press-while-playing (return+pause) / press-while-paused (define+
   preview) / release (return) contract. `instantRestart` restarts
   from it without touching sync/master clock.

6. **Roll is a gated sized loop holding a slip ref.** `loopRollIdx`
   uses the extended `LoopSizeController` table (1/32…1, then 2/4/…);
   `loopRollEnd` releases the `Roll` ref — with `SlipBegin`/`SlipEnd`
   ordered by the scheduler around the loop commands.

7. **Pad semantics live in `PadModeManager`, 4 decks.** Roll mode is
   gated (press = size `arg`, release = 0); Loop mode pads 5-8 emit
   sized loops (`arg = 10 + sizeIndex`). `PadFeedbackEngine` is a pure
   per-pad light model shared by the UI pad row and hardware LEDs —
   FLX4 gets intensity levels (`flx4LedValue`).

8. **Undo captures states, not gestures.** `EngineCommands` records
   before/after cue/loop/grid snapshots in `UndoManager` entries;
   `perf.undo`/`perf.redo` restore verbatim (`restoreCue`,
   `restoreLoopRange`, `gridRestore` → `saveUserGrid` + re-attach).
   One record per command — a continuous drag produces one undo step.

9. **`PerformancePanel` hosts the deck performance surface.** Pads,
   loop cluster + size stepper, jump cluster, roll gate, bend, memory
   strip (custom-drawn — `StateButton` has no secondary-click).
   `DeckView` refactored to host it; `AppShell` feeds memory cues,
   pad lights (via `PadFeedbackEngine`), and size readouts per tick —
   the panel itself keeps zero engine access.

## Consequences

- rt-thread boundary preserved: the engine emits only resolved frame
  numbers through the existing POD queue; no allocations/locks/DB
  reach the audio callback. Verified: 160 mixed actions + 160 render
  blocks in ~10 ms (Debug).
- The Phase-11 loop-pad layout changed (pads 5-8 = sized loops, no
  dedicated toggle pad) — loop toggle remains on the panel/command.
- Hot-cue-under-slip semantics changed deliberately: re-anchor
  instead of snap-back (old test updated to encode the new contract).
- 36 new tests, 393 cases / 1,644,293 assertions green.
