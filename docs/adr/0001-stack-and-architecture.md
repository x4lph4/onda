# ADR 0001 — Stack, licensing, threading model

Date: 2026-09-26. Status: accepted.

## Decision

- **Stack**: C++20 core + JUCE 8 (audio devices: ASIO/WASAPI/CoreAudio; MIDI; UI).
- **License**: GPLv3 → JUCE and Rubber Band usable under GPL without cost.
- **Platforms**: Windows + macOS from the start.
- **Architecture**: layered — `core` (pure C++, no JUCE) / `platform` (JUCE-backed I/O) / `app` (UI). Engine never depends on UI; swapping UI or host layers leaves core untouched.

## RT threading rules

- Audio thread: no allocation, no locks, no syscalls, no logging, no throwing past the callback.
- UI → RT: typed `Command` via `SpscRingBuffer` (drained at callback top).
- RT → UI: atomic-field `AudioStats` (single writer); heavier state will use immutable snapshots + atomic swap.
- Workers (analysis/import) on a separate pool; decode threads will sit between disk and per-deck ring buffers.

## Hardware

First target controller: Pioneer DJ DDJ-FLX4 — USB class-compliant (standard MIDI + USB audio). Mapping is our own JSON over standard MIDI messages; no Pioneer assets or binaries.

## Consequences

- JUCE types must not leak into `src/core` (enforced by include discipline + tests building without JUCE).
- Time-stretch goes behind `ITimeStretcher` (SoundTouch initially; Rubber Band/élastique interchangeable).
- Commercial relicensing later would require buying JUCE + Rubber Band licenses; acceptable.
