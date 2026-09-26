# Session Recording (Phase 12)

Signal path:

```
MixerEngine.render
  → master bus (limiter/comp applied)
  → RecorderRt.pushPostMaster         (audio thread: bounded copy + atomics)
  → SpscRing (1<<20 frames ≈ 8s @48k, ~8 MB, preallocated)
  → RecordingWriter thread            (8192-frame chunks → WavEncoder → disk)
  → Session_YYYY-MM-DD_HH-MM-SS.wav  (+ .journal + .meta.json sidecars)
```

The audio callback does NO allocation, locking, filesystem, or logging
for recording — `pushPostMaster` returns in one atomic load when not
capturing, and writes are bounded interleaved copies into the ring.
Writer/disk/journal/database work happens off the audio thread.

## Components (`src/core/rec/`)

| file | role |
|---|---|
| `RecordingTypes.h` | `Source` (Master live; Channel/Cue/External/Mic/Sampler reserved), `State`, `BitDepth` (Pcm16/Pcm24/F32), `ClipEvent`, `Meter`, `SessionInfo`, `TrackPlay`, `Diagnostics` |
| `RecordingBuffer.h` | `SpscRing` (lock-free SPSC, pow2, drops counted never blocked) + `RecorderRt` (block/session peaks, running RMS, clip events, dropped frames, fill %) |
| `RecordingEncoder.h` | `WavEncoder`: 44-byte header patched on finalize; PCM16/24 (tag 1), float32 (tag 3); `validate()`; `patchHeader()` repairs only the two size fields in place; `kMaxWavData` 3.8 GB → auto-split |
| `RecordingSession.h` | `Journal` (append-only crash-safe event log, `fflush` per event), `makeWavPath` (safe names + collision suffixes), `estimateBytes`, `diskFreeBytes` |
| `RecordingWriter.h` | writer thread: ring → staging → encoder; split requests handled on its own thread (no frame gap); write-latency/throughput atomics; finalize + parts list |
| `RecordingRepository.h` | sqlite on `onda.db`: `recording_sessions`, `recording_files`, `recording_events`, `session_tracks`, `recording_recovery` (idempotent `init()`) |
| `RecordingRecovery.h` | `scan()` (journal without `finalize_completed`/`recovered`/`discarded`), `recover()` (in-place header patch — audio untouched), `discard()` (removes partial WAV, marks journal) |
| `RecordingExport.h` | session metadata JSON + CSV/TXT setlist builders |
| `RecordingManager.h` | session list/rename/delete/export; never deletes audio files |
| `RecordingEngine.{h,cpp}` | façade: state machine, arm/start/pause/resume/stop/split/toggle, tick(), deck-play setlist edges → `session_tracks` + `HistoryManager.recordPlay` |

## State machine

```
IDLE ──arm──> ARMED ──start──> RECORDING ──pause──> PAUSED
 │              │disarm          │  │                 │  │
 │              └──────────────> │  │<──resume────────┘  │
 │                               ▼  ▼                    ▼
 └──quick start─────────────────>│  └──────────> STOPPING ──> FINALIZING
                                 │                            │
        FAILED <──any live───────┴◄──────── COMPLETED ◄───────┘
```

`canTransit()` enforces the table; `Completed`/`Failed`/`Recoverable`
are terminal per file — a new run is a new session.

## Pause semantics

`pause()` disarms the RT tap — frames simply stop flowing into the
ring (no silence written, no gap accounting needed). Duration derives
from frames/rate, so paused time is naturally excluded.

## Splitting

`split()` (Ctrl+Shift+R or right-click on the REC widget) rotates the
WAV under the writer thread: current file finalizes, a `_pN` file
opens, the ring keeps buffering — no frames lost. Auto-split fires at
`kMaxWavData` (3.8 GB payload) before the RIFF 4 GiB limit.

## Recovery

Every event is journaled to `<file>.journal` with an `fflush` per
line. On boot `scanRecovery()`:

1. marks `recording_sessions` rows left in a live state
   (`ARMED..FINALIZING`) as `RECOVERABLE` in the DB;
2. scans the recording dir for journals missing a terminal line →
   `RecordingRecovery.Item` with the event list for Inspect.
   `recover()` patches only the RIFF/data size fields to the real
   on-disk payload — the audio bytes are never rewritten. `discard()`
   removes the partial WAV and marks the journal `discarded` (audit
   trail kept).

## Setlist + history

`tick()` watches deck play edges (`Ports.deckPlaying`/`deckPath`):
each play-start writes a `session_tracks` row (deck, ts, path,
title/artist/BPM/key/trackId from the library); play-stop or
stop() fills `played_sec` and calls `HistoryManager.recordPlay` —
recording history and library play history share the same rows'
timestamps. Mid-play track swaps close the previous row first.

## Disk handling

- `arm()` refuses without a writable dir or ~10 min of headroom.
- `tick()` polls free space ~1 Hz; warns under ~1 min remaining.
- Writer throughput vs the ~0.29 MB/s need → `diskWarn` → toast.
- Write failure → `Failed` state + journal `error` line; the
  unfinalized file is recoverable on next boot.

## Commands / UI

`rec.arm|start|stop|pause|resume|split|toggle` (0x7000 space) via
`CommandManager` — UI, keyboard (Ctrl+R toggle, Ctrl+Shift+R split),
MIDI/HID share them. `RecordingView` sits in the TopBar: REC dot
(red/amber/blink), elapsed, state label, mini stereo meter,
CLIP/DROP/GB-free line. Quick Record = `rec.toggle` → audio check →
disk check → session → rolling, one step.

## Tests (`tests/recording_tests.cpp`, 19 cases)

state table, SPSC roundtrip/overflow, RT metering/clips, WAV
16/24/32-bit headers + 24-bit payload roundtrip, filename safety,
disk space, full lifecycle (pause excludes frames), invalid dir,
split frame accounting, CommandManager quick record, repo + setlist +
HistoryManager link, crash→scan→recover→validate→discard, writer
drain, known-signal equivalence (err < 1.5e-6 @24-bit), and a
4-hour-equivalent 48 kHz stereo 24-bit stream: 691.2M frames, ~4.15 GB,
auto-split into 2 files, zero dropped frames, bounded memory.

## Limitations

- Only the Master tap is wired; Channel/Cue/External/Mic/Sampler are
  enum-reserved (RecordingInput abstraction exists conceptually via
  `Source` + the single tap point).
- Mono recording not exposed (stereo only, matching the master bus).
- Recovery UI is a boot-time notification + API — no recover/inspect
  dialog yet; setlist export is API-only (JSON/CSV/TXT via
  `RecordingManager.exportSession`).
- `RecordingExporter` (FLAC/transcode) is deferred — WAV only.
- Clip events journal to `recording_events` only when a repo exists.
