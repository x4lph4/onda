# ADR 0032 — Professional mixer engine (AudioBus contract, crossfader hamster/reverse, registry-driven UI ranges, detent marks, latency documentation)

## Status

Accepted — Phase 32.

## Context

Phases 5, 17 and 24 had already built most of the professional
mixer spec: eight channels (four decks + sampler + spare), strip
DSP (trim, 3-band EQ with isolator mode, bipolar filter, fader,
pan, sends, cue/mute/solo, xf assign), crossfader with four curves,
master/booth/headphone/recording buses, limiter, metering, the
`ParameterRegistry`/`MixerController` sanitized command path,
`ParamEvent` sample-scheduled updates, scenes v2, presets,
diagnostics, and DDJ-FLX4 mappings through `ControlRegistry`.

The Phase-32 audit against the remaining spec found these real gaps:

- **§26** — no crossfader reverse/hamster mode.
- **§5** — the bus contract between sources and strips was
  implicit (raw `inL()/inR()` pointers); no formal `AudioBus` type.
- **§9/§13** — `MixerView` hardcoded knob/fader ranges that could
  drift from `ParameterRegistry` (e.g. CUE VOL was clamped to 0..1
  while the registry allows 0..2).
- **§44** — no `MixerState` named container (the scene struct
  already carried the state).
- **§48–§49** — knobs had no visual neutral/detent mark; the filter
  knob did not announce HPF/LPF direction.
- **§58–§59** — parameter-update and cue latency were architectural
  but undocumented and untested.

## Decision

- Reverse is a **flag on `CrossfaderEngine`**, not a fifth curve:
  `gainFor(assign)` swaps the `sideA` flag before evaluating, so it
  composes with Linear/EqualPower/Cut/Custom and leaves `Thru`
  untouched. It travels as `BusParam::XfReverse` (0/1, quantized)
  through the same registry → `SetParam` → RT path as every other
  parameter, and persists in `MixerScene` v2 (`xfReverse`),
  `MixerBusDiag::xfReverse`, `MasterSnapshot::xfReverse`, the
  `mixer.xfReverseToggle` command and the REV button.
- `mixer::AudioBus` is a **non-owning POD view** `{L, R, frames}`
  obtained from `ChannelStrip::inBus(frames)`; buffer ownership stays
  in the strip, so the callback remains allocation-free and the bus
  adds zero copies.
- UI ranges come from `ParameterRegistry::find(id)` via
  `rngLo`/`rngHi` in `MixerView` — one source of truth for UI,
  controller and RT sanitization.
- `using MixerState = MixerScene;` — the spec name aliases the
  existing capture/apply container rather than duplicating state.
- Detent marks are a `Knob::setCenterMark` paint tick (EQ/filter/pan
  at 0, trim at unity); the filter caption announces `HPF`/`LPF`
  around center. Behavior (drag, wheel, keyboard, double-click
  reset) is unchanged.
- Latency is **documented analytically** (≤1 block queue drain +
  per-param smoother τ; scheduled `ParamEvent` applies on the exact
  `targetFrame`) and covered by deterministic tests — queue
  losslessness, bounded monotonic convergence — rather than
  wall-clock benchmarks.

## Consequences

- Hamster DJs get real A↔B swap with one command/UI toggle; scene
  recall, diagnostics and snapshots observe it.
- New sources (external inputs, FX returns) implement the §5 bus
  contract without touching `ChannelStrip`.
- Registry range changes propagate to UI, MIDI learn and RT
  sanitization together — no drift.
- `[mix32]` adds 10 focused cases (~64 k assertions) for the deltas;
  no existing `[mixer]`/P17/P24 behavior changed.
- Follow-ups kept honest: external/mic sources remain silent seams;
  insert/master algorithmic latency stays 0 until an FX introduces
  delay (accessors already exist).

## Validation

- `[mix32]`: 10 cases / 63,942 assertions — reverse on all curves +
  custom + end-to-end, registry/quantize/scene/diag coverage,
  AudioBus contract, registry ranges, SPSC queue, bounded monotonic
  smoother convergence, reverse-flip stress, `MixerState` alias.
- `[mixer]`: 21 cases / 512,134 assertions — unchanged.
- P17+P24 suites: 33 cases / 12,070 assertions — unchanged.
- Full suite: all green.
