# ADR 0024 — Mixer engine completion (pan, isolator, booth/recording buses, scheduled params)

## Status

Accepted — Phase 24.

## Context

The Phase-24 spec asked for the central mixer engine. Phase 17
already shipped most of it (strips, EQ, filter, faders, crossfader,
master/cue buses, scenes, diagnostics). Audit showed five real gaps:
no pan stage, the "isolator" was only a re-voiced shelf EQ, no booth
output bus, the recording tap lived outside the mixer abstraction,
and no sample-scheduled parameter events.

## Decisions

1. **Pan is a balance law, not constant-power.** The strip carries a
   stereo program — pan = 0 must not change the mix. Linear balance
   keeps centre at unity gain and reaches true silence on the far
   side. Placed post-fader so meters and PFL stay honest.
2. **Isolator is a 3-way filter bank** (`LP4 / HP4→LP4 / HP4`), not a
   shelf EQ. RBJ shelves at −72 dB leak ~−4.6 dB across bands — a
   shelf cannot isolate. Band gains are linear scalars smoothed
   per-sample; crossover coefficients are static (no coefficient
   steps). Exact band-kill ≈ −72 dB.
3. **Booth taps pre-master-gain.** Independence from house level and
   master mute is the point of a booth output. One extra program
   copy per block into preallocated scratch.
4. **RecordingBus is a named seam, not new processing.** The existing
   `RecorderRt` SPSC path is unchanged; the bus owns the attachment
   point (atomic pointer) so `MixerEngine` is the single owner of the
   post-master program fan-out (outs / recorder / master-cue).
5. **`rt::ParamEvent` rides the existing queue; render splits at
   boundaries.** A 32-deep pending set on the RT thread + an absolute
   audio-frame counter gives sample-accurate application without new
   threads or locks. Overflow applies immediately rather than
   dropping edits.
6. **Scene v2, not a new format.** Added `pan/eqMode/filterType/
   boothGain` fields with v1-compatible defaults; capture/apply stay
   table-driven.
7. **Spec preset names alongside Phase-17 names** — Default/Classic/
   Modern are factory entries; Custom = user-stored scene; Flat/
   Scratch/etc. kept for compatibility.

## Consequences

- `MixerView` gained pan + booth controls; `ChannelSnapshot`/
  `MasterSnapshot` carry the new fields.
- `AudioEngine::attachRecorder` now publishes into the mixer; the
  manual post-render push was removed (single tap owner).
- Isolator unity-gain reconstruction has small crossover ripple —
  documented, intentional (hardware isolators share the character).
- Everything else — queue bounds, smoothing cadence, atomic meters —
  follows the established Phase-17 contract.
