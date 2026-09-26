# ADR 0012: Recording Engine + Session Management

## Decision

Session recording is a first-class engine (`src/core/rec/`) with a
hard thread split: an audio-thread tap performs bounded copies into a
lock-free SPSC ring, and a dedicated writer thread owns all file IO,
conversion, journaling, and diagnostics.

```
Master bus (post-limiter)
  → RecorderRt.pushPostMaster     (RT: meter + clip + ring push)
  → SpscRing (pow2, ~8 s @48 kHz)
  → RecordingWriter thread        (8 192-frame chunks → WavEncoder)
  → WAV + .journal + .meta.json
```

## Key choices

- **Tap after `MasterBus.process`**: the recorded program matches what
  reaches the hardware outputs (limiter included); the tap is
  read-only and cannot alter playback. Sources beyond Master are
  enum-reserved for future taps (cue bus, channel strips, external
  ins, mic, sampler bus).
- **Drop-counted overflow, never blocking**: a full ring drops the
  remainder of that block and increments `droppedFrames` — the
  alternative (grow/lock/sleep) is RT-unsafe. The ring (~8 s) plus
  paced writer drain makes drops a failure symptom, not a design
  feature.
- **Placeholder-header WAV**: header sizes patched at finalize;
  recovery patches only the two size fields in place so audio bytes
  are never rewritten. 24-bit PCM is the default (masters-grade,
  half the size of float32); F32 and 16-bit selectable.
  `kMaxWavData` (3.8 GB) triggers a seamless auto-split before the
  4 GiB RIFF ceiling.
- **Journal sidecar for crash detection**: `fflush`ed event lines —
  `started/chunk/paused/resumed/split/stopped/finalize_started/
  finalize_completed/error/clip`. A journal without a terminal line
  (`finalize_completed|recovered|discarded`) is `RECOVERABLE`.
  `recovered`/`discarded` are written back so scans converge.
- **Setlist lives in `session_tracks` + `HistoryManager`**: the
  recorder reuses the existing performance-history path rather than
  duplicating it — deck-play edges feed both the session row and
  `HistoryManager.recordPlay` with identical timestamps.
- **Commands, not buttons**: `rec.*` commands through
  `CommandManager`; the TopBar `RecordingView` paints
  `RecordingSnapshot` values only.

## Failure semantics

Config/disk problems before capture → `Failed` (terminal; arm again
to retry). Writer death mid-session → `Failed` + journal `error`;
the partial file is recoverable next boot. Process kill mid-write →
journal lacks completion → `scanRecovery()` marks DB rows
`RECOVERABLE` and returns repairable items.

## Alternatives rejected

- Streaming through `SampleRecorder` (Phase 11): too small — no
  state machine, sessions, journal, or recovery; kept as the sampler's
  foundation.
- Blocking write in the callback: violates RT rules outright.
- Per-session worker pools: one writer thread per session is
  sufficient; splits rotate the file inside the same thread.
