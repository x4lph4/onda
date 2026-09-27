# Controller Engine — architecture & integration guide

Status: **implemented**. Phase 9/10 built the core (device manager,
mapping engine, feedback, learn, profiles, DDJ-FLX4 plugin); Phase 27
completed the engine with explicit lifecycle, logical control
registry, layers, long-press, priority, SysEx, LED blink patterns,
generic jog/pad surfaces and factory/user profile split.

```
ControllerEngine
├── DeviceManager        attach/detach/reconnect events
├── DeviceState          per-device: info, mapping, feedback, lifecycle
├── ProfileManager       library: factory (in-memory) + user (on disk)
├── MappingEngine        hardware input → action (pure data in)
├── FeedbackEngine       app state → LED/motor output (state diffing)
├── ControlRegistry      logical ids ("decka.play") → targets
├── JogWheelEngine       generic jog modes/sensitivity/browse
├── PadEngine            8 pads × 4 decks × shift, velocity
├── LearnMode            arm → next input captures a mapping
├── MidiClock            24 PPQN sink/source (decoupled)
├── HidManager           driver seam (stub — no fake HID)
├── ControllerDiagnostics bounded monitor ring + latency stats
└── device plugins       FLX4: shift, pad banks, jog decode, LEDs
```

## Pipeline (never bypass it)

```
hardware → adapter (JuceMidi / virtual) → MidiMsg
        → LearnMode? → device plugin? → MappingEngine
        → ports (CommandManager / MixerController / jog)
app state → FeedbackEngine → sendRaw → hardware
```

A raw MIDI message is **never** wired straight to a mixer parameter —
everything flows through a `Mapping` entry. Device plugins may consume
input *before* the mapping table (pad banks, jog touch, shift), but
they emit through the same ports.

## Device lifecycle (§5)

`DISCONNECTED → DETECTED → CONNECTING → CONNECTED|READY|ERROR →
DISCONNECTING → DISCONNECTED`

- `Detected` — `attachDevice` ran, lookup pending.
- `Connecting` — auto-match/apply in progress.
- `Connected` — transport open, no profile (unmapped device usable
  via Learn).
- `Ready` — profile applied; feedback restored on reconnect.
- `Error` — last operation failed; `lastError` explains.
- Disconnect never destroys musical state — `DeviceState` is kept for
  the reconnect window; `restoreFeedback` repaints LEDs on return.

`ControllerState` (§46) is the copyable snapshot for UI:
id/name/manufacturer/model/transport, connected, initialized,
lifecycle, profileId, activeLayer, mappingCount, counters, lastError.

## Protocols & transports (§2-3)

`Transport`: `MidiUsb | MidiDin | MidiBle | Hid | Virtual`.
`DeviceInfo`: id, OS name, manufacturer, model, capability bits
(`CAP_*`), hasInput/hasOutput. Devices are matched by OS-reported
name — USB VID/PID is **not** assumed where unpublished.

- **MIDI 1.0**: note on/off, CC (7-bit and 14-bit MSB/LSB pairs via
  `hiRes`), pitch bend, program change, channel pressure, realtime
  clock (24 PPQN, decoupled).
- **SysEx** (§13): separate `SysExMsg` path (bounded to 64 B — FLX4
  position dumps fit), routed to the `onSysex` hook + monitor ring;
  never reaches MappingEngine.
- **HID**: `HidManager` driver seam only — real drivers attach when a
  documented interface exists. Nothing proprietary is emulated.
- **MIDI 2.0**: prepared (`SysExMsg::isUmp` detects UMP packets);
  no fake support claimed while JUCE/platform lacks UMP I/O.

## Logical controls (§10)

`controlRegistry()` maps `"<scope>.<control>"` → resolved `Target`
(CommandManager id / ParameterRegistry id / jog port):

```
decka.play decka.cue decka.sync decka.jog decka.pitch
decka.eq.low decka.filter decka.fader decka.pad.1 decka.pad.1.shift
ch1.cue mixer.crossfader mixer.headmix browser.next app.panic …
```

Profiles may set `target.control` instead of numeric ids — resolved
at `applyProfile` (unknown id → profile rejected). `skeleton(id)`
produces a ready-to-fill mapping for Learn/editor UIs.

## Mapping model (§12-19, §49, §53)

`Mapping` = declarative entry: `in` (MidiAddr) → `target` → mode:

| Field | Purpose |
|---|---|
| `mode` | Toggle / Momentary / Absolute / Relative / IncDec / Pickup |
| `kind` | hardware class: Button, Toggle, Momentary, Encoder, Fader, Knob, Jog, Pad, Switch (§12) |
| `curve` | Linear / Inverse / Expo / Log |
| `encoder` | Absolute / rel64 / 2's-complement / sign-magnitude |
| `hiRes` | 14-bit CC pair (MSB at n, LSB at n+0x20) |
| `outMin/outMax` | scaled range (param units or cmd value) |
| `layer` | 0=Default, 1=Shift, 2=Alt, 3=Custom — one active per device (§19) |
| `priority` | higher dispatches first on shared inputs (§53) |
| `longMs/longCmd` | Momentary hold past `longMs` fires `longCmd` once; release ends it (§17) |
| `verified` | `false` = UNVERIFIED protocol entry — works, flagged in `findIssues` (§9) |

Layer resolution: a binding on the *active* layer shadows Default-layer
siblings on the same input. Devices with hardware-split shift
channels (FLX4 pads ch 7–10) don't need layers — firmware encodes it.

`MappingEngine::findIssues(maps)` (§49): duplicates/conflicts per
layer, hiRes out of range, encoder mode on non-CC, longMs without
longCmd, invalid param ids, UNVERIFIED entries.

Values normalize to 0.0–1.0 (`msgValue01`), raw bytes kept in the
monitor ring for debugging (§14).

## Jog wheels (§20-26)

`JogWheelEngine` is the device-agnostic layer: plugins feed decoded
`turn(deck, ticks, reqMode, isPlatter)` + `touch(deck, down)`.

- **Modes**: `Vinyl` (touch+platter → scratch, free → decoder mode),
  `Cdj` (platter → seek), `Search` (amplified seek), `Browse`
  (library steps, **deck untouched** — §25).
- **Sensitivity** per deck (`cfg().sensitivity`); `searchBoost` in
  Search; `touchCapable=false` for wheels without plates (§22).
- Edge vs platter is explicit (`isPlatter`) — a side nudge never
  becomes a scratch.
- The engine never implements scratch DSP (§23) — `ScratchEngine`/
  deck code owns that.

## Performance pads (§27-34)

`PadEngine`: `pad(deck, idx, shift, vel01)` → state + `forPad` command.
`PadState` = {pressed, velocity, color, active} — doubles as the LED
model (§28). Semantics live in `ui::pads::PadLayer`/`PadModeManager`
(§30) — modes HotCue/PadFx/Loop/Sampler/Stems/BeatJump map to domain
commands; the controller layer never re-implements loops/stems/FX.

## LED feedback (§41-45)

`FeedbackEngine` polls *application* state (command/param ports) →
emits only on change (state diffing, §44) + per-output throttle
(§45). `LedPattern`: Off/On, Blink ~2 Hz, SlowBlink ~0.5 Hz,
FastBlink ~4 Hz (§42) — computed from wall-clock phase, exact
regardless of tick rate. Echo suppression (150 ms window) prevents
motorized-fader feedback loops. `tick(now, force)` repaints
everything after reconnect.

## Profiles (§8, §47-48, §54)

Versioned JSON (`profileVersion: 1`), pure data — never code.
Contains manufacturer/model/nameContains, `protocolRev` (§54),
capabilities, mappings, feedback outs.

- **Factory**: `installFactory()` — in-memory, ships in the binary,
  can never be written/deleted/renamed (§48).
- **User**: `installProfile()` — `dir/<id>.json`; `duplicateProfile`
  (works on factory → user copy), `renameProfile`, `deleteProfile`,
  `resetToFactory`. Auto-match prefers user over factory.

## MIDI Learn & Monitor (§50-52)

`learnArmFor(dev, template)` → next relevant input captures `in` →
mapping added + `onLearnedMapping` hook + notification. Diagnostics
ring (256 entries, bounded) logs direction, timestamp, decoded
control name (§50); `dump()` is the §51 raw logger export — bounded,
never auto-saved. Latency stats: avg/max µs input→action.

## DDJ-FLX4

- `flx4::Flx4Plugin` consumes: shift notes, pad-mode selectors, pad
  banks (→ `PadEngine`), jog family (→ `JogWheelEngine`), browse
  rotary, shift+SYNC tempo-range cycle, mode-selector LED repaint.
- `flx4::buildProfile()` — everything else declaratively. `[OFF]`-
  sourced entries `verified=true`; community-only ([MIX]/[SLP])
  entries `verified=false` (UNVERIFIED — functional, flagged).
- `protocolRev = "flx4-midi-fw1.02"`; USB VID/PID unknown — match by
  OS name only. See `docs/controllers/ddj-flx4.md` for the full
  verification matrix and real-hardware checklist.

## Real-time safety (§61, §64-65)

Controller processing never touches the audio callback: messages are
pumped on the UI/MIDI thread, actions become `rt::CommandQueue`
pushes or `CommandManager` invokes. Floods are O(1) per message
(hash lookup + flat buckets); measured ~390–400k msg/s on the test
rig with zero drops. `ControllerEngine` and `AudioDeviceEngine` are
independent — an FLX4 controller can drive Onda through any other
audio interface.

## Debugging

- Monitor ring: `diag().ring()`/`dump()` — IN/OUT lines with decoded
  control; SysEx shown as bounded hex.
- `state(deviceId)` — full snapshot incl. lifecycle + lastError.
- `findIssues(profile.mappings)` — validate a user edit offline.
- Tests: `[ctrl]` (core), `[flx4]` (device), `[ctrl27]` (this phase)
  via `VirtualMidiController`/`VirtualFlx4` — no hardware needed.
