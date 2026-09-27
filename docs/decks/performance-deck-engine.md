# Performance Deck Engine (Phase 28)

Every deck is a complete DJ performance system. UI, MIDI, HID,
controller plugins and internal automation all reach the same
implementation — nothing is duplicated per input path.

```
PerformanceEngine (shared, 4 decks)
└── PerformanceDeckEngine deck(n)          — per-deck facade (§1)
    ├── TransportEngine                   — unified transport + brake
    ├── CueEngine                         — cue storage/trigger
    ├── HotCueManager / MemoryCueManager  — hot & memory cues
    ├── SmartLoopEngine / LoopSizeController — loops
    ├── LoopRollEngine                    — held temporary loops
    ├── BeatJumpEngine                    — musical jumps
    ├── QuantizeEngine                    — grid snapping
    ├── SlipEngine                        — shadow timeline refs
    ├── KeyLockEngine                     — key-lock bookkeeping
    ├── ReverseEngine                     — forward/reverse
    ├── PitchEngine                       — range/reset/bend
    ├── JogEngine                         — seek/bend/scratch/browse
    ├── BeatgridController                — musical position authority
    └── PerformanceAction[]               — state machine + resolver
```

The facade emits POD `rt::Command`s through the
`PerformanceScheduler` (priority ordering, coalescing) into the
bounded `rt::CommandQueue` (512). The audio callback owns all
timeline truth; `DeckStats` echoes it back atomically.

## Transport (single implementation)

`PerformanceEngine::transport(deck, Transport)` is the **only**
entry point for PLAY / PAUSE / STOP / CUE press+release / RESTART /
BRAKE. `TransportEngine` (per-deck, non-RT) owns brake edge logic
(`Brake` toggle, `BrakeOn`/`BrakeOff` momentary) and the ramp length
(`brakeSec`, default 0.55 s).

**Brake** is a turntable start/stop latch: engaging spins the deck
down with a smoothstep ramp until it halts; releasing spins back up
and resumes — even if the release arrives after the ramp completed
(`brakeStopped_` latch in `DeckEngine`). The `stats.brake` echo stays
engaged for the whole latch so LEDs show the true state; `poll()`
reconciles the non-RT flag against the RT echo.

## Cue

`PrimaryCuePoint` = durable session marker `{frame, beat, createdAt}` —
kept separately from hot cues. `cueButton(press)` while playing:
seek to the point + pause (all behaviors). While paused the
**`CueBehavior`** decides:

| Behavior   | Press                              | Release            |
|------------|------------------------------------|--------------------|
| `Hold`     | redefines the point here, previews | stop + return      |
| `ToStart`  | seeks to stored point, previews    | stop + return      |
| `ToLastCue`| seeks to stored point, previews    | stop + return      |

`ToLastCue` additionally re-anchors the point on every cue action —
triggering a hot cue moves the primary cue point to it.

Cue playback (deck audio) vs cue monitoring (PFL/headphones) are
distinct paths — monitoring routes through the mixer `chN.cue`,
never through this engine.

## Hot cues / memory cues

`setCue(deck, slot, CueType::Hot|Memory|Loop, frame, …)` — one
storage path for all cue kinds. **Snap-on-create**: with quantize ON
and a grid attached, hot/loop cues snap to the configured quantum
(1/4…1/32…CUSTOM); memory cues keep their authored frame.
`HotKind` = `Instant | Cue | Marker` — logical types, no duplicated
behavior. Triggering follows `HotCueMode` (`JumpPlay` when playing,
configurable when stopped). `deleteCue` removes one slot without
touching other cues, memory cues or the grid.

## Loops

Manual: `loopIn()` arms, `loopOut()` resolves — with quantize ON both
ends snap to the **configured quantum** (not a fixed beat). Auto:
`loopBeats(b)` / `loopBeatsIdx(i)` over the configurable size table
`1/32 … 32` beats. `loopHalve/loopDouble` resize live,
`loopMove(beats)` shifts by musical amounts through the grid,
`reloop`/`exitLoop`/`loopToggle` control state. Saved loops persist
per track in `PerformanceStore`.

## Loop roll

`loopRoll(beats)` arms a slip ref (shadow timeline keeps running),
installs a beat loop and records it in `LoopRollEngine`. A second
`loopRoll` while held **resizes** the running roll (resolver
`Decision::Resize`). `loopRollEnd()` releases: loop off, slip ref
dropped, playhead resumes where the track *would have been*.
Rolling while scratching is denied; scratching while rolling is
denied (see resolver).

## Slip

`SlipEngine` holds named refs (`UserMode`, `Roll`, momentary
actions). While any ref is held the RT layer advances `slipShadow_`
on the real timeline — exposed as `stats.slipShadowSec` (**Slip
Return Position**, -1 when idle) — and on release seeks back to it.
Slip composes with roll/scratch/reverse/hot cues; `SlipReanchor`
re-bases the shadow mid-flight (never coalesced with seeks).

## Beat jump

`beatJump(beats)` — ±1/32…±64 through the size table or raw beats.
With a grid it lands on exact musical positions via the tempo map
(no error accumulation); without a grid it falls back to seconds.
Play state is preserved in both directions.

## Quantize

`QuantizeEngine`: `snapToBeat` / `snapToBar` / `snapToSubdivision` —
all resolve through the attached `BeatGrid` (the tempo map; never an
independent clock). Resolutions: Beat1, Half, Quarter, Eighth,
Sixteenth, ThirtySecond, Bar — plus **Custom**
(`setQuantizeCustom(beats)`), settable per deck but deliberately
outside the hardware cycle. Sources: `BeatGrid` tempo map only —
no grid → actions degrade to time-position fallback.

## Key lock / pitch / jog

`setKeyLock` toggles the stretcher path through the facade
(`KeyLockEngine` tracks state + algo slot); tempo changes keep the
key within the WSOLA algorithm's limits, toggling mid-play is
click-free (DSP crossfade in `DeckEngine`).

`PitchEngine`: hardware range ±6/±10/±16/WIDE(50) —
`setPitchRange`/`cyclePitchRange` persist as a pref. `pitchReset`
returns tempo to 0 % through the policy path (respects sync).
`pitchBend` is momentary — never rewrites the fader target.

`JogEngine` turns one tick into a decision by `DeckJogMode`:
`Seek` (seconds nudge), `PitchBend` (clamped bend), `Scratch`
(platter wiggle, action-tracked), `Browse` (**never** touches the
playhead). Controller jog modes share the same numeric values —
hardware events flow straight through.

## Beatgrid authority

`BeatgridController` owns the per-deck grid pointer, track id,
attachment and every musical conversion (`snap`, `frameToBeat`,
`beatToFrame`, bar math, confidence). All musical actions consult it
when a grid exists; without one they degrade to frame/second math.

## Performance actions (state machine)

Every gesture is a `PerformanceAction{type, state, slot, timestampMs,
value}` in a per-deck table — never a bare boolean. States:
`Idle → Armed → Active → Releasing → Completed` (completed retires
back to Idle). Timestamps come from an injectable `setNowFn` clock —
bookkeeping only, never a timing dependency.

**Resolver policy** (`PerformanceActionResolver`) — deterministic
rules, documented, applied before any command is emitted:

- LoopRoll while rolling → `Resize` (live size change)
- Scratch while rolling → `Deny` (corrupts the roll window)
- LoopRoll while scratching → `Deny`
- Everything else → `Allow` (slip refs compose)

Denials emit `PerfEventType::ActionRejected` + `PerfError`.

## Persistence

`PerformanceStore` persists cues + saved loops per track (already)
plus session prefs per deck: quantize on/off, quantum, custom beats,
slip, key lock, pitch range, cue behavior. Transient state (active
scratch, roll-in-flight, preview) is never stored.

## Real-time safety

Non-RT side: policy, snapping, persistence, events. RT side
(`DeckEngine`): `applyDeckCommand` consumes POD commands only — no
allocation, locks, file/db/network in the callback. The scheduler
bounds bursts; the queue is bounded; commands are plain structs.
