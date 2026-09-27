# ADR 0023 — Smart playlists + intelligent library tools + energy/discovery

Phase 23. Status: implemented.

## Context

The library needs organisation and discovery tooling at scale: live
smart playlists/crates, auto tagging, an energy descriptor, explainable
track discovery, history presets, batch metadata ops and cache
hygiene — without duplicating the Phase-18 query engine, without
inventing an opaque "DJ score", and without ever making mix decisions
for the DJ.

## Decisions

1. **Extend, don't duplicate.** `SmartPlaylistEngine` (rules JSON →
   `FilterEngine` SQL) already existed and stayed the single query
   engine. Phase 23 added the missing operators (`not_contains`,
   `ends_with`), fields (`energy_score`, date/play pseudo-fields,
   `session`, `unplayed`, `autotag`) and a `SmartQueryValidator`
   in front of it, rather than building a parallel language (§2-8).

2. **Store the question, not the answer.** Smart lists persist the
   `SmartRules` JSON; contents are generated per read through the
   normal repository path — indexed, bounded by `limit`, always
   current. Smart crates are crate folders containing smart
   playlists, not a second engine (§5/§6).

3. **Analysis-derived data stays separate from user data, forever.**
   `analysis.energy` (0..1, auto) vs `tracks.energy` (0-10, manual);
   `auto_tags(track_id, tag, confidence, status)` vs `track_tags`.
   Rejected auto tags persist as rows so they cannot silently
   reappear; no analysis pass can delete a user tag, cue, loop,
   rating or playlist — cache cleanup deletes only orphaned
   `analysis`/`waveforms`/`key_history` rows (§10-12/§44).

4. **Honesty over certainty.** Energy is a named *analysis feature*,
   not truth: confidence travels with auto tags, energy transitions
   and structure segments; `userDefined` vs detected is explicit;
   missing analysis projects as −1, never disguised as a real 0.
   Low-confidence inferences surface as suggestions (§11/§13/§17/§18).

5. **Explanations, not scores.** `TrackDiscoveryEngine` returns
   candidates with per-result `reasons` built from the row's real
   data (`bpm: +2.0`, `key: adjacent Camelot`). Sorting uses an
   inspectable configurable weighting — there is deliberately no
   single hidden score, and nothing is applied to a deck (§19-22/§26/§27).

6. **One undo entry per batch, one transaction.** `undoLast()`
   restores prior state atomically. Tag membership is restored with
   inline SQL because repository helpers open their own transactions
   — calling them inside the undo `Tx` made `COMMIT`s fire mid-loop
   and intermittently wedged the WAL connection (test-found bug;
   `Database::Tx` does not nest). Apply itself stays per-item so a
   cancel leaves a clean partial state (§39/§40).

7. **Everything off the UI/audio path.** Queries are plain indexed
   SQL; batch work posts to the existing library worker; the smart
   analysis queue is a priority mapping onto `AnalysisQueue`, which
   already yields to playback. The audio thread never sees any of
   this (§41/§43).

8. **Energy without new DSP.** Scalar from `lufs_i`/rms/peak;
   curve from the existing waveform overview pyramid (≤96 max-pooled
   points); transitions/structure derived from the curve. Zero extra
   decode or analysis passes; curves serialise into v9 JSON columns
   for future waveform overlay (§13-16).

9. **Compact UI seam.** `SmartQueryPanel` (rows + AND/OR/NOT +
   live preview + save hooks) integrates into `BrowserView` beside
   the harmonic panel; the visual model *is* `SmartRules`, so panel
   and engine can never disagree (§46-48).

## Consequences

- Schema v9: `analysis.energy`, `energy_curve`, `energy_transitions`,
  `structure_json` + `auto_tags` table/indexes (`first_sound_frame`
  pre-exists and feeds intro/outro rules).
- `Track.energyScore` distinguishes "not analyzed" (−1) from low
  energy (0) via `COALESCE`.
- 466 tests / 1,647,157 assertions green; 100k previews ~51 ms,
  discovery ~61 ms.
- Known limits: energy is a loudness heuristic, structure detection
  is conservative, undo depth is one batch, vocal/instrumental
  auto-tags wait for a real feature, nested-group editing in the
  panel is a UI follow-up (engine already supports it).
