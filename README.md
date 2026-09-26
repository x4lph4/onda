# Onda (codename)

Professional desktop DJ application. C++20 + JUCE 8, GPLv3.

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
src/app/        JUCE application + UI shell
tests/          Catch2 unit tests
docs/adr/       architecture decision records
```
