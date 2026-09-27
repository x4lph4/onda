# ADR 0029 — Music library engine completion (import queue, auto-import, missing ops, metadata write-back, fuzzy search, playlist metadata, smart snapshots, artwork LRU, backup gating, semantic integrity)

## Status

Accepted — Phase 29.

## Context

Phases 7/14/18/22/23 built most of the professional library:
`MusicLibraryEngine` facade + worker, `TrackRepository`/`PlaylistManager`/
`CrateManager`/`TagManager`/`RatingManager` over a migrated SQLite
schema (v9), FTS5 search with field tokens and folded text, nested
smart playlists, scanner + debounced watcher, missing/relink,
duplicates, JSON/CSV backup, integrity checks and 50k/100k perf
validation.

Audit against the professional-library spec found these real gaps:
no resumable/cancellable import queue (imports were one-shot sync
calls), watcher roots had no auto-import flag and watch config was
not persisted, missing-file management lacked ignore/remove
operations, no explicit metadata write path, no tag normalization
helpers, no typo-tolerant search, playlists lacked description/color/
sort-mode, no smart→static snapshot, artwork had no in-memory cache,
sessions had no rollup view, the library→deck hand-off was untyped,
JSON import accepted newer-version backups, integrity checks were
physical-only, and the color palette was not configurable.

## Decisions

1. **`ImportQueue` drives itself through `post_`.** Each item runs
   one worker task (`step` re-posts the next), so the queue stays
   O(1) per message and cancel/resume are trivially race-free:
   `cancel()` marks untouched items `Cancelled` and the in-flight
   file always completes (per-file transactional imports — never
   half-written rows). `Skipped` fingerprints complete normally and
   count as skipped, not failures.
2. **Watch configuration lives in `kv`.** `WatchRoot` gained
   `autoImport`; `addWatchRoot`/`setAutoImport` persist
   `watch.root.<dir>` and `restoreWatchRoots()` replays them on
   `open()`. The flag gates only the *watcher's* new-file pickup —
   an explicit `scan()` still reconciles every root (scans are user
   intent, watchers are ambient).
3. **Missing removal is status-gated.** `removeMissing` only deletes
   rows actually `Missing`; `ignore`/`unignore` toggle `Invalid` so
   nothing is deleted but the row leaves the nag list. Unconditional
   deletion stays explicit through `removeFromCollection`.
4. **Metadata write-back is provider-injected and file-first.**
   `MetadataWriter::writeBack` writes the file through an injected
   `Writer`, then persists the same values + `edited_fields` mask —
   on file failure the DB is untouched, so the collection never
   claims a write the file rejected. No path writes audio files
   automatically.
5. **Fuzzy search is exact-first, bounded re-rank.** Missed queries
   build a bounded OR-prefix FTS candidate set — per-token prefixes
   plus the first-4-chars prefix of the squashed query (required so
   "daftpunk" can reach "daft punk" rows) — scored by banded
   Levenshtein over folded, space-stripped text. `candCap` bounds
   work; the threshold scales with query length.
6. **Playlist sort mode is a view, never a rewrite.** Schema v10 adds
   `description/color/sort_mode`; `tracksView()` honors the stored
   `PlaylistSort` while `Manual` keeps `playlist_items.position`
   canonical — sorting never mutates stored positions.
7. **Snapshots freeze evaluation.** `snapshotSmart` copies the live
   result set into a static playlist — the collection is never
   modified implicitly afterwards.
8. **Artwork bytes are LRU-bounded.** `ArtworkManager` layers a
   byte-budgeted in-memory LRU (keyed `trackId:px`) over the lazy
   disk cache; providers generate thumbnails lazily and `invalidate`
   drops stale entries. The paint path touches only `bytes()`.
9. **`TrackReference` is the deck boundary.** Identity + path +
   display/analysis echo — no audio data, no playback control.
10. **Restore is version-gated; DB backup is physical.**
    `importJson` refuses newer-version backups; `backupDb` uses
    `VACUUM INTO` for a consistent copy without quiescing writers.
11. **Integrity checks are semantic.** Orphans in
    `playlist_items`/`track_tags`/`history`, broken playlist
    references and FTS drift are reported alongside the physical
    `integrity_check`/`foreign_key_check`.

## Consequences

- 250k-track library stays interactive: FTS ~54 ms, filter+sort page
  ~121 ms, fuzzy re-rank ~0.5 s, 107 MB db.
- Watch roots survive restarts; auto-import stays opt-in per folder.
- `LibraryScanner::addRoot(dir)` call sites are source-compatible.
- Fuzzy candidates are capped; a pathological prefix can miss a far
  match — accepted vs per-keystroke cost.
- `removeMissing` semantics changed (status-gated) — safer for batch
  UI operations.
- New surfaces (`ImportQueue`, `MetadataWriter`, `ArtworkManager`,
  `TagNormalizer`, snapshot/meta APIs) are headless and tested; UI
  wiring (queue view, palette picker, write-back action) is pending.
