# ADR 0030 — Track analysis deepening (component masks, job state, downbeat, tap BPM, true-peak, spectral bands, energy, failure persistence)

## Status

Accepted — Phase 30.

## Context

Phases 3/14/19/22/23 built a mature analysis pipeline:
`MusicAnalysisEngine` (decode → onsets → BPM → key → waveform →
transients → loudness → beatgrid + validation → energy), a worker
`AnalysisQueue` with priorities/retries/CPU gate, `AnalysisService`
with overrides + edit surface, and `AnalysisStore` persistence with
fingerprint/version staleness (schema v10).

Audit against the track-analysis spec found these real gaps:

- No partial analysis — every job ran the full pipeline even when a
  deck load only needs BPM/grid/waveform (§51–53).
- No public job state — callers saw events but no lifecycle model
  (Queued/Decoding/Analyzing/Caching/…, §3–4).
- Downbeat voting existed inline inside `BeatGridEngine`; no
  standalone detector or `TimeSignature` surface (§29–31).
- No tap-BPM input path for manual correction (§24/§56).
- `LoudnessResult` had no intersample peak (§45); no spectral band
  data for colored waveforms (§42–43); energy score not stored on
  the analysis row (§44).
- A permanently-undecodable file re-queued forever — no persisted
  `Failed` state + error (§60).
- Analysis rate was a fixed 48 kHz constant (§11).

## Decision

**Masks over pipelines.** `AnalysisMask` is a bitmask over
`AnalysisComponent`; `analyzeTrack/analyzeBuffer` accept it and skip
masked-out stages (left `NotAnalyzed`, never faked). The deck-load
path becomes two jobs — `Essential` at IMMEDIATE, the remainder at
LOW — and `AnalysisService::mergePrior` folds previously-analyzed
components into the fresh result on save. This gives §52/§53
(deck-ready fast, full analysis later) without a second pipeline or
re-analysis (§8).

Important detail: merge decisions use the *fresh* run's component
states captured before statuses are copied — otherwise a skipped
component inherits "Analyzed" status and its data would never merge.

**Job state is derived, not stored.** `snapshotJobs()` maps each
job's last queue event + progress to `JobState` (progress < 0.15 →
Decoding; `Event::Saving` → Caching). No second state machine to
keep consistent with the event stream.

**Small standalone analyzers.** `DownbeatDetector` (onset vote per
beat-of-bar), `TapBpmTracker` (median inter-tap interval, gap
re-anchor — the gap itself is not an interval), `EnergyAnalyzer`
(loudness+waveform score with coverage confidence), `SpectralAnalyzer`
(Butterworth band split per overview bin → `WaveformData::bands`,
plus Hann-windowed radix-2 FFT centroid). All pure/worker-side.

**Failures are data.** `saveFailed` persists `Failed` + the technical
error so rescans report the failure instead of re-queueing forever;
`errorFor` is the read side channel (the track row is never deleted,
§60). `loudness.valid` is derived on load rather than stored.

**Schema v11**: `analysis.true_peak`, `analysis.centroid_hz`,
`analysis.energy_conf`, `waveforms.bands`.

## Consequences

- Deck-load latency: essentials-only job skips key/energy/spectral
  passes; the follow-up merges so nothing is re-analyzed.
- True-peak is a documented 4× Catmull-Rom approximation — adequate
  for intersample-over warnings, not EBU R128 certification.
- The Butterworth band split leaks a little phase error into `mid`
  (~15–20% worst case on pure tones) — acceptable for waveform
  coloring and loudness-balance display; documented in tests.
- `AnalysisService::analyzeTrackSync` stays the synchronous path for
  tests/tools; the queue path remains the only UI-facing one.
- `TrackAnalysisPanel` provides the §55 read surface; wiring it into
  a dock/tab is host-shell work, out of this phase's scope.
