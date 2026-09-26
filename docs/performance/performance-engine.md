# Performance engine — hot cues, memory cues, loops, rolls, jumps, quantize, slip, reverse

Phase 20 (`docs/adr/0020-performance-engine.md`). The musical
performance layer sits between every input source (UI, keyboard,
MIDI, DDJ-FLX4) and the audio thread. It translates musical actions
into resolved `rt::Command` values through a deterministic scheduler —
the audio callback only ever receives frame-accurate, POD commands.

```
UI / keyboard / MIDI / DDJ-FLX4 / automation
        │  shared command ids (ui::cmd)
        ▼
CommandManager ── PadLayer / PadModeManager (pad semantics)
        │
        ▼
PerformanceEngine                     (non-RT, caller thread)
 ├── CueEngine                        cue storage + trigger math
 ├── HotCueManager                    typed facade (pad-space cues)
 ├── MemoryCueManager                 typed facade (nav markers)
 ├── SmartLoopEngine                  grid-aware loop math
 ├── LoopSizeController               canonical size tables
 ├── BeatJumpEngine                   signed beat targets
 ├── QuantizeEngine                   grid snapping
 ├── SlipEngine                       refcounted shadow sources
 ├── ReverseEngine                    latch + momentary XOR
 ├── PerformanceScheduler             priority + coalescing
 └── PerformanceDiagnostics           counters + last-error
        │
        ▼  POD commands only
rt::CommandQueue ──► audio callback ──► DeckEngine
```

## Hot cues

`Cue` (`PerformanceTypes.h`) carries: `slot`, `type` (Hot/Memory/Loop),
`kind` (`HotKind::Instant|Cue`), `frame`, `frame2` (loop-out),
`beat`/`beat2` (musical positions, re-resolved after grid edits),
`argb`, `label`, `comment`, `enabled`, `quantized`, `createdAt`.

- Storage is unbounded — 8 pads is a display convention, not a cap.
- Actions: `setCue`, `triggerCue`, `deleteCue`, `moveCue`, `renameCue`,
  `recolorCue`, `setCueEnabled`, `setCueKind`, `setCueQuantized`,
  `setCueComment`. Command ids `forHotCue(deck, slot, action)` cover
  trigger/set/delete; field edits flow through `deck.*` handlers.
- Trigger semantics (§5): while playing → jump + keep playing. While
  stopped → `HotCueMode::JumpPlay` (default, seek+play) or `JumpCue`
  (seek, stay cued). `HotKind::Cue` always takes the cued path.
- A disabled cue rejects with `PerfError::Disabled`; an empty slot
  with `EmptySlot`. Both emit `ActionRejected` and preserve state.

## Memory cues

`CueType::Memory` — permanent navigation markers. Never auto-play on
trigger (seek only). Carry `label`/`comment`/`argb`. Rendered as
hollow flags in the waveform (type=1 markers) and as a dedicated
strip in `PerformancePanel` — left click = goto, right = set,
ctrl+right = delete (`forMemCue(deck, slot, 0|1|2)`). The
`MemoryCueManager` facade forces `type == Memory` so the two cue
families can never silently collide in a slot.

## Loops

`SmartLoopEngine` does all math on the `BeatGrid` (beat-aligned even
under tempo maps). Live range tracked per deck (`loopRange`).

- Manual: `loopIn` (arms), `loopOut` (commits; quantize snaps both
  boundaries to beats when on). `out ≤ in` rejects `BadRange`.
- Auto: `loopBeatsIdx(deck, idx)` through the canonical table —
  1/32, 1/16, 1/8, 1/4, 1/2, 1, 2, 4, 8, 16, 32 beats.
- Adjust: `loopHalve`, `loopDouble`, `loopMove(±beats)` — all keep the
  in-anchor and grid alignment. `reloop`, `exitLoop`, `loopToggle`.
- Saved loops persist per track (`saveLoop`/`triggerSavedLoop`/
  `deleteSavedLoop`, `saved_loops` table).
- `LoopSizeController` is the single size table every surface shares —
  UI steppers, pad sizes, roll sizes (1/32…1 in-table, extended
  beyond), and jump magnitudes (1…64 beats, signed at the call site).

## Loop roll

`loopRollIdx(deck, idx)` / `loopRollEnd(deck)`. The roll holds a
`SlipEngine::Roll` reference — `SlipBegin` anchors the shadow, a
beat-quantized temporary loop plays, and releasing ends the loop and
returns to where the track *would have been* (no rewind, no click —
the recovery is a faded seek inside `DeckEngine::endSlip`).

## Slip

`SlipEngine` is **reference-counted**: `UserMode` (the toggle),
`Roll`, `Momentary` each hold a ref; the shadow runs until the last
releases. Releasing a roll while user-slip is armed keeps slipping —
the ordering hazard a boolean flag can't express.

`DeckEngine` owns two positions: the heard playhead and
`slipShadow_` (advanced at the resolved rate every block). Permanent
navigation under slip (hot cue, beat jump, instant restart) posts
`SlipReanchor` **after** the seek so `slipShadow_ = displayedPos()`
snapshots the new position — slip-off continues from the cue/jump
timeline instead of snapping back.

## Reverse

`ReverseEngine`: effective = latch XOR momentary. Latched ON +
momentary press = temporary *forward* (hardware semantic); release
restores the latch. `DeckEngine::setReverse` resets the fade ramp so
direction flips don't click. Under slip the shadow keeps advancing
forward — slip-off lands past the anchor, not where reverse left it.

## Cue point + cue button

`cuePoint` is the session's temporary CUE (distinct from hot cues —
`DeckCueSet`/`setCuePoint`). `cueButton(deck, CuePhase)`:

- press while playing → return to cue point + pause;
- press while paused → *define* the point at the playhead and preview
  while held;
- release → stop + return to the point.

`instantRestart` seeks to the cue point + plays without touching the
sync mode, master deck, or session clock.

## Beat jump

`beatJumpIdx(deck, idx, dir)` — 1/2/4/8/16/32/64 beats, signed by
`dir`. Targets resolve through the grid (never seconds): unquantized
jumps preserve beat phase exactly; quantized jumps land on
boundaries. Playback state is untouched — only a faded seek posts.

## Quantize

Per deck (`setQuantize`, `rt::SetQuantize` echo). Resolutions
`Quantize::Beat1…ThirtySecond,Bar` (`cycleQuantize`,
`setQuantizeResolution`). `QuantizeEngine::snap` recomputes the
*target position* onto the grid and seeks immediately — quantize
means "the target is musical", never "wait". Per-cue `quantized=false`
opts a cue out of snapping. `quantizeSnaps` counts snapped actions.

## Performance scheduler

Every public mutator posts through `PerformanceScheduler` and flushes
at return (`commit()`). Priority (lower emits first):

| Prio | Class     | Contents                                    |
|-----:|-----------|---------------------------------------------|
| 0    | SlipEnd   | release shadow before repositioning         |
| 1    | SlipBegin | anchor before the action that needs it      |
| 2    | LoopExit  | `SetLoopOn(false)` before re-arming         |
| 3    | Seek      | `DeckSeek`/`DeckSeekFrame`; `SlipReanchor`  |
|      |           | rides here (same prio, seq-ordered after    |
|      |           | its seek, never coalesced)                  |
| 4    | LoopArm   | `SetLoopFrames`/`SetLoop`/`SetLoopOn(true)` |
| 5    | Transport | play/pause/stop                             |
| 6    | State     | reverse, quantize, sync, pitch…             |

Same-class same-deck commands coalesce (newest wins) — two seeks in
one action emit only the final target. Queue overflow reports
`PerfError::SchedulerFull` and keeps prior valid state (bounded
64-entry pending list, zero allocation on flush).

## Diagnostics + failure recovery

`PerformanceDiagnostics` per-deck counters: cues set/deleted/
triggered, cue presses, instant restarts, loops entered/exited,
rolls, resizes, beat jumps, quantize snaps, slip begin/end, reverses,
commands posted/dropped/sent, rejections. `lastError(deck)` reports
the last `PerfError` (`Ok`, `NoDeck`, `NoTrack`, `NoGrid`,
`EmptySlot`, `Disabled`, `BadRange`, `SchedulerFull`) and every
rejection emits `ActionRejected` with the reason in `value`. An
action on an empty deck rejects `NoTrack` — commands are never
emitted into a deck with nothing loaded.

## MIDI architecture

```
MIDI → ControllerEngine → MappingEngine → CommandManager
          → (pads) PadLayer → PadModeManager → deck.* commands
          → PerformanceEngine → scheduler → rt queue
```

Hardware carries zero semantics: the FLX4 emits `pad.press`/
`pad.mode`; `PadModeManager::resolve` maps (deck, pad, shift, press)
to an action per mode — HotCue (press=trig, shift+press=del), Sampler,
BeatJump (pads 1-4 back, 5-8 fwd), Loop (1-4 halve/double/in/out,
5-8 sized loops `arg=10+idx`), Roll (gated, `arg=idx+1` press /
0 release). `kDecks = 4` — all four decks resolve independently.
`PadFeedbackEngine` produces per-pad light states
(Off/Dim/Set/Selected/Playing) + `flx4LedValue` intensity levels;
AppShell feeds it from the snapshot each tick, and the UI pad row +
hardware LEDs share the same model.

## Persistence + relocation

Schema v6 adds `kind`, `enabled`, `quantized`, `comment` to the `cues`
table; `saved_loops` unchanged. Everything persists write-through
against `track_id` — never the file path. `MissingFileManager::
relinkVerified` keeps the same `track_id` row on move (verified
content hash), so cues/loops survive relocation. `attachGrid` reloads
the track's cues+loops on track change.

## Undo

`EngineCommands` wraps cue/loop/grid mutators in `UndoManager::record`
entries that capture before/after state (`restoreCue`,
`restoreLoopRange`, `gridRestore` port → `AnalysisService::
saveUserGrid` + re-attach). `perf.undo`/`perf.redo` walk the stacks.
Continuous drag-style ops record once per command invocation — one
logical undo step, never one-per-frame.

## Timing

All target math is frame-exact f64 on the `BeatGrid` (tempo-map safe:
`frameToBeat`/`beatToFrame` through `TempoSegment`s). Verified at
90/120/128/140 BPM: quantized cue triggers emit seek targets exactly
on beat boundaries; auto-loops span the requested beats to within
one render block; roll recovery lands within ~0.04 s of the
uninterrupted-timeline position.

## Limits

- `PerformancePanel` is compact (pad row + loop/jump clusters + memory
  strip); a dedicated large pad grid is future UI work.
- Slip sources beyond UserMode/Roll/Momentary (scratch, cue-hold
  performance) are enum slots, not yet wired.
- `Phrase` sync mode resolves to bar boundaries (Phase 15 contract).
- FLX4 pads are single-intensity LEDs — `flx4LedValue` maps the five
  states to levels; per-pad RGB remains a UI-only extra.
- Undo covers cue/loop/grid edits; transport and mode toggles are
  intentionally not undoable (they're performance, not edits).

## Verification

`tests/performance_phase20_tests.cpp` — 36 cases covering the full
matrix: component units (size tables, slip refcount, reverse XOR,
scheduler priority/coalescing, pad modes, pad feedback, command-id
encodings), engine behavior (trigger modes, memory cues, cue button,
instant restart, sized/manual/roll loops, beat jumps, quantize at
4 BPMs, slip layering + reanchor, reverse latched/momentary/slip),
undo restore APIs, schema-v6 persistence round-trip, verified-relink
relocation, failure recovery, 4-deck stress (160 mixed actions in
~10 ms), and feedback-state honesty. Full suite: 393 tests green.
