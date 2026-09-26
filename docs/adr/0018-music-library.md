# ADR 0018 — Professional music library

Phase 18. Status: implemented.

## Context

Phase 7 built a solid library base: `MusicLibraryEngine` + worker,
`TrackRepository` with FTS5, `PlaylistManager` (folders/static/smart
rule storage), history, duplicates, missing files, watcher, and
transactional migrations. The Phase-18 spec asks for the complete
professional surface: token search, accent-insensitive indexing,
*evaluated* smart playlists, named crate/tag/rating surfaces, key
notations, backup/export/import, diagnostics, user-edited metadata
protection, playability states, and a 100k-track performance story.

## Decisions

1. **Extend the Phase-7 stack; no parallel library.** New named
   components are thin facades over the shared repo/managers
   (`CrateManager` → `PlaylistManager`, `TagManager`/`RatingManager`
   → `TrackRepository`, `LibraryDiagnostics` → read-only rollups).
   One DB mutex, one FTS index, one worker — zero duplication.

2. **Folded text in the FTS index, not a second index.** `TextFold`
   (ASCII lower + Latin diacritic fold) applies at index time in
   `ftsUpdate`/`reindexFts` and to query tokens in `matchExpr`, so
   accent-insensitivity has no extra storage and no query-time fold
   of the corpus. Migration v5 rebuilds `tracks_fts` (adds
   label/composer columns); `kv.fts_folded` drives a one-time bulk
   reindex on `open()`.

3. **Smart playlists evaluate the query — never rows.** Rules stay
   JSON on `playlists.rules`; `SmartPlaylistEngine` compiles them to
   whitelist SQL (`FilterEngine::clause`) and runs
   `TrackRepository::queryWhere`. AND/OR via top-level `match` +
   nested `groups`; NOT via per-leaf `negate`. Membership therefore
   follows data changes with zero maintenance work.

4. **Nested booleans need a raw WHERE — kept whitelisted.**
   `QuerySpec.filters` is a flat AND list; groups need `(a OR b)`.
   Rather than widen the spec shape, `queryWhere` accepts a
   pre-built WHERE produced *only* through `FilterEngine::clause`
   (the same whitelist), preserving the no-injection contract.

5. **`edited_fields` bitmask protects user edits.** §8 demands user
   metadata survive rescans. One integer column marks which
   file-metadata fields the user touched (`setMetadataFields` OR-s
   the mask); `TrackImporter` seeds the reimport `Track` with old
   values and only refreshes unmasked fields. Twelve maskable fields;
   user columns (rating/color/tags/comments/playlist membership)
   never pass through the importer at all.

6. **Playability is orthogonal to presence.** §6 wants
   "UNSUPPORTED FORMAT" separate from "metadata read but not
   decodable". `play_state` (Playable/UnsupportedFormat/
   MetadataOnly) lives beside `status` (Available/Missing/Relocated/
   Invalid) — a file can be present-but-unsupported or
   missing-and-supported; enums can't express both in one field.

7. **Crates are playlists, nesting is folders.** The schema already
   had both (`playlist_items.position`, `folders.parent_id`); the
   spec's crate concept maps 1:1. `CrateManager` gives crates their
   own vocabulary + a depth cap (8) instead of a parallel table set
   (`crates`/`crate_tracks`) that would duplicate everything.

8. **64-bit ids serialize as decimal strings in JSON.** The first
   export round-trip proved JSON doubles truncate `content_hash`
   (53-bit mantissa) and silently break hash matching — strings are
   exact and version-tolerant (`u64val` accepts numbers too).

9. **MiniJson, not a dependency.** Smart-playlist rules and backup
   need tolerant JSON both ways; the hand-rolled parser is ~250 LOC,
   keeps insertion order (diffable exports), and avoids a new third-
   party for two internal formats.

10. **Search tokens stay small.** §13's examples are implemented
    (field:value, ranges, `+`, `>`/`<`, quotes, any-notation keys);
    no boolean infix grammar — `AND/OR/NOT` lives in smart-playlist
    rules where it belongs.

## Consequences

- Every library op remains off the audio path; heavy work rides the
  existing single worker (`post`/`importPathsAsync`/`scanAsync`).
- One migration (v5) carries all schema delta; existing DBs upgrade
  transactionally, FTS repopulates in a single bulk pass.
- 100k-track synthetic benchmark: FTS ~24 ms, filter+sort ~30 ms,
  smart eval ~55 ms — interactive; deep-offset sorted paging ~1 s is
  the documented worst case.
- Browser search box gained structured tokens transparently
  (`SearchQuery::toSpec` in `applySpec`).
