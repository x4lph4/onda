# ADR 0021 — Professional stems engine

Phase 21. Status: implemented.

## Context

Modern DJ products ship "stems" — real-time performance on separated
musical components (vocals/drums/bass/instruments). The spec demands
a model-swappable architecture: separation off the RT/UI threads,
persistent results, per-stem mute/solo/gain/pan, pad + UI control,
original-track fallback, and 4-deck scalability — without duplicating
`DeckEngine`, `MixerEngine`, or the UI layer per stem.

## Decisions

1. **Stems ride the deck's existing timeline.** A `StemSet` is four
   (≤9) interleaved-f32 stereo buffers at the *device* rate — the
   same internal format as `TrackAudio`. A POD `DeckStemBuffers`
   view hands off through `pendingStems_`/`retiredStems_` — the
   identical lock-free protocol as tracks and grids — and
   `DeckEngine` samples stems at `pos_` inside `renderVarispeed`
   and `renderStretch` (WSOLA feed is staged through a preallocated
   `stemStage_` chunk). Varispeed, key-lock stretch, sync, slip,
   loops, reverse apply identically (§36-38) with *zero* duplicated
   transport code and no second clock.

2. **The model is a pure interface seam.** `StemSeparationModel`
   (load/unload/analyze/separate/capabilities/info) +
   `StemModelProvider` (hardware probe → `StemModelConfig`,
   `createModel`). `DeckEngine`/`MixerEngine`/UI never name a model.
   The shipping `BandSplitModel` is deterministic DSP whose
   instruments stem is the exact residual — sum(stems) ≡ source —
   giving honest labels, perfect-reconstruction tests, and no model
   files or dependencies.

3. **Background-only processing.** `StemScheduler` mirrors the
   proven `AnalysisQueue`: worker threads, priority heap,
   cooperative cancel via a polled `StemCancel` token, and a CPU
   gate (`setMinActivePriority`) so separation yields while playback
   is active (§45). Phase 21's runner also had to fix a real bug —
   the `cv_.wait` predicate must *peek*, not pop (predicate-side
   `topRunnable()` destroyed jobs).

4. **Cache = files, store = metadata (§29).** `StemCache` writes
   float32 WAVs + a `meta` sidecar under a key =
   `FNV(contentHash|modelId|version|quality|rate|channels)` —
   relocation-safe, fully config-versioned, atomic via tmp-dir →
   rename. `stem_sets` (schema v7) holds only availability records;
   audio never enters SQLite. `readRange` is the §31 streaming seam.

5. **Solo/mute semantics are explicit and smoothed.** §14: any solo
   silences all non-solo stems; mute beats solo per stem. Resolved
   targets feed per-stem `smL/smR` one-pole ramps inside the deck —
   toggles are click-free. Pan is constant-power (§16). A stem action
   auto-enables stems mode when a set is present.

6. **Fallback is unconditional (§25/§41).** No adopted set, retired
   set, mode off, or failed model → the original buffer renders.
   Decode/model failure writes a `Failed` store row so the library
   shows STEMS FAILED instead of retrying forever.

7. **Member destruction order is part of the contract.**
   `StemsEngine::sched_` is declared last so its destructor joins
   workers *before* `mu_`/`produced_`/`cache_`/`store_`/`diag_` die —
   a job in flight at shutdown touches all of them. Decks must
   outlive `StemsEngine` (it clears `pendingStems_` on bound decks in
   its destructor).

8. **One set, many decks.** Published sets are shared read-only
   (`shared_ptr<const StemSet>`) across decks playing the same
   track — each deck gets its own handle/view for per-deck retire
   tracking without copying buffers (§44).

## Alternatives rejected

- **Separate stem transport/sync** — would duplicate loops, slip,
  slip-shadow, WSOLA feeding, beat-boundary math; guaranteed drift
  bugs. Riding `pos_` is strictly stronger.
- **Stem buffers as a second mixer layer** — stems already reach
  trim/EQ/filter/fader/PFL through the deck's `ChannelStrip` (§19);
  a parallel mixer doubles code and meters.
- **Store PCM blobs in SQLite** — violates §29; files stream better
  and the DB stays small.
- **Model runs on analysis queue** — `AnalysisQueue` owns library
  analysis jobs; stems need quality/GPU config and deck-bound
  publishing, so a dedicated `StemScheduler` keeps both honest.

## Consequences

- `DeckEngine` grew `stems_`/`stem_[]`/`stemStage_` + `setStemMode`/
  `setStemParam` — still allocation-free, lock-free RT.
- New rt commands: `SetStemMode`, `SetStemParam` (queue capacity
  unchanged).
- Command ids `0x0090-0x0093`, `0x00C0+` per deck (no collisions with
  `DeckMemCueBase 0x00A0+`).
- `PadMode::Stems` now *implemented* — pads 1-4 mute (shift=solo),
  5-8 gate; `PadFeedbackEngine` gains `stemFlags` states.
- `StemsPanel` is snapshot-fed (`stemFlags`/`stemGain`/`stemsAvail`/
  `stemMode`); zero engine access from the view.
- Learned models plug into `StemModelProvider::createModel` — this
  ADR's seams (interface, cache key, scheduler, deck handoff) don't
  change.
