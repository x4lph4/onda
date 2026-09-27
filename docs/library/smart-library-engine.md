# Smart Library Engine (Phase 23)

Advanced organisation, analysis-derived description and discovery on
top of `MusicLibraryEngine`. These tools **inform and organise** —
they never make mix decisions automatically (no auto-load, no
auto-sync, no hidden "DJ score").

Headers under `src/core/smart/` (`namespace onda::smart` abbreviated
`S::` below). Everything is header-only; nothing runs on the audio
thread or blocks it — queries are indexed SQL, batch work posts to
the library worker, analysis stays on `AnalysisQueue`.

## Architecture

```
SmartLibraryEngine                (facade over MusicLibraryEngine)
├── SmartPlaylistEngine           (existing — query JSON → SQL)
├── SmartCrateEngine              (smart playlists filed in crate
│                                  folders; same query engine)
├── AutoTagEngine                 (auto_tags CRUD + suggest/accept/
│                                  reject/ignore, user tags separate)
├── EnergyAnalysisEngine          (scalar 0..1 + curve + transitions
│                                  + structure, pure functions over
│                                  waveform/loudness data)
├── TrackDiscoveryEngine          (criteria-driven candidates with
│                                  per-result MatchReasons)
├── LibraryPresets                (recent/most/unplayed/session +
│                                  date + playcount query builders)
├── LibraryStatistics             (totals, distributions, gaps)
├── LibraryBatchEngine            (async, cancelable, one-op undo)
├── LibraryResourceManager        (worker/cache limits + safe cleanup)
├── SmartAnalysisQueue            (named-priority mapping onto the
│                                  existing analysis queue)
└── SmartLibraryDiagnostics       (counters + failure capture)
```

## Smart Queries

`SmartPlaylistEngine::SmartRules` is the stored model — a JSON
document (`match: all|any`, `rules[]`, nested `groups[]`), never a
materialised row set. Evaluation compiles to `WHERE` clauses via
`FilterEngine` and runs through `TrackRepository::queryWhere`, so
every smart list is live: saving stores the question, not the answer.

Operators (§4): `eq`, `neq`, `contains`, `not_contains`,
`starts_with`, `ends_with`, `lt`, `lte`, `gt`, `gte`, `between`,
`in`, `not_in`, `isnull` (empty), plus `negate` on any leaf for NOT.
Groups nest AND/OR arbitrarily (§3).

- `SmartQueryValidator` (§8) rejects unknown fields (checked against
  `FilterEngine`'s column whitelist), operators the field's type
  can't take (`contains` on a number, `between` on text), impossible
  ranges (`b BETWEEN 130 AND 120`), and empty queries — before any
  save.
- `SmartQueryPanel` (§46/47) is the visual editor: rule rows
  (field | operator | value), add/remove, AND/OR/NOT toggles, live
  preview count without touching the library, save-as-playlist /
  save-as-crate / duplicate hooks. The visual model is the same
  `SmartRules` JSON — no second representation.

## Smart Playlists + Smart Crates

`Playlist` rows with `kind = 'smart'` store `rules` JSON; contents
generate on read. `SmartCrateEngine` files smart playlists inside
crate folders — crates are containers, not a second query engine.

## Auto Tags (§9-12)

`AutoTagEngine` infers tags from real columns only: High/Low Energy
(analysis `energy` threshold), Warmup/Peak Time (energy band), Short
Intro/Long Intro (`first_sound_frame`), plus metadata-derived hints
(no vocals claim without data — "Instrumental"/"Vocal" rules fire
only when a feature exists; low-confidence suggestions surface as
suggestions, never facts).

- Table `auto_tags` (schema v9): `(track_id, tag, confidence, status)`
  with `status = suggested | accepted | rejected | ignored`.
- `reject` writes a row — a rejected tag cannot silently reappear on
  the next suggestion pass.
- `track_tags` (user tags) is never touched by the engine; analysis
  re-runs can supersede auto tags but cannot delete user tags.

## Energy Analysis (§13-18)

`EnergyAnalysisEngine` — pure functions, no extra DSP pass:

- `energyOf(lufs_i, rms, peak)` → scalar 0..1 (normalised loudness
  composite). **An analysis feature, not an objective truth** — the
  column is `analysis.energy`, separate from user `energy` (0-10)
  which stays manual forever. `Track.energyScore` projects it;
  missing analysis yields −1 via `COALESCE`, distinguishable from a
  genuine 0.
- `curveOf(waveform.overview)` → `EnergyPoint[]` `{start, end, value}`
  (≤96 points, max-pooled) for future waveform overlay.
- `transitionsOf(curve)` → `EnergyTransition[]` `{start, end, delta,
  confidence}` where adjacent points shift beyond a threshold.
- `structureOf(curve, duration)` → `StructureSegment[]` (intro/outro/
  breakdown/build/drop labels) each with `confidence` and
  `userDefined` — the UI must render detected ≠ user-defined, and
  low-confidence sections simply don't appear.
- `EnergyLevels` — VERY_LOW/LOW/MEDIUM/HIGH/VERY_HIGH with
  configurable bounds; serialised into `analysis.energy_curve` /
  `energy_transitions` / `structure_json` columns (schema v9).

## Discovery + Related Tracks (§19-27)

`TrackDiscoveryEngine` takes explicit `DiscoveryCriteria` — BPM
window (same/±1/±2/±4/custom), key relationship (same/relative/
Camelot-adjacent/custom), energy band (same/higher/lower/range),
genre/tags/artist/rating/history — and returns candidates each
carrying `reasons`: `bpm: +2.0`, `key: adjacent Camelot`, `genre:
House`, `energy: similar`. Reasons are derived from the row's actual
data; nothing is invented, and there is **no opaque combined score** —
results sort by a configurable, inspectable weighting, not a black
box. Tempo/key transitions are described (current 124 → candidate
128, +4 BPM; 8A → 9A adjacent) — never applied to a deck.

## Library Presets + History (§28-33)

`LibraryPresets` builds `QuerySpec`s for: Recently Added
(`date_added` desc), Recently Played, Most Played (`play_count`
desc), Unplayed (`play_count = 0`, not just `last_played` null),
Not Played Recently, Played This Session (`history.session_id`
join), date buckets (Today/Yesterday/Last 7/Last 30/This
Year/Custom), play-count buckets (0 / 1-5 / 5-10 / 10+ / custom).
`SessionLibraryQuery` returns the tracks of a given session —
prepared for Recording/Set History/Export integration.

## Statistics (§34-36)

`LibraryStatistics`: total tracks/duration, artist/album/genre
counts, BPM average+range, key distribution, analyzed vs unanalyzed,
missing files, duplicates, stem availability, average energy +
energy-range distribution (descriptive, not a quality judgment),
and tag stats (most-used user tags, auto tags, unused tags for
manual cleanup).

## Batch Operations + Undo (§39-40)

`LibraryBatchEngine::apply(ids, op, ...)` — add/remove tag, set
rating, set color, analyze/reanalyze. Synchronous `apply` or
`applyAsync(post, ...)` on the library worker; a shared atomic cancel
flag is checked per item; `progress()` is pollable. One batch = one
undo record: `undoLast()` restores prior membership/values in a
single transaction (never N ops). Undo does its tag membership
restore inline — repo helpers open their own transactions, which
would corrupt a surrounding one (found by tests: nested `BEGIN` +
premature `COMMIT` under WAL).

## Resources + Cleanup (§41-44)

- `SmartAnalysisQueue` maps named priorities (CurrentDeck >
  PreloadedTrack > UserRequested > RecentlyAdded > LibraryBackground)
  onto the existing `AnalysisPriority` queue; the playback CPU gate
  still yields to audio.
- `LibraryResourceManager` — worker count, memory/cache-size caps in
  `kv`, `cacheStats()`, and `cleanOrphanedAnalysis()` which deletes
  orphaned `analysis`/`waveforms`/`key_history` rows only. User
  metadata (cues, loops, ratings, tags, playlists) is structurally
  out of scope — FK constraints already make true orphans rare; the
  call is a verified no-op on a clean library.
- `AutoAnalysisSettings` — Analyze on Import / Manually / When Idle.

## Search + Filters (§45)

`SearchQuery` tokens: `energy:high|low|0.6-0.9`, `added:today|...`,
`played:...`, `unplayed:1`, `session:N`, `autotag:NAME`.
`FilterEngine` gained `energy_score` (joins `a.energy`), date/play
pseudo-fields (`added_after/before`, `played_after/before`,
`session`, `unplayed`, `autotag`), `not_contains`/`ends_with`
operators, and energy sort. FTS still covers tags; all new clauses
are indexed column comparisons — 100k queries stay in tens of ms.

## Validation (Phase 23)

- 17 test cases / 384 assertions (`tests/smart_phase23_tests.cpp`)
  covering queries, builder, playlists, crates, auto tags, energy,
  discovery, related tracks, presets/history, statistics, batch +
  undo, resources, failure modes, and the facade.
- Full suite: **466 test cases, 1,647,157 assertions — all green.**
- 100k-library test (§51): seed 100k rows ~2.1 s; smart-playlist
  preview 16,667 matches in ~51 ms; criteria discovery 32 candidates
  in ~61 ms. Memory bounded by `limit` on every preset.
- Failure modes (§53): corrupt query JSON rejects with a readable
  error, missing analysis ⇒ `energyScore = −1` (not 0), cancelled
  batch reports `progress().cancelled`, missing files/failed analysis
  leave the library intact.
- DJ keeps control: discovery returns explained candidates only;
  nothing loads, syncs, shifts keys or changes tempo automatically.

## Limitations

- Energy scalar is a loudness/RMS heuristic — deliberately presented
  as a descriptor, not a quality judgment. Structure detection is
  conservative (intro/outro from curve edges); breakdown/build/drop
  labels only emit above confidence thresholds and never claim a
  section the data doesn't support.
- Auto-tag rules are metadata-driven; vocal/instrumental inference is
  absent until a vocal-presence feature exists (stems work may add
  one later).
- Undo is one-operation-deep (last batch only), metadata-only —
  file operations are never touched and never undone.
- Session queries cover recorded plays; a session with no history
  rows yields nothing.
- Smart crates are folders containing smart playlists — a crate
  itself holds no query.
- `SmartQueryPanel` is a compact rule editor (rows + match +
  preview); nested-group editing parity with the JSON model is a UI
  follow-up, though the engine supports it.
