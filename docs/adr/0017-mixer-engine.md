# ADR 0017 — Professional mixer engine

Phase 17. Status: implemented.

## Context

Phase 5 built a strong RT mixer core (strips, EQ, filter, curves,
cue bus, limiter, meters, send/return FX, output mapping). The Phase-17
spec asks for the full professional surface on top: remappable source
routing, a named cue system with master cue, filter resonance,
configurable EQ range, phase invert, scene/preset persistence,
diagnostics, 2/4-deck modes, an undo seam, and a latency contract.

## Decisions

1. **Extend, don't replace.** Every RT structure from Phase 5 stays
   the signal path. New capabilities are added as (a) packed-atomic
   routing descriptors, (b) new param kinds in free ordinals, (c) a
   new CueManager tap, (d) control-plane containers (scene/preset/
   diagnostics) that never run in the callback.

2. **Sources are descriptors, not call sites.** `ChSource{type,index}`
   packed to uint32 and published per channel. Deck/sampler/external/
   silence decode each block. Any deck on any channel; invalid indices
   degrade to silence rather than crash — remapping can never kill
   audio, just quieten it.

3. **Silenced channels still process.** A `Silence` source memsets the
   strip input and skips only the insert FX rack. The strip keeps
   running on zeros so meters decay correctly and post-fader taps
   (which send buses read) are always clean — the alternative
   (skipping process) leaves stale audio in `postL_/postR_`.

4. **Deck mode = routing, not backend.** `setDeckMode(2)` remaps
   channels 2/3 to `Silence`; strips remain allocated and their state
   survives a mode round-trip. UI visibility follows
   `MasterSnapshot.deckMode` — layout is a read of engine state, never
   a second source of truth.

5. **Master cue taps post-limiter program.** `CueManager` injects the
   master bus output into the cue sum with a ~6 ms smoothed gate. The
   DJ hears what the audience hears (club-mixer semantics), and the
   tap is strictly read-only — verified: program RMS identical with
   master cue on vs off.

6. **Scenes capture commanded values.** `MixerScene` round-trips the
   `MixerController` value store + source mirror, not smoothed RT
   state — a scene saved mid-fade restores the target, not an
   in-flight sample. `sceneField()` is a single mapping table so
   capture and apply cannot drift.

7. **Undo seam = coalesced deltas.** `setParamEdit` groups a burst
   into one `{id, from, to}` transaction. No UndoManager is built —
   the seam delivers exactly what one needs to record.

8. **Latency is measured, not faked.** `latencySamples(ch)`/
   `masterLatencySamples()` report rack-reported latency (0 for every
   current processor). PDC consumes these accessors when a
   latency-bearing processor exists — spec §42: measure first.

9. **Enum compatibility preserved.** `ChParam::FilterRes` and
   `BusParam::MasterCue` occupy previously free ordinals; persisted
   param ids are unchanged.

## Consequences

- 17 new tests (10.3k assertions): remap/isolation/graph tags, deck
  modes, master-cue independence + smoothing, filter Q emphasis, EQ
  range clamps, phase-invert anti-symmetry, registry bounds, scene
  round-trip incl. sources, factory presets, diagnostics contents,
  transaction coalescing, the full command surface, and a 4-deck
  + cue + sends + filters stress pass.
- Full suite: 323 cases / 1,638,185 assertions green.
- The audio callback remains allocation/lock/IO-free; only memsets
  and atomic loads were added to the render path.
