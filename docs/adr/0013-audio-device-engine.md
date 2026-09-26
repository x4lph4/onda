# ADR 0013: Audio Device Engine + Low-Latency Device Layer

## Decision

Introduce `onda::adevice` — a JUCE-free domain layer that owns device
enumeration, stable identity, capabilities, selection, sample-rate and
buffer policy, channel routing, latency accounting, health monitoring,
recovery and diagnostics — and drive it through a `Backend` seam whose
production implementation adapts the existing
`juce::AudioDeviceManager` (`JuceAudioBackend`).

## Key choices

- **JUCE stays the platform backend.** The project already runs
  `juce::AudioDeviceManager` + `AudioIODeviceCallback`, which is the
  correct cross-platform abstraction for Windows (WASAPI/ASIO/DirectSound)
  and macOS (CoreAudio). Writing native WASAPI/CoreAudio code now would
  duplicate what JUCE already does correctly. The `Backend` seam keeps
  that an implementation detail: swapping to bespoke native backends
  later touches `JuceAudioBackend`, never the domain.
- **Domain is JUCE-free and fake-driven.** `AudioDeviceEngine`,
  `DeviceCatalog`, `AudioRouting`, `BufferManager`, `SampleRateManager`,
  `LatencyManager`, `AudioHealthMonitor`, `AudioRecoveryManager` and
  `AudioTestTool` are plain C++ — every test uses `FakeBackend`.
- **Device id = backend + endpoint name.** JUCE does not expose stable
  hardware GUIDs across all backends; names alone are ambiguous. Ids
  like `WASAPI|DDJ-FLX4` are persisted in `DeviceProfile` JSON in
  config (`audio.profile.*` keys).
- **Sample-rate policy: engine follows the device.** No app-level
  resampler — decks already time-stretch per track and the recorder
  stamps the real device rate. `SampleRateManager::choose()` snaps to
  the nearest supported rate and `explain()` surfaces the mismatch.
- **Buffer policy: presets → nearest supported.** Low/Balanced/Stable
  map to 128/256/512 ideals; `BufferManager::nearest()` snaps to what
  the device actually advertises — never an assumed size.
- **Routing is data, not convention.** `ChannelMapping {mL,mR,cL,cR}`
  is validated against probed channel counts, persisted per device,
  auto-derived from `AudioRouting::defaults(n)` or the documented
  DDJ-FLX4 topology (1-2 master, 3-4 phones — detected by endpoint
  name, never hardcoded indices without detection).
- **Mixer reads an atomic output map.** `MixerEngine::setOutputMap`
  publishes 4 atomics; `render()` zeroes every channel then writes the
  mapped master/cue pairs — unmapped outs are guaranteed silence and
  cue is simply absent when unmapped or the device lacks the outs.
- **Safe switching with rollback.** `select()` = validate → probe →
  resolve rate/buffer/map → snapshot → `backend.apply()` → verify
  `running` → rollback via `reopenDefault()` on failure. All on the
  control thread; the RT callback never participates.
- **Recovery is an explicit FSM.** Healthy → Warning/DeviceLost →
  Reconnecting → Recovered → Healthy; `Failed` only leaves via a
  successful reopen. `tick()` polls enumeration at 2 Hz and retries
  recovery every 3 s — no blocking in any callback.
- **Latency is labelled, never faked.** `LatencyReport.measured` is
  false until a real loopback test exists; the estimate is
  buffer/rate + driver-reported in/out latencies.
- **Health feed is pull-based.** `AudioHealthMonitor::feed()` consumes
  the existing `rt::AudioStats` atomics (new `underrunCount` for
  over-budget callbacks, `deviceErrors` from `audioDeviceError`) once
  per UI tick — nothing new measured inside the RT path. Warnings are
  deduplicated (5 s) so the user is informed, not flooded.
- **Test signal replaces program audio post-mixer.** `AudioTestTool`
  renders silence/sine/L/R/in-phase/alt-ping/cue-sine into the mapped
  outs after the recorder tap — pure function, RT-safe, and the same
  code the tests verify.
- **Mic/input permission is a port.** `requestMicPermission` goes
  through `juce::RuntimePermissions::recordAudio` — inputs stay off
  by default (the FLX4 mic is hardware-mixed, not USB-input, anyway).

## Consequences

- Musical code (decks/mixer/FX/sampler/recorder) is oblivious to
  WASAPI/CoreAudio/ASIO/ALSA — it sees mapped channel buffers.
- `SettingsView` keeps the proven `AudioDeviceSelectorComponent` for
  backend/channel config and adds an Onda panel (latency preset, test
  signal, copyable diagnostics, live info) driven by `adevice_`.
- Channel-count probing opens devices briefly on the control thread —
  best-effort (`caps.known=false` on busy endpoints), documented.
- Custom WASAPI-exclusive/ASIO tuning and a measured round-trip test
  remain future work behind the same seam.
