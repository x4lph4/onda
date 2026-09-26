# ADR-0006: Professional FX Engine

## Status

Accepted — implemented in Phase 6.

## Context

Phase 5 left explicit extension points for effects: `FxSend`/`FxReturn`
nodes in the routing graph, a post-fader tap in `ChannelStrip`, and a
parameter address space that only covered the mixer. Phase 6 builds the
FX engine on those points: a modular, low-latency system that can host
insert effects, send/return buses, beat-synchronized time effects and
master processing — without the engine ever knowing an effect's
internals, and without the UI knowing anything but parameter ids.

## Architecture

```
FXEngine                              (one per MixerEngine, non-RT owned)
├── FXRack  × kNumTargets             DeckA..DeckD, SendA, SendB, Master, Spare
│   └── FXSlot × kMaxSlots            bounded chain, sequential processing
│       └── FXProcessor               Delay / Echo / Reverb / Filter /
│                                     Flanger / Phaser / Gate  (+ None)
├── Send buses × 2                    sendL/sendR scratch, accumulated per block
├── SyncContext per deck              bpm from DeckEngine::effBpm / sync master
├── FXPresetManager                   factory + user presets, state blobs
└── FXEvent FIFO                      RT -> control events (POD, polled)
```

Every effect implements `FXProcessor`: `prepare() / process() / reset() /
setParam*(native|normalized) / latencySamples() / setSyncContext() /
saveState() / restoreState()`. `FXBase<N>` provides the
normalized↔native mapping plus per-sample `GainSmoother`s, so every
parameter of every effect is smoothed identically and effects never
see a discontinuity. The engine only drives this contract — adding an
effect means writing a class and one `createFX` switch arm.

### Signal flow (verified in `MixerEngine::render`)

```
Deck d render → strip[d].inL/inR
  → fx.processDeckInsert(d)      INSERT rack, in-place, pre-strip
  → ChannelStrip: trim → EQ → filter → [pre-fader meter + PFL cue tap]
                  → fader × mute × solo × cut → postL/postR tap
                  → xf-assign gain → mixL/mixR
post tap → sendAccum(bus, post × sendGain) → send rack → × returnLevel
           → summed into mixL/mixR
mixL/mixR → fx.processMaster → MasterBus (gain → limiter → meters) → out
```

Cue tap stays **PFL, post-insert**: headphones hear the deck including
its insert FX but before fader/mute — the standard pre-listen. Send
taps are **post-fader, pre-crossfader-assignment**: a channel pulled
out by the crossfader can still feed an echo trail only while its
fader is up; send level = fader position, never crossfader position.

### Beat sync

`AudioEngine` feeds `mixer.setMasterTempo(decks[master].effBpm())`
each callback; `MixerEngine` additionally pushes every deck's own
`effBpm()` into `fx_.setTempo(d, …)`. Each `SyncContext` carries
bpm + `synced` + `division`; `divisionFrames()/divisionMs()` derive
sample-accurate lengths for 1/32…4 bars from the *source's* tempo —
no UI involvement. Delay/Echo read synced time inside `process`;
Flanger/Phaser/Filter-LFO quantize one LFO cycle per division.

### Slot mechanics

- `loadEffect()` allocates a new processor **off the audio thread**
  (via `MixerController`/engine callback), holds it as *pending*; the
  rack adopts it at the next `process()` — the RT path never allocates.
- Enabled/bypass is a smoothed wet crossfade (`smEn_`), so toggles are
  click-free; a disabled slot whose ramp finished hibernates and costs
  nothing (`busy()` accounts for pending effects so adoption happens).
- I/O safety: input is sanitized before the dry copy is taken
  (NaN/Inf → 0, ±`kClamp`), the wet path is sanitized after `process()`
  (same rule + runaway detector: ≥4 consecutive bad blocks resets the
  effect), and `sanitized`/`cpuUs`/in/out peaks are published
  atomically for diagnostics.
- `DelayLine::read` wraps the read point in both directions — a
  negative offset followed by `rp += sz` can round to exactly `sz` in
  float32 on large buffers (found via an out-of-bounds read on the
  reverb pre-delay; this is why the wrap is `if (rp >= sz) rp -= sz`
  rather than a comment).

### Parameter space

`fx::paramId(target, slot, kind)` = `0x1000 | target<<8 | slot<<4 | kind`.
`ParameterRegistry` recognizes the space and serves names/ranges/
defaults/steps to `MixerController::setParam` — UI, MIDI, HID and
automation share one address space with the mixer. `Type` is the only
kind that leaves the RT path: `setFxType` routes through the non-RT
loader callback because swapping an effect allocates.

### Presets & state

`FXPreset` = kind + slot header + native params (no pointers).
`FXPresetManager` captures from a slot, restores through `createFX` +
`restoreState`, ships factory presets per kind, and serializes a whole
engine (`serializeAll`) as `{version,count,target,slot,FXState…}` —
enough to rebuild a session.

## Latency

Every effect reports `latencySamples()`; today all report 0 (the
reverb's pre-delay is a *wet-path* delay, not throughput latency).
`FXSlot::latency()` and rack sums exist; compensation is a stub until
an effect actually needs lookahead (e.g. a limiter with look-ahead).

## Verification

25 `[fx]` cases (772 assertions): per-effect impulse/sine/noise/
silence, decay and pre-delay timing, ping-pong stereo offset, synced
vs free timing (120 BPM → division frames), slot bypass crossfade
continuity, NaN/Inf/±50 input → sanitized + ≤ ±8 out, param
smoothing, presets round-trip + state serialization, engine routing
(send/insert/master), parameter registry coverage, and the mixer
integration trio (insert into channel, post-fader send/return,
master rack). Full suite: 116 tests / 1,592,245 assertions — the
500-block deck+FX stress run keeps output finite and stable.

## Limitations

- Insert racks run on `strip.inL` — pre-trim/EQ. A "post-channel"
  insert point (after fader) is a wiring change, not an architectural
  one; the same rack class serves both spots.
- Send buses are fixed at 2 (`kSendBuses`) and mono-summed inside the
  send path is not supported — buses carry stereo.
- Sync tracks `effBpm` (resolved rate × grid bpm) — it follows the
  *tempo*, not the *downbeat phase*; synced FX align to division
  length, not grid phase (beat-phase sync is a documented next step).
- Echo freeze is bounded by feedback clamp + the slot's sanitizer;
  there is no dedicated freeze-decay envelope yet.
- `TypeRequested` events cover the swap path; per-parameter
  `ParamChanged` events exist but UI-side value echo is basic.
- Gate/Stutter ships as a tempo-synced trapezoid gate — beat-repeat
  semantics land on the same abstraction later.
