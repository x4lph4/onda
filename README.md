# Onda (codename)

Professional desktop DJ application. C++20 + JUCE 8, GPLv3.

## Status — Phase F3 (mini-player)

Real audio playback end to end: ffmpeg decode on streamer threads → lock-free
AudioFifo → RT deck (gain + level) → mixer (sum, master gain, clip count) → out.

- `core/audio/AudioFifo`: SPSC bulk ring of interleaved float frames
- `core/decode/IDecoder` + `platform/decode/FFmpegDecoder` (ffmpeg 9 shared build, `third_party/ffmpeg`, gitignored)
- `core/engine/AudioFileStreamer`: per-deck decode thread, ~8 s prebuffer
- `core/engine/DeckEngine` ×4 (2 exposed), `MixerEngine` (sum, master, clip detect)
- UI: per-deck LOAD/PLAY/gain strip + live state/position; header shows DSP %, XRUN, CLIP
- Tests: 15 Catch2 cases incl. full non-UI playback chain against a generated WAV

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
