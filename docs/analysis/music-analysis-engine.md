# Music Analysis Engine

`src/core/analysis/` — asynchronous, decode-once analysis that produces
everything sync/quantize/loops/FX/UI need: BPM + confidence, tempo
candidates, tempo map, beatgrid (with bars/downbeats), musical key,
waveform pyramid, transients, loudness, silence map, per-component
status and review flags.

## Pipeline

```
analyzeTrack(path)                        analyzeBuffer(pcm)
       │                                        │
 decoder → 48 kHz PCM            onsets → BPM → key → silence →
                                          waveform → transients →
                                          loudness → grid → validate
```

`MusicAnalysisEngine::analyzeBuffer` runs the pure-PCM pipeline —
deterministic and decoder-free, so tests feed synthetic signals
directly. `analyzeTrack` adds the FFmpeg decode front-end (§5).

## Components

| File | Role |
|------|------|
| `OnsetDetector` | spectral-flux onset events (frame + strength) |
| `BpmAnalyzer` | estimated BPM, confidence, stability, octave candidates, tempo map, variable flag |
| `BeatGridEngine` | build grid from BPM+onsets; all manual edit ops |
| `BeatGrid` | piecewise-constant tempo segments; frame↔beat, bars, downbeats, `rescaled(rate)` |
| `BeatGridValidator` | grid plausibility → `needsReview` + reasons |
| `KeyAnalyzer` | 24-key index, standard + Camelot names, confidence |
| `TransientAnalyzer` | compact marker list (≤4000, strongest-first cap) |
| `LoudnessAnalyzer` | peak, RMS, gated integrated LUFS, p10–p95 LRA |
| `WaveformGenerator/Data` | 3-level pyramid: overview (4096 bins) / mid (1024-frame bins) / detail (256-frame bins) |
| `AnalysisQueue` | priority heap + workers + cancel + retry + CPU gate |
| `AnalysisService` | app-facing API: jobs, cached reads, overrides, edits, sync refs |
| `AnalysisStore` | SQLite persistence + fingerprint/version staleness |

## Queue & workers (§2–4, §28–30)

Priorities: `Realtime(3) > High(2) > Normal(1) > Low(0)` — deck loads
go in High, manual imports Normal, bulk scans Low. Workers run at OS
below-normal priority and poll a cancel token between stages.

`setMinActivePriority(p)` is the CPU gate: while set, only jobs with
priority ≥ p start; the rest stay queued and resume automatically when
the gate reopens. `EngineBridge` drives it from the UI snapshot —
playing decks or an active recording gate background analysis out.

`analysis.workers` config key selects the worker count (0 = auto:
half the cores, capped at 4).

## Waveform pyramid (§6–8)

Three levels in one `WaveformData` object, each `WaveformBin{min,max,rms}`
per channel:

| Level | Granularity | Use |
|-------|------------|-----|
| overview | 4096 bins / track | full-track strip |
| mid | 1 bin / 1024 frames (~21 ms) | scrolling/zoom mid |
| detail | 1 bin / 256 frames (~5.3 ms) | beat-level zoom |

`query(start,end,maxBins,out,ch)` picks the cheapest level that fits
the budget and aggregates — the UI never touches PCM.

## Grid, tempo map, downbeats (§12–14)

`BeatGrid` = `firstBeatFrame` + `segments[]` of
`{startFrame,startBeat,bpm}` (piecewise constant → arbitrary tempo
maps), plus `beatsPerBar`/`downbeatOffset`. All math is f64 with no
accumulated drift (verified to 10 min).

`BeatGridValidator` flags implausible tempo (<55/>220 BPM), low
confidence, <55% onset↔grid agreement, sparse coverage and variable
tempo on a constant grid. Flagged results keep their data but report
`needsReview` and `SyncReference::reliable=false`.

## Confidence (§10, §19)

BPM and key each carry `confidence` 0–1; BPM also exposes the full
candidate list (half/double-time alternatives preserved) and
`stability`. The candidate you see is *an* interpretation, never
silently the only one.

## Overrides & edits (§16, §25, §41)

Detected (`analysis`, `beatgrids` tables) and user edits
(`user_overrides`: `bpm`, `key_index`, `grid_json`, `downbeat_offset`)
are strictly separate. Re-analysis refreshes detection freely; user
data is never overwritten. `AnalysisBundle::effectiveGrid()` merges at
read time.

Edit surface (command range `0x9000 + deck*0x400`):
`ana.setFirstBeat` (playhead), `ana.nudgeLeft/Right` (±10 ms),
`ana.bpmDouble/bpmHalf`, `ana.setDownbeat` (beat at playhead),
`ana.gridReset`, `ana.reanalyze`. `AnalysisService::applyEdit` is the
service-level API; grids republish to the deck automatically.

## Persistence (§23–24, §36)

`analysis_version = 2`. Each row carries fingerprint (size+mtime+edge
samples) and version; staleness → `Outdated` → re-analyze. Component
status is a bitmask; transients/segments/silences stay compact-packed.

## Sync integration (§32)

`AnalysisService::syncReference(path)` → `SyncReference{effectiveBpm,
confidence, variable, reliable, effectiveGrid, firstBeatSec}` — the
single object sync should consume (Phase 15 builds the real
SyncEngine on it).

## Deck integration (§31)

Load → `attachGrid` immediately if analyzed, else `ANALYSIS PENDING`
badge + High-priority job; on `Completed` the effective grid is
`rescaled(deviceRate)` and handed to the deck via the atomic
pending/retired slot pair.

## Testing

Synthetic signals only (§39–40): `clickTrack` (known BPM, tempo
changes), `chordTrack` (known key), WAV writer. Coverage: BPM±1%,
candidates, tempo maps, key, pyramid queries, validator verdicts,
loudness honesty, transient caps, override separation+persistence,
queue gate, service edits, grid rescaling, 100-track throughput
benchmark (~187 ms/track Debug).

## Phase 30 — deepening (masks, job state, downbeat, tap, true-peak, spectral bands, energy, failure persistence)

### Component masks (§51–53)

`AnalysisMask` selects which stages a job runs (`Essential =
Bpm|BeatGrid|Waveform`, `All`, or any subset). Masked-out components
stay `NotAnalyzed` — no phantom results. `analyzeForDeck` enqueues an
IMMEDIATE-priority essentials job plus a LOW-priority follow-up for
the rest; `analyzePartial` runs an arbitrary subset. On save,
`mergePrior` folds prior analyzed components into the fresh result so
a partial run completes the record instead of replacing it.

### Job lifecycle (§3–4)

`AnalysisJob` is the public descriptor: `trackId`, `path`, `priority`,
`requestedAnalyses`, `state` (`Queued/Decoding/Analyzing/Caching/
Completed/Failed/Cancelled`), `progress`, `attempts`, `error`.
`AnalysisQueue::snapshotJobs()` exposes it; `Event::Saving` marks the
persistence stage. `setMinActivePriority` is the CPU gate — during
playback only ≥HIGH jobs run.

### New analyzers

- `DownbeatDetector::phase` — every onset votes for the beat-of-bar
  position it lands on; winner = bar start + vote-share confidence.
- `TapBpmTracker` — median inter-tap interval (outlier-robust), gap
  > `resetGapMs` re-anchors without recording the gap as an interval.
  `AnalysisEdit::TapBpm` feeds it; ≥2 taps become a BPM override.
- `EnergyAnalyzer::score` — normalized 0..1 energy from loudness +
  waveform (falls back to waveform-RMS when loudness is invalid) +
  coverage-based confidence.
- `SpectralAnalyzer` — Butterworth LP(250)/HP(4k) split gives
  low/mid/high fractions per overview bin (`WaveformData::bands`,
  palette-agnostic §42 color data) plus a Hann-windowed radix-2 FFT
  spectral centroid.

### Measurements

- `LoudnessResult.truePeak` — 4× Catmull-Rom intersample estimate.
  Documented as an approximation, not a polyphase true-peak meter.
- `BeatGrid::timeSignature()` — `{beatsPerBar, 4}`; `beatsPerBar` is
  a field, 4/4 is the default not the only possibility.

### Failure persistence (§60)

`AnalysisStore::saveFailed` writes `analysis_state=Failed` +
technical error; `errorFor(path)` reads it (the UI side channel —
`load()` only serves Analyzed bundles). The track row is never
deleted; a definitively-failed job stops re-queueing forever.

### Schema v11

`analysis.true_peak`, `analysis.centroid_hz`, `analysis.energy_conf`,
`waveforms.bands` (per-bin {low,mid,high} floats). `load()` restores
all of them plus `loudness.valid` derived from stored measurements.

### UI (§55)

`TrackAnalysisPanel` (`src/app/ui/`) — read-only snapshot view:
BPM+confidence, grid status + review flag, key (standard+Camelot),
waveform/loudness/energy status, spectral centroid, analysis version,
per-component states. Pulls from `AnalysisService`; never decodes on
the message thread.

### Validation

`tests/analysis_phase30_tests.cpp` — 22 cases `[an30]`: mask
semantics, configurable analysis rate (§11), downbeat vote, tap
tracker (median + reset-gap), true-peak ≥ sample-peak, spectral band
dominance, energy ordering, full-pipeline population, job snapshot/
gating/pause, `analyzeForDeck` split, `saveFailed` + `errorFor`,
partial-merge preservation, v11 persistence round-trip, tap→override,
24-job queue throughput (~4.6 jobs/s), pause isolation.
