# ADR 0027 — Controller engine completion (lifecycle, control registry, layers, SysEx, generic jog/pad)

## Status

Accepted — Phase 27.

## Context

Phase 9/10 built the controller core (`ControllerEngine`,
`DeviceManager`, `MappingEngine`, `FeedbackEngine`, `LearnMode`,
`ProfileManager`, FLX4 plugin + profile, virtual test doubles).
Audit against the professional-controller spec found the remaining
gaps: no explicit device lifecycle beyond a bool, no canonical
logical-id namespace for controls, no mapping layers/priority/
long-press, no SysEx path, no generic jog/pad surfaces (only the
FLX4 decoder), no LED blink patterns, no factory/user profile split,
and no UNVERIFIED marking for community-sourced protocol entries.

## Decisions

1. **Lifecycle is explicit, not derived.** `DeviceLifecycle`
   (`Disconnected…Disconnecting`) lives on `DeviceState` and is driven
   by DeviceManager events + `applyProfile` outcome. `applyProfile`
   owns the transition (`Ready` on success, `Error` + `lastError` on
   validation/resolution failure) so plugin-applied profiles and
   auto-match behave identically. Musical state is never stored in
   the device layer — disconnect only flips `connected`.
2. **Logical ids are the public namespace.** `ControlRegistry`
   generates `"decka.play"`-style ids → resolved `Target`s for 4
   decks + mixer + browser + app. Profiles may address `target.control`
   and are rejected early on unknown ids. The registry stores the
   *real* CommandManager names as `cmdName` — required because
   `cmd == 0` is a valid id (`forDeck(DeckPlayToggle, 0)`), not an
   "unset" marker.
3. **One active layer per device; Default is a fallback.** A mapping
   fires on its own layer; Default-layer entries are shadowed only
   when an active-layer binding owns the same input. This models
   SHIFT/ALT/CUSTOM without assuming the hardware exposes them —
   FLX4's firmware-split pad channels never need layers at all.
4. **Long-press rides the UI tick.** `MappingEngine::poll(nowSec)`
   checks Momentary mappings stamped on the press edge; past
   `longMs` it emits `longCmd` once and the release ends `longCmd`
   too. Zero per-message cost when unused.
5. **SysEx stays out of `MidiMsg`.** The channel-message POD stays
   8 bytes; `SysExMsg` is a bounded 64-byte struct on a parallel path
   (`processSysex` → `onSysex` hook + monitor ring). It can never
   reach MappingEngine — SysEx has no address.
6. **Generic jog/pad engines own policy, plugins own decode.**
   `JogWheelEngine` applies mode (Vinyl/CDJ/Search/Browse),
   sensitivity, touch gating and the platter-vs-edge distinction;
   `JogWheelProcessor` (FLX4) only translates bytes into turns/touches.
   `PadEngine` tracks surface state + emits `forPad` — the domain
   `PadLayer` stays the single source of pad semantics for every
   controller.
7. **Blink is a render pattern, not a state source.** `LedPattern`
   alternates the output byte inside `tick()` from wall-clock phase —
   source state stays binary; rates are exact at any poll cadence.
8. **Factory profiles never touch disk.** `installFactory` registers
   in memory only; delete/rename refuse them; `duplicateProfile` is
   the only way to customize one. `autoMatch` prefers user profiles —
   a user remap shadows the shipped mapping without modifying it.
9. **Unverified bytes are flagged, not hidden.** `Mapping::verified =
   false` marks community-only FLX4 entries ([MIX]/[SLP] sources);
   `findIssues`/`unverifiedCount` report them. They still work —
   the flag documents what needs hardware confirmation (§9).

## Consequences

- `DeviceInfo` gained `model` — old aggregate initializers needed
  updating (done member-wise where practical).
- Profile JSON v1 stays compatible: new fields (`layer`, `priority`,
  `longMs`, `longCmd`, `verified`, `control`, `protocolRev`, `origin`)
  are emitted only when non-default.
- `Target` gained `control`/`cmdName` resolution — numeric-id
  profiles are unaffected.
- No new RT risk: all additions are UI-thread-side; the flood test
  measures ~400k msg/s with zero drops.

## Validation

- 19 new `[ctrl27]` cases: lifecycle transitions + error state,
  control registry resolution + skeleton + applyProfile, layer
  shadowing, long-press, priority ordering, `findIssues`, SysEx path,
  blink patterns, JogWheelEngine modes/sensitivity/browse guard,
  PadEngine state/range-safety, factory/user split + shadowing,
  JSON roundtrip, FLX4 UNVERIFIED marks, VirtualFlx4 end-to-end
  (READY + pads + jog), monitor decode, multi-controller, 40k-msg
  flood, `ControllerState` coverage.
- `[ctrl]`/`[flx4]` regression: 30 cases green; full suite green;
  app smoke stable.
