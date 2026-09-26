# ADR-0009 — Controller Engine (MIDI/HID + mapping system)

Status: accepted (Phase 9)
Date: 2025

## Context

The app must be controllable from external hardware (DJ controllers)
with the same guarantees as the UI: no blocking of the audio thread,
no direct mutation of engine internals, testable without hardware or
a windowing system. The first concrete target is a future Pioneer DJ
DDJ-FLX4 profile — this ADR covers only the universal, vendor-neutral
architecture. No proprietary protocols were reverse-engineered; the
system speaks standard MIDI (Note/CC/Clock) and exposes a HID
abstraction point for future legally-clean drivers.

## Architecture — two layers again

```
src/controller/    JUCE-FREE CORE (testable, no OS calls)
├── MidiTypes        MidiMsg{type,ch,d1,d2,us} — library-independent
│                    message model (NoteOn/Off, CC, PC, PitchBend,
│                    ChannelPressure, Clock*, ActiveSense…)
├── MappingTypes     MidiAddr{kind,ch,d1}, Mapping{in→target}, modes
│                    (Toggle/Momentary/Absolute/Relative/IncDec/Pickup),
│                    EncoderMode (Absolute/RelBinaryOffset/
│                    RelTwosComplement/RelSignMagnitude), JogMode
│                    (Scratch/PitchBend/Seek), CAP_* capability bits
├── MiniJson         ~250-line JSON parse/write (profiles are pure
│                    data — never code)
├── ParamNames       name↔ParameterRegistry id table: CH_n_GAIN,
│                    CROSSFADER, FX_DECK_A_0_P3, MASTER_GAIN… — the
│                    stable names profile files store
├── MappingEngine    addr→mapping table, value scaling (linear/
│                    inverse/square/sqrt curves), encoder deltas,
│                    pickup arming, echo-suppression hook
├── FeedbackEngine   state→LED/motor watches, per-output throttle,
│                    sent-message window for echo detection
├── ControllerProfile  versioned JSON (v1): meta + mappings[] +
│                    out feedback; validate() before any load
├── ProfileManager   profile library on disk: install/load/unload/
│                    delete/import/export/reset + per-device assign
│                    + name-contains auto-match
├── DeviceManager    OS device list diffs → connected/disconnected/
│                    reconnected events
├── ControllerDiagnostics  rolling in/out message log (MIDI monitor)
│                    + counters + latency stats
├── LearnMode        arm → next physical control captured → Mapping
├── MidiClock        input tracker (24 PPQN→BPM) + output generator;
│                    decoupled from audio MasterClock — master/slave
│                    is an explicit app decision
├── HidManager       stub façade — a future documented/vendor-allowed
│                    HID driver plugs in here; nothing proprietary
└── ControllerEngine   façade: per-device DeviceState{mapping,
                    feedback, sendRaw, caps}, learn routing, profile
                    application, feedback restore on reconnect

src/platform/midi/
└── JuceMidi         MidiManager: juce::MidiInput/Output adapters,
                     OS device-list poll (800 ms) → hot-plug events,
                     juce::MidiMessage ↔ MidiMsg conversion
```

## The single control path (repeated, intentionally)

```
Hardware → MidiManager(JUCE) → ControllerEngine::processInput
        → LearnMode? (armed → capture, done)
        → MidiClock? (clock bytes → clock tracker)
        → FeedbackEngine::isEcho? (motor echo → drop + count)
        → MappingEngine → one of the host Ports:
              sendCommand  → CommandManager::invoke(id, Midi, v)
              setParam     → MixerController::setParam (validated)
              jog          → rt::CommandQueue tryPush (lock-free)
              notify       → NotificationCenter
```

UI, keyboard, MIDI and (future) HID all terminate at the same
`CommandManager` / `MixerController` / `rt::CommandQueue` — the
controller layer adds *no new mutation paths*.

## Key decisions

- **Command names in profiles.** Mappings persist `cmdName`
  (e.g. `decka.playToggle`) — ids resolve at load via
  `commandIdFor`. Renumbered ids never break profiles.
- **paramId 0 is valid.** `chParam(0, Gain) == 0` — a `paramId != 0`
  guard in FeedbackEngine would have silently dropped channel-0 gain
  watches; `followValue` alone selects the param path.
- **Echo suppression is value+time based.** Every feedback output is
  recorded in a 150 ms window; an identical inbound byte on the same
  address is device echo (motorized faders) → dropped and counted.
- **Pickup reads the live port.** Pickup mode compares against
  `paramState(id)` (the real mixer value), not a cached copy — safe
  across UI/keyboard/hardware changes.
- **Relative encoders are 7-bit.** `RelTwosComplement` sign-extends
  bit 6 (0x7F = −1), matching actual controller firmware.
- **Feedback is polled.** Watches read command/param state each tick
  (~20 Hz), throttle per address, and re-send everything (forced) on
  reconnect — LEDs stay right regardless of which input flipped state.
- **HID is deliberately a stub.** `HidManager` exposes the seam; a
  vendor-documented driver can be added later without touching core.
- **MIDI Clock is decoupled.** The tracker reports measured BPM; who
  masters transport is an app-level decision, not implied here.

## Latency accounting

`ControllerDiagnostics` keeps a rolling log + counters: messages
in/out, dropped echoes, learn captures, per-message processing µs
(avg/max). Measured: 30 000 synthetic messages → ~1.05 M msg/s,
avg processing ≈ 0 µs (sub-µs), 0 dropped.

## Testing

`VirtualMidiController` (test-only device) attaches like hardware:
buttons, knobs, faders, encoders (all 4 modes), jogs, clock; captures
every outbound message. 15 test cases / 143 assertions cover: note &
CC dispatch, scaling/curves/inverse, pickup arming, all encoder modes,
feedback LED + throttle + motorized-fader echo, diagnostics, learn,
profile JSON roundtrip + validation (incl. injected `exec` field
ignored — profiles carry no code), ProfileManager install/assign/
auto-match/import/export/delete, device hot-plug/reconnect + feedback
restore, param-name roundtrip, MIDI clock, and the 30 k-msg stress run.

## Files

- `src/controller/` — 13 headers + 8 cpp (core, JUCE-free)
- `src/platform/midi/JuceMidi.{h,cpp}` — OS adapter + hot-plug
- `tests/controller_tests.cpp` — suite + VirtualMidiController usage
- `src/app/MainComponent.cpp` — ports wiring, `midi_->start()`,
  20 Hz `ctrl_->tick()`
- Profile dir: `<config>/controllers/*.json`

## Limitations / next steps

- No DDJ-FLX4 profile yet — that's the next phase and will use only
  documented MIDI (the unit is class-compliant MIDI) or vendor-allowed
  interfaces; nothing invented or reverse-engineered.
- `HidManager` is an interface point only (no HID transport yet).
- MIDI clock output generator exists; UI toggle + master/slave policy
  is app-level work pending the settings surface.
- Settings/Device views have placeholder sections for controllers;
  a visual mapping editor + learn UI is deliberately out of scope.
- `ParamNames` covers mixer + FX ids; deck-transport params remain
  command-id based (buttons), matching how the UI drives them.
- Bluetooth LE MIDI and multi-byte (14-bit) CC merging: 14-bit decode
  exists in MappingEngine; LE-MIDI transport is JUCE/OS dependent.
