# Mixer Engine (Phase 17)

Professional mixer architecture — deepens the Phase-5 foundation into
the complete spec surface: remappable sources, formal cue system,
filter resonance, configurable EQ range, phase invert, scenes,
presets, diagnostics, and 2/4-deck modes.

## Signal flow

```
Deck A ──→ Channel 1 ──┐
Deck B ──→ Channel 2 ──┤
Deck C ──→ Channel 3 ──┼→ Crossfader → Master FX → Master
Deck D ──→ Channel 4 ──┘                       │
Sampler ─→ Channel 5                           ├→ Recording tap
External─→ Channel 6..8                        │
                                               └→ Headphone/Cue
```

Per-channel detail (one `ChannelStrip`, no deck coupling):

```
source renders into inL/inR      (decks, sampler, external, silence)
  → insert FX rack               (FXEngine target DeckA..D/ch)
  → input trim (smoothed) × polarity (phase invert, smoothed)
  → EqProcessor (3-band, 4th spare, modes, configurable range)
  → FilterProcessor (LP/HP/bipolar, cutoff + resonance, smoothed)
  → [pre-fader meter + PFL cue tap]
  → fader (curve, smoothed) × mute × solo × instant-cut
  → [post tap → FX send buses A/B]
  → crossfader assignment (A/B/Thru)
  → master sum
master sum → master insert FX → master gain/mute → limiter → meters
           → device outs (mapped) + recording tap + master-cue tap
cue sum    → CueManager master-cue injection → HeadphoneBus
             (cueVol × cueMix) → cue outs
```

## Channel sources (§3/§34)

`ChSource{type,index}` packed into per-channel atomics — written on
the control thread, decoded every block. Any deck can feed any
channel; out-of-range indices render silence until config catches up.

- `setChannelSource(ch, src)` / `mapDeckToChannel(deck, ch)` — remap.
- `ChSourceType`: `Deck`, `Sampler`, `External` (seam — silence until
  the device layer feeds it), `Silence`.
- Routing graph (`RoutingEngine`) tags source nodes (`'deck'`, `'smpl'`,
  `'ext '`, `'sil '`) so a routing UI can introspect the table.

## Deck modes (§32/§33)

`setDeckMode(2|4)` remaps sources: channels ≥ n go silent (memset +
insert-FX skip — the strips themselves are never removed). The sampler
channel (4) stays live in every mode. `MixDeckModeTgl` flips 2↔4 and
the UI hides C/D strips via `MixerView::setChannelCount` — backend
state untouched.

## Cue system (§17–20)

- Per-channel PFL lives on the strips (pre-fader, post insert-FX —
  the DJ hears exactly what the channel produces).
- **Master cue**: `CueManager` adds a smoothed tap that injects
  post-master program (post-limiter — what the audience hears) into
  the cue sum. `BusParam::MasterCue` 0/1, ~6 ms clickless ramp.
- `HeadphoneBus`: `phones = (cueSum·(1−mix) + masterPost·mix)·cueVol`
  — cue and master remain fully independent; nothing in the cue path
  writes back into the mains.

## Filter resonance (§10–11)

`ChParam::FilterRes` (0.2–8 Q, default 0.707) applies to LP, HP and the
bipolar position sweep; coefficients refresh at block rate behind a
dirty flag — MIDI/hardware sweeps stay smooth, no zipper noise.

## EQ range (§9)

`EqProcessor::setRangeDb(lo,hi)` — default −30/+12 internally
(registry clamps the control surface to −26/+6); isolator-style
presets can reach −60. Re-clamps existing band targets on change.

## Phase invert (§40)

`ChParam::PhaseInvert` — smoothed ±1 multiplier in the trim stage
(~0.5 ms through zero — clickless, no discontinuity).

## Scenes (§38)

`MixerScene` — versioned POD of every channel field + bus param +
source table + deck mode. `MixerController::captureScene/applyScene`
round-trips commanded values (what the user set, not smoothed RT
state); `sceneField()` maps each `ChParam`/`BusParam` to its field so
capture and apply can never drift apart. 8 recall slots live in
`EngineBridge` (`MixSceneSave/Recall` commands).

## Presets (§47)

`MixerPresetManager` — factory configurations: **Flat** (neutral),
**Scratch** (Cut xf curve + taper faders), **Broadcast** (limiter on,
conservative master), **Booth Monitor** (master cue), **Dub Sends**
(staged sends + hot returns), **Two Deck** (deck mode 2). User scenes
register into the same bank.

## Diagnostics (§40–41)

`MixerEngine::snapshot(bus, chans, n)` fills copyable POD
(`MixerBusDiag`/`MixerChannelDiag`): per-channel state, meters,
clips, solo/cue counts, bus levels, limiter GR, latencies, deck mode.
`diagnosticReport()` → clipboard via `MixDiagCopy`.

## Latency (§42)

`latencySamples(ch)` reports the insert-rack latency per channel and
`masterLatencySamples()` the master rack's — the PDC contract.
Every current processor reports 0; the seam exists so compensation
measures real numbers before acting (no fake alignment).

## Grouped edits (§39)

`MixerController::setParamEdit` + `begin/endParamEdit` — a knob drag
or fader ride coalesces into one `{id, from, to}` delta for an
UndoManager while every intermediate value still reaches the engine.
Edits on a different param auto-close the open one.

## Commands (0x2F09–0x2F0F)

| command | action |
|---|---|
| `mixer.masterCueToggle` | master → phones tap on/off |
| `mixer.deckModeToggle` | 2-deck ↔ 4-deck sources |
| `mixer.diagCopy` | diagnostics text → clipboard |
| `mixer.sceneSave` / `sceneRecall` | scene slots 0..7 (value = slot) |
| `mixer.presetNext` / `presetPrev` | cycle + apply preset bank |

Same `CommandManager` path UI/keyboard/MIDI/HID/FLX4 share — the
mixer never sees a hardware handle.

## Real-time safety

- Source table: packed atomics; render decodes per channel — no
  locks, no allocations.
- Silenced sources: memset + insert-FX skip; strips still process
  zeros (meters decay, post taps stay clean for send reads).
- Master cue: per-sample smoothed gain, post-master tap only.
- All smoothing unchanged: trim/fader/mute/cut/phase on strips,
  vol/mix on phones, position on crossfader, gains on EQ bands
  (≤3 dB/block steps), filter position per-sample.

## Files

- `src/core/mixer/CueManager.h` — master-cue tap (§17–20)
- `src/core/mixer/MixerScene.h` — versioned state snapshot (§38)
- `src/core/mixer/MixerPresetManager.h` — factory presets (§47)
- `src/core/mixer/MixerDiagnostics.h` — snapshot + report (§40–41)
- `src/core/mixer/MixerTypes.h` — FilterRes, MasterCue, ChSource
- `src/core/mixer/FilterProcessor.h` — resonance
- `src/core/mixer/EqProcessor.h` — configurable range
- `src/core/mixer/ChannelStrip.h` — phase invert, silence()
- `src/core/mixer/MixerEngine.{h,cpp}` — source table, deck mode,
  master-cue tap, latency seam, diagnostics
- `src/core/mixer/MixerController.h` — routing sinks, transactions,
  scene capture/apply
- `src/ui/CommandIds.h`, `src/ui/EngineCommands.{h,cpp}` — 0x2F09+
- `src/app/ui/EngineBridge.{h,cpp}` — scene bank, preset manager,
  diag copy, snapshot fields
- `src/app/ui/MixerView.h` — channel count + master-cue button
- `src/app/ui/AppShell.h` — deck-mode → strip visibility
- `src/ui/UiState.h` — MasterSnapshot.masterCue/deckMode
- `tests/mixer_phase17_tests.cpp` — 17 cases

## Limitations

- `External` sources render silence until the AudioDeviceEngine input
  layer feeds them — the descriptor is the seam.
- Latency compensation is reporting-only (all processors are 0
  samples); real PDC lands when a latency-bearing processor exists.
- Split cue (L/R PFL/master) not implemented — cueMix covers the
  blend; the CueManager is where it would extend.
- `sendGain()` block-rate smoothing on sends is coarser than the
  per-sample trim — acceptable at send levels, noted for future
  per-sample smoothing.
