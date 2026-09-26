# Onda (codename)

Professional desktop DJ application. C++20 + JUCE 8, GPLv3.

## Status — Phase 8 (UI framework + live-performance views)

Two-layer UI: `src/ui/` is a **JUCE-free core** (commands, shortcuts,
snapshots, theme, waveform math, virtual table model, layout math —
all unit-tested without a window) and `src/app/ui/` holds the JUCE
views that render only from plain-value snapshots.

- `CommandManager` + `CommandIds`: one stable command surface for
  every input — UI, keyboard, and later MIDI/HID invoke identical
  handlers. `EngineCommands` registers ~120 commands once against
  ports (rt `CommandQueue`, `MixerController`, `PerformanceEngine`,
  `MusicLibraryEngine`, deck stats).
- `ShortcutManager`: named-key grammar (`Ctrl+Shift+A`, `F1`, `Space`),
  default DJ profile, remappable + serializable.
- `EngineBridge` is the only engine gateway: builds `UiSnapshot` from
  atomics, feeds edge-triggered `NotificationCenter` toasts (xruns,
  clip deltas, device loss, analysis events).
- `AppShell`: TopBar + deck/waveform row + mixer + fx strip + browser;
  `LayoutSpec` computes regions with collapse priorities (browser →
  fx → wave strip shrink; transport never hides), 1280×720 … 4K.
- `WaveformView`: overview + detail from cached `AnalysisBundle`
  bins (never recomputes audio), grid markers (beat/bar/downbeat),
  cue/loop overlays, zoom-at-anchor, scroll, seek.
- `DeckView` (info/transport/perf toggles/pitch/loop/8 cue pads),
  `MixerView` (gain/EQ/filter/fader/PFL/meters/xf + master),
  `FXView` (generic racks), `BrowserView` (search + rail +
  virtualized table), `SettingsView` (12 sections incl. controller
  placeholders).
- `ThemeEngine`/`DesignTokens`: pro dark theme, accent, density,
  per-deck waveform colors; `StateButton` encodes state by
  shape + label + fill, not hue alone.
- UI-FPS + frame time measured live in the TopBar.

Tests: +14 `[ui]`/`[stress]` cases (148 total, 1.59M assertions) —
command dispatch, shortcut grammar, theme roundtrip, paged model
over 50k, waveform math, notifications, layout priorities, and a
stress run: 600 frames over 50k rows avg 1.0 ms/frame
(worst 4.8 ms) — no DSP in UI code, engine writes only via
lock-free command push / non-RT facades.

## Status — Phase 7 (professional music library)

`core/library/` — `MusicLibraryEngine` facade owning the SQLite
database (schema v3, WAL, recursive mutex, RAII nested transactions)
and a dedicated library worker: the audio thread never touches it,
heavy ops are async, queries are indexed + paged so the UI never
loads the whole library.

- `TrackRepository`: full DJ track model (identity, embedded meta,
  analysis refs, user meta), CRUD, FTS5 sync, `QuerySpec` =
  search + filters + sort + page + total.
- `FileFingerprint`: `fingerprint` (size+mtime+sampled FNV-1a →
  change detection) vs `contentHash` (mtime-free → move/duplicate
  identity) — the scanner separates "modified" from "moved".
- `TrackImporter`: files/folders/recursive; fingerprint → dedup →
  FFmpeg metadata → transactional upsert → analysis queue. Batch
  import keeps one transaction per track.
- `LibraryScanner`: new / modified / missing / relocated detection;
  vanished rows become `MISSING` (never deleted), content-hash
  match at a new path relinks the same track id.
- `SearchEngine` (FTS5, quoted prefix tokens) + `FilterEngine`
  (parameterized WHERE: bpm/key/genre/rating/dates/tags/missing/
  analyzed, Camelot-compatible key matching) + indexed sorting.
- `PlaylistManager`: static playlists with explicit order, folders/
  crates (nested), smart playlists re-evaluated from rule JSON.
- `HistoryManager`: sessions, entries, configurable play-count rule
  (≥60 s or ≥50 %, loading never counts), async writes.
- `DuplicateDetector` (hash + size/duration/meta candidates — never
  deletes), `MissingFileManager` (relink/locate),
  `FileWatcher` (polling foundation), artwork provider + disk cache.
- `platform/metadata/FFmpegMetadataReader`: read-only tags/stream
  info; embedded vs user metadata stay separate. Formats:
  WAV/AIFF/FLAC/MP3/M4A (FFmpeg decode — see ADR-0007 licensing).
- Integrity: FK cascade/SET NULL everywhere, `integrityCheck()`,
  WAL crash recovery — no orphans, no partial imports.

Tests: +18 `[lib]` cases (134 total, 1.59M assertions) — import,
fingerprint, search/filter/sort, playlists/smart, history, tags,
missing/relink/move, duplicates, transactions, worker concurrency,
and a 50k-track perf suite (FTS 15 ms, filter+sort+page 30 ms,
DB 17.5 MB).

## Status — Phase 6 (professional FX engine)

`core/fx/` — modular effects engine on top of the mixer: insert racks
per deck/channel, two send/return buses, a master insert rack and
beat-synced time effects. The engine only drives the `FXProcessor`
contract — every effect is swappable off-RT and every parameter is
smoothed per-sample.

- `FXProcessor` + `FXBase<N>`: prepare/process/reset, native +
  normalized params, sync context, latency, save/restore state.
- Effects: `DelayFX` (free/synced time, feedback, damping filter,
  ping-pong), `EchoFX` (DJ echo: divisions, tone, ping-pong, bounded
  freeze), `ReverbFX` (Schroeder: 8 combs + 4 allpasses per channel,
  size/decay/damping/pre-delay/width), `FilterFX` (TPT SVF LP/BP/HP,
  resonance, LFO or beat-synced sweep), `FlangerFX`, `PhaserFX`
  (stereo-stage allpass chain), `GateFX` (synced trapezoid gate —
  stutter/beat-repeat foundation).
- `FXSlot`: type, enabled (smoothed bypass crossfade), dry/wet mix,
  in/out gain, sync mode + division, pending-effect adoption — the
  RT path never allocates (effect swap allocates on a non-RT loader).
- `FXRack`: bounded slot chains per target; `FXEngine`: racks for
  DeckA–D, SendA/B, Master + spare, send accumulation, return levels,
  per-target tempo, event FIFO.
- Beat sync: `SyncContext` (bpm, division 1/32…4 bars) fed from each
  deck's `effBpm()` / the sync master — division-locked timing,
  UI-independent.
- Routing: deck insert (pre-strip) → post-fader send taps → send racks
  → return → master insert rack → master bus; cue stays PFL post-insert.
- Safety: input sanitize before dry copy (NaN/Inf→0, ±8 clamp), wet
  sanitize + runaway reset after process, bounded feedback, zero
  allocations inside render.
- `ParameterRegistry`: FX space at `0x1000|target<<8|slot<<4|kind` —
  same control path as the mixer for UI/MIDI/HID/automation.
- `FXPresetManager`: factory + user presets, native-param blobs,
  whole-engine `serializeAll`/`restoreAll` (kind+params, no pointers).

Tests: +25 `[fx]` cases (116 total, 1.59M assertions) — impulse/sine/
noise/silence through every effect, decay & pre-delay timing, ping-pong,
synced vs free divisions, bypass continuity, NaN/Inf sanitize + clamp,
presets/state round-trip, engine routing, registry coverage, mixer
insert/send/master integration, 500-block stress.

## Status — Phase 5 (professional mixer engine)

`core/mixer/` — modular mixer: sources render into channel strips,
strips feed master + headphone buses. Decks are now pure sources
(trackGain + transport mute); the channel path (trim → EQ → filter →
fader/mute/solo → xf assign) lives in `ChannelStrip`, so sampler /
external / FX-return / mic sources can feed the same strip later.

- `ChannelStrip`: smoothed trim, `EqProcessor` (3-band standard mode,
  4-band capable, per-band gain/freq/Q/bypass, ≤3 dB/block steps),
  `FilterProcessor` (position knob LP↔HP or typed cutoff), fader
  (Linear / AudioTaper / Custom curves), mute, solo, instant cut, PFL
  cue, crossfader assignment A/B/Thru, pre-fader meters.
- `CrossfaderEngine`: smoothed position, Linear / EqualPower / Cut /
  Custom curves.
- `MasterBus`: smoothed gain/mute → `LimiterProcessor` → meters
  (peak/RMS/hold/clip + GR) → out 0-1.
- `HeadphoneBus`: cue sum + master blend (cueVol × mix) → out 2-3;
  never feeds back into master.
- `MeteringEngine`/`ChannelMeters`: atomic peak/RMS/hold/clip per
  channel and bus — single-writer RT, read by UI anywhere.
- `RoutingEngine`: node/edge graph of the topology (introspection +
  extension point for FX sends/returns, recording, extra sources).
- `ParameterRegistry`: static metadata for every parameter
  (id/default/min/max/step/smoothMs); channel ids = `(ch<<5)|kind`,
  bus ids at 0x400+ — the single address space UI/MIDI/HID/automation
  and tests share.
- `MixerController` + `rt::SetParam` command: validated parameter
  writes to the audio thread; all legacy commands map to the same
  strip/bus setters, so nothing upstream changed.
- App unchanged on the outside; `engine.mixerCtl()` is the new
  validated entry point for future controller mappings.

Tests: +21 `[mixer]` cases (91 total, 1.59M assertions) — synthetic
sine/noise/impulse/silence through strips and buses, EQ/filter/fader/
crossfader/cue/solo/mute/limiter behavior, param routing, registry,
4-channel render, 500-block RT integrity run.

## Status — Phase 4 (performance engine)

`core/performance/` — non-RT layer translating musical actions to RT
commands; `SyncRtCore` + grid/slip/sync fields inside `DeckEngine` do
the continuous work in the callback.

- **Sync**: master/follower, Off/Bpm/Beat/Phrase modes. Follower rate =
  masterBpm/ownBpm slewed per block (user pitch ignored while synced);
  beat phase locked by predicted-phase comparison with deadband +
  bounded chase + one faded snap when far off. Deterministic statuses
  (NoGrid/OutOfRange/…) — failures are reported, not hidden.
- **Quantize**: target snapping on the BeatGrid (1..1/16 beat), not
  deferred triggering. Applies to cues, loops, jumps, slip actions.
- **Cues**: hot/memory/loop types, 8+ slots, frame+beat stored (survive
  grid re-edits), persisted in `cues` (migration v2) via
  `PerformanceStore`; `saved_loops` + `kv` too.
- **Loops**: beat loops of 1/32..32 beats, in/out/toggle/reloop,
  halve/double/move — all grid-aligned through the tempo map.
- **Beat jump**: ±beats preserving phase (or snapped with quantize).
- **Slip**: `slipShadow_` musical clock inside the deck — roll / hot cue
  hold / scratch restore the "where it would be" position on release.
  `loopRoll` = slip + temporary loop.
- **Events**: `PerfEvent` on actions + `poll()` emitting
  beat/bar/loop/slip transitions — ready for ControllerEngine/MIDI.
- App: SYNC/QNT buttons per deck; grid attaches when analysis finishes;
  `MasterClock` via `perf.masterClock()`.

Tests: 70 Catch2 cases — sync tempo convergence, phase lock, master
switch, out-of-range, no-grid rejection, quantize cue landing, hot vs
memory cue semantics, cue persistence roundtrip, beat loops,
halve/double/move, loop wrap, beat jump ±N, slip roll/cue recovery,
and a 30-op stress run with sync+loops+cues+jump+slip.

## Status — Phase 3 (music analysis engine)

Background analysis pipeline: decode → onset (spectral flux) → BPM
(autocorrelation + octave rescoring, ranked candidates, tempo map) →
key (chromagram + Krumhansl-Schmuckler, standard + Camelot) → silences
→ waveform (overview 4096 bins + detail 256-frame bins, cached POD
blobs) → beatgrid (piecewise-constant tempo segments, f64, editable).

- `core/analysis/`: OnsetDetector, BpmAnalyzer, BeatGrid + BeatGridEngine
  (setFirstBeat/move/nudge/adjustBpm/tempo segments/reset), KeyAnalyzer,
  WaveformGenerator + WaveformData (region queries), MusicAnalysisEngine
  (stage pipeline with cooperative cancel), AnalysisQueue (low-priority
  workers, priority heap, pause/resume/cancel/retry, events)
- `core/data/`: Database (SQLite wrapper, WAL, versioned migrations),
  AnalysisStore (fingerprint = size+mtime+head/tail hash; kAnalysisVersion;
  detected values in `analysis`, user corrections in `user_overrides` —
  never overwritten)
- `AnalysisService`: analyzeTrack (bg) / analyzeTrackSync / getAnalysis /
  getBeatGrid / getWaveform / getKey / getBpm (override-aware) /
  setUserBpm/setUserKey/saveUserGrid / invalidateStale
- App: `onda.db` opens beside `config.json`; deck LOAD enqueues analysis;
  state label shows BPM/key when done
- Tests: 51 Catch2 cases — BPM 70/128/174 + half/double candidates +
  tempo-change map, grid math/edits/downbeats/variable tempo, key C/Am,
  waveform levels + region query, store round-trip/override/fingerprint
  staleness, queue e2e/failure, cancellation, grid drift

Requires `third_party/sqlite` = SQLite amalgamation (gitignored, public
domain) and `third_party/ffmpeg` as before.

## Status — Phase 2 (audio engine + deck engine)

Full-track-in-RAM playback engine: worker `TrackLoader` (ffmpeg decode,
progress, cancel, errors) → atomic handoff → `DeckEngine` ×4 →
`MixerEngine` (faders, crossfader curves, cue bus, master, limiter,
meters) → out.

- `core/audio/TrackAudio`: whole track decoded to device-rate stereo float in RAM
- `core/engine/TrackLoader`: async decode/metadata/progress/cancel; never in RT
- `core/engine/DeckEngine`: transport, musical clock, varispeed cubic,
  pitch/range/bend, key lock (`ITimeStretcher`), loops, seek, reverse,
  quantize flag, EQ 3-band + DJ filter + gains; states
  Empty/Loading/Ready/Playing/Paused/Seeking/Error
- `core/engine/ITimeStretcher` + `core/dsp/WsolaStretcher`: replaceable
  WSOLA (zero-alloc circular buffers)
- `core/engine/MixerEngine` (superseded by `core/mixer/` in Phase 5):
  channel faders, crossfader (linear / equal-power / cut), PFL cue bus +
  cue vol/mix on outs 2-3, master gain, optional peak limiter,
  peak/RMS meters, clip counter
- `core/dsp/`: Biquad, ThreeBandEq, DjFilter, PeakLimiter,
  CrossfaderCurves, cubic resample
- UI shell: full functional controls per deck (seek, pitch+range, bend,
  KEY, loops IN/OUT//2/x2, REV, PFL, trim, EQ, filter, fader) + mixer
  column (master, cue vol/mix, crossfader + curve) + live master meters
- Tests: 31 Catch2 cases incl. RT zero-allocation probe and a ~110 s
  stress scenario (2 decks, loops, keylock, moving crossfader, cue)

Requires `third_party/ffmpeg` = gyan.dev `ffmpeg-release-full-shared.7z` (dev headers + import libs + DLLs).

## Build (Windows)

Requires VS BuildTools 2026 (MSVC 14.5x) with Windows SDK. CMake/Ninja ship with VS:

```bat
tools\cmake.cmd -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
tools\cmake.cmd --build build
build\src\onda.exe          :: app
build\tests\onda_tests.exe  :: tests
```

## macOS

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

## Layout

```
src/core/       pure C++20 engine primitives (no JUCE)
src/platform/   JUCE-backed platform layer: audio HAL, log, config
src/ui/         JUCE-free UI core: commands, shortcuts, snapshots,
                theme, waveform math, virtual table model, layout
src/app/        JUCE application + UI views (app/ui/)
tests/          Catch2 unit tests
docs/adr/       architecture decision records
```
