# Music Library Engine (Phase 18)

Professional library for large collections — extends the Phase-7
foundation into the complete spec surface: field-token search,
accent-insensitive FTS, evaluated smart playlists, named crate/tag/
rating managers, key notations, backup/export/import, diagnostics,
and user-edited metadata protection.

## Architecture

```
MusicLibraryEngine            (one worker thread, never audio)
├── TrackRepository           CRUD + indexed queries + FTS + integrity
├── TrackImporter             file/folder import, meta provider, §5 pipeline
├── LibraryScanner            registered roots → ScanDiff
├── PlaylistManager           folders + static playlists + smart playlists
├── CrateManager     (new)    crates = static playlists + nested folders (§16-17)
├── SmartPlaylistEngine (new) rules JSON → whitelist WHERE → live eval (§20-22)
├── TagManager       (new)    global tags: counts/rename/merge/delete (§23)
├── RatingManager    (new)    0–5 stars / color / comments / energy (§24-26)
├── SearchQuery      (new)    field:value tokens → FilterCond + FTS (§13)
├── KeyNotationConverter(new) Standard/Camelot/OpenKey + parse (§43)
├── TextFold         (new)    accent+case folding for index & queries (§11)
├── LibraryBackup    (new)    JSON export/import + CSV, conflict policies (§48-50)
├── LibraryDiagnostics (new)  counts, db size, integrity, report (§ diag)
├── HistoryManager            plays/sessions/count rules
├── DuplicateDetector         content-hash / size+duration / metadata
├── MissingFileManager        detect/list/relocate/verify
├── FileWatcher               debounced folder watch → worker rescan
└── Database                  SQLite WAL, FK on, migrations v1..v5
```

All managers share one `Database` recursive mutex; any thread may call
the synchronous APIs (short indexed queries); heavy work posts to the
worker. **Nothing here runs on the audio callback.**

## Schema (migration v5)

```sql
tracks      id, path UNIQUE, fingerprint, content_hash, file_size, mtime,
            status(0 avail,1 missing,2 relocated,3 invalid),
            title, artist, album, album_artist, genre, year, track_num,
            disc_num, duration_sec, bitrate, channels, sample_rate,
            format, artwork_hash, label, composer,        -- v5
            play_state(0 ok,1 UNSUPPORTED,2 META-ONLY),   -- v5, §6
            edited_fields (bitmask, §8),                  -- v5
            rating, color, energy, comments,
            play_count, last_played, date_added,
            analysis_state, analysis_version, error, updated_at
analysis    track_id PK → bpm/key/loudness/transients/needs_review…
beatgrids   track_id PK → segments JSON, manual flag
waveforms   track_id PK → overview/detail/mid blobs
user_overrides  track_id PK → user bpm/key/grid (kept separate)
cues, saved_loops, kv, tags, track_tags,
folders     id, parent_id (nested, cascade)               ← crates nest here
playlists   id, name, folder_id, kind(0 static,1 smart), rules JSON
playlist_items  playlist_id, position, track_id (PK playlist+track)
sessions, history(track, session, deck, started_at, played_sec, counted)
tracks_fts  FTS5 contentless: title artist album genre
            label composer filename tags comments         ← v5 adds 2 cols
```

Indexes on title/artist/genre/rating/date_added/status/content_hash/
label + analysis.bpm/key + playlist_items(playlist,position) +
history(track,session).

## Track identity (§3)

`fingerprint` = file size + mtime + content sample (cheap, changes on
edit). `content_hash` = content only (survives rename/move). Identity
is never filename alone — `TrackImporter` keys rows on canonical path,
`MissingFileManager` verifies relinks by content hash, duplicates are
detected by content hash + size/duration + metadata similarity.

## Import pipeline (§5)

```
File → validation (exists, extension list isAudioFile)
     → fingerprint + content hash
     → skip when unchanged
     → metadata provider (FFmpegMetadataReader: title/artist/album/
        album_artist/genre/year/track/disc/label/composer/comment +
        stream info + artwork marker)
     → upsert (preserves user fields; honours edited_fields mask)
     → FTS row update (folded text)
     → analysis hook (optional AnalysisService enqueue)
     → play_state: Playable | UNSUPPORTED FORMAT | METADATA ONLY (§6)
```

`importFile/importFiles/importFolder` synchronous-batch; the engine's
`importPathsAsync`/`post` run them on the worker for UI/drag&drop.

## User metadata vs file metadata (§8)

User fields (rating/color/energy/comments/tags/playlist membership/
cues/overrides) live in separate columns/tables and never come from
the file. `edited_fields` marks which *file-metadata* fields the user
overrode via `setMetadataFields`; reimports skip masked fields —
file metadata refreshes only unedited columns (title/artist/album/
album_artist/genre/year/track/disc/label/composer). A file comment
fills empty user comments only, never overwrites.

## Search (§11-13)

- **FTS5** contentless index over folded text (TextFold: ASCII
  lower + Latin diacritic fold + combining-mark drop — pragmatic,
  not full NFKD). Index and query tokens fold identically →
  `beyonce` = `Beyoncé`, `ECOUTER` = `écouter`.
- **Tokens** (`SearchQuery`, wired into the browser search box):
  `artist:x album:"y z" genre:g title:t label:l comment:c tag:t
  filename:f path:p color:c bpm:120-128 bpm:>120 bpm:<130 year:a-b
  rating:4+ plays:n duration:s key:Am|8A|1d missing:1 analyzed:1`
  Unknown tokens stay in the FTS text. Tokens map to the FilterEngine
  whitelist — no SQL injection path.
- Plain free text → `MATCH` over prefix terms AND-ed.

## Filters / sort / page (§14-15)

`FilterCond{field,op,text,a,b,ints,negate}` — fields: title artist
album album_artist genre filename comments format label composer path
bpm key key_index rating color energy duration year track_num
date_added play_count last_played file_size status analysis_state
play_state analyzed missing waveform beatgrid needs_review tags
key_compatible. Ops: eq neq lt lte gt gte contains starts_with between
in not_in isnull + per-condition `negate`. Sort: the 13 SortKey
columns. Every query is `WHERE whitelist + ORDER BY + LIMIT/OFFSET`
with a separate `COUNT(*)` for `TrackPage.total` — pages never load
the whole library (virtualized browsing contract).

## Crates & playlists (§16-19)

- Static playlists & crates: `playlist_items(playlist_id, position,
  track_id)` — explicit ordering, `moveTrack`/`setOrder` transactions.
- `CrateManager` = named facade: crates are static playlists;
  `createCrateFolder` nests under `folders` (depth cap 8); delete
  removes membership only — files and tracks are untouched.
- Folders: `parent_id` tree with `ON DELETE CASCADE`/re-parent rules.

## Smart playlists (§20-22)

Rules stored as JSON on `playlists.rules`:

```json
{"match":"all"|"any",
 "rules":[ {"field":"bpm","op":"between","a":120,"b":128},
           {"field":"genre","op":"eq","text":"House"},
           {"field":"bpm","op":"gt","a":135,"not":true} ],
 "groups":[ {"match":"or","rules":[…]} ]}
```

Ops: eq neq lt lte gt gte contains starts_with between in not_in
isnull + `not` per leaf. `SmartPlaylistEngine::evaluate` turns rules
into a whitelist `WHERE` (FilterEngine::clause) and runs
`TrackRepository::queryWhere` — evaluation is live, so membership
follows data changes; `PlaylistManager::tracks/trackIds` dispatch to
it automatically for `kind=Smart`.

## Tags / ratings / colors (§23-26)

- `TagManager`: `tagsWithCounts`, `rename` (name collision → merge:
  memberships re-point, old row deleted), `remove`, `merge`,
  `addToTracks` bulk; FTS tag text refreshed after global ops.
- `RatingManager`: `setRating` 0–5 clamped, `setColor` ARGB,
  `setComments`, `setEnergy` 0–10, bulk variants; palette names
  none/red/orange/yellow/green/blue/purple/pink/gray.

## Status surface (§35-37, §6)

`Track` exposes `analysisState` (NOT_ANALYZED/ANALYZING/ANALYZED/
FAILED via `analysisStateText`), `needsReview`, `hasBeatgrid`,
`hasWaveform` (EXISTS on beatgrids/waveforms), `playState`
(`playStateText` → `UNSUPPORTED FORMAT`/`METADATA ONLY`) and
`status` (missing/relocated/invalid). Helper texts are the canonical
UI strings.

## Backup / export / import (§48-50)

`LibraryBackup`:
- `exportJson(path)` — `{format:"onda-library",version:1,tracks:[…],
  folders:[…],playlists:[{name,kind,rules|items:[{content_hash,path}]}]}`;
  64-bit ids serialized as decimal strings (doubles lose precision).
- `exportCsv(path)` — flat metadata table, RFC-4180 quoting.
- `importJson(path, Skip|Merge|Replace)` — tracks matched by
  content_hash then path; user fields applied per policy (Merge keeps
  local non-zero values); playlists created if missing, items append
  (Replace clears first), smart rules replaced on Replace. Never
  deletes rows or files. Audio files are never bundled.

## Diagnostics (§ diag)

`LibraryDiagnostics::snapshot()` → counts (tracks/missing/invalid/
unsupported/metaOnly/analyzed/pending/failed/needsReview/beatgrid/
waveform/tags/playlists/smart/folders/history/ftsRows) + db file
size; `report()` → one-line text + `integrity_check` +
`foreign_key_check`.

## Failure recovery (§52)

- Every multi-row write is a `Database::Tx` (BEGIN IMMEDIATE …
  COMMIT, RAII rollback) — interrupted imports/scans/migrations leave
  no partial state; WAL + `synchronous=NORMAL` survive process kill.
- Migrations are transactional and versioned via `schema_migrations`;
  v5's FTS drop/recreate is repopulated by the `fts_folded` kv flag
  reindex on next `open()`.
- Duplicate rows are impossible (`path UNIQUE`, PK on
  playlist_items/track_tags); relinks verify content hash.

## Performance (measured, synthetic)

100k tracks seeded in ~2.9 s (~40 MB db):
fts search ~24 ms · token search ~230 ms · filter+sort page ~30 ms ·
page 500/1000 sort ~1.03 s · smart eval ~55 ms · 5k playlist add ~14 ms ·
playlist page ~26 ms · diagnostics ~86 ms · integrity ~1.09 s.
The paged-sort full COUNT is the slowest op by design (count is
exact); everything else is comfortably interactive at 100k.

## Known limits

- TextFold covers common Latin diacritics only (CJK/Cyrillic pass
  through unchanged — searchable verbatim, not folded).
- Sort=page-500 latency grows linearly (SQLite OFFSET); acceptable
  for virtualized scrolling which pages forward.
- `play_count`/`last_played` are not re-synthesised on library import
  (they derive from history rows).
- Tag/playlist names collide NOCASE; rename-to-existing merges.
- Artwork manager exists (lazy disk cache via `artworkPath`); no
  in-memory thumbnail pyramid yet.
- File watcher coverage is debounced rescan-on-change; OS-level
  recursive watching depth is platform-bounded.
