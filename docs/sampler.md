# Sampler + Performance Pads (Phase 11)

> **Phase 34 update** — canonical doc moved to
> [`sampler/sampler-engine.md`](sampler/sampler-engine.md)
> (ADR `0034`). Deltas: `Retrigger` playback mode, quantize
> `1/2`/`2Bar`/`4Bar` + quantized stop, per-slot attack/release
> envelope, working per-slot tilt filter (`SP_Filter`),
> non-destructive trim/loop window (`SP_TrimStart/End`,
> `SP_LoopIn/Out`), `slot.missing` + `checkMissing`/`relinkSample`,
> gesture-scoped undo/redo, and seven new persisted slot columns
> (guarded `ALTER TABLE` migration).

Signal path:

```
Controller / UI / keyboard
  → pad.<deck>.<n>[.shift] + pad.<deck>.mode   (CommandManager)
  → ui::pads::PadLayer → PadModeManager        (domain resolve)
  → deck command | sampleTrig/Release/Stop      (one resolver, all hw)
  → SamplerEngine.trigger/release/stop          (app thread)
  → rt::CommandQueue { SampleTrigger, SampleRelease, SampleStop,
                       SampleSlotParam, SamplerParam }
  → SamplerRt                                   (audio thread)
  → mixer channel 4 strip → master/cue outs
```

## Components (`src/core/sampler/`)

| file | role |
|---|---|
| `SamplerTypes.h` | `PlaybackMode` (OneShot/Loop/Hold/Gate), `QuantizeMode` (Off, 1/4..1/32, Bar), `StealPolicy`, `SlotState`, `SP_*` slot params, `SampleData` (planar stereo @ host rate), `SampleSlot` |
| `SamplerRt.h` | RT engine: 32 slots, 32 voices, atomic mailbox `publishSlot`, `applyCommand`, `renderPcm` (sums into strip), `setClock` |
| `SamplerEngine.{h,cpp}` | app-thread façade: banks, async load, params, persistence; mirrors everything to RT through `rt::CommandQueue` |
| `SampleLoader.h` | one worker thread; decodes via `IDecoder` (FFmpeg); analysis per load: frames/rate/channels/peak/coarse waveform; BPM = caller hint |
| `SampleCache.h` | LRU + byte cap (256 MB default) + `pin()` preload |
| `SampleBank.h` | dynamic slot container (8 today, deque for 16/32+) |
| `SampleRepository.h` | sqlite tables `samples`, `sample_banks`, `sample_slots` on the shared `onda.db` |
| `SampleVoice.h` | POD voice state + anti-click env (fade-in/out) |
| `SampleSync.h` | `speedFor`, `framesToBoundary` — shared by RT + domain |
| `SampleGain.h` | optional normalization gain (metadata only — file untouched) |
| `SamplePitchProcessor.h` | replaceable pitch strategy (default: rate scaling `2^(semi/12)`; no time-stretch yet) |
| `SampleRecorder.h` | SPSC ring → float WAV; `Source::None` default, `arm()` explicit; drops oldest on overrun |

## Pads (`src/ui/pads/`)

- `PadTypes.h` — `PadMode` (HotCue, Sampler, BeatJump, PadFx, Loop +
  reserved Stems/Roll/Slicer/Keyboard/KeyShift), `PadAction`,
  `PerformancePad` (velocity/pressure optional → capability flags),
  `beatJumpBeats` (pads 1-4 back, 5-8 fwd; 1/2/4/8 beats).
- `PadModeManager.h` — the *single* resolution table:
  - HotCue: press → `forHotCue(d,pad,trig)`, shift → `del`
  - Sampler: press → trig(pad,vel), release → release, shift → stop
  - BeatJump: pads 0-3 back, 4-7 fwd (beats as arg)
  - Loop: halve / double / in / out / toggle
  - PadFx + reserved modes: consumed, labeled, no-op until their
    surface exists
- `PadLayer.h` — registers `pad.<a|b>.<0-7>[.shift]` +
  `pad.<a|b>.mode` into `CommandManager`; `Ports` route sample
  actions to `SamplerEngine`.

## Commands (`src/ui/CommandIds.h`)

- `PadPressBase 0x5000` + deck·0x20 + pad + shift·0x10
- `PadModeSetBase 0x5080` + deck (value = `PadMode`)
- `SamplerTrigBase 0x6000` + slot, `SamplerStopBase 0x6040` + slot,
  `SamplerStopAll 0x6080`, `SamplerQuantize 0x6081`,
  `SamplerSyncTgl 0x6082`, `SamplerBankNext/Prev/Set 0x6083-5`

## RT guarantees

- `renderPcm`/`applyCommand`: no alloc, no locks, bounded loops.
- Loader → RT handoff: `publishSlot` atomic exchange; old `SlotBox`
  retires to a lock-free stack reclaimed by `tick()` on the app
  thread — audio never freed under a playing voice.
- Quantize: `AudioEngine` feeds master-deck `bpm`+`beatPos` per
  block; triggers compute frames-to-next-boundary; no clock →
  immediate fire.
- Voice stealing is envelope-faded — no clicks. `maxVoices` /
  `StealPolicy` configurable (`SamplerParam{1, n}`).

## Wiring

- `Main.cpp`: `SampleRepository` on shared `onda.db` → `SamplerEngine`
  (FFmpegDecoder factory) → `engine.attachSampler(&sampler.rt())`
  (enables mixer channel 5) → `loadAll()`. Shutdown: `stopAll()` →
  `saveAll()` → engine stops → detach.
- `EngineBridge`: `ports_.sampler` + owns `pads::PadLayer` (registers
  pad commands, wires `sampleTrig/Release/Stop` → engine façade).
- `MainComponent`: `sampler_->tick()` at ~20 Hz (loader drain +
  trash retire).
- `Flx4Plugin`: pad notes → `pad.<d>.<n>` commands + selectors →
  `pad.<d>.mode` — zero sampler knowledge in hardware code.

## Verified vs pending

Verified by tests: all 4 playback modes, retrigger-no-pileup, 32-
voice cap + 3 steal policies + deny, stop-all, mute/solo/volume/
pan/velocity, quantized delay to grid, sync speed 2x, fuzz safety,
LRU/pin cache, sqlite roundtrip, real-WAV async decode (FFmpeg),
pad resolution per mode incl. domain dispatch, FLX4 virtual pad →
sampler trig/stop, recorder ring→WAV, 1600-trigger stress (finite,
bounded).

Pending / documented limits:

- Time-stretch for samples: not implemented — `ISamplePitchProcessor`
  seam exists (rate-scaling default). `syncOn` changes speed only.
- `PadFx` mode reserves the surface; FX-preset bank not built yet.
- `SampleRecorder`: sources exist but none wired to engine taps —
  arming requires explicit host routing (by design).
- Folder drag&drop, per-pad LED color feedback states, slot reorder:
  follow-up UI work.
- HARDWARE TEST PENDING — physical DDJ-FLX4 pad behavior verified on
  `VirtualFlx4` only.
