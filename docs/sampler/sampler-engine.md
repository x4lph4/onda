# Sampler Engine (Phase 34)

Professional performance sampler: banks → slots → async decode →
bounded RT voices → sampler bus → mixer channel 4.

## Signal path

```
Controller / UI / keyboard / FLX4
  → PadLayer → PadModeManager           (one resolver, all hardware)
  → SampleTrig / SampleRelease / SampleStop
  → SamplerEngine                       (app/control thread)
  → rt::CommandQueue { SampleTrigger, SampleRelease, SampleStop,
                       SampleSlotParam, SamplerParam }
  → SamplerRt                           (audio thread only)
  → mixer channel 4 strip → master/cue outs
```

Threading contract: the audio callback never does file I/O, DB
queries, decoding, blocking locks or avoidable allocation. Loading,
cache eviction, persistence, missing-file checks and undo history all
live on control threads; audio data crosses via an atomic mailbox
(`publishSlot` → `collectInbox`, `retireTrash` off-thread).

## Banks and slots

`SampleBank` — id, name, position, dynamic slot array (8 on pads;
deque ready for 16/32). `SamplerEngine` owns N banks (A/B/C/D…);
`selectBank` republishes params + cached audio to RT slots.

`SampleSlot` metadata:

| field | notes |
|---|---|
| `sampleId` `path` `name` `colorArgb` | identity (sample row shared) |
| `volume` `pan` `gain` `pitch` | smoothed in RT; pitch = semitones ±48 |
| `mode` | OneShot / Loop / Hold / Gate / **Retrigger** |
| `quant` | Off, 1/4…1/32, Bar, **1/2, 2Bar, 4Bar** |
| `syncOn` `loopOn` `mute` `solo` | flags |
| `filterPos` | per-slot tilt filter −1..+1 |
| `attackMs` `releaseMs` | envelope overrides, 0 = engine default |
| `startF` `endF` `loopIn` `loopOut` | non-destructive window (frames, −1 = unset) |
| `missing` | file gone — assignment kept until user removes |

## Playback modes

| mode | press | release | same-slot press while playing |
|---|---|---|---|
| `OneShot` | start → plays to end | — | choke + restart |
| `Hold` | start | release envelope out | choke + restart |
| `Gate` (toggle) | start | — | release (second press = stop) |
| `Loop` | start → repeats | — | choke + restart |
| `Retrigger` | start a **new** voice every press | — | none — true polyphony |

## Voices, polyphony, stealing

`SamplerRt`: 32 slots × 32 voices. `StealPolicy`: Oldest,
Quietest, SameSample, Deny (lowest-priority maps onto the same
preference chain). Voice = POD: source `shared_ptr<const SampleData>`
(RT-owned → cache eviction can't pull the rug), position, step
(pitch ± sync), envelope, tilt-filter state, window bounds,
`delay` (quantized start) and `releaseAfter` (quantized stop).

## Envelope (click prevention)

Per-slot attack/release in milliseconds → env step once per trigger.
`0 ms` = engine defaults (≈0.2 ms fade-in, ≈21 ms release).
Every start, stop, choke and steal fades — never a hard cut.

## Trim / loop (non-destructive)

`SP_TrimStart/End` bound the playback window; `SP_LoopIn/Out` the
loop region inside it. All metadata — the file and `SampleData` are
never touched. A valid region (`loopOut > loopIn`) wraps at
`loopOut` back to `loopIn`; unset fields fall back to window edges.

## Quantization

`SampleSync::framesToBoundary` + the shared master clock fed via
`setClock(bpm, beatPos)` — the same `BeatClock`/`SyncContext` math
the FX engine uses, no parallel BPM. Applies to trigger **and** to
stop/release (`releaseAfter` countdown) so quantized loops end on
the grid.

| mode | beats |
|---|---|
| 1/32 1/16 1/8 1/4 | 0.125–1 |
| 1/2 | 2 |
| Bar / 2Bar / 4Bar | 4 / 8 / 16 |

## Filter and FX

- **Per-slot tilt filter** (`SP_Filter`): one-pole, coefficient
  computed once per trigger. `f<0` → LP sweep 20 kHz→80 Hz (cubic);
  `f>0` → HP tilt 40 Hz→9 kHz. Per-voice state, ~0 cost when |f|<0.02.
- **Sample FX**: voices sum into the sampler bus → mixer channel 4
  strip → the channel FX rack (`FXTarget` insert). Per-sample FX are
  the per-voice DSP above + per-slot params; a dedicated
  `SampleFXChain` per voice is a documented seam — route via the
  existing `FXEngine` insert rather than a parallel engine.
- **Sampler master gain**: `SamplerParam{0, gain}` (bank gain),
  smoothed in RT.

## Loading, cache, memory

`SampleLoader`: one worker, `IDecoder` (FFmpeg), decode + peak +
coarse waveform (≤512) per load. `SampleCache`: 256 MB LRU, `pin()`
preloads pad-assigned samples. Ownership is `shared_ptr<const>` —
an in-flight voice keeps its data alive even after eviction/unload.

## Missing samples

`SamplerEngine::checkMissing()` re-validates every assigned path
(`fs::exists`) and sets `slot.missing` — runs at `loadAll()` and on
demand. The assignment is never deleted automatically:

- **Locate/Relink** → `relinkSample(pad, newPath)` keeps all slot
  metadata (name, color, params, window) and reloads.
- **Remove** → `unloadSample(pad)` (explicit).
- Decode failures isolate to `SlotState::Error` — AudioEngine and
  other slots are unaffected.

## Undo/redo

`beginSlotEdit(pad)` → gesture → `commitSlotEdit(pad)` pushes a
before/after snapshot (64-deep, drops oldest). `undoSlotEdit()` /
`redoSlotEdit()` restore the editable fields only — never the
path/sampleId — and re-mirror to RT when the slot is on the active
bank. No-op gestures don't push.

## Persistence

`SampleRepository` (shared `onda.db`): `samples`, `sample_banks`,
`sample_slots`. Phase-34 columns (`attack_ms`, `release_ms`,
`trim_start`, `trim_end`, `loop_in`, `loop_out`, `filter_pos`) are
added by guarded `ALTER TABLE` in `init()` — old DBs migrate in
place, `init()` stays idempotent. Enum additions are append-only
(`Retrigger=4`, `Q_1_2=6`, `Q_2Bar=7`, `Q_4Bar=8`) so persisted
indices stay valid.

## Pads / DDJ-FLX4

`PadModeManager` resolves every mode to one action type — Sampler
mode → `SampleTrig(slot, velocity)` / `SampleRelease` / shift →
`SampleStop`. `PadLayer` dispatches through `CommandManager`
(`pad.<deck>.<n>[.shift]`). The FLX4 pad surface (8 pads × layer,
velocity, LED states Off/Active/Playing/Looping/Armed/Selected)
uses only documented MIDI notes — no proprietary internals.
`PerformancePad` capability flags carry velocity where hardware
provides it (constant otherwise).

## Realtime safety

- `applyCommand`/`renderPcm`/`publishSlot`/`retireTrash` boundaries
  are documented and enforced: only the first two run on the audio
  thread.
- Commands are bounded POD `std::variant` in a lock-free queue.
- Voice memory is a fixed pool — no allocation in the callback.
- Filter coefficient and envelope rates compute once per trigger
  (algebraic, no transcendentals), not per sample.

## Limitations

- **Time-stretch** (§25): `SamplePitchProcessor` is the seam
  (rate-scaling today); `syncOn` follows master BPM via rate — no
  independent stretch engine yet.
- **Sample key / harmonic triggering** (§27): BPM + pitch exist;
  key metadata is stored on the track, not yet the slot.
- **Per-slot meters** (§36): engine exposes per-slot state atomics;
  metering is at the sampler bus today.
- **Quantized stop** uses the last clock snapshot (`setClock`); the
  audio engine refreshes it per block — standalone RT use needs the
  same feeding discipline.
- `Gate` kept the name from Phase 11; spec "TOGGLE" == `Gate`.
- Undo granularity is an edit **gesture** (begin/commit), not every
  `setSlotParam` call — a continuous knob drag is one undo step.
