# Onda (codename)

Professional desktop DJ application. C++20 + JUCE 8, GPLv3.

## Status — Phase F2 (skeleton)

- JUCE app shell: header bar with device name / sample rate / buffer size / DSP load / xrun counter
- `AudioEngine`: device HAL (ASIO/WASAPI/CoreAudio via JUCE), RT callback emitting silence, underrun detection, persisted device setup
- `core/rt`: lock-free SPSC ring buffer, typed command queue UI→RT, atomic stats
- Catch2 test suite for RT primitives

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
