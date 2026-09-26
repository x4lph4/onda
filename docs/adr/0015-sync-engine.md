# ADR 0015 — Professional Sync Engine

Status: accepted (Phase 15).

## Context

Phase 4 shipped a working real-time sync path: `SyncRtCore` predicts the
master's phase one block ahead, `DeckEngine::applySyncCorrection` slews
the follower's rate and chases bounded phase error, and
`PerformanceEngine` owns control-plane state. Phase 15's brief required
the full professional architecture around that core: explicit tempo
master selection, pause policies, per-deck sync state + confidence,
pitch-interaction policies, sync lock, extended quantization,
beatgrid/tempo-map following and copyable diagnostics — while keeping
the audio callback free of locks, allocation and I/O.

## Decisions

### Control plane in `core/sync`, execution stays RT-native
New header-only module `src/core/sync/` holds the pure-policy pieces —
`TempoMaster`, `TempoFollower`, `BeatPhaseTracker`, `SyncCorrection`,
`MasterClock`/`ClockSource`, `DeckSyncState`, `SyncDiagnostics`. They
mirror the RT math (same thresholds, same wrap) so tests and
diagnostics exercise identical rules without touching the audio
thread. The RT corrector inside `DeckEngine` is unchanged — Phase 15
deepens the control plane rather than reimplementing proven RT code.

### TempoMaster: explicit sources + documented AUTO rules
`MasterSource` = `Auto`, `DeckA..D` (pinned), `External`, `None`
(§4-5). AUTO keeps the current master while it remains playing+synced
— the resolver never re-picks a valid master, so the clock cannot
surprise-switch mid-set. `None` refuses sync deterministically
(`SyncStatus::NoMaster`). A pinned deck is the master as a *role* —
its own sync state is irrelevant; the RT layer still guards
playing/grid. `cycleMaster()` steps Auto→A→B→C→D→Auto for hardware.

### Pause behaviour is a policy, not an accident (§22)
`MasterPausePolicy` is evaluated in `PerformanceEngine::poll()` on the
control thread:
- **ClockHolds** (default): RT stops correcting when the master
  transport stops (`SyncRtCore` already no-ops) — followers hold their
  last converged tempo. Deterministic, zero surprise.
- **ClockStops**: the RT master is released (`SyncSetMaster 255`)
  while the master is paused; the session clock is explicitly invalid
  until playback resumes.
- **FollowOtherDeck**: AUTO re-resolution promotes the lowest-indexed
  playing synced deck — one bounded transition, then stability rules
  apply again.

### Sync lock decides what a track swap means (§24)
`syncLock` (default ON): a different track's grid arriving while
synced re-anchors the deck — the session clock survives. OFF: the
attach drops the deck to `Off`, honest manual control. Re-anchoring
emits `SyncStateChanged` so UI/controllers refresh.

### One entry point for every user pitch move (§26)
UI faders and controller deck-params both call
`PerformanceEngine::onUserPitch`. Policy `Override` (default) ignores
the fader while synced; `Disable` drops sync and applies the fader;
`ManualAsBend` routes it to `SetPitchBend`. Physical fader position and
effective tempo stay separate — hardware and software can never fight.

### Quantize gains 1/32 + Bar (§17)
`Quantize::ThirtySecond` (0.03125 beats) and `Quantize::Bar`, which
resolves against the attached grid's `beatsPerBar` — a 3/4 grid
quantizes to 3-beat bars. `cycleQuantize()` walks all seven
resolutions for hardware stepping.

### Confidence is honest (§30)
`syncConfidence` combines the grid confidence captured at attach time
(analysis `bpm.confidence`) with RT phase stability (`syncErrMs`).
Out-of-range followers report ≤0.2 → the UI shows `SYNC UNCERTAIN`
instead of pretending perfection. Diagnostics are one copyable struct
+ `toText()` (§39) behind `SyncDiagCopy`.

### Hardware stays behind the command layer (§27)
New `0x9100` commands: `sync.masterCycle`, per-deck `syncLock`,
`syncQuantRes`, `syncBpmMode`, `syncDiagCopy`. MIDI/HID invoke them
through `CommandManager` like everything else — no direct
hardware→SyncEngine path exists.

### External clock is a seam, not a feature (§5)
`ClockSource` is `std::function<ClockSample()>`; `masterSource ==
External` consults it for `masterClockView()`/`masterClock()`. RT sync
does not chase an external tempo yet — documented limitation, the
interface is in place for a future Link-style feed.

## Consequences

- `attachGrid` gained a `confidence` parameter; `EngineBridge` feeds it
  the analysis BPM confidence on every attach path (load, completion,
  edit republish).
- `SyncStatus`/`SyncMode` semantics are unchanged — `Phrase` mode still
  maps to bar-boundary behaviour and is documented as such (BAR/PHRASE
  remain prepared-but-not-claimed, per spec §7).
- `poll()` is now load-bearing for master maintenance — apps and tests
  call it periodically (it already was for beat/bar events).
- Four decks are first-class: the stress test runs all four synced
  with loops/jumps/cues at ~250× realtime headroom in Debug.
