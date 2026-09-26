# Onda (codename)

Professional desktop DJ application. C++20 + JUCE 8, GPLv3.

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
- `core/engine/MixerEngine`: channel faders, crossfader
  (linear / equal-power / cut), PFL cue bus + cue vol/mix on outs 2-3,
  master gain, optional peak limiter, peak/RMS meters, clip counter
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
