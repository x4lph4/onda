# Sync Engine (Phase 15)

Musical synchronization between decks: BPM, beat phase, beatgrid,
bars and tempo-map following — with an explicit tempo master,
configurable pause behaviour, honest confidence and no blocking work
on the audio thread.

```text
MasterClock
↓
TempoMaster
↓
SyncEngine  (PerformanceEngine control plane + SyncRtCore RT pass)
├── Deck A
├── Deck B
├── Deck C
└── Deck D
```

## Layered design

| Layer | Code | Thread |
|-------|------|--------|
| Clock source | `sync::ClockSource` / deck stats | audio |
| RT execution | `SyncRtCore`, `DeckEngine::applySyncCorrection` | audio |
| Control plane | `PerformanceEngine` + `core/sync/` policies | control/UI |
| Diagnostics | `sync::SyncDiagnostics`, `DeckSnapshot` | control/UI |
| Commands | `0x9100` ids via `CommandManager` | any input |

The audio callback runs `syncRt_.apply()` before deck rendering: the
master's phase is predicted one block ahead, then each follower
slews its rate multiplier (±0.002/block) toward `masterBpm/ownBpm`
and chases wrapped phase error inside deadband/chase/reposition bands.
No locks, no allocation, no I/O — UI timers are never the clock.

## MasterClock (§2-3)

`sync::ClockSample` — bpm, beatPos, bar, phase, beatDurSec,
barDurSec, beatsPerBar, timestampSec (master playhead, audio-clock
domain). `masterClockView()` adds `stopped` and the feeding deck.
`ClockSource` is the seam for a future external feed
(`setMasterSource(External)` + `setExternalClock(fn)`); today only
decks feed the clock and RT sync does not chase externals.

## TempoMaster (§4-5, §22-23)

`MasterSource`: `Auto`, `DeckA..D`, `External`, `None`.

AUTO rules (in order): keep a still-valid master (playing+synced);
else lowest-indexed playing synced deck; else first synced deck;
else none. The master never re-picks mid-set while valid.

`MasterPausePolicy` (evaluated in `poll()`):
- `ClockHolds` — default; master stops → corrections no-op, followers
  hold converged tempo.
- `ClockStops` — RT master released while paused; clock invalid.
- `FollowOtherDeck` — AUTO promotes another playing synced deck once.

`cycleMaster()`: Auto→A→B→C→D→Auto (command `sync.masterCycle`).

## DeckSyncState (§6)

`deckSyncState(d)`: enabled, isMaster, mode, status, bpm, targetBpm,
beatPos, phase, tempoLock (range limit), syncLock, confidence —
mirrored into `DeckSnapshot` for the UI.

## Modes (§7-9)

`Off`, `Bpm` (tempo only — no phase chase), `Beat` (tempo+phase),
`Phrase` (maps to bar-boundary behaviour today; BAR_SYNC/PHRASE_SYNC
are prepared extension points, not claimed). Tempo never moves the
physical fader — `pitchPct` (physical) and `effBpm` (effective) stay
separate.

## Beatgrid & tempo-map following (§11-12)

Followers read their own `BeatGrid::bpmAt(beat)` — a variable-tempo
track's segment changes are followed automatically. The master's
`effBpm()` is likewise grid-derived: a 128→130 BPM tempo map change
re-targets every follower without special-casing.

## Corrections (§14-16)

`sync::SyncCorrection` mirrors the RT bands: <8 frames deadband
(silence), ≤35%-of-a-beat chase (≤5% of block, inaudible slew),
larger → one beat-aligned faded snap (`nudgeFrames`) — never a
position jump that breaks the grid.

## Quantize (§17-21)

`Quantize`: Beat1, Half, Quarter, Eighth, Sixteenth, ThirtySecond,
Bar (resolves `beatsPerBar` from the deck's grid). Quantized actions
snap their *target* onto the grid — no added latency. Beat jumps are
exactly N grid beats; loop in/out respect the same engine.

## Sync lock & pitch policies (§24-26)

- `syncLock` ON (default): track swap while synced keeps the mode —
  the new grid re-anchors; OFF: attach drops to Off.
- `onUserPitch(d, pct)` is the single entry for fader moves:
  `Override` ignores while synced, `Disable` drops sync + applies,
  `ManualAsBend` routes to `SetPitchBend`.

## Confidence & diagnostics (§30, §39)

`syncConfidence` = grid confidence (captured at attach) × phase
stability; <0.45 → `SYNC UNCERTAIN` badge, `syncLimited` → ≤0.2.
`syncDiagnostics(d)` / `syncDiagText(d)` → copyable:
`master=A 128.00 BPM | deck B: 128.00 -> 128.00 BPM | beat 41.96
phase 0.96 | grid offset +0.000 beats | tempo err 0.00% |
phase err 0.4 ms | conf 63% | SYNCED`.

## UI surface (§28-29)

DeckView badges: `MASTER`, `SYNC UNCERTAIN`, `SYNC LIMITED`,
`ANALYSIS PENDING`; beat indicator = `beatsPerBar` dots, active beat
lit with phase-falloff. All state comes from `DeckSnapshot` — no sync
logic in widgets.

## Commands (§27)

`0x9100` range: `sync.masterCycle` (app-level), per-deck
`syncLock`, `syncQuantRes`, `syncBpmMode`, `syncDiagCopy`.
Hardware reaches them through `CommandManager` → `EnginePorts` —
never directly.

## Recovery (§32)

`attachGrid` while synced re-publishes the grid to RT and emits
`SyncStateChanged`; the RT follower re-anchors phase next block —
no transport jumps.

## Limitations

- `External` clock is view-only today; RT sync does not slave decks
  to it (seam ready for a Link-style feed).
- `Phrase` mode reuses bar boundaries; true phrase detection is a
  Phase-16+ concern.
- BAR/PHRASE as distinct engine behaviours are extension points —
  only OFF / TEMPO_SYNC / BEAT_SYNC are implemented per spec.
