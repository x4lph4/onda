# ADR 0016 — Professional FX Engine

Status: accepted (Phase 16).

## Context

Phase 6 shipped a substantial FX foundation: `FXProcessor` contract,
`FXBase<N>` with normalized/native params + smoothing, `FXSlot` with
dry/wet + bypass crossfade + sanitize/auto-reset, `FXRack` serial
chains, `FXEngine` with deck inserts, send/returns and a master rack,
lock-free non-RT effect swaps, and `FXPresetManager`. Phase 16's
brief required six additional processors, a canonical FX clock with
triplet divisions, declarative routing, reorderable chains, macros,
an automation seam, deeper diagnostics and a hardware-ready command
surface — while keeping the audio callback free of locks,
allocation and I/O.

## Decisions

### Extend the foundation, don't rebuild it
The Phase-6 architecture already satisfied most of the spec
(processor interface, RT safety, parameter smoothing, rack
topology). Phase 16 adds capability on top: new `FXKind` values are
appended to the enum so persisted kinds stay valid, and all new
processors derive from `FXBase<N>` — inheriting parameter smoothing,
clamping, serialization and sync-context handling for free.

### `FXClock` is the only place divisions become time (§23)
`SyncContext::divisionFrames()/divisionMs()` mirror `FXClock` math
exactly; the header can't be included inside `FXTypes.h` (FXClock
depends on it), so the comment documents the mirroring and tests
assert equivalence for every division at 120/128/140 BPM. Triplets
`1/16T, 1/8T, 1/4T` are appended to `BeatDiv` — persisted division
indices unchanged.

### Chain reorder via packed order indirection (§7)
Rather than moving FXSlot objects or swapping processors, `FXRack`
holds a `uint32` order table (four 2-bit physical indices, one per
chain position). `moveSlot()` rewrites it atomically on the control
thread; the RT thread reads the packed order once per block. Zero
allocation, zero locks, zero re-prepare. All external addressing
(slot(), events, commands, macros, diagnostics) is chain-position
based — a reorder is invisible to every consumer.

### Routing is declarative, not hardcoded (§9)
`FXRouting`/`FXRoute`/`routingFor()` describe the topology
(DeckInsert per deck, SendReturn post-fader, MasterInsert,
SamplerInsert future). Effects know nothing about where they sit;
the mixer topology is built to match the table and diagnostics /
presets read the descriptor.

### BeatRepeat latches, BeatRoll rides (§21-22)
Two distinct capture-based effects on shared vocabulary:
- **BeatRepeat** freezes: on engage it copies the last
  `division×SizeMult` frames into a preallocated slice buffer and
  loops it (with optional Pitch and Backstep offsets). Disengaged =
  passthrough while the capture ring keeps recording.
- **BeatRoll** moves: capture never stops; each slice boundary the
  read window re-locks onto the freshest region — a rolling repeat
  that follows the groove. Gate adds stutter texture.
Both fade slice edges (~2 ms) and resolve sizes exclusively through
the sync context.

### Compressor is stereo-linked by detector, not by accident
The detector reads `max(|l|,|r|)` so one gain value applies to both
channels — the stereo image cannot wander. Soft-knee gain computer
operates in the dB domain with per-sample attack/release.

### Macros + automation are control-side constructs
`FXMacro::evaluate()` emits plain param ids (slot params included —
`Mix`, `Enabled`, not just P1..P8); `kFocusTarget` resolves against
`FXSurface`'s focus so one bank drives any rack. `FXAutomation` is a
breakpoint lane model sampled against the session beat — pushed
through `SetParam`, it is indistinguishable from hardware input. No
sequencer yet (§25).

### One command surface for every input (§29-30)
`0xA000` commands act on `FXSurface`'s focus: enable, type, mix,
P1..P8 set/nudge, division, sync, preset nav+apply, macro0..7,
slotMoveL/R, diagCopy. UI, keyboard, MIDI, HID and the DDJ-FLX4 all
invoke the same ids through `CommandManager` — processors never see
a controller.

### Diagnostics are snapshots, not live objects (§39/§43)
`FXSlot::faults` counts auto-resets (bad output ≥4 blocks) and emits
a `Fault` event. `FXEngine::fillDiagnostics()` builds
`FXRackDiag` snapshots on the control thread from atomic meters —
kind, enabled, mix, sync, division, peaks, cpu µs, sanitized,
faults, latency — formatted by `diagnosticReport()` for
`fx.diagCopy`. A faulting FX resets in place; playback never stops.

## Consequences

- `ParameterRegistry::kFxType`/`kFxDiv` maxima now derive from
  `FXKind::kCount`/`beatDivCount()` — new kinds/divisions can't be
  clamped away again (was a real bug: kinds >7 sanitized to Gate).
- Persisted data (presets, session blobs) remains valid: enum values
  were only appended.
- Reorder during performance is a sound-design action: macro
  mappings track chain positions, so moved mappings follow the new
  order — documented in `docs/fx/fx-engine.md`.
- All processors report 0 latency today; `latencySamples()` and
  `maxLatencySamples()` are the contract a future latency-bearing FX
  (e.g. FFT-based) consumes.
