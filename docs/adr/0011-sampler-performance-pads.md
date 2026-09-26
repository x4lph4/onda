# ADR-0011 — Professional Sampler + Performance Pads

## Context

Phase 11 adds a performance sampler: pads must trigger samples with
the same reliability as deck hot-cues, while the UI, keyboard, MIDI
and HID all share one command path. Hard constraints from earlier
phases still apply: the audio thread never locks, never allocates,
never blocks; hardware stays optional and semantics-free.

## Decision

**Two engines, one handoff — plus a single domain pad layer.**

```
SamplerEngine (app thread façade)
  ├── SampleBank[]   8-slot banks (deque — scalable to 16/32+)
  ├── SampleLoader   1 worker thread, FIFO, decodes via IDecoder
  ├── SampleCache    LRU + byte cap + pin (preload)
  ├── SampleRecorder SPSC ring → WAV (no source armed by default)
  └── SampleRepository  sqlite: samples / sample_banks / sample_slots
        │
        │  rt::CommandQueue (same queue as decks/mixer)
        ▼
SamplerRt (audio thread)
  ├── 32-slot published SampleData (atomic mailbox + retire list)
  ├── 32-voice fixed pool (steal: oldest/quietest/same-sample/deny)
  ├── quantize delay (frames to next grid boundary)
  └── renderPcm → mixer channel 4 strip (gain/EQ/filter/fader/
      cue/FX-send for free)

ui::pads::PadLayer (domain, controller-free)
  └── PadModeManager → PadAction → CommandManager / sampler ports
```

Key choices:

- **One semantic source of truth.** Controllers emit
  `pad.<d>.<n>` + `pad.<d>.mode` *commands*; `PadModeManager` resolves
  them into deck commands or sampler actions. The FLX4 plugin carries
  zero pad semantics — a different controller gets identical behavior
  by emitting the same commands.
- **Data vs commands.** Decoded `SampleData` (shared_ptr, immutable)
  crosses loader→RT through a per-slot atomic mailbox; *commands*
  cross app→RT through `rt::CommandQueue`. Retired boxes are freed
  by the app-side `tick()`, never on RT.
- **Sampler is a mixer channel** (`kSamplerChannel`=4), not a special
  bus — every existing strip feature works on it without new code.
- **Persistence shares the library DB** — a sample row can back many
  slots across banks; slots store all per-slot params + color/mode/
  quantize/sync.

## Consequences

- HOLD/GATE semantics live in RT (`SampleRelease` fades held voices,
  GATE toggles on second press) — releases must not map to hard stop.
- `publishSlot` is lock-free single-producer (loader only); the RT
  side owns the pointer via `SlotBox` — eviction can never pull audio
  from under a playing voice.
- Quantize needs the master clock: `AudioEngine` feeds bpm+beatPos of
  the sync master each block; no master → quantize falls back to
  immediate trigger.
- Voice steal is env-faded (no clicks); `maxVoices` and `StealPolicy`
  are RT members, configurable via `SamplerParam`.

## Verification

198 test cases / 1.59M assertions green; `[sampler]` suite covers
modes, voice pool, quantize, sync speed, mute/solo/pan/velocity,
cache LRU/pin, sqlite roundtrip, real-WAV decode, pad resolution,
FLX4 pad→sampler routing (virtual), recorder ring, and a 1600-
trigger stress run — all output finite, voices bounded.
