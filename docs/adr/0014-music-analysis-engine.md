# ADR 0014 — Professional Music Analysis Engine

Status: accepted (Phase 14).

## Context

Phase 3 shipped a working analysis foundation (BPM/key/waveform/beatgrid,
queue, SQLite store, user BPM/key overrides). Phase 14's brief required
a *professional* analysis engine: per-component fault isolation, review
flags, loudness, transients, a waveform pyramid, tempo-map support,
sync-facing data, a real user-override model, and — critically — the
generated beatgrid actually reaching playing decks.

## Decisions

### Extend, don't rewrite
The Phase 3 engine already had the right bones (decode → onsets → BPM →
key → silence → waveform → grid). Phase 14 deepens it in place rather
than building a parallel "AnalysisManager". Component types were added
as separate headers (`LoudnessAnalyzer`, `TransientAnalyzer`,
`BeatGridValidator`, `AnalysisOverride`, `SyncReference`) to keep each
stage independently testable.

### Per-component status ledger
`AnalysisResult::components[kNumComponents]` records each stage's
state + progress (§26–27). A key failure no longer marks a track
"failed" — Waveform/BPM/BeatGrid can still be Analyzed while Key is
Failed. The DB stores a bitmask (`analysis.comp_status`).

### Review flags instead of silent trust
`BeatGridValidator` scores grid↔onset agreement, checks tempo
plausibility (55–220 BPM), confidence and coverage. Low-quality results
set `needsReview` + human-readable reasons — UI can flag them; sync
refuses to rely on them (`SyncReference::reliable`).

### Compact storage, not row-per-event
Transient markers and beatgrid segments stay packed (JSON/compact
columns), mirroring the existing `silences`/`bpm_candidates` pattern —
a 10k-track library does not gain million-row tables (§36).

### User overrides are strictly additive
Detected rows (`analysis`, `beatgrids`) are refreshed freely on
re-analysis. All user edits live in `user_overrides` —
`grid_json`, `bpm`, `key_index`, `downbeat_offset` — so re-analysis can
never destroy a manual correction (§25). Readers merge via
`AnalysisBundle::effectiveGrid()/effectiveBpm()`;
`AnalysisService::getBeatGrid` returns the effective grid.

### Grids attach at device rate
Analysis runs at the 48 kHz reference; decks position themselves in
device-rate frames. `BeatGrid::rescaled(rate)` converts frame fields
losslessly on publish — grids are stored/edited at analysis rate and
rescaled at attach time.

### Completion-driven grid handoff
`EngineBridge` listens for queue `Completed` events and marshals
`perf_.attachGrid(deck, rescaledGrid, trackId)` to the message thread
(attachGrid is control-thread only — it mutates non-atomic deck context
and may read the store). Loading an already-analyzed track attaches
immediately. This closes the production gap where `attachGrid` was only
exercised in tests.

### CPU gate, not thread micromanagement
`AnalysisQueue::setMinActivePriority(p)` lets workers wait when queued
jobs fall below a priority floor. `EngineBridge::snapshot` drives
`AnalysisService::setPlaybackActive(playing||recording)` once per UI
tick — background analysis yields automatically; HIGH-priority
deck-load analysis always proceeds (§29–30).

### Loudness is informational
`LoudnessAnalyzer` implements BS.1770-flavoured K-weighting (shelf +
high-pass pre-stage), gated integrated LUFS and p10–p95 LRA.
Coefficients are fixed for 48 kHz — documented approximation, not a
certified meter. It never modifies audio (§21).

## Commands

`0x9000 + deck*0x400` range registered for all decks A–D:
`ana.setFirstBeat`, `ana.nudgeLeft/Right`, `ana.bpmDouble/bpmHalf`,
`ana.setDownbeat`, `ana.gridReset`, `ana.reanalyze` — all routed through
`EnginePorts.analysisEdit` → `AnalysisService::applyEdit` →
`perf_.attachGrid` republish.

## Consequences

- Decks receive real grids in production: sync/quantize/beat-jump
  actually have data.
- `analysis_version` bumped to 2 — older caches report `Outdated` and
  re-analyze (§24). User overrides survive: they live outside the
  versioned detection payload.
- UI: `ANALYSIS PENDING` badge on unanalyzed deck tracks (§31).
- `analysis.workers` config key controls worker count (0 = auto).
- Playhead-derived edits convert device→analysis rate in the bridge.

## Limitations

- Transient markers reuse onset positions (hop granularity ~23 ms) —
  fine for slicing markers, not sample-accurate.
- Loudness K-filters are 48 kHz-fixed; other rates are approximations.
- Downbeat phase is onset-strength voting — correct on most electronic
  music, weak on rubato/acoustic.
- No golden audio corpus yet (§39) — synthetic signals only.
