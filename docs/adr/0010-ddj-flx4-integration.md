# ADR-0010 — DDJ-FLX4 integration

## Context

Phase 9 delivered a universal controller engine: declarative JSON
profiles, MappingEngine, FeedbackEngine, LearnMode, hot-plug. Phase 10
makes the Pioneer DDJ-FLX4 a first-class device on top of it — with
the hard constraint that no proprietary/invented protocol may be used.

## Decision

**Research-first, then split static vs semantic behavior.**

Three public sources were cross-checked (official AlphaTheta MIDI
list, Mixxx open-source mapping, slipmate measurements). Only values
present in ≥1 documented source are used; disagreements resolved in
favor of official + Mixxx agreement.

The FLX4 surface splits cleanly:

- **Static** (buttons, EQ, faders, tempo, FX, 14-bit CCs) → generated
  declarative `ControllerProfile` (`Flx4Profile::buildProfile`),
  dispatched by MappingEngine like any other device.
- **Semantic** (pad-mode banks, shift held-state, jog touch/vinyl
  routing, browse rotary direction, tempo-range cycling, mode LED
  repaint) → `Flx4Plugin`, a device plugin on a new
  `ControllerEngine::preDispatch` hook that sees each message *after*
  LearnMode, *before* static mappings, and consumes only what it owns.
  Everything else falls through to the static table.

`onDevice` lifecycle hook lets the plugin bind on connect (name-match
`ddj-flx4`, case-insensitive), install+apply the profile on first
connect, and repaint mode LEDs on reconnect.

## Consequences

- One extension point (`preDispatch`/`onDevice`) serves any future
  modeful controller — no per-device branching inside the engine.
- `hiRes` mapping flag + MSB/LSB cache in MappingRuntime is generic —
  reusable by any 14-bit controller.
- `DECK_<n>_PITCH`/`PITCH_RANGE` in a dedicated 0x8000 param space
  keeps mixer and deck parameter namespaces disjoint; MainComponent
  routes them to `rt::SetPitch`/`SetPitchRange` (lock-free queue).
- Pad banks that need the Phase-11 sampler are consumed silently and
  documented as reserved — no fake behavior.
- Mic input cannot be captured (hardware never sends it over USB) —
  recorded as a limitation, not worked around.

## Alternatives rejected

- **All-static profile**: cannot express "pad N does X in mode M,
  shift+pad does Y" without a per-address mapping explosion and
  stateful mode tracking — rejected.
- **Per-device subclassing of ControllerEngine**: breaks the
  façade/single-flow rule — rejected.
- **Emitting on MSB in 14-bit pairs**: wastes a dispatch on a stale
  LSB — current impl merges whichever half arrived and is kept.

## Limitations

- Real hardware was not available: all byte values are documented,
  not measured on-device. HARDWARE TEST PENDING (checklist in
  `docs/controllers/ddj-flx4.md`).
- Scratch uses seek-based `rt::DeckSeek` scaling (0.0018 s/tick
  touched, 0.04 seek, 0.12 search) — a timecode-grade platter model is
  future work.
- Firmware version is not queryable through documented MIDI.
