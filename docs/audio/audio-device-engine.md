# Audio Device Engine — device management + low-latency layer

Phase 13 adds `onda::adevice`: the professional device layer that sits
between the app and the OS audio backend. It owns enumeration,
identity, capabilities, selection, safe switching, sample-rate and
buffer policy, channel routing, latency accounting, health and
recovery. The real-time callback itself is unchanged and still lives
in `audio::AudioEngine` — this layer is control-plane only.

**Phase 26 completes it**: booth pair in `ChannelMapping`, typed
`AudioError` codes + `AudioDeviceStatus`, `DjAudioProfile`
capability-based detection (DDJ-FLX4), explicit `autoRoute` with
Manual-Required, headphone mono fold-down, stream-start output fade,
and `OutputGuard` (sanitize + DC monitor) on every mapped pair.

```
Application
↓
AudioDeviceEngine        (domain, JUCE-free — src/core/adevice/)
↓
Platform Backend         (adevice::Backend — JuceAudioBackend in app,
                          FakeBackend in tests)
↓
Audio Device             (WASAPI / CoreAudio / ASIO / ALSA / JACK —
                          whatever JUCE's AudioDeviceManager exposes)
↓
Output
```

## Architecture

| Component | File | Role |
|---|---|---|
| `AudioDeviceEngine` | `core/adevice/AudioDeviceEngine.h` | orchestrator: enumerate/select/switch/recover/profile/diagnostics |
| `Backend` (seam) | `core/adevice/AudioBackend.h` | platform interface: enumerate, status, probe, apply, reopenDefault |
| `JuceAudioBackend` | `platform/audio/JuceAudioBackend.h` | production impl over `juce::AudioDeviceManager` |
| `DeviceCatalog` | `core/adevice/DeviceCatalog.h` | raw→`DeviceInfo`, stable ids, defaults, FLX4 detect |
| `AudioRouting` | `core/adevice/AudioRouting.h` | `ChannelMapping` defaults/validation/span |
| `BufferManager` | `core/adevice/BufferManager.h` | Low/Balanced/Stable presets + nearest supported |
| `SampleRateManager` | `core/adevice/SampleRateManager.h` | choose + mismatch explanation |
| `LatencyManager` | `core/adevice/LatencyManager.h` | estimated latency report |
| `AudioHealthMonitor` | `core/adevice/AudioHealthMonitor.h` | underruns/xruns/budget/overload, dedup'd warnings |
| `AudioRecoveryManager` | `core/adevice/AudioRecoveryManager.h` | explicit FSM |
| `AudioTestTool` | `core/adevice/AudioTestTool.h` | routing-test signal generator (RT-safe) |
| `DeviceProfile` | `core/adevice/DeviceProfile.h` | per-device-id persisted settings (JSON v2: booth keys) |
| `DjAudioProfile` | `core/adevice/DjAudioProfile.h` | DJ-controller audio profile from detected caps (§27-28) |
| `OutputGuard` | `core/mixer/OutputGuard.h` | per-pair sanitize + DC monitor + over-full counters (§40-42) |
| `AudioDeviceTypes` | `core/adevice/AudioDeviceTypes.h` | POD types + `AudioError`/`DeviceResult`/`AudioDeviceStatus` (§31-32) |

## Backend architecture (§2, §37)

`Backend` is the only platform seam:

```cpp
virtual std::vector<RawDevice> enumerate() = 0;
virtual BackendStatus status() = 0;
virtual DeviceCapabilities probe(const std::string& name) = 0;
virtual std::string apply(const DesiredSetup&) = 0;   // "" on success
virtual std::string reopenDefault() = 0;              // last resort
```

`JuceAudioBackend` adapts `juce::AudioDeviceManager`, which already
abstracts WASAPI (shared), DirectSound, ASIO (when enabled) on Windows
and CoreAudio on macOS. **No proprietary drivers ship in the app**;
ASIO support comes from the host's installed driver + JUCE config.

JUCE caveats handled by the adapter:
- Channel counts aren't enumerable without opening a device →
  `enumerate()` reports presence (≥1), `probe()` fills real counts.
- `probe()` briefly opens the endpoint on the control thread — on a
  busy/exclusive device it legitimately returns `known=false` and the
  engine falls back to "ask and see" mode.
- The currently-open device is never re-opened for probing — caps are
  read from the live stream.
- `apply()` snapshots the previous `AudioDeviceSetup` and restores it
  if `setAudioDeviceSetup` fails — rollback is guaranteed inside the
  adapter contract.

## Device detection + identity (§3-6)

- `DeviceInfo.id` = `"<backend>|<endpoint name>"` — never just the
  display name; JUCE exposes no cross-backend hardware GUID.
- Defaults come from `AudioIODeviceType::getDefaultDeviceIndex` per
  direction — System Default Input and Output are tracked separately
  (`isDefaultIn`/`isDefaultOut`).
- Input and output are never assumed to be the same device;
  `DeviceSettings` carries `outputId` and `inputId` independently.
- DDJ-FLX4 is detected by endpoint name (`ddj-flx4`/`ddj flx4`) and
  gets its documented topology (outs 1-2 master, 3-4 phones, 44.1/48
  kHz) as the *default* mapping — still verified against probed
  capabilities. Mic input is hardware-mixed on the device, not a USB
  endpoint.

## Sample-rate policy (§7-8)

**The engine follows the device — no application resampler.**

- `SampleRateManager::choose()` picks the supported rate nearest the
  request (preferred 48 kHz when the device offers it).
- `explain()` produces a clear message when the applied rate differs
  from the request ("does not support 96000 Hz — running at 48000 Hz;
  no resampling is applied").
- On any device (re)start, `AudioEngine::audioDeviceAboutToStart`
  re-prepares decks, mixer (+FX, headphone bus) and now the sampler —
  nothing keeps buffers tuned to a stale rate (§26).
- The recorder stamps the real device rate; decks resample per track
  as part of normal playback — so an engine-level resampler would be
  pure added latency. If one is ever needed, it goes behind a
  documented `Resampler` node with source/dest/quality/latency fields.

## Buffer policy (§9, §27)

- Presets: Low→128, Balanced→256, Stable→512 (nearest supported wins).
- Exact sizes are accepted only when the device advertises them.
- Buffer changes go through the same `select()` path — JUCE restarts
  the stream, `audioDeviceAboutToStart` republishes `stats_.bufferSize`,
  and the health budget (`buffer/rate`) updates automatically. The app
  is never restarted.

## Latency (§10-11)

`LatencyReport` = `bufferMs` (bufferSize/rate) + device-reported
input/output latencies (driver + hardware) → estimated round trip.

- `measured` is **false** unless a real loopback ran — the flag exists
  for a future round-trip test (§31) that physically cannot run
  without an input+output pair wired together.
- Monitoring/cue implication: cue lives on a mapped pair of the same
  stream → same buffer latency as master; no extra estimate.

## Routing + channel management (§14-17, §26, §45-46)

`ChannelMapping {masterL/R, cueL/R, boothL/R, inL/R}` — Phase 26 adds
the booth pair (the mixer's `BoothBus` already owned `outMap[4..5]`;
the domain layer now models it):

- validated by `AudioRouting::check` (in-range, complete stereo pairs,
  no overlap between master/cue/booth);
- persisted per device id in `DeviceProfile` (JSON v2 `bL/bR` keys —
  v1 files read back as `booth=-1`);
- auto-derived via `AudioRouting::autoRoute()`: detected DJ profile →
  `defaults(outCh)` (master 0-1, cue 2-3 iff ≥4 outs, booth 4-5 iff
  ≥6 outs) — or **Manual Configuration Required** when the device
  cannot even host a master pair (never a guessed route, §46);
- published to `MixerEngine::setOutputMap` (6 atomics) — `render()`
  zeroes all channels then `emitPair()` writes each bus to its mapped
  pair. Unused channels carry silence; a pair collapsed onto one
  channel (`mL==mR`) is mono-summed, not overwritten (§15);
- `emitPair` is also where the stream-start fade and `OutputGuard`
  apply (below).

## Typed errors + device status (§31-32) — Phase 26

`selectEx()` / `setChannelMappingEx()` return `DeviceResult
{AudioError code, std::string message}` — the legacy `select()` keeps
the string view. Codes: `DEVICE_NOT_FOUND`, `UNSUPPORTED_SAMPLE_RATE`,
`UNSUPPORTED_BUFFER_SIZE`, `CHANNEL_CONFIGURATION_FAILED`,
`INITIALIZATION_FAILED`, `DEVICE_DISCONNECTED`, `STREAM_START_FAILED`,
`STREAM_STOP_FAILED`, `MANUAL_CONFIGURATION_REQUIRED`. The engine also
keeps `lastResult()` and derives `AudioDeviceStatus`
(`UNAVAILABLE/AVAILABLE/INITIALIZING/ACTIVE/DISCONNECTED/ERROR/
RECOVERING`) from backend status + recovery FSM — surfaced in the
diagnostics text and the Settings audio panel.

## DJ-controller audio profiles (§27-30) — Phase 26

`DjAudioProfile::detect(DeviceInfo, DeviceCapabilities)` produces a
profile **only from detected capabilities**: DDJ-FLX4 with ≥4 outs →
`DjController` (master 1-2, phones 3-4 — documented topology); any
interface with ≥4 outs → `DjInterface` (master, phones, booth iff ≥6).
A device named "DDJ-FLX4" that reports only 2 outs gets **no**
controller profile — capabilities are never invented from a name.
Controller (MIDI/HID) detection lives in `ctrl::` and is fully
independent: FLX4-as-controller + different-audio-device is a legal
configuration.

## Output stage (§15, §24, §38-42) — Phase 26

- **Headphone mono** — `BusParam::HeadphoneMono (0x40C)`: smoothed
  L+R fold-down on the headphone bus, separate from routing collapse.
- **Stream-start fade** — `audioDeviceAboutToStart` calls
  `MixerEngine::onStreamRestart()`; the next blocks ramp every mapped
  out 0→1 over ~40 ms via a precomputed gain curve (`fadeG_`) — no DC
  jump/pop on device (re)start or switch (§38-39).
- **OutputGuard** — per mapped pair (master/cue/booth), after fade:
  non-finite and over-`kClamp` samples are zeroed/clamped and counted,
  `|x|>1.0` counted separately, plus a ~200 ms DC-offset estimator —
  counters published in `MixerBusDiag` (`outSanitized`, `outOverFull`,
  `masterDc/cueDc/boothDc`). Last-resort protection only — the master
  limiter stays the gain-staging boundary and remains optional (§41-42).
- **Mono master** — `mL==mR` writes the L+R sum, not last-wins (§15).

## Recovery + hot-plug (§11-13, §32)

FSM: `Healthy → Warning | DeviceLost → Reconnecting → Recovered →
Healthy`, plus `Failed` (leaves only via successful reopen).

- `tick()` (UI thread, ~2 Hz enumerate) detects: current output gone
  from the device list, or stream stopped while expected running.
- Recovery retries every 3 s: preferred device if it's back, else
  `reopenDefault()` — the mixer map is re-pushed on success.
- Every failure path emits an actionable notification ("device busy —
  restored default output", "stream did not start", …).
- Nothing here ever runs on the audio callback.

## Real-time safety (§12-13)

The callback (`AudioEngine::audioDeviceIOCallbackWithContext`) still
obeys: no filesystem, no network, no DB, no UI, no logging, no locks,
no exceptions, no repeated allocation. Phase 13 added only:

- one atomic load (`testMode_`), and when a test signal is active a
  bounded `memset`+sine write into the already-delivered output block
  (`AudioTestTool::render` — no state changes, no allocation);
- `mixer_.outMap()` — 6 relaxed atomic loads per block;
- `underrunCount` / `deviceErrors` atomic increments;
- `streamFadeReq_` atomic consume + preallocated `fadeG_` curve +
  `OutputGuard` math per mapped pair (Phase 26 — still pure math and
  atomics, no allocation/locks).

All enumeration, probing, permission requests, switching and recovery
run on the control/UI thread.

## Health monitoring (§20-23)

`AudioHealthMonitor::feed()` runs once per app tick and consumes the
existing `rt::AudioStats` atomics plus the new counters:

- **budget** = `bufferSize / sampleRate` per callback;
- **underrun** = callback duration over budget (`underrunCount`,
  stamped per event → timestamped `UnderrunEvent` ring, 32 entries);
- **xrun** = scheduling gap between callbacks (`xrunCount`);
- **overload** = smoothed CPU > 85% of budget → warning;
- **device errors** = `audioDeviceError` counter (driver-reported).

Warnings are deduplicated (5 s per class) — the user sees a warning,
not a flood. `HealthReport` exposes the recent underrun events with
timestamp, duration, buffer and CPU for diagnostics.

## Diagnostics (§29)

`AudioDeviceEngine::diagnostics()` returns copyable text:

```
DEVICE / BACKEND / SAMPLE RATE / BUFFER (+ms) / INPUT CH / OUTPUT CH /
ROUTING (master->L/R cue->L/R booth->L/R) /
ESTIMATED LATENCY (+estimated|measured tag) / CPU % / UNDERRUNS /
XRUNS / STREAM STATUS / DEVICE STATUS / LAST ERROR / RECOVERY
```

The Settings → Audio panel shows it live and offers **COPY
DIAGNOSTICS**; the snapshot also carries `backend`, `estLatencyMs`,
`recovery`, `underruns`, `deviceErrors` to the UI.

## Permissions (§33)

`requestMicPermission()` routes through
`juce::RuntimePermissions::request(recordAudio, cb)` — the official
API on platforms that gate input (macOS, mobile; no-op on desktop
Windows). Inputs stay disabled by default; when a future feature opens
an input, permission is requested first and denial is surfaced as an
actionable error.

## Audio test tool (§30-31)

Internal routing verifier (Settings → Audio → "Signal:"):

| Mode | Effect |
|---|---|
| off | normal program |
| silence | zeroes all outputs (checks residual bleed) |
| sine 440Hz | stereo master pair |
| left only / right only | channel identity + swap check |
| in-phase | identical signal on L/R (phase check) |
| alternate L/R | 0.5 s ping-pong (routing check) |
| cue sine | 440 Hz on the headphone pair only |

It replaces program audio on the mapped outs — designed to expose
wrong channel maps, master/cue swaps, missing outputs, duplicated
routing and phase errors by ear.

## Testing + stress + limitations

`tests/adevice_tests.cpp` (20 cases): catalog/ids/defaults, rate and
buffer policy, latency labelling, routing validation, select happy
path, missing device, unsupported rate/buffer snapping, rollback on
busy, dead-stream detection, hot-plug loss→reconnect→recover, recovery
FSM, health dedup/budget/events, profile JSON persistence, FLX4
topology, live remapping, diagnostics keys, the full test-signal
matrix, and mixer-level output-map honoring.

Stress coverage: the existing full-suite stress tests (decks+FX+
sampler+recorder, hours-equivalent recording) exercise the same RT
path the device layer drives; device switching itself is control-plane
and covered by the fake-backend scenarios above.

Limitations:
- Channel counts are probe-time best-effort (JUCE limitation).
- `exclusiveCapable` stays false — WASAPI-exclusive/ASIO toggles need
  a bespoke backend later.
- Measured round-trip latency requires a physical loopback — reported
  honestly as estimated until then.
- `UNSUPPORTED_SAMPLE_RATE`/`UNSUPPORTED_BUFFER_SIZE` codes exist but
  current policy *snaps* to supported values instead of failing — the
  codes are for backends that reject explicitly.
- Device-switch crossfade is a fade-in after restart (the outgoing
  stream cannot be kept alive across a JUCE device switch); no
  gapless audio during the switch itself.
- `DjAudioProfile` for non-FLX4 hardware needs a real detection table
  per model — currently only FLX4 is name-matched; everything else
  falls back to generic out-count defaults.
- Hardware verification on a real DDJ-FLX4 is pending; topology is
  applied only after name detection + capability check, never assumed.
- The JUCE selector still owns backend-type selection; the Onda panel
  covers preset/test/diagnostics + routing/status readout on top of it.
