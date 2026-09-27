# Mixer Engine (Phases 17, 24, 32)

Completion phase for the professional mixer. Phase 17 built the
foundation (remappable sources, cue system, filter resonance, scenes,
presets, diagnostics, 2/4-deck modes); Phase 24 adds the missing
spec components: per-channel **pan**, a **true isolator**, dedicated
**Booth** and **Recording** buses, **sample-scheduled parameter
events**, scene v2, and the spec-named presets.

## Signal flow

```
Deck A ──→ Channel 1 ──┐
Deck B ──→ Channel 2 ──┤
Deck C ──→ Channel 3 ──┼→ Crossfader → Master FX ─┬→ Booth bus → booth outs
Deck D ──→ Channel 4 ──┘                        ├→ Master bus → master outs
Sampler ─→ Channel 5                            ├→ RecordingBus → recorder ring
External─→ Channel 6..8                         └→ CueManager → HeadphoneBus → cue outs
```

Per channel (one `ChannelStrip`, no source-type coupling):

```
source renders into inL/inR   (deck i, sampler, external, silence)
  → insert FX rack            (FXEngine, prepared off-RT)
  → trim (smoothed) × polarity (phase invert, smoothed)
  → EqProcessor               (Standard 3-band | Isolator 3-way split)
  → FilterProcessor           (Position bipolar | LP | HP, resonance)
  → [pre-fader meter + PFL cue tap]
  → fader (curve, smoothed) × mute × solo × instant-cut
  → PanProcessor              (balance law, smoothed)
  → [post tap → FX send buses A/B]
  → crossfader assignment     (A / B / Thru — configurable per strip)
  → master sum

master sum → master insert FX → ┬ booth tap (pre-master-gain!)
                                └ master gain/mute → limiter → meters
                                  → master outs + RecordingBus push
cue sum    → CueManager (PFL + optional master-cue) → HeadphoneBus
             (cueVol × cueMix) → cue outs
```

Tap-point rationale: **booth taps pre-master-gain** — the DJ's booth
level is fully independent of the house level and master mute (§33).
**Recording taps post-master** — the file captures exactly what the
audience hears, limiter included (§34).

## Components

| Component | File | Role |
|---|---|---|
| `MixerEngine` | `core/mixer/MixerEngine.*` | source render → strips → buses |
| `ChannelStrip` | `core/mixer/ChannelStrip.h` | trim/EQ/filter/fader/pan/meter |
| `EqProcessor` | `core/mixer/EqProcessor.h` | Standard shelf cascade + mode switch |
| `IsolatorProcessor` | `core/mixer/IsolatorProcessor.h` | §12 — 3-way filter bank, true kill |
| `FilterProcessor` | `core/mixer/FilterProcessor.h` | bipolar position / LP / HP + resonance |
| `PanProcessor` | `core/mixer/PanProcessor.h` | §22 — balance law, smoothed |
| `CrossfaderEngine` | `core/mixer/CrossfaderEngine.h` | A/B/Thru assign + curves |
| `MasterBus` | `core/mixer/MasterBus.h` | gain/mute → limiter → meters |
| `BoothBus` | `core/mixer/BoothBus.h` | §33 — independent level + meters |
| `RecordingBus` | `core/mixer/RecordingBus.h` | §34 — post-master recorder seam |
| `HeadphoneBus` | `core/mixer/HeadphoneBus.h` | cue/master blend for phones |
| `MeteringEngine` | `core/mixer/MeteringEngine.h` | peak/RMS/hold/clip atomics |
| `MixerDiagnostics` | `core/mixer/MixerDiagnostics.h` | POD snapshots + report |
| `ParameterRegistry` | `core/mixer/ParameterRegistry.h` | hardware/UI-neutral param ids |
| `MixerController` | `core/mixer/MixerController.h` | validated control surface |
| `MixerScene` / `MixerPresetManager` | `core/mixer/` | v2 snapshots + factory presets |

## EQ modes (§9–§12)

`EqMode::Standard` — cascaded biquads: low shelf 250 Hz, mid peak
1 kHz Q 0.8, high shelf 4 kHz; registry range −26…+6 dB; gains ramp
≤3 dB/block.

`EqMode::Isolator` — `IsolatorProcessor` filter bank:

```
lo  = LP4(x)                (LR4, 300 Hz default)
mid = LP4_5k(HP4_300(x))    (band-formed, 300 Hz..5 kHz)
hi  = HP4(x)                (LR4, 5 kHz default)
out = lo·gL + mid·gM + hi·gH
```

A shelf-based "isolator" was rejected: at −72 dB the RBJ shelf slope
leaks ~−4.6 dB into neighbouring bands, so a band can never reach
silence. The filter bank gives real spectral separation — a killed
band's contribution approaches silence. Unity reconstruction carries
a few dB of ripple at the crossover edges (same character as hardware
isolators). Band gains are linear scalars smoothed per-sample (~1.5 ms);
coefficients are static → no coefficient steps, ever.

Kill semantics: target ≤ −26 dB (the registry detent) maps to −72 dB
applied; ramp ≤6 dB/block so a kill lands in ~80 ms.

## Pan (§22)

`PanProcessor` — stereo **balance** law (not mono constant-power):
`gL = 1−max(0,p)`, `gR = 1+min(0,p)`; centre = unity gain so pan = 0
leaves the mix untouched; ±1 fully removes the opposite side.
Position smoothed ~6 ms. Applied **post-fader** — pre-fader meters
and PFL cue stay honest; FX sends and the master sum carry the panned
image.

## Buses

**MasterBus** (existing): smoothed gain/mute → limiter (toggleable)
→ peak/RMS/hold/clip meters → mapped outs.

**BoothBus** (new): taps the program after master insert FX, before
master gain/mute/limiter. Own smoothed gain (0…2), meters, clip
telemetry; routes to `outMap[4..5]` (−1 disables). `idle()` keeps
smoothers advancing when unmapped.

**RecordingBus** (new): named seam that owns the post-master tap.
`AudioEngine::attachRecorder()` now publishes the `RecorderRt*`
atomically into the bus; `MixerEngine::render` calls
`recording_.push(mixL, mixR, n)` each block → the same bounded SPSC
ring as before, zero extra copies, ~free when detached.

**HeadphoneBus** (existing): `(cueSum·(1−mix) + masterPost·mix) · cueVol`
→ `outMap[2..3]`.

## AudioBus contract (§5, Phase 32)

`mixer::AudioBus { float *L, *R; int frames; }` is the formal
non-owning stereo window a producer fills each render block and a
`ChannelStrip` consumes — decks, the sampler and future external-in /
FX-return sources all hand the mixer the same shape. The strip never
knows what produced the signal; the producer never knows which
channel it feeds (routing lives in `ChSource`, not the bus).
`ChannelStrip::inBus(frames)` returns the handle over its RT-owned
input buffers; ownership never leaves the strip, so the callback
stays allocation-free.

## Crossfader orientation (§26, Phase 32)

`BusParam::XfReverse` (0/1, registry id `0x40D`) toggles
hamster/reverse on `CrossfaderEngine`: with reverse on, position −1
plays rail B and cuts rail A — the swap happens inside
`gainFor(assign)`, so it composes with every curve including
`Custom` (the `sideA` flag is what flips) and leaves `Thru`
untouched. Exposed through `MixerController::xfReverse`,
`MixerBusDiag::xfReverse`, `MixerScene::xfReverse`, the
`mixer.xfReverseToggle` command, and the REV button in `MixerView`.

## UI parameter surface (§9, §48–§49, Phase 32)

`MixerView` knobs/faders now take their ranges from
`ParameterRegistry::find(id)` (`rngLo/rngHi`) — the same table the
RT path sanitizes with — instead of hardcoded literals. `Knob`
gained `setCenterMark(on, value)`: a tick at the neutral detent
(EQ/filter/pan at 0, trim at unity gain). The filter knob's caption
announces the sweep direction (`HPF`/`LPF`/`FILT`) around the
center. Double-click return-to-default is unchanged.

## Latency budget (§58–§59, Phase 32)

Parameter-update latency is deterministic and documented:

| stage | bound |
|---|---|
| UI/controller → `MixerController` sanitize + `CommandQueue::tryPush` | non-blocking, same call |
| queue drain at block start | ≤ 1 audio block (~10.7 ms @ 512/48 k, less at smaller buffers) |
| unscheduled `SetParam` → smoother settle | per-param `smoothMs` time constant (xf 5 ms, gains 8 ms); ~5τ to full convergence |
| scheduled `ParamEvent` | applies on the exact `targetFrame` (render splits at the boundary) — zero quantization error |
| insert/master DSP | `masterLatencySamples()` / `fxLatency()` report algorithmic delay (0 today — all FX are zero-latency IIR) |

Cue path (§59): the PFL tap is pre-fader inside the strip, so
cue-on latency is the same ≤1-block + 8 ms smoother budget — no
extra queue hops, and the headphone mix adds no buffering.

## Scheduled parameters (§44)

`rt::ParamEvent { uint16_t id; float v; int64_t targetFrame; }` rides
the existing bounded `CommandQueue`. The audio thread maintains an
absolute `audioFrame_` counter; events landing inside a block park in
a 32-deep RT-owned pending set, and `AudioEngine` **splits the render
at the event boundary** — the parameter applies on the exact frame,
segments render contiguously. Overflow applies immediately (counted
in `schedOverflow_`) — a user edit is never silently dropped.

```cpp
engine.scheduleParam(id, value, engine.audioFrame() + 4800); // +100 ms
```

## Parameter exposure (§40–§41)

New channel ids (`ChParam`, 5-bit kind per channel ×8):
`Pan` (−1…1, smoothMs 6), `EqMode` (0/1), `FilterType` (0..2).
New bus id: `BusParam::BoothGain` = `0x40B` (0…2). All reachable via
`MixerController`/`setParam`/`ParameterRegistry::sanitize` — same path
UI, MIDI, HID and automation use.

## Persistence (§53–§54)

`MixerScene::kVersion = 2` adds per-channel `pan`, `eqMode`,
`filterType` and bus `boothGain`; v1 defaults reproduce a v1 mixer
exactly. Capture/apply are table-driven through `kSceneChParams` /
`kSceneBusParams` → `sceneField` — new fields round-trip through the
existing controller machinery.

Factory presets now include the spec names: **Default** (neutral),
**Classic** (EQP crossfader, standard EQ), **Modern** (cut crossfader,
audio-taper faders, isolator EQ on deck channels, limiter on).
**Custom** is whatever the user stores via `MixerPresetManager::store`
(deduped by name). Phase-17 names (Flat/Scratch/…) are preserved for
compatibility.

## Real-time contract

Unchanged and enforced by tests: no allocations, locks, file I/O or
logging inside the callback; every parameter is POD through bounded
queues; smoothers tick on RT-owned state; meters are atomic writes.
Booth adds one `memcpy` of the program into preallocated scratch;
RecordingBus is a null-check plus an SPSC write; ParamEvents add a
32-entry linear scan only when events exist.

## Validation

`tests/mixer_phase24_tests.cpp` — 16 cases:

- pan law (unity centre, full-travel, click-free sweeps)
- isolator: detent → ~silence vs Standard −26 dB residue; per-band
  selectivity; slam-move stability
- booth: independence from master gain/mute, meters + clip telemetry
- recording: armed ring receives post-master program; detach → free
- registry ids + sanitize + engine routing for all new params
- typed filter mode (LP shapes regardless of position)
- scene v2 round-trip incl. diagnostics fields
- Default/Classic/Modern/Custom presets through the controller
- 120-block automation hammer (EQ/filter/fader/xf/pan/master): finite,
  bounded-step output
- 4-deck routing/metering/cue/xf-assignment
- decks + sampler channel coexistence
- 400-block stress (4 decks + isolators + filters + xf + limiter +
  booth + recorder): all-finite, bounded callback time

## Limitations

- EQ range remains −26…+6 dB at the registry surface (the isolator's
  deeper range is internal to `IsolatorProcessor`); exposing a
  per-mode configurable knob range is follow-up UI work.
- `External`/`Mic`/`Line` source types are declared seams — silent
  until the device layer feeds strip inputs.
- Booth/record output *device* mapping is a config concern; the
  mixer only exposes the two extra `outMap` slots.
- Custom fader/crossfader curves use `std::function` — install on the
  control thread only (unchanged Phase-17 contract).
