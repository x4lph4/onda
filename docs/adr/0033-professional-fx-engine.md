# ADR 0033 — Professional FX engine (parameter curves, automation modes, quality tiers, BeatClock view)

## Status

Accepted — Phase 33.

## Context

Phases 6, 16 and 25 had already built most of the professional FX
spec: `FXProcessor` interface + `FXBase` (normalized↔native params,
per-param smoothing), `FXSlot` (enable/mix/in-out gains, smoothed
bypass, tail-drain, input/output sanitization, fault auto-reset,
pending-param stash, per-slot meters), `FXRack` (4-slot chains with
atomic reorder), `FXEngine` (deck inserts ×4, send A/B buses, master,
per-stem slots), 13 processors (Filter, Delay, Echo, Reverb,
Flanger, Phaser, Gate, Chorus, Distortion, BitCrusher, Compressor,
BeatRepeat, BeatRoll, Noise), `FXClock` + triplet `BeatDiv`,
`SyncContext` (MANUAL/DECK/MASTER tempo source), `FXAutomation`
data model, `FXPresetManager` (save/load/delete/duplicate/rename +
factory list), macros, diagnostics, command surface, FLX4 FX-section
inventory and LED bindings, and `docs/fx/fx-engine.md`.

The Phase-33 audit found the remaining spec gaps:

- **§37** — no parameter curves: normalized→native was hard-wired
  Linear, so every `Hz` param collapsed octave resolution into the
  top of the knob (FilterFX.Cutoff at norm 0.5 was 10 kHz).
- **§40** — automation lanes had no mode tag (Off/Manual/Latch/
  Touch/Write).
- **§55** — no quality/CPU tier mechanism.
- **§33** — the consultable `BeatClock` (beat, phase, bpm,
  subdivision) existed only as static math (`FXClock`), not as a
  view a processor can hold.

## Decision

- `FXParamInfo::curve` + `FXBase::curveToNative`/`curveToNorm`
  implement Linear/Log/Exp/Custom. All 13 `Hz` params are tagged
  `Log` — equal knob travel per octave. Presets store *native*
  values, so persisted data is unaffected; only UI/MIDI knob feel
  changes. `Custom` maps to Linear in `FXBase` — the extension point
  is overriding the normalized setters, documented.
- `FXAutomationMode` tags each lane: `Off` is honored by
  `evaluate()`; `Manual` is the playback default; `Latch`/`Touch`/
  `Write` are capture policies the model already expresses so
  presets/session files can carry intent before the recorder lands.
- `FXQuality` is a **configuration seam**, not a realtime param —
  the 4-bit slot-kind id space is full, and quality is a resource
  choice, not a performance gesture. `FXSlot::setQuality` covers
  live + pending + future adoptions; `FXEngine::setQuality` routes;
  ReverbFX implements `Low` (4 combs + 2 allpasses, tap-count gain
  compensation).
- `BeatClock` is a POD view over the per-block `SyncContext`
  (`BeatClock::from(ctx, rate)`) mirroring `FXClock` math exactly —
  processors consult beat/phase/bpm/division without reaching into
  deck/mixer internals.

## Consequences

- Knob/MIDI travel on frequency params is now musical (two existing
  `fx_tests` assertions were recalibrated: linear→log, same intent).
- Quality tiers are control-thread only by design; a registry id is
  a possible future addition if the packing scheme widens.
- Capture-mode automation is modeled but not recorded — deliberate.
- `[fx33]` adds 7 cases / 917 assertions; all prior `[fx]`/P16/P25
  behavior preserved.

## Validation

- `[fx33]`: 7 cases — curve endpoints/monotonicity/round-trips,
  Log enforcement across every Hz param on all 13 processors,
  FilterFX end-to-end log mapping, automation-mode gating, ReverbFX
  quality tiers (finite + tail survives), slot/engine quality
  passthrough incl. adoption replay, BeatClock≡FXClock equivalence.
- `[fx]` suite: 25 cases / 786 assertions green after two
  intentional recalibrations (log-curve expectations).
- Full suite: all green.
