# ADR-0005: Professional Mixer Engine

## Status

Accepted — implemented in Phase 5.

## Context

Phase 2 shipped a functional but monolithic `MixerEngine`: per-deck
buffers, faders, crossfader, PFL cue bus, master gain, peak limiter and
meters lived in one class, and the *channel* processing (trim, 3-band
EQ, DJ filter) was embedded inside `DeckEngine`. That layout works for
two decks but is wrong structurally: a mixer channel is not a deck
property, and future sources (sampler, external inputs, FX returns,
microphone, stems, DVS) must flow through the same strip without being
decks.

Phase 5 rebuilds the mixer as a modular, channel-oriented engine while
keeping the RT contract and every observable behavior of the old one.

## Architecture

```
sources (decks today; sampler / external-in / FX-return / mic later)
  → ChannelStrip[i].inL/inR            (engine renders straight into the strip)
      trim (GainSmoother 12 ms)
      → EqProcessor                    (3-band today, kMaxBands = 4)
      → FilterProcessor                (position knob or typed LP/HP)
      → [pre-fader meter + PFL cue tap → HeadphoneBus]
      → ChannelFader (curve: Linear / AudioTaper / Custom) × mute × solo
        × instant-cut × crossfader-assignment gain
      → [post meter] → summed into MasterBus
  MasterBus:  smoothed gain/mute → LimiterProcessor → MeteringEngine → out[0..1]
  HeadphoneBus: (cueSum·(1−mix) + masterPost·mix) · cueVol → out[2..3]
```

Components (`src/core/mixer/`):

- `MixerTypes.h` — `ChParam`/`BusParam` parameter kinds, `XfAssign`
  (A/B/Thru), `FaderCurve`, `FilterType`, `EqMode`.
- `GainSmoother` — per-sample one-pole, millisecond time constant.
- `EqProcessor` — up to 4 cascaded stereo biquads; per-band gain
  (dB, stepped ≤3 dB/block), frequency, Q, bypass; `EqMode::Standard`
  (+ `Isolator` slot) so voicings can change without touching the strip.
- `FilterProcessor` — classic single-knob position mode
  (−1 = LP sweep 20k→200 Hz, +1 = HP sweep 20 Hz→8 kHz, 0 = bypass)
  or explicit `FilterType::LowPass/HighPass` with `setCutoffHz`;
  smoothed per-sample → no zipper noise.
- `ChannelStrip` — owns the whole channel path + pre-fader meters;
  source-agnostic (decks render into its input; anything else can too).
- `CrossfaderEngine` — smoothed position, `XfCurve`
  (Linear / EqualPower / Cut / Custom std::function), per-channel
  A/B/Thru assignment.
- `LimiterProcessor` — zero-lookahead peak limiter facade (threshold,
  enable, GR meter). Deliberately minimal until the FX phase.
- `MasterBus`, `HeadphoneBus` — independent buses with own meters.
- `MeteringEngine` / `ChannelMeters` — peak, RMS, peak-hold with decay
  (~6 dB/s), clip flag/counter, limiter GR — published through atomics.
- `RoutingEngine` — node/edge graph describing the topology
  (source→strip→{xf→master, pre-fader tap→cue}→outputs). Introspection
  + extension point for FX sends/returns, extra sources, recording tap.
- `ParameterRegistry` — static metadata table: every parameter has
  ID / default / min / max / step / smoothing hint. Channel params are
  encoded `(ch << 5) | kind` (8 channels × 32 kinds), bus params live at
  `0x400+`. `find()`/`sanitize()` are constexpr-friendly, zero state.
- `MixerController` — non-RT facade used by UI/MIDI/HID/automation:
  validates through the registry, stores last-set values for read-back,
  and forwards to a sink that enqueues `rt::SetParam`.

## Signal path (exact order)

Per audio block, inside `MixerEngine::render`:

1. `decks[i].renderPcm(strips[i].inL(), strips[i].inR(), n)` — the deck
   now outputs *source-level* audio (trackGain × play/pause mute
   smoothing). EQ/filter/fader no longer live in the deck.
2. `xf_.tickPos(n)` — crossfader position advanced n samples.
3. For each active channel, `ChannelStrip::process`:
   trim → EQ → filter → pre-fader meters → PFL cue tap →
   fader curve × mute × instant-cut × solo logic × xf-assignment gain →
   accumulate into master sum.
4. `master_.process(mix)` — gain/mute → limiter → meters → out[0..1].
5. `phones_.process(cue, masterPost)` → out[2..3] (never writes back
   into the master sum).
6. Legacy `MasterStats` fields are republished from bus meters so the
   UI and diagnostics stay unchanged.

## Parameter flow

```
UI / MIDI / HID / automation / tests
  → MixerController::setParam(id, v)
      ParameterRegistry::sanitize (clamp + step quantize)
      → rt::SetParam{id,v} → CommandQueue
        → AudioEngine::applyCommand → MixerEngine::setParam
          → ChannelStrip / bus setter (RT thread, smoothed)
```

Legacy commands (`SetFader`, `SetEqBand`, `SetFilter`, `SetPfl`,
`SetCrossfader`, `SetMasterGain`, `SetCueVol`, `SetCueMix`,
`SetDeckGain`→trim) are mapped to the same strip/bus setters, so the
existing UI and every old test keep working unchanged.

## Thread safety

- All channel/buffer storage is preallocated in `prepare()`
  (`kMaxBlockFrames`); `render()` performs zero allocations.
- Parameter changes reach the RT thread only through the lock-free
  `rt::CommandQueue` (POD `SetParam` or legacy variants).
- Meters are single-writer atomics; UI may read freely, never writes.
- `ParameterRegistry` is a pure static metadata table — no mutable
  shared state, no lock.
- The RT callback still does no I/O, no DB, no logging, no blocking.

## Extensibility

- `kMaxChannels = 8` (registry-encoded): 4 decks + sampler + external
  input + FX return + mic fit without a structural change. A source
  only needs to write `strip.inL/inR` before `process()`.
- `setActiveChannels(n)` + `RoutingEngine` describe the topology;
  adding nodes/edges for FX sends, recording or DVS does not touch
  the strip/master code.
- `EqMode`, `FilterType`, `FaderCurve::Custom`, `XfCurve::Custom`
  leave room for isolator EQs, alternate filters and user curves.
- DeckEngine is now a pure source engine — correct layering for stems
  and multi-source channels later.

## Decisions

- **Channel DSP moved from DeckEngine to ChannelStrip.** This is the
  honest mixer architecture; it also fixed the layering for non-deck
  sources. `deck.levelL/R` stats are still published (deck writes
  source level, strip overwrites channel pre-fader level when bound).
- **Cue tap is pre-fader, post-filter** — same point as Phase 2, so
  headphone cue hears the channel's EQ/filter (standard PFL behavior).
- **Crossfader smoothing is block-resolved**: position advances n
  samples then each strip applies one gain for the block (5 ms
  smoother ⇒ sub-millisecond residual error, zero zipper noise).
- **Solo model**: any-solo mutes non-solo channels in the master sum
  only; cue/PFL taps are unaffected.
- **Limiter stays zero-lookahead** (~50 ms release, −1 dBFS threshold)
  — adequate safety net; a lookahead design is deferred to the FX phase.
- **Clip detection is post-limiter**, so a hot mix is reported even
  while the limiter catches the peaks — no hiding gain-staging errors.

## Known limitations

- Only deck sources are wired today; sampler/external/FX-return slots
  exist in the architecture but have no producer yet.
- Channel strips above `numDecks` are skipped in render (inputs stay
  silent) until a source type registers for them.
- No dedicated recording bus yet (graph node reserved).
- `Isolator` EQ mode shares the standard topology placeholder; steeper
  slopes arrive with the FX phase.
- Meters are per-block peak/RMS; LUFS/EBU metering is future work.

## Verification

- 21 new `[mixer]` cases: gain trim (min/max/smooth/no-click), EQ
  (low/mid/high cut+boost, bypass, ≤3 dB/block transitions), filter
  (LP/HP/cutoff/no discontinuity), fader (endpoints, AudioTaper, Custom,
  mute, instant cut), crossfader (A/B/Thru, Cut curve), cue tap
  isolation, solo logic, meters (peak/RMS/hold), end-to-end routing,
  setParam routing, ParameterRegistry sanitize/metadata,
  MixerController sink, stereo separation, impulse/noise/silence
  integrity, 4-channel operation, 500-block RT run (finite output,
  solo/cue toggles live), block-cost bound.
- Full suite: **91 test cases, 1,591,473 assertions — all green**,
  including every pre-existing audio/performance/analysis regression.
