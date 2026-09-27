# ADR 0034 — Professional sampler (retrigger, extended quantize, envelope, filter, windows, missing/relink, undo)

## Status

Accepted — Phase 34.

## Context

Phase 11 (`docs/adr/0011-sampler-performance-pads.md`) built the
sampler core: `SamplerEngine` façade, `SamplerRt` (32 slots × 32
voices, atomic mailbox, fixed voice pool), async `SampleLoader`,
`SampleCache` (LRU + pin + byte cap), `SampleRepository`,
`OneShot/Loop/Hold/Gate` modes, six quantize divisions, four steal
policies, pitch via rate-scaling, fixed anti-click envelope, mixer
channel-4 render, and the pad stack
(`PadLayer → PadModeManager → SampleTrig/Release/Stop`) that the
FLX4 mapping consumes.

The Phase-34 audit found the remaining spec gaps:

- **§9 Retrigger** — every same-slot press choked the playing voice
  (shared path with OneShot/Loop/Hold); no true polyphonic restart.
- **§20 quantize** — missing `1/2`, `2Bar`, `4Bar`; stop was always
  immediate (no quantized stop, §23).
- **§28 envelope** — attack/release were hard constants
  (`kFadeIn`/`kFadeOut`), identical for every slot.
- **§32 filter** — `SP_Filter`/`filterPos` was declared, persisted
  and mirrored but never applied in `renderVoice`.
- **§59–61 windows** — no trim start/end or loop in/out metadata;
  playback was always the full decoded range, loop wrapped at the
  sample end to position zero.
- **§65 missing samples** — a decode failure marked the slot
  `Error`, but vanished files were never detected and no
  locate/relink/remove workflow existed.
- **§62 undo** — no metadata edit history.

## Decision

- **`PlaybackMode::Retrigger = 4`** — appended (persisted indices
  stay valid). `startVoice` skips the same-slot choke for Retrigger;
  stealing still bounds the pool at 32.
- **`QuantizeMode` extended** (`Q_1_2=6`, `Q_2Bar=7`, `Q_4Bar=8`)
  via the existing `quantizeBeats` table — one math path, no new
  clock. **Quantized stop** reuses `quantDelay`: `onStop`/`onRelease`
  set `voice.releaseAfter` instead of `releasing` when the slot
  quantizes; the release opens exactly on the boundary.
- **`SampleEnvelope` per slot** — `attackMs`/`releaseMs` on
  `SampleSlot` → `SlotRt` → `voice.envInc/envDec` computed once per
  trigger (`ms → env step`, algebraic). `0` keeps the Phase-11
  defaults so behavior of untouched slots is identical.
- **Tilt filter per voice** — `SP_Filter` now drives a one-pole:
  `f<0` LP sweep 20 kHz→80 Hz (cubic taper), `f>0` HP tilt
  40 Hz→9 kHz, coefficient `w/(fs+w)` computed once per trigger —
  zero per-sample branching when bypassed, no transcendentals.
- **Non-destructive window** — `startF/endF/loopIn/loopOut`
  (decoded frames, −1 = unset) mirrored through `SP_TrimStart/End`,
  `SP_LoopIn/Out`. `renderVoice` resolves `playEnd` as
  `min(endF, loopOut)` when a valid region loops, so a mid-sample
  loop region repeats while trims still bind it.
- **Missing/relink** — `SampleSlot::missing` atomic +
  `checkMissing()` (whole-library path re-validation, called by
  `loadAll` and on demand) + `relinkSample(pad, path)` which keeps
  every metadata field and only re-points the file. Removal stays
  explicit (`unloadSample`).
- **Undo seam** — gesture-scoped: `beginSlotEdit` /
  `commitSlotEdit` capture before/after snapshots (64-deep);
  `undoSlotEdit`/`redoSlotEdit` restore editable fields only (never
  path/sampleId) and re-mirror to RT on the active bank. Sits fully
  on the control thread.
- **Repository** — seven new `sample_slots` columns added by
  guarded `ALTER TABLE` inside `init()` (PRAGMA-checked →
  idempotent); old DBs migrate in place.

## Consequences

- Retrigger relies on the existing steal path for its bound — 64
  presses on one pad saturate at `kMaxVoices` and degrade via the
  configured policy, never via allocation.
- Quantized stop inherits the clock-snapshot semantics: it uses the
  last `setClock` feed — correct while AudioEngine refreshes the
  clock per block, documented for standalone RT use.
- The tilt filter is intentionally cheap (one pole) — a resonant
  multimode per voice is the documented future step, not this one.
- `Gate` keeps its Phase-11 name; spec "TOGGLE" is the same
  semantic (press toggles play/stop).
- Undo is gesture granularity, not per-`setSlotParam` — a knob drag
  collapses to one step; API consumers own the begin/commit pairing.
- `docs/sampler/sampler-engine.md` documents the full subsystem;
  `[smp34]` adds 13 test cases / 92 assertions covering every delta
  plus engine→queue→RT and repository round-trips.
