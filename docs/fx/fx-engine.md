# Onda — Professional FX Engine (Phase 16)

Modular, realtime-safe effects architecture. Phase 6 laid the
foundation (slot/rack/engine, dry/wet, lock-free effect swaps,
sanitization); Phase 16 completes it with six new processors, a
canonical FX clock with triplet divisions, declarative routing,
reorderable chains, macros, automation seams, presets and
diagnostics.

## Architecture

```text
                     ┌──────────────── FXEngine (RT, inside MixerEngine)
                     │   owns racks + send buses + sync contexts
                     │
   deck source ──────┼──► insert rack [DeckA..D] ──► channel strip
   strip post-fader ─┼──► send bus ──► rack [SendA/B] ──► return ─┐
   channel sum ──────┼──► master rack [Master] ──► master bus ◄───┘
                     │
   cue bus ──────────┘   (isolated — never routed through FX)

   FXRack (per target)         FXSlot (chain position)
   ┌──────────────────┐        ┌──────────────────────────┐
   │ packed order map │───────►│ dry→inGain→FX→outGain→wet │
   │ pos0..3 → phys   │        │ mix×bypass crossfade      │
   └──────────────────┘        │ sanitize/auto-reset/fault │
                               └──────────────────────────┘
                                        │ FXProcessor (DSP)
                                        ▼
                        Delay/Echo/Reverb/Filter/Flanger/Phaser/
                        Gate/Chorus/Distortion/BitCrusher/
                        Compressor/BeatRepeat/BeatRoll
```

Control side (non-RT, same entry for UI/keyboard/MIDI/HID/FLX4):

```text
UI / keys / MIDI / HID / DDJ-FLX4
        │
        ▼
CommandManager (0xA000 ids)
        │
        ▼
FXSurface ──► MixerController ──► rt::SetParam ──► FXEngine::setParam
  focus      (sanitize+store)        (lock-free)
  macros ──► FXMacro::evaluate
  presets ──► FXPresetManager / applyFxPreset
  reorder ──► moveFxSlot ──► FXRack packed order
```

An effect never sees the UI, the controller layer, MIDI or
DeckEngine — it only implements `FXProcessor`.

## Processors

| FX | Params (P1..) | Synced |
|----|---------------|--------|
| Filter | Cutoff, Resonance, Mode(LP/BP/HP), LfoDepth, LfoRate | LFO |
| Delay | Time, Feedback, Damping, PingPong | time |
| Echo | Feedback, Tone, PingPong, Freeze | time |
| Reverb | Size, Decay, Damping, PreDelay, Width | — |
| Flanger | Rate, Depth, Feedback, Phase | rate |
| Phaser | Rate, Depth, Feedback, Stages, Center | rate |
| Gate | Shape, Steps, Depth, Mix | rate |
| **Chorus** | Rate, Depth, Delay, Width, Tone | rate |
| **Distortion** | Drive, Tone, Curve, Output, Asym | — |
| **BitCrusher** | Bits, Downsample, PostLP, Dither | — |
| **Compressor** | Threshold, Ratio, Attack, Release, Knee, Makeup | — |
| **BeatRepeat** | Engage, SizeMult, Backstep, Pitch | slice |
| **BeatRoll** | Engage, SizeMult, Gate, Tone | slice |

New in Phase 16 in **bold**.

- **Chorus** — two modulated delay lines, quarter-cycle LFO offset;
  `Width` morphs L/R from correlated to split movement.
- **Distortion** — `tanh` soft-clip blended into a folded-sine
  hard shape; output bounded by construction before the tone LP and
  output gain. `Asym` biases the shaper for even harmonics.
- **BitCrusher** — amplitude quantization (1..16 bit) + sample-and-hold
  decimation (1..60×), optional deterministic xorshift dither, post LP.
- **Compressor** — stereo-linked peak detector (`max(|l|,|r|)` so the
  image never wanders), attack/release envelope in the dB domain,
  soft knee, makeup.
- **BeatRepeat** — always-on capture ring (2 s). Engaging latches the
  last `division×SizeMult` frames (offset `Backstep` slices into the
  past) and loops them; `Pitch` scrubs playback rate. ~2 ms fade at
  the slice edge — no clicks. Disengaged = untouched passthrough.
- **BeatRoll** — moving roll: capture never stops; every slice
  boundary the read window re-locks onto the freshest region, so the
  roll rides the groove instead of freezing a bar. `Gate` mutes the
  tail fraction of each cycle for stutter textures.

## FXClock — canonical sync math (§23)

`src/core/fx/FXClock.h` is the **only** place a division becomes
time. `SyncContext::divisionFrames()/divisionMs()` mirror it
exactly; no effect contains BPM math.

```text
BeatDiv → beatsOf() → ×60/BPM → ms → ×sampleRate → frames
```

`BeatDiv` gained triplets in Phase 16 (appended — persisted indices
unchanged): `1/16T` = 1/6 beat, `1/8T` = 1/3 beat, `1/4T` = 2/3 beat.
Verified at 120/128/140 BPM for every division.

`bpm` arrives via `FXEngine::setTempo(target, bpm, phase)` — fed by
the deck's BeatGrid effBpm (tempo-map aware) or the sync master for
send/master racks, i.e. it *is* the MasterClock value upstream.

## Chains and reordering (§7)

`FXRack` holds 4 slots and a packed order table (`uint32` — four
2-bit physical indices). `moveSlot(from,to)` rewrites the table
atomically on the control thread; the RT thread picks the new order
at the next block — no allocation, no lock, no processor swap. Slot
addressing is **chain-position** based everywhere (`slot(pos)`,
events, commands, macros), so a reorder is invisible downstream.

## Routing (§9)

`FXRouting` declares the topology instead of hardcoding it inside
effects (`core/fx/FXRouting.h`):

| Target | Route | Node | Post-fader | Cue |
|--------|-------|------|------------|-----|
| DeckA..D | DeckInsert | 0..3 | no | no |
| SendA/B | SendReturn | 0/1 | yes | no |
| Master | MasterInsert | — | no | no |
| Spare | SamplerInsert (future) | 0 | no | no |

The cue bus is deliberately unrouted — `affectsCue` is false on every
row. `FXEngine::routing(t)` exposes the descriptor for diagnostics
and preset/session metadata.

## Presets (§26-27)

`FXPresetManager` keeps immutable factory presets + user presets with
versioned binary blobs (`ONFX`/`ONFP`). Phase 16 factory collection:
Classic Delay, Dub Echo, Room Verb, Reso Sweep, Jet, Phaser Sweep,
Trance Gate, **Echo Out, Filter Sweep, Reverb Wash, Deep Filter,
Tape Echo, Build Up, Down Sweep, Space Delay, Crush, Pump, Roll 16,
Stutter**.

`MixerController::applyFxPreset(target, slot, preset)` — control
thread only — loads the type via the loader path, pushes
sync/division/mix/in/out gains, then normalizes each native param
through a scratch processor's `paramInfo` and writes P1..P8. Nothing
reaches the audio thread but sanitized SetParams.

## Macros (§28)

`FXMacro` = one knob driving up to 8 mappings
`{target, slot, param, min, max, curve}`. `evaluate()` emits concrete
param ids — including slot-level params like `Mix` and `Enabled`,
not just processor params. `kFocusTarget` mappings resolve to the
surface's focused rack, so one bank drives whichever unit is
selected. Curves: linear / square / sqrt; every destination keeps its
own min/max bounds. `FXSurface` ships a default bank: Build, Wash,
Crush, Echo Out (+4 empty).

## Automation (§25)

`FXAutomation`/`FXAutomationLane` — data model only: breakpoint lanes
of `(beat → normalized value)` with linear interpolation, addressed
by the same param ids everything else uses. A control-side owner
samples lanes against the session beat and pushes SetParams — to the
engine, automation is indistinguishable from a fader. No recorder or
sequencer yet, deliberately.

## Commands (0xA000)

| Id | Name | Value |
|----|------|-------|
| A000 | fx.focusTarget | FXTarget |
| A001 | fx.focusSlot | 0..3 |
| A002 | fx.enable | toggle |
| A003/A004/A005 | fx.typeNext/Prev/Set | ±1 / FXKind |
| A006 | fx.mix | 0..1 |
| A007 | fx.syncToggle | toggle |
| A008/A009/A00A | fx.divNext/Prev/Set | ±1 / BeatDiv |
| A010..17 | fx.p1..p8 Set | normalized |
| A018..1F | fx.p1..p8 Up | +0.02 |
| A020..27 | fx.p1..p8 Dn | −0.02 |
| A028/29/2A | fx.presetNext/Prev/Apply | index |
| A030..37 | fx.macro0..7 | 0..1 |
| A038/39 | fx.slotMoveL/R | reorder |
| A03A | fx.diagCopy | clipboard |

Hardware path: `DDJ-FLX4 → ControllerEngine → MappingEngine →
CommandManager → FXSurface/MixerController`. Never hardware →
FXProcessor.

## Diagnostics & failure isolation (§39/§43)

Every slot already sanitized bad frames and auto-reset runaway
processors; Phase 16 adds the explicit surface:

- `FXSlot::faults` — auto-reset counter; `FXEventType::Fault` event.
- `FXSlotDiag`/`FXRackDiag` — kind, enabled, busy, pending, mix,
  sync mode, division, in/out peak, cpu µs, sanitized count, faults,
  latency per slot; per-rack totals + order + route.
- `FXEngine::fillDiagnostics(t, out)` / `diagnosticReport()` →
  clipboard via `fx.diagCopy`.

A faulting processor is reset in place (playback never stops);
sustained faults stay counted for investigation.

## Realtime safety

- `prepare()` allocates everything (capture rings, slice buffers,
  delay lines); `process()` performs zero allocation, zero locks,
  zero I/O. BeatRepeat's slice buffer is preallocated at worst case
  so `latch()` only copies.
- Effect swaps allocate on the control thread and hand off via the
  lock-free pending/trash atomics — adoption at block boundary.
- Every processor reports `latencySamples()` (all zero today);
  `FXEngine::maxLatencySamples()` is what a future PDC pass consumes.
- Bypassed + settled slots hibernate (`busy()` false) — the rack and
  mixer skip them at zero cost.

## Known limitations

- Send/master sync follows `fxSyncMaster` (master deck) — per-target
  tap points not yet implemented.
- Automation is a data model + evaluator — no recorder/sequencer.
- `SamplerInsert` route is declared for the Spare target; sampler FX
  wiring lands with the sampler-bus work.
- Macros address chain *positions*; a mid-performance reorder moves
  the target of already-playing macro mappings (documented behavior —
  reorder is a sound-design action).
- All current processors report 0 latency; dry/wet alignment is
  trivially exact today and the reporting surface is what a future
  latency-bearing FX consumes.
