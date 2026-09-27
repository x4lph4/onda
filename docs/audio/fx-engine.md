# FX Engine — Phase 25

Professional, modular, realtime-safe audio effects engine. Phase 16
built the foundation (processor interface, slots, racks, routing,
presets, automation, diagnostics); Phase 25 completes the professional
semantics: **tail-aware bypass, per-stem FX, sampler insert routing,
MASTER/DECK/MANUAL tempo sources, param type metadata, and pending
parameter preservation**.

## Architecture

```
MixerEngine
└── fx::FXEngine
    ├── FXRack[kNumTargets]        one rack per routing target
    │   └── FXSlot[kSlotsPerRack=4]  chain of up to 4 effects
    │       └── FXProcessor*        the DSP (swapped lock-free)
    ├── send buses L/R (SPSC-accumulated post-fader taps)
    ├── FXSlot* stemSlots_[4][4]    borrowed deck-owned stem slots
    ├── SyncContext ctx_[targets]   per-target tempo feed
    └── FXEvent SPSC ring           RT->control notifications

DeckEngine (each of 4 decks)
└── FXSlot stemFx_[4]               per-stem insert (Vocals/Drums/Bass/Inst)
    └── staged per-block, processed before joining the deck mix
```

`FXManager`/`FXRegistry`/`FXChain`/`FXScheduler` concepts from the spec
map onto: `FXFactory` (registry of kinds), `FXRack` (chain + reorder),
`FXSlot` (instance: id/type/enabled/mix/bypass/sync), `FXClock` +
`SyncContext` (shared beat clock), `FXPresetManager` (presets),
`FXRouting` (declarative route table), `FXDiagnostics` (CPU/faults/
latency/peaks). One `FXEngine` owns all of it — there is no per-effect
clock.

## Effect interface (`FXProcessor`)

```cpp
prepare(sampleRate) / reset()
process(l, r, n)               // in-place stereo, RT-only
paramCount() / paramInfo(i)    // name, min, max, default, unit, type
setParamNormalized(i, v)       // smoothed, click-free
latencySamples()               // PDC seam (all current FX report 0)
hasTail()                      // delay/echo/reverb → true
setSyncContext(ctx)            // stamped per block by the slot
```

`FXBase<N>` implements the normalized↔native mapping and per-param
smoothing; concrete effects only fill a static `FXParamInfo` table.
`FXParamType` tags (Float, Int, Bool, Enum, BeatDiv, Freq, Time, Ms,
Db, Percent) let UIs and controllers render/edit correctly without
per-effect knowledge.

### Effects (FXKind)

Filter (LP/BP/HP + resonance), Delay, Echo (incl. Freeze), Reverb
(size/decay/damping/pre-delay/width), Flanger, Phaser, Noise, Gate,
Chorus, Distortion, BitCrusher, Compressor, BeatRepeat, BeatRoll —
registered in `FXFactory`. BeatRepeat/BeatRoll/Gate/Crush are the
performance-FX seams: they live in the FX path (block processing),
deliberately separate from `LoopEngine`/`SlipEngine` timeline logic.

## Routing

| Channel / node            | FXTarget   | Route         |
|---------------------------|------------|---------------|
| Deck A..D (strips 0..3)   | 0..3       | DeckInsert    |
| Send buses A/B            | 4,5        | SendReturn    |
| Master bus                | 6          | MasterInsert  |
| Sampler (channel 4)       | 7 (Spare)  | SamplerInsert |
| Deck stems (slots 4..7)   | deck slots | StemInsert    |

**Insert** — `source → FX chain → strip input`. The mixer maps
channels 0..3 to deck racks and **channel 4 (sampler) to `Spare`**
(`channelFxTarget`) — Phase 25 fixed the bug where the sampler aliased
`SendA`, double-processing that rack.

**Send/return** — each strip taps its post-fader (post-pan) signal into
send bus A/B at a per-channel send gain; the accumulated bus runs
through `rack[SendA+B]`, scales by the smoothed return level, and sums
into the master mix. Cue/PFL never sees the send return.

**Stem inserts** — a deck stem whose slot is enabled/busy renders into
the slot's input stage instead of the inline mix; `applyStemFx` runs
the staged block and sums the slot output (its own dry/wet blend).
This holds in both render paths: varispeed stages per output block,
the stretch path stages source-domain chunks so the WSOLA stretcher
still sees coherent post-FX audio. Stem slots are owned by the deck
but bound into the FX id space (`target=deck, slot=4..7`) at startup
via `FXEngine::bindStemSlots` — same params, presets, MIDI, macros,
diagnostics as any rack slot.

## Wet/dry and bypass

Each slot mixes `dry*(1-mix) + wet*mix` per sample with a 10 ms
smoothed mix. Enable is a 10 ms wet-gate ramp — click-free.

**Tail-aware bypass (§42)** — effects with `hasTail()` (Delay, Echo,
Reverb) do not cut on disable:

- the wet gate stays open and the processor is fed **zeros**;
- the dry gate (`smEnDry_`) ramps back to unity — the bypass is fully
  transparent underneath while the tail rings on top;
- the slot drains when wet peak < −80 dBFS or after an 8 s cap, then
  hibernates (zero CPU).

Effects without tails skip processing entirely once the gate closes
(§45 CPU management). `busy()` covers enabled **or** draining — callers
must keep feeding the slot until it reports idle.

## Beat sync

`FXClock` computes a `SyncContext` — `{bpm, beatPhase, synced,
division}` — per slot per block. `divisionFrames()`/`divisionMs()`
turn `BeatDiv` values (`1/32 … 4 bars`, plus `1/16T/1/8T/1/4T`
triplets) into sample times. Synced effects (Delay/Echo/Flanger/
Phaser/…) read their time/depth targets from the context; tempo
changes re-target the smoothers, so taps slide to the new grid without
steps (§23). No effect owns a clock.

### Tempo source — SyncMode (§22)

| Mode | Source |
|------|--------|
| 0 MANUAL | free params (Time/Rate), `synced=0` |
| 1 DECK   | the slot's own target tempo (`ctx_`) |
| 2 MASTER | the sync-master deck (`masterCtx`) |

The mixer feeds each target's `ctx_` from its source (deck effBpm,
sampler clockBpm); `fxSyncMaster_` selects which deck ctx serves as
MASTER for decks/sends/master/stem slots. Send and master racks always
run on the master ctx.

## Parameter model & registry

FX ids live in the `0x1000` namespace:
`id = 0x1000 | (target<<8) | (slot<<4) | kind`. Slot kinds: Type,
Enabled, Mix, InGain, OutGain, SyncMode (0..2), Division, P1..P8 —
all registered in `mixer::ParameterRegistry` (MIDI/HID-map ready,
`deckA.fx.*` naming via `ParamNames`). Deck slots 4..7 address the
deck's stem slots through the same id space.

**Pending-param preservation** — writes to P1..P8 that arrive while a
pending processor is waiting for adoption are stashed per-index and
replayed onto the new processor in `adoptPending`. Without this,
presets and controller writes raced with the type swap and were lost.

**Automation** — `FXAutomation` stores beat-domain lanes per (target,
slot, param); events ride the same `rt::ParamEvent` sample-scheduled
queue as mixer params. **Macros** — `FXMacro` maps one normalized
control to several params with ranges; exposed as normal registry
params.

## Presets

`FXPreset` — versioned POD (`version`, `effectKind`, `params[]`, name);
binary serialization with a version field and a migration point.
`FXPresetManager` — `store/load/rename/remove/duplicate`, factory +
user banks. Factory presets include the spec set: Clean Filter, Echo
Out, Short Echo, Long Echo, Room Reverb, Hall Reverb, Classic Flanger,
Classic Phaser (Room/Hall are preset data on the same reverb DSP, not
separate implementations).

## Realtime safety

- No allocations, locks, file/db access, or heavy work in `process()`.
- Effect swaps: control thread constructs + prepares → atomic pending
  pointer → adopted at block start; retired processors freed by
  `collectTrash()` on a control thread.
- All stages (`stemFxInL/R`, send buses, dry copies) are preallocated
  at `prepare()`.
- Sanitization per block: NaN/Inf inputs zeroed, output clamped to
  `kClamp`, wet-peak tracked for tail drain; faults/sanitized counters
  feed `FXDiagnostics`. Feedback in Delay/Echo is clamped below unity;
  Reverb runs an output-gain safety bound (§18, §49).
- `latencySamples()` is summed per rack (`targetLatency`,
  `maxLatencySamples`) — every current effect reports 0; the report is
  the seam a future PDC pass consumes.

## UI

`FXView` shows Deck A–D, Send A/B, Master and Sampler racks plus
per-deck stem rows: effect type selector, enable, wet/dry, sync mode,
beat division, P1..P8 knobs, input/output peaks (updated per UI tick,
never per sample), preset load, and slot reorder/remove/duplicate.
Snapshots (`UiState::fxRacks`, `fxSampler`, `fxStems`) are filled by
`EngineBridge` from `FXEngine::fillDiagnostics`.

## DDJ-FLX4 preparation

Every FX parameter is a registry param — hardware binding is a
`ControllerEngine` map, not DSP code. The id layout `(target<<8)|
(slot<<4)|kind` lets one mapping pattern cover decks/sends/master/
sampler/stems. FX button/knob/level and SHIFT combinations are left
unbound until the real HID/MIDI protocol is validated (Phase 26+;
see `docs/adr/0010-ddj-flx4-integration.md`).

## Known limitations

- `getLatency()`/PDC is a reporting seam only — no inter-rack
  compensation is applied (all current DSP is zero-latency).
- Stem FX runs pre-stretch in the keylock path: an FX'd stem is
  processed in source-domain chunks (512 frames), so stem-FX beat sync
  quantizes to chunk boundaries, not device frames — audible phase is
  unaffected but sync edges are coarser by ≤512 source frames.
- Performance FX (BeatRepeat/Roll) exist as DSP but the deck-timeline
  capture protocol (§26–28) is a seam pending the performance-FX phase.
- Cue/PFL path is intentionally insert-free; send returns never enter
  cue (§48).
