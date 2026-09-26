# ADR-0007 — Professional Music Library Engine

Status: accepted (Phase 7)
Date: 2025

## Context

A DJ library must manage tens of thousands of tracks — import,
analysis, search, playlists, history — while the audio engine keeps
rendering at callback rate. All library work must happen off the
audio thread, off the UI thread for heavy ops, and every write must
be transactional so a crash mid-import/scan never leaves corrupt
rows.

## Architecture

```
MusicLibraryEngine (facade, owns worker thread)
├── Database            SQLite WAL, recursive mutex, RAII Tx, v3 schema
├── TrackRepository     CRUD, FTS sync, query(filter+sort+page), counts
├── TrackImporter       file/folder/recursive import, transactional
│                       fingerprint → dedup → metadata → upsert → analyze
├── LibraryScanner      new / missing / modified / moved detection
├── MetadataReader      FFmpeg avformat (platform/metadata/), no file writes
├── SearchEngine        FTS5 match-expr builder ("daft pun" → "daft"* AND "pun"*)
├── FilterEngine        FilterSpec → parameterized WHERE (BPM range, key, …)
├── PlaylistManager     static playlists, explicit order, folders/crates,
│                       smart playlists (rule text re-evaluated per query)
├── HistoryManager      sessions, entries, play-count rule, async writes
├── DuplicateDetector   fingerprint / content-hash / size+duration+meta
├── MissingFileManager  AVAILABLE / MISSING / RELOCATED / INVALID, relink
└── FileWatcher         polling foundation (debounce, fingerprint check)
```

Threading contract:

- The audio thread never touches anything in `core/library/`.
- Queries are synchronous, indexed, and hold the DB mutex only
  briefly — safe for the UI thread.
- Heavy ops (import, scan, history writes) run on the single library
  worker via `post()` / `*Async()` entry points.
- All managers share one guarded `sqlite3*` (recursive mutex +
  single-writer WAL). `Database::Tx` gives nested-transaction
  semantics (SAVEPOINT stack).

## Schema v3

`tracks` gains the full DJ model: filename, album, album_artist,
genre, year, track/disc numbers, bitrate, channels, format,
file_size, mtime, content_hash, artwork_hash, rating, color, energy,
comments, play_count, last_played, date_added, status.

New tables:

- `tags` / `track_tags` — many-to-many, CASCADE both ways
- `folders` — nested crates (parent_id CASCADE)
- `playlists` (kind: 0 static, 1 smart, rules text) + `playlist_items`
  (explicit `position`, PK (playlist_id, track_id), CASCADE)
- `sessions` / `history` — `history.session_id ON DELETE SET NULL`
  (deleting a session keeps entries, detached), `track_id CASCADE`
- `tracks_fts` — FTS5 unicode61 over
  title/artist/album/genre/filename/tags/comments, `content=''`
  (external content; repository owns row sync on upsert/remove)

Indexes: title, artist, genre, rating, date_added, status,
content_hash, analysis.bpm, analysis.key_index, history(track_id),
history(session_id), playlist_items(playlist_id, position).

## File fingerprint

`FileFingerprint::compute(path)` produces two hashes:

- `fingerprint` — size + mtime + FNV-1a over head/middle/tail
  samples. Detects *modification*: same path, new bytes → new fp.
- `contentHash` — same sampling but **mtime-free**. Identical
  content hashes identically regardless of rename/move/touch.

This separates the two questions the scanner asks:

| file state          | fingerprint | contentHash |
|---------------------|-------------|-------------|
| same file, unchanged| same        | same        |
| modified in place   | differs     | may differ  |
| moved/renamed       | n/a (path)  | same        |
| touched only        | differs     | same        |

Content-identical files at two paths are *indistinguishable* by
design — if one vanishes the scanner relinks to the survivor, which
is the desired semantic for a true move and a documented edge for
genuine duplicates (DuplicateDetector reports them for user choice
instead).

## Import flow

```
importFile(path)
  → supported extension?  (wav/wave/aif/aiff/aifc/flac/mp3/m4a)
  → fingerprint + contentHash
  → existing by path?      unchanged → skip | modified → re-import
  → existing by contentHash?          → report duplicate path, relink
  → MetadataReader (FFmpeg, read-only)
  → Tx { upsert track; FTS sync }     — ROLLBACK on any failure
  → AnalysisService.enqueue(id)       — pending state on the row
```

`importFolder` recurses through the same per-file path; batching
keeps each track its own transaction so one bad file can't corrupt
the batch.

## Scanner semantics

`scan()` fingerprints every audio file under the roots, diffs
against rows whose path is under a root:

- path match + same fingerprint → unchanged
- path match + different fingerprint → `modified` → re-import +
  re-analysis
- path match + status MISSING → restored to AVAILABLE
- row missing on disk + contentHash found at another path →
  **relocated** (setPath + updateFileIdentity, same track id —
  user data survives a move)
- row missing on disk, no hash match → **MISSING** (row kept)
- unseen file → new → importer

Nothing is ever deleted by the scanner.

## Search / filter / sort

`QuerySpec` = free-text `search` + `filters[]` + `sort` + page.

- Search → `tracks_fts MATCH '"tok"* AND …'` (each user token is
  quoted + prefix — syntax chars sanitized, no injection).
  Tokens match in **any** indexed column ("artist 499" hits both
  `Artist 499` rows and `Track 499xx` titles — verified 199/50k).
- Filters → parameterized WHERE via FilterEngine: bpm (eq/range),
  key (+ Camelot-compatible `keyCompat`), genre/artist/album
  (exact, NOCASE), rating/color/duration/year/date/play_count
  (eq/gte/between), analyzed, missing, tags (join on track_tags,
  LIKE), comments.
- Sort → indexed columns only + deterministic tie-break by id.
- Pagination → LIMIT/OFFSET with `total` from a COUNT of the same
  WHERE — the UI virtualizes; the full library is never loaded.

## Playlists, crates, smart playlists

- Static playlist: ordered `playlist_items` rows; reorder/move are
  position rewrites inside a transaction; positions stay dense.
- `duplicate()` copies name + folder + items.
- Folders nest (parent_id); playlists live in a folder; a track may
  be in any number of playlists (rows, not files).
- Smart playlist stores FilterSpec JSON in `playlists.rules`;
  `items(smart)` re-runs the query — always current, zero copies.
- Track deletion cascades items; missing tracks stay listed
  (playlist shows status, never corrupts).

## History & play count

`recordPlay(track, deck, startedAt, playedSec, session)` inserts a
history row; `counted` is decided by `PlayCountRule`
(default: played ≥ 60 s **or** ≥ 50 % of track, whichever is less —
loading alone never counts). Counted entries bump
`play_count`/`last_played` in the same transaction.

`recordPlayAsync` enqueues onto the library worker — playback is
never blocked by SQLite.

## Duplicates

`DuplicateDetector` groups candidates by (a) identical contentHash,
(b) equal size+duration+artist+title heuristics. It returns
`DuplicateGroup` lists for the UI — it never deletes or merges
anything.

## Formats & licensing

Decoder/metadata: **FFmpeg** (libavformat/libavcodec), already the
project decoder since Phase 2.

| format  | status     | notes                                  |
|---------|------------|----------------------------------------|
| WAV     | supported  | PCM, free                              |
| AIFF    | supported  | PCM, free                              |
| FLAC    | supported  | free codec                             |
| MP3     | supported  | patents expired; decoders free         |
| AAC/M4A | supported  | decodes via FFmpeg build; patent pool —<br>redistribution notices kept in 3rd-party docs |
| others  | extension-gated | importer whitelist only — anything<br>FFmpeg demuxes can be added to the list |

FFmpeg is linked per the existing vendored/shared setup; its LGPL/
GPL obligations and codec-patent notices apply to redistribution
(documented in `third_party/` notes). Metadata reads are read-only —
embedded metadata and user metadata are separate columns/tables;
the app never writes tags back to audio files.

## Crash recovery / integrity

- WAL + synchronous=NORMAL: a killed process replays/rolls back the
  journal on next open — no torn writes.
- Every multi-statement write is wrapped in `Database::Tx`.
- `foreign_keys=ON` + CASCADE/SET NULL as designed above: no orphan
  items, no dangling playlist/history/tag rows.
- `integrityCheck()` runs `PRAGMA integrity_check` plus orphan
  sweeps (playlist_items/history/track_tags without parents, FTS
  rows without tracks).
- Import crash → at worst a track row; scanner/restore completes or
  reverts it — never a partial file+track+meta combo.

## Verification (this phase)

- 18 library test cases, 923 assertions — all pass.
- Full suite: 134 cases / 1,593,164 assertions — all pass.
- 50k synthetic tracks: seed 1.2 s; FTS search 15 ms;
  genre+bpm+rating filter+sort+page 30 ms; artist sort 24 ms;
  5000-item playlist add 12 ms; DB 17.5 MB.
- App smoke: audio device opens, runs ≥ 5 s under timeout.
- Library never calls into audio code; verified by architecture
  (no audio-header includes in `core/library/`).

## Known limitations

- FileWatcher is a polling foundation (debounced rescan), not yet
  platform-native ReadDirectoryChanges/FSEvents — the interface is
  stable for that swap.
- Artwork extraction is provider-based + disk-cached; no thumbnail
  downscale yet (UI phase will add size hints).
- Backup/export/restore: schema + `kv` settings + integrity tools
  are the foundation; a `VACUUM INTO` / file-copy export helper is
  the remaining step before the UI phase needs it.
- Metadata reader covers the common tags + artwork presence;
  exotic tag formats normalize through FFmpeg's key names.
- Smart-playlist rule editor UI is deferred; rules are JSON today.
- Analysis-state mirroring depends on `AnalysisService` wiring in
  the app shell (setAnalysisService); without it rows stay PENDING.
