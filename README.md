# Onda (codename)

Professional desktop DJ application. C++20 + JUCE 8, GPLv3.

## Status — Phase 26 (audio device engine completion)

Completed the device layer on top of the Phase-13 `adevice` foundation
(`docs/audio/audio-device-engine.md`, `docs/adr/0026-audio-device-engine.md`)
— audit showed ~70% of the spec already existed; this phase closed the
real gaps.

- **`ChannelMapping` + booth pair** (§17): `boothL/boothR` with full
  validation (complete pairs, no overlap), `defaults()` maps booth on
  4/5 only with ≥6 detected outs, `DeviceProfile` JSON v2 (`bL/bR`,
  backward compatible), all three pairs flow to `setOutputMap` (6
  atomics).
- **Typed errors + status** (§31-32): `selectEx()` returns
  `DeviceResult{AudioError, message}` — `DEVICE_NOT_FOUND`,
  `CHANNEL_CONFIGURATION_FAILED`, `INITIALIZATION_FAILED`,
  `STREAM_START_FAILED`, `DEVICE_DISCONNECTED`,
  `MANUAL_CONFIGURATION_REQUIRED`, …; `AudioDeviceStatus` view
  (UNAVAILABLE…RECOVERING) from backend + recovery FSM; diagnostics
  print ROUTING/DEVICE STATUS/LAST ERROR.
- **`DjAudioProfile`** (§27-30): capability-based DJ-device profile —
  FLX4 with ≥4 detected outs → master 1-2 / phones 3-4; generic
  ≥4-out interface → master+cue(+booth ≥6); **never invented** —
  a 2-out "FLX4" gets no cue suggestion. Controller detection stays
  independent (`ctrl::`).
- **`autoRoute`** (§46): returns `{map, ok, reason}` — safe defaults
  or explicit *Manual Configuration Required*, never a guess; mono
  devices get a legal collapsed master.
- **Output stage** (`emitPair`, §15/§38-42): every mapped pair passes
  mono fold-down (collapsed pair = L+R sum, not last-wins), the
  ~40 ms stream-start fade (`onStreamRestart` from
  `audioDeviceAboutToStart`, kills restart pops/DC jumps), and
  `OutputGuard` — NaN/Inf/over-clamp sanitize + `|x|>1` counter +
  ~200 ms DC estimator per pair, all preallocated/atomic.
- **Headphone mono** (§24): `HeadphoneMono` registry id `0x40C`,
  smoothed L+R fold-down on the phones bus (distinct from routing
  collapse).
- **17 new `[adev26]` tests**: booth validation, autoRoute/manual,
  DJ profiles, typed errors + status, disconnect→recovery (ok+fail),
  profile v2, cue xf-independence, mix sweep, 4-deck cue, master cue,
  phones mono, OutputGuard, stream fade, mono master, mock-FLX4
  end-to-end, state preservation, 4-deck stress.

## Status — Phase 25 (FX engine completion)

Completed the professional FX semantics on top of the Phase-16 engine
(`docs/audio/fx-engine.md`, `docs/adr/0025-fx-engine.md`) — the audit
showed most of the spec already existed; this phase closed the five
real gaps.

- **Tail-aware bypass (§42)**: disabling a Delay/Echo/Reverb no longer
  cuts the tail — the slot keeps the wet gate open, feeds zeros, and a
  new dry gate (`smEnDry_`) returns the dry path to unity over the same
  10 ms ramp: output becomes `dry + wet·mix` (a true "echo out"), then
  the slot drains (wet peak < −80 dBFS or 8 s cap) and hibernates.
  `FXProcessor::hasTail()` drives the decision generically; `busy()`
  covers enabled-or-draining everywhere.
- **Real per-stem FX (§47)**: each deck owns `FXSlot stemFx_[4]`; a
  stem with an active slot is staged into a preallocated block buffer,
  processed, and summed — in both render paths (varispeed per block;
  keylock in source-domain chunks pre-stretch so the WSOLA output
  stays coherent). `FXEngine::bindStemSlots` maps them into the FX id
  space as `target=deck, slot=4..7` → same params/presets/MIDI/
  diagnostics as any rack slot.
- **Sampler insert routing fix**: `channelFxTarget` maps the sampler
  strip to `FXTarget::Spare` — previously it aliased `SendA` and
  processed that rack twice per block.
- **Tempo source select (§22)**: `SyncMode` 0/1/2 = MANUAL/DECK/MASTER;
  every block the slot stamps either its target ctx or the
  sync-master-deck ctx into the processor — tempo changes retarget
  smoothers, no steps (§23). Sends/master always follow the sync
  master.
- **Pending-param preservation**: P1..P8 writes that land between
  `loadEffect` and adoption are stashed and replayed on the new
  processor — fixes preset loads and controller writes racing the
  type swap.
- **Metadata + effect**: `FXParamType` tags on `FXParamInfo`
  (Float/Int/Bool/Enum/BeatDiv/Freq/Time/Ms/Db/Percent) for correct
  UI/controller rendering; `NoiseFX` added (registry param `Noise`).
- Diagnostics carry a per-deck `stems[4]` row; FXView shows Deck C/D,
  the Sampler rack and per-deck stem rows.
- 14 new `[fx25]` tests: tail drain at slot + stem level, the
  Vocals→Echo/Drums→HP/Bass→Phaser/Inst→Reverb simultaneous matrix,
  sampler→Spare (no SendA double-process), MANUAL/DECK/MASTER timing,
  pending-param stash, preset round-trip/duplicate, registry coverage,
  and a 4-deck + stems + sends + master stress run.

## Status — Phase 24 (mixer engine completion)

Completed the central mixer architecture (`docs/audio/mixer-engine.md`,
`docs/adr/0024-mixer-engine-completion.md`) on top of the Phase-17
foundation — new components only where the spec had real gaps.

- `PanProcessor` per channel: balance law (unity centre, true
  silence at the edges), ~6 ms smoothing, applied post-fader so
  meters/PFL stay honest; `channelN.pan` registry id + UI knob.
- `IsolatorProcessor` — true 3-way filter bank (`LP4` / band-formed
  mid / `HP4`, LR4 crossovers at 300 Hz/5 kHz) replacing the
  re-voiced-shelf placeholder: a band at the −26 dB detent reaches
  ~−72 dB (≈silence) with real spectral separation; linear band gains
  smoothed per-sample, static coefficients — no clicks, ever.
  `EqMode` exposed per channel and persisted.
- `BoothBus`: taps the program pre-master-gain → booth level is
  independent of house level and master mute; own gain/meters/clip
  telemetry; routes to new `outMap[4..5]` pair.
- `RecordingBus`: the mixer now owns the post-master tap —
  `attachRecorder` publishes the `RecorderRt*` atomically; render
  pushes each block into the same bounded SPSC ring, ~free detached.
- `rt::ParamEvent` (§44): POD `{id, v, targetFrame}` on the existing
  queue; absolute audio-frame counter + 32-deep pending set → the
  audio callback splits the render at the event boundary for
  sample-accurate parameter application; overflow applies early,
  never drops.
- Scene v2 (pan/eqMode/filterType/boothGain, v1-compatible defaults);
  spec presets Default/Classic/Modern added (Custom = user scene);
  UI: per-strip PAN knob, master-column BOOTH knob; diagnostics
  carry booth meters + recorder state + pan/eqMode per channel.
- 16 new `[mixer24]` tests (pan law, isolator kill/selectivity/
  stability, booth independence+telemetry, recording tap, registry,
  typed filter, scene v2, presets, 120-block automation hammer,
  4-deck routing, sampler coexistence, 400-block full-mixer stress).

## Status — Phase 23 (smart playlists + intelligent library tools)

Organisation, analysis-derived descriptors and explainable discovery
over the library (`docs/library/smart-library-engine.md`,
`docs/adr/0023-smart-library-engine.md`) — automates organisation
tasks, never makes mix decisions.

- `SmartLibraryEngine` facade over `MusicLibraryEngine`: smart
  playlists (stored rules JSON, live contents), `SmartCrateEngine`
  (smart playlists in crate folders — same query engine),
  `AutoTagEngine`, `EnergyAnalysisEngine`, `TrackDiscoveryEngine`,
  `LibraryPresets`, `LibraryStatistics`, `LibraryBatchEngine`,
  `LibraryResourceManager`, diagnostics.
- Smart query surface: `not_contains`/`ends_with` operators,
  `energy_score`/date/play/session/unplayed/autotag fields, nested
  AND/OR/NOT groups; `SmartQueryValidator` rejects bad fields,
  incompatible operators, impossible ranges and empty queries before
  save; live preview without library writes.
- Schema v9: `analysis.energy` (0..1 scalar) + `energy_curve`/
  `energy_transitions`/`structure_json` JSON + `auto_tags` table
  with confidence + suggested/accepted/rejected/ignored status —
  auto tags and analysis values stay separate from user metadata
  (user `energy` 0-10, `track_tags`, cues, loops, ratings untouched).
- Energy: scalar from loudness/RMS, ≤96-point curve from the waveform
  overview pyramid, transitions + conservative structure segments,
  configurable VERY_LOW…VERY_HIGH levels — a descriptor, not a
  quality verdict.
- Discovery/related tracks return per-result `reasons` from real
  row data (`bpm: +2.0`, `key: adjacent Camelot`) — explainable
  criteria, no opaque score; tempo/key relationships described,
  never applied.
- Presets/history: Recently Added/Played, Most Played, Unplayed
  (`play_count = 0`), Not Played Recently, session join, date buckets
  (Today…This Year/Custom), play-count buckets; `LibraryStatistics`
  totals + key/energy/tag distributions.
- `LibraryBatchEngine`: async+cancelable bulk tag/rating/color/
  analyze with single-transaction one-op undo (nested-`Tx` bug
  found and fixed); `LibraryResourceManager` limits +
  orphan-analysis cleanup that structurally cannot touch user data;
  `SmartAnalysisQueue` named priorities over the existing
  playback-yielding queue.
- `SmartQueryPanel` rule editor (rows + match + live count + save
  as playlist/crate) in the browser rail next to HarmonicPanel;
  energy column in the track table; `energy:`/`added:`/`played:`/
  `unplayed:`/`session:`/`autotag:` search tokens.
- 17 new tests (**466 cases / 1,647,157 assertions green**) —
  queries, builder, playlists, crates, auto tags, energy, discovery
  explanations, presets/history, stats, batch+undo, resources,
  failure modes; 100k library: preview 16,667 hits ~51 ms,
  discovery ~61 ms; app smoke stable.

## Status — Phase 22 (key analysis + harmonic mixing engine)

Professional tonal analysis and musical compatibility
(`docs/analysis/key-analysis-engine.md`,
`docs/adr/0022-key-analysis-harmonic-engine.md`) — informs the DJ,
never decides for them.

- `KeyAnalysisEngine` coordinator + `KeyAnalysisProvider` /
  `KeyDetector` seam (chromagram + Krumhansl–Schmuckler shipping impl,
  FAST/BALANCED/HIGH_QUALITY tiers) — background-only, one chromagram
  pass feeds both the legacy `KeyResult` and the rich
  `KeyAnalysisResult` (alternatives, windowed `TonalSegment`s,
  algorithm/version/timestamp).
- `KeyAnalysisResult`: primary key + margin-based confidence +
  alternatives on ambiguity + `key_ambiguous` flag — uncertain
  results render as uncertain everywhere.
- Five notations over one canonical index (maj 0-11 / min 12-23):
  Standard/Flat/Sharp/Camelot/OpenKey via `KeyNotationConverter` +
  `KeyNormalizer` enharmonics; `nA/nB/nd/nm` parsing.
- `CamelotWheel` canonical math (`(pc*7+8|5) mod 12`) — strict wheel
  neighbourhood (same + relative + ±1 same side) as
  `key_camelot_compatible`; broad `key_compatible` semantics
  preserved.
- `HarmonicCompatibilityEngine` (SameKey/Relative/Adjacent/
  EnergyChange/Distant/Unknown + explanation),
  `TempoCompatibility` (diff/ratio/pitch %), `MusicalCompatibility`,
  `KeyShiftEngine` (semitone math, octave wrap — never touches
  audio).
- Schema v8: `key_algo/version/at/alt_json/segments_json/ambiguous` +
  `key_analysis_history`; `KeyAnalysisCache` invalidates on
  algorithm/version/quality change. User key override stays separate
  from detected values.
- Library: `mode:`/`camelot:`/`keyconf:`/`compatible:` search tokens;
  `key_mode`/`keyconf`/`key_camelot_compatible` filters; Key/Camelot/
  Mode/Confidence browser columns; `RelatedTracksEngine` criteria +
  per-result explanations.
- UI: deck key badge with confidence styling (all 4 decks, shared
  model); `HarmonicPanel` (current vs candidate + relationship +
  tempo) in the browser; `CamelotWheelView`.
- DDJ-FLX4 prep is data-only (snapshot key fields); no invented
  controls.
- 24 new tests (**449 cases / 1,646,801 assertions green**):
  synthetic-key detection, confidence/ambiguity, all-24 Camelot
  round-trips, notation round-trips, compatibility matrix, BPM math,
  key-shift wraps, cache round-trip+validity+history, filter SQL,
  related-track explanations, quality tiers, 100k-track perf
  (related-tracks 32 hits in ~65 ms), 4-deck shared model.

## Status — Phase 21 (professional stems engine)

Track separation into VOCALS/DRUMS/BASS/INSTRUMENTS with real-time
per-stem performance (`docs/stems/stems-engine.md`,
`docs/adr/0021-stems-engine.md`) — stems ride the deck's own
timeline, so pitch/keylock/sync/loops/slip/reverse apply identically.

- `StemsEngine` coordinator: `StemScheduler` (priority jobs,
  cooperative cancel, CPU gate that yields to playback),
  `StemManager` per-deck state, `StemMixer` (gain/mute/solo/pan
  resolution, explicit solo semantics, click-free ramps),
  `StemRouter`/`StemFXRouter` (legal routing plans + FX intent),
  `StemPresetManager`, `StemDiagnostics`.
- Model-swappable seam: `StemSeparationModel` interface +
  `StemModelProvider` (hardware probe → quality/GPU config, CPU
  fallback that never blocks). Reference `BandSplitModel` —
  deterministic DSP whose instrument stem is the exact residual, so
  Σstems ≡ source (RMS diff < 1e-4 verified).
- `StemCache`: float32 WAVs keyed by
  FNV(contentHash|model|version|quality|rate|channels) —
  relocation-safe, atomic tmp→rename commits, validated reads,
  `readRange` streaming seam, storage stats/clear. `stem_sets`
  (schema v7) holds metadata only — no audio in SQLite.
- RT path: POD `DeckStemBuffers` handoff via `pendingStems_` (same
  protocol as TrackAudio); `renderVarispeed`/`renderStretch` mix
  stems at `pos_`. Unconditional original-track fallback; partial
  sets legal; model failure never stops playback.
- Commands `0x0090-0x0093` + `0x00C0+` per deck: stemsMode/analyze/
  cancel/unmute + per-stem mute/solo/gain±/pan±/gate/set. First stem
  action auto-enables stems mode.
- `PadMode::Stems` implemented: pads 1-4 mute (shift=solo), 5-8
  momentary gate; `PadFeedbackEngine` stem states (Set/Off/Playing/
  Dim). `StemsPanel` per deck — 4 strips, M/S, drag-gain, status —
  snapshot-fed, zero engine access.
- Real bugs found by tests: scheduler wait-predicate was popping
  jobs; `StemMixer` masked `stem & 8` (kStemMax=9 ≠ power of two);
  `StemsEngine` member order let workers outlive their state;
  shared-set publishing moved buffers out from under deck 2;
  decode failure never wrote a Failed row.
- 32 new tests (**425 cases / 1,646,300 assertions green**):
  separation/alignment/reconstruction, cache keys+round-trip+range+
  cancel+clear, store CRUD+rollup, mixer/router resolution,
  scheduler order/cancel/CPU-gate, deck RT (fallback, mute→silence,
  solo, gain scale, partial sets, keylock path), engine E2E incl.
  two decks sharing one set, pads+feedback, provider GPU policy,
  4-deck stem stress.

## Status — Phase 20 (professional performance engine)

Complete musical performance layer
(`docs/performance/performance-engine.md`,
`docs/adr/0020-performance-engine.md`) — every action resolves
through a deterministic scheduler into frame-accurate rt commands.

- `PerformanceEngine` facade over named components: `CueEngine`,
  `HotCueManager`/`MemoryCueManager` (typed facades, shared storage),
  `SmartLoopEngine`, `LoopSizeController` (canonical 1/32…32 beat
  tables), `BeatJumpEngine`, `QuantizeEngine`, `SlipEngine`
  (reference-counted sources), `ReverseEngine` (latch XOR momentary),
  `PerformanceScheduler` (7-class priority + per-deck coalescing),
  `PerformanceDiagnostics` (per-deck counters + last-error).
- Hot cues: kind (Instant/Cue), enabled, per-cue quantize opt-out,
  comments; stopped-deck `HotCueMode` (JumpPlay/JumpCue). Memory cues:
  navigation-only, comment, hollow waveform flags + panel strip.
- Loops: manual in/out, sized auto-loops, halve/double/move, saved
  loops — all grid-resolved. Loop roll = gated sized loop holding a
  slip ref; release recovers the shadow timeline (no snap-back).
- Beat jump ±1…64 beats through the grid; quantize per deck with
  selectable resolution; slip re-anchor on permanent moves; momentary
  + latched reverse; session cue point with CUE press/release/hold;
  instant restart that never touches sync/master clock.
- Commands `0x0086+`: `cueSet`, `instantRestart`, loop/jump size
  steppers, `loopSized`, gated `roll`, `revHold`, memory-cue ops,
  global `perf.undo`/`perf.redo` (before/after state capture for
  cue/loop/grid edits via `UndoManager`).
- `PadModeManager` → 4 decks + Roll gate mode; `PadFeedbackEngine`
  per-pad light model (Off/Dim/Set/Selected/Playing) shared by the UI
  pad row and FLX4 LEDs (`flx4LedValue` levels).
- `PerformancePanel` per deck — pads, loop/jump clusters, roll gate,
  bend, memory strip — fed by the snapshot, zero engine access.
- Schema v6 persists cue kind/enabled/quantized/comment keyed by
  `track_id`; verified relink survives file moves.
- 36 new tests (**393 cases / 1,644,293 assertions green**): component
  units, trigger semantics, sized/manual/roll loops, beat jumps,
  quantize + timing at 90/120/128/140 BPM, slip layering + reanchor,
  reverse, persistence/relocation round-trip, failure recovery,
  4-deck stress (160 mixed actions ≈ 10 ms Debug).

## Status — Phase 19 (waveform performance view)

Professional deck visualization layer
(`docs/ui/waveform-performance-view.md`,
`docs/adr/0019-waveform-performance-view.md`) — a pure render-plan
engine drives the JUCE shell; zero audio-thread coupling.

- `WaveformViewEngine` (`src/ui`, JUCE-free, unit-tested): viewport,
  anchored zoom (cursor/playhead/center), auto-scroll
  Off/Center/Smart, playhead smoother with jump detection, and the
  full frame plan — normalized columns, beat/bar/downbeat markers
  with bar numbers + density gating, current-beat/bar band, cue
  flags (hot/memory types), loop rects, state badges.
- `WaveformView` renders through a raster cache (`juce::Image` keyed
  on viewport+data+style+HiDPI scale) — `WaveformData::query` runs
  only when the key changes; cues/playhead/flashes draw live on top.
- Interactions → `WaveCmd` values routed by the host (§42): seek,
  scrub seam, cue trigger/set/armed-delete, loop edge/body drags,
  grid-edit click/drag → `analysisEdit`, overview pan, wheel zoom.
- Grid-edit mode (`wave.gridEdit` command) on top of the Phase-14
  edit surface; markers emphasize, `GRID EDIT` badge, crosshair.
- `DeckView`: ELAPSED/REMAINING/TOTAL time modes (click or
  `deck.timeMode`), `eff→target` BPM under sync, key-notation pref,
  artwork thumb, PAUSED/READY/SEEK/PFL badges.
- New commands `0x0080+` per deck: `wave.zoomIn/Out/Fit`,
  `wave.autoScroll`, `wave.gridEdit`, `deck.timeMode` — routed via
  `EnginePorts::viewAction`.
- 4-deck real: `LayoutSpec::deckMode` → `waves[4]/decks[4]` (A C |
  mixer | D B), all four views live up front, visibility follows the
  master deck-mode toggle; 2-deck geometry unchanged.
- HiDPI rasters at the display scale factor; `WaveformDiagnostics`
  per view (build ms, cache hits/misses, level, counters).
- 19 new tests (**357 cases / 1.64M assertions green**): pyramid
  selection, zoom anchors, auto-scroll, smoother+jumps, frame
  contents, interaction state machine, cache keys, 4-deck layout,
  diagnostics — plus 5 min/30 min/2 h frame-build benchmarks and a
  4-deck stress loop.

## Status — Phase 18 (professional music library)

Completed the Phase-7 library into the full professional surface
(`docs/library/music-library-engine.md`,
`docs/adr/0018-music-library.md`) — no audio-path coupling.

- `SearchQuery` field tokens wired into the browser search box:
  `artist:x bpm:120-128 rating:4+ key:Am|8A|1d tag:t year:a-b
  missing:1` — unknown text still goes to FTS.
- Accent/case-insensitive FTS: `TextFold` applied at index and query
  time; v5 migration rebuilds `tracks_fts` (label/composer columns),
  one-time bulk reindex via `kv.fts_folded`.
- `SmartPlaylistEngine`: rules JSON (eq/neq/lt/lte/gt/gte/contains/
  starts_with/between/in/not_in/isnull + NOT per leaf + AND/OR
  groups) evaluated live through `queryWhere` — no materialized rows.
- `CrateManager` (nested crate folders, depth cap), `TagManager`
  (counts/rename-merge/delete with FTS refresh), `RatingManager`
  (0–5 stars, ARGB colors, comments, energy + bulk).
- `KeyNotationConverter`: Standard/Camelot/OpenKey display + `parse`
  for `key:` tokens.
- `edited_fields` mask: user-edited file metadata survives rescans
  (`setMetadataFields` + importer mask honor); `play_state` surfaces
  UNSUPPORTED FORMAT vs METADATA ONLY; Track exposes
  hasBeatgrid/hasWaveform/needsReview status + text helpers.
- `LibraryBackup`: `onda-library` JSON export/import with
  Skip/Merge/Replace policies (hash-then-path matching, 64-bit ids as
  decimal strings) + CSV export; `LibraryDiagnostics` counts/size/
  integrity report.
- FilterEngine: `whereJoin` OR groups, `negate`, StartsWith/NotIn
  ops, label/composer/waveform/beatgrid/needs_review/analysis_state/
  play_state fields.
- 15 new tests (**338 cases / 1.64M assertions green**): tokens,
  accents, key notations, smart codec/eval/OR/NOT, crates/nesting,
  tags merge/delete, ratings persistence, edited-fields, play states,
  status filters, diagnostics, backup merge/replace/skip, CSV, and a
  100k synthetic benchmark (FTS ~24 ms, filter+sort ~30 ms, smart
  eval ~55 ms at 100k rows).

## Status — Phase 17 (professional mixer engine)

Deepened the Phase-5 mixer into the full professional architecture
(`docs/mixer/mixer-engine.md`, `docs/adr/0017-mixer-engine.md`).

- Remappable channel sources: `ChSource{Deck,Sampler,External,Silence}`
  packed atomics — any deck on any channel via `mapDeckToChannel`;
  invalid indices degrade to silence. Routing graph tags sources
  (`'deck'/'smpl'/'ext '/'sil '`) for introspection.
- `CueManager` master cue: post-master (post-limiter) program tapped
  into the cue bus with a smoothed gate — phones carry the program at
  cueMix=0 without touching the mains. Verified read-only.
- Filter resonance (`ChParam::FilterRes`, Q 0.2–8, all filter modes),
  configurable EQ range (`EqProcessor::setRangeDb`), clickless phase
  invert (`ChParam::PhaseInvert`) — all behind existing smoothing.
- `MixerScene` versioned snapshot (every channel + bus + routing +
  deck mode) — capture/apply round-trips commanded values through
  `MixerController`; 8 recall slots on the bridge.
- `MixerPresetManager` factory set: Flat, Scratch, Broadcast, Booth
  Monitor, Dub Sends, Two Deck; user scenes join the same bank.
- 2/4-deck modes: `setDeckMode` remaps sources (strips never removed);
  `MixerView::setChannelCount` follows `MasterSnapshot.deckMode`.
- `MixerDiagnostics` snapshot + `mixer.diagCopy` clipboard report:
  per-channel state, meters, clips, solo/cue counts, bus levels,
  limiter GR, per-rack latencies.
- Latency seam: `latencySamples(ch)` / `masterLatencySamples()`
  report rack latency (0 today — PDC contract, measure first).
- Grouped param edits (`setParamEdit`) coalesce a knob/fader burst
  into one `{id,from,to}` delta — the UndoManager seam.
- `0x2F09+` commands: master cue, deck-mode toggle, diag copy,
  scene save/recall, preset prev/next — UI/MIDI/HID/FLX4 share them.
- 17 new tests (**323 cases / 1.64M assertions green**): remap,
  isolation, graph tags, deck modes, master-cue independence/smoothing,
  filter Q, EQ range, phase anti-symmetry, registry bounds, scene
  round-trip, presets, diagnostics, transactions, commands, stress.

## Status — Phase 16 (professional FX engine)

Deepened the Phase-6 FX foundation into a full professional
architecture (`docs/fx/fx-engine.md`, `docs/adr/0016-fx-engine.md`).

- Six new processors: **Chorus** (stereo modulated delay pair),
  **Distortion** (bounded tanh/foldback blend + tone), **BitCrusher**
  (1–16 bit quantize + 60× decimation + dither), **Compressor**
  (stereo-linked soft-knee, attack/release, makeup), **BeatRepeat**
  (latched slice loop w/ backstep + pitch) and **BeatRoll** (moving
  roll that re-locks the freshest slice each cycle + gate).
- `FXClock` canonical division→time converter + triplet divisions
  (`1/16T, 1/8T, 1/4T`) — verified at 120/128/140 BPM; no effect
  contains BPM math.
- Reorderable chains: `FXRack::moveSlot` rewrites a packed order
  table atomically — new order adopted next block with zero
  allocation; chain-position addressing throughout.
- Declarative `FXRouting` (deck inserts, post-fader send/returns,
  master insert, sampler future; cue bus never routed).
- `FXMacro` bank (8 mappings, curves, `kFocusTarget` resolution) +
  `FXAutomation` breakpoint-lane seam — both resolve to plain
  SetParams, indistinguishable from hardware input.
- `FXDiagnostics`: per-slot faults counter (auto-reset on sustained
  bad output), sanitized totals, cpu µs, `FXRackDiag` snapshots +
  `fx.diagCopy` clipboard report.
- `0xA000` command surface via `FXSurface` facade: focus target/slot,
  enable/type/mix/P1–P8/division/sync, preset navigation + apply,
  macros, slot reorder — same path UI/MIDI/HID/FLX4 share.
- 20 factory presets incl. the spec collection (Echo Out, Build Up,
  Space Delay…); `applyFxPreset` normalizes native params through
  the processor's own paramInfo.
- Real fix: `ParameterRegistry` was clamping FX types/divisions to
  the Phase-6 maxima — bounds now derive from the enums.
- 29 new tests (**306 cases / 1.63M assertions green**): new
  processors (silence/extremes/reset/stereo-link), crusher grid,
  compressor attenuation, repeat/roll period vs division, triplets,
  macros, routing, moveSlot, diagnostics+faults, presets, command
  surface, chain tests and a 4-deck × 4-slot mixed-FX stress run.

## Status — Phase 15 (professional sync engine)

Deepened the Phase-4 sync foundation into a full professional
architecture (`docs/sync/sync-engine.md`,
`docs/adr/0015-sync-engine.md`).

- New `core/sync` policy module: `TempoMaster` (Auto/pinned/External/
  None sources + documented AUTO rules — never surprise-switches a
  valid master), `MasterPausePolicy` (ClockHolds default / ClockStops /
  FollowOtherDeck), `TempoFollower`, `BeatPhaseTracker`,
  `SyncCorrection`, `MasterClock` + `ClockSource` seam,
  `DeckSyncState`, `SyncDiagnostics`.
- Sync lock (§24): default ON — track swaps re-anchor to the session
  clock; OFF drops to manual on a different track's grid.
- `onUserPitch` single entry for UI+hardware faders — policies
  Override / Disable / ManualAsBend (§26); physical pitch and
  effective tempo stay separate.
- Quantize extended: 1/32 + Bar resolved against the grid's
  beatsPerBar; `cycleQuantize` hardware stepping.
- `syncConfidence` = grid confidence × phase stability →
  `SYNC UNCERTAIN` badge; `MASTER` badge + beatsPerBar beat indicator
  on DeckView; copyable `SyncDiagnostics` behind `SyncDiagCopy`.
- `0x9100` command range: `sync.masterCycle`, per-deck `syncLock`,
  `syncQuantRes`, `syncBpmMode`, `syncDiagCopy` — same CommandManager
  path MIDI/HID use.
- Beatgrid/tempo-map following verified: a 128→130 BPM master
  re-targets followers through grid segments automatically.
- 28 new tests (**277 cases / 1.6M assertions green**): AUTO rules,
  pause policies, pinned/None/External sources, sync lock, pitch
  policies, confidence, diagnostics, bar/1-32 quantize, tempo-map
  follower, command surface, and a 4-deck sync stress run
  (~250× realtime headroom in Debug).

## Status — Phase 14 (professional music analysis engine)

Deepened the Phase-3 analysis foundation into a professional engine
(`docs/analysis/music-analysis-engine.md`,
`docs/adr/0014-music-analysis-engine.md`).

- New components: `LoudnessAnalyzer` (peak/RMS, BS.1770-style gated
  LUFS + LRA — informational only), `TransientAnalyzer` (≤4000
  strongest-first markers for slicer/sampler/beat-repeat),
  `BeatGridValidator` (tempo plausibility, onset↔grid agreement,
  confidence/coverage → `needsReview` + reasons), `AnalysisOverride`
  (user edits strictly separate from detection), `SyncReference`
  (sync-ready view with a `reliable` gate).
- Per-component status ledger — a key failure no longer sinks a good
  waveform/grid; persisted as `analysis.comp_status` (schema v4).
- Waveform pyramid → three levels: overview 4096 bins, mid 1024-frame
  bins, detail 256-frame bins; `query()` picks the cheapest fit.
- User grid overrides move to `user_overrides.grid_json` — re-analysis
  refreshes detection freely, edits survive (verified round-trip).
- Production fix: analysis completion now actually attaches the
  effective grid (`rescaled(deviceRate)`) to every deck holding the
  track, and loading an already-analyzed track attaches instantly —
  sync/quantize get real data outside tests.
- CPU gate: `AnalysisQueue::setMinActivePriority` yields background
  analysis while any deck plays or recording runs; `analysis.workers`
  config controls worker count.
- `0x9000` command range (per deck): set-first-beat at playhead,
  ±10 ms nudge, BPM ×2/÷2, set-downbeat, grid reset, re-analyze —
  all republish the effective grid to the deck.
- `ANALYSIS PENDING` badge on unanalyzed deck tracks.
- 15 new tests (**250 cases / 1.6M assertions green**): loudness
  honesty, transient caps, validator verdicts, mid level, store-v4
  round-trip, override persistence, service edits, queue gate,
  rescaled grids, sync refs, 100-track throughput benchmark
  (~187 ms/track, 5.3 tracks/s Debug).

## Status — Phase 13 (audio device engine + low-latency layer)

Professional device layer over JUCE's `AudioDeviceManager`
(`docs/audio/audio-device-engine.md`,
`docs/adr/0013-audio-device-engine.md`).

- `src/core/adevice/` (JUCE-free, fake-backend tested):
  `AudioDeviceEngine` orchestrator, `Backend` seam, `DeviceCatalog`
  (stable `backend|name` ids, defaults, FLX4 detect), `AudioRouting`
  (`ChannelMapping` validate/persist/auto-derive), `BufferManager` +
  `SampleRateManager` (preset→nearest-supported; engine follows the
  device rate — no hidden resampler), `LatencyManager` (estimated,
  never faked), `AudioHealthMonitor` (underruns/xruns/budget/overload
  with dedup'd warnings), `AudioRecoveryManager` (explicit FSM),
  `AudioTestTool` (silence/sine/L-R/in-phase/ping/cue routing tests),
  `DeviceProfile` JSON per device id.
- `src/platform/audio/JuceAudioBackend.h`: enumerate/status/probe/
  apply/reopenDefault over `juce::AudioDeviceManager` with rollback;
  musical code never sees WASAPI/CoreAudio/ASIO.
- `MixerEngine`: atomic output map — unmapped outs always silent,
  master/cue can't duplicate. `AudioEngine`: `audioDeviceError`
  counter, over-budget `underrunCount`, test-signal injection,
  sampler reprepare on device restart.
- Safe switching (`select()` = validate→probe→apply→verify→rollback)
  and hot-plug recovery (2 Hz enumerate, 3 s retry, default fallback)
  — all control-thread; the RT callback gained only atomic loads.
- Settings → Audio: latency preset, routing-test signal, copyable
  diagnostics (device/backend/rate/buffer/channels/est-latency/CPU/
  underruns/xruns/status/recovery) + live info; snapshot exposes
  backend/latency/recovery to the UI.
- 20 new `[adev]` tests (**235 cases / 1.6M assertions green**):
  catalog, caps, rate/buffer policy, routing, safe-switch rollback,
  hot-plug, recovery FSM, health, profiles, diagnostics, test-signal
  matrix, mixer map honoring.

## Status — Phase 12 (recording engine + session management)

Professional session recording: master bus → realtime SPSC ring →
background WAV writer, with crash recovery, setlists and a
command-driven REC strip (`docs/recording.md`,
`docs/adr/0012-recording-engine.md`).

- `src/core/rec/`: `RecorderRt` (audio-thread tap: bounded copy +
  atomics only — no locks, no fs, no alloc), `SpscRing` (~8 s buffer),
  `RecordingWriter` thread (`WavEncoder` PCM16/24/float32, chunked,
  3.8 GB auto-split), `Journal` (crash-safe event log),
  `RecordingRepository` (`recording_sessions/files/events/
  session_tracks/recovery` on `onda.db`), `RecordingRecovery`
  (journal scan → in-place header repair), `RecordingExport`
  (JSON/CSV/TXT), `RecordingManager`, `RecordingEngine` façade with
  an enforced state machine (Idle→…→Completed/Failed/Recoverable).
- Tap point: post-master bus in `AudioEngine` after
  `MixerEngine.render` — recording is transparent to decks/FX/
  sampler. Pause excludes frames; split loses none.
- Setlist: deck-play edges → `session_tracks` rows + shared
  `HistoryManager.recordPlay`; `<file>.meta.json` sidecar written at
  finalize, audio file never modified.
- Commands `rec.arm|start|stop|pause|resume|split|toggle`
  (0x7000 space) — Ctrl+R quick record, Ctrl+Shift+R split;
  `RecordingView` in the TopBar (REC dot, elapsed, meter, CLIP/DROP,
  disk space).
- 19 new `[rec]` tests (**215 cases / 1.59M assertions green**):
  state machine, ring, encoder roundtrip, lifecycle, split, crash →
  scan → recover → validate → discard, HistoryManager link, known-
  signal equivalence (<1.5e-6 @24-bit), and a 4-hour-equivalent
  48 kHz stereo 24-bit stream: 691M frames ≈ 4.15 GB, auto-split,
  zero dropped frames.
- Also fixed a latent FLX4 test UAF (stale `DeviceState*` after
  reconnect) exposed by heap layout changes.

## Status — Phase 11 (professional sampler + performance pads)

Performance sampler on a JUCE-free RT core, plus a domain pad layer
shared by every control surface (`docs/sampler.md`,
`docs/adr/0011-sampler-performance-pads.md`).

- `src/core/sampler/`: `SamplerRt` (32 slots via atomic mailbox,
  32-voice pool with oldest/quietest/same-sample/deny steal,
  envelope-faded, quantize delay vs master clock, renders into mixer
  channel 4 — full strip EQ/filter/fader/cue/FX-send for free),
  `SamplerEngine` façade (banks, async loader worker, LRU+pinned
  cache, sqlite `SampleRepository`, `SampleRecorder` ring→WAV
  foundation — no source armed by default).
- `src/ui/pads/`: `PadModeManager` (one resolution table: HotCue /
  Sampler / BeatJump / Loop now, PadFx + Stems/Roll/Slicer reserved)
  + `PadLayer` (`pad.<d>.<n>[.shift]`, `pad.<d>.mode` commands —
  controllers emit commands, the domain resolves them).
- New `rt::Command`s: `SampleTrigger/Release/Stop/SlotParam/
  SamplerParam`; new command space `0x5000` (pads) / `0x6000`
  (sampler) wired in `EngineCommands`.
- DDJ-FLX4 pads now route through the domain layer — hardware stays
  semantics-free; the same commands drive UI and keyboard.
- 21 new `[sampler]` tests + reworked FLX4 pad test (**198 cases /
  1.59M assertions green**): playback modes, voice pool+steal,
  quantize-to-grid, sync speed, mute/solo/pan/velocity, cache LRU,
  sqlite roundtrip, real-WAV decode, pad dispatch, recorder,
  1600-trigger stress — all finite, bounded.
- HARDWARE TEST PENDING — physical pad behavior validated on
  `VirtualFlx4` only.

## Status — Phase 10 (Pioneer DDJ-FLX4 integration)

First-class DDJ-FLX4 support built only on **documented public MIDI
data** (official AlphaTheta MIDI list + Mixxx mapping + community
measurements, cross-checked — `docs/controllers/ddj-flx4.md` lists
every source per control). No proprietary protocol, no invented
messages. HARDWARE TEST PENDING — validated via `VirtualFlx4`.

- `src/controller/flx4/`: `Flx4Ids` (channel/CC/note constants),
  `Flx4Inventory` (evidence-backed control table + `controlNameFor`
  diagnostic decoder), `Flx4Profile` (generated declarative profile,
  installed as `ddj-flx4.json` on first connect), `Flx4Plugin`
  (preDispatch semantic layer: pad-mode banks, shift state, jog
  touch/vinyl routing, browse direction, tempo-range cycle, mode LED
  repaint), `PadModes`, `JogWheelProcessor`, `ScratchEngine`,
  `Flx4AudioProfile` (master ch1-2 / phones ch3-4, 44.1/48 kHz, mic =
  hw-only), `ConnectionFlow` (staged bring-up diagnostics),
  `VirtualFlx4` test device speaking real wire protocol.
- `ControllerEngine` gains two generic seams: `preDispatch`
  (per-device semantic consume, after LearnMode/before mappings) and
  `onDevice` (lifecycle observer) — reusable by any modeful hardware.
- `Mapping::hiRes` merges 14-bit MSB+LSB CC pairs (tempo, trim, EQ,
  faders, crossfader, head-mix, FX depth) — full precision, one
  dispatch per pair.
- `DECK_<n>_PITCH` / `DECK_<n>_PITCH_RANGE` params (0x8000 space)
  route through `rt::SetPitch`/`SetPitchRange`; mixer params still go
  through `MixerController`. Both are the same `setParam` port.
- 14 new `[flx4]` tests (177 total): profile validation+roundtrip,
  auto-match, transport commands, 14-bit merge, pad modes+LEDs,
  shift/range-cycle/browse, jog modes, LED feedback, hot-plug,
  audio/connection-flow, garbage-byte safety, decoupled scratch model.

## Status — Phase 9 (controller engine: MIDI/HID + mapping)

Universal, vendor-neutral controller layer. `src/controller/` is a
**JUCE-free core**; `src/platform/midi/JuceMidi.*` adapts OS devices.
Hardware input travels `MidiManager → ControllerEngine → MappingEngine
→ CommandManager / MixerController / rt::CommandQueue` — the same
ports the UI uses, never a direct path into the engines.

- `MappingEngine`: Note/CC/PC/PitchBend → command|param|jog; modes
  Toggle/Momentary/Absolute/Relative/IncDec/Pickup; curves
  linear/inverse/square/sqrt; 4 encoder decodes (absolute,
  64-offset, 7-bit two's-complement, sign-magnitude); jog modes
  scratch/pitch-bend/seek + touch events.
- `FeedbackEngine`: state→LED/motor watches polled at ~20 Hz, per-
  output throttle, 150 ms echo window — motorized-fader echoes are
  dropped and counted, never re-dispatched.
- `ControllerProfile` (JSON v1, pure data) + `ProfileManager`:
  install/load/import/export/delete, per-device assign, name-contains
  auto-match; command *names* persist, ids resolve at load.
- `DeviceManager` + JUCE device-list poll: connect/disconnect/
  reconnect, feedback restore on reconnect — no crash on unplug.
- `LearnMode` (arm → next control captured → mapping), `MidiClock`
  (in tracker + out generator, decoupled from MasterClock),
  `ControllerDiagnostics` (rolling MIDI monitor + latency counters),
  `HidManager` seam for a future documented/vendor-allowed driver.
- `VirtualMidiController` test device simulates every control type
  and captures all outbound traffic.

Tests: 15 `[ctrl]` cases (163 total, 1.59M assertions) — dispatch,
scaling, pickup, encoders, feedback/echo, diagnostics, learn, profile
roundtrip+validation (injected `exec` fields ignored), auto-match,
hot-plug/reconnect, clock, param-name roundtrip; stress: 30 k msgs
in ~29 ms (~1.05 M msg/s), 0 dropped, sub-µs processing.

## Status — Phase 8 (UI framework + live-performance views)

Two-layer UI: `src/ui/` is a **JUCE-free core** (commands, shortcuts,
snapshots, theme, waveform math, virtual table model, layout math —
all unit-tested without a window) and `src/app/ui/` holds the JUCE
views that render only from plain-value snapshots.

- `CommandManager` + `CommandIds`: one stable command surface for
  every input — UI, keyboard, and later MIDI/HID invoke identical
  handlers. `EngineCommands` registers ~120 commands once against
  ports (rt `CommandQueue`, `MixerController`, `PerformanceEngine`,
  `MusicLibraryEngine`, deck stats).
- `ShortcutManager`: named-key grammar (`Ctrl+Shift+A`, `F1`, `Space`),
  default DJ profile, remappable + serializable.
- `EngineBridge` is the only engine gateway: builds `UiSnapshot` from
  atomics, feeds edge-triggered `NotificationCenter` toasts (xruns,
  clip deltas, device loss, analysis events).
- `AppShell`: TopBar + deck/waveform row + mixer + fx strip + browser;
  `LayoutSpec` computes regions with collapse priorities (browser →
  fx → wave strip shrink; transport never hides), 1280×720 … 4K.
- `WaveformView`: overview + detail from cached `AnalysisBundle`
  bins (never recomputes audio), grid markers (beat/bar/downbeat),
  cue/loop overlays, zoom-at-anchor, scroll, seek.
- `DeckView` (info/transport/perf toggles/pitch/loop/8 cue pads),
  `MixerView` (gain/EQ/filter/fader/PFL/meters/xf + master),
  `FXView` (generic racks), `BrowserView` (search + rail +
  virtualized table), `SettingsView` (12 sections incl. controller
  placeholders).
- `ThemeEngine`/`DesignTokens`: pro dark theme, accent, density,
  per-deck waveform colors; `StateButton` encodes state by
  shape + label + fill, not hue alone.
- UI-FPS + frame time measured live in the TopBar.

Tests: +14 `[ui]`/`[stress]` cases (148 total, 1.59M assertions) —
command dispatch, shortcut grammar, theme roundtrip, paged model
over 50k, waveform math, notifications, layout priorities, and a
stress run: 600 frames over 50k rows avg 1.0 ms/frame
(worst 4.8 ms) — no DSP in UI code, engine writes only via
lock-free command push / non-RT facades.

## Status — Phase 7 (professional music library)

`core/library/` — `MusicLibraryEngine` facade owning the SQLite
database (schema v3, WAL, recursive mutex, RAII nested transactions)
and a dedicated library worker: the audio thread never touches it,
heavy ops are async, queries are indexed + paged so the UI never
loads the whole library.

- `TrackRepository`: full DJ track model (identity, embedded meta,
  analysis refs, user meta), CRUD, FTS5 sync, `QuerySpec` =
  search + filters + sort + page + total.
- `FileFingerprint`: `fingerprint` (size+mtime+sampled FNV-1a →
  change detection) vs `contentHash` (mtime-free → move/duplicate
  identity) — the scanner separates "modified" from "moved".
- `TrackImporter`: files/folders/recursive; fingerprint → dedup →
  FFmpeg metadata → transactional upsert → analysis queue. Batch
  import keeps one transaction per track.
- `LibraryScanner`: new / modified / missing / relocated detection;
  vanished rows become `MISSING` (never deleted), content-hash
  match at a new path relinks the same track id.
- `SearchEngine` (FTS5, quoted prefix tokens) + `FilterEngine`
  (parameterized WHERE: bpm/key/genre/rating/dates/tags/missing/
  analyzed, Camelot-compatible key matching) + indexed sorting.
- `PlaylistManager`: static playlists with explicit order, folders/
  crates (nested), smart playlists re-evaluated from rule JSON.
- `HistoryManager`: sessions, entries, configurable play-count rule
  (≥60 s or ≥50 %, loading never counts), async writes.
- `DuplicateDetector` (hash + size/duration/meta candidates — never
  deletes), `MissingFileManager` (relink/locate),
  `FileWatcher` (polling foundation), artwork provider + disk cache.
- `platform/metadata/FFmpegMetadataReader`: read-only tags/stream
  info; embedded vs user metadata stay separate. Formats:
  WAV/AIFF/FLAC/MP3/M4A (FFmpeg decode — see ADR-0007 licensing).
- Integrity: FK cascade/SET NULL everywhere, `integrityCheck()`,
  WAL crash recovery — no orphans, no partial imports.

Tests: +18 `[lib]` cases (134 total, 1.59M assertions) — import,
fingerprint, search/filter/sort, playlists/smart, history, tags,
missing/relink/move, duplicates, transactions, worker concurrency,
and a 50k-track perf suite (FTS 15 ms, filter+sort+page 30 ms,
DB 17.5 MB).

## Status — Phase 6 (professional FX engine)

`core/fx/` — modular effects engine on top of the mixer: insert racks
per deck/channel, two send/return buses, a master insert rack and
beat-synced time effects. The engine only drives the `FXProcessor`
contract — every effect is swappable off-RT and every parameter is
smoothed per-sample.

- `FXProcessor` + `FXBase<N>`: prepare/process/reset, native +
  normalized params, sync context, latency, save/restore state.
- Effects: `DelayFX` (free/synced time, feedback, damping filter,
  ping-pong), `EchoFX` (DJ echo: divisions, tone, ping-pong, bounded
  freeze), `ReverbFX` (Schroeder: 8 combs + 4 allpasses per channel,
  size/decay/damping/pre-delay/width), `FilterFX` (TPT SVF LP/BP/HP,
  resonance, LFO or beat-synced sweep), `FlangerFX`, `PhaserFX`
  (stereo-stage allpass chain), `GateFX` (synced trapezoid gate —
  stutter/beat-repeat foundation).
- `FXSlot`: type, enabled (smoothed bypass crossfade), dry/wet mix,
  in/out gain, sync mode + division, pending-effect adoption — the
  RT path never allocates (effect swap allocates on a non-RT loader).
- `FXRack`: bounded slot chains per target; `FXEngine`: racks for
  DeckA–D, SendA/B, Master + spare, send accumulation, return levels,
  per-target tempo, event FIFO.
- Beat sync: `SyncContext` (bpm, division 1/32…4 bars) fed from each
  deck's `effBpm()` / the sync master — division-locked timing,
  UI-independent.
- Routing: deck insert (pre-strip) → post-fader send taps → send racks
  → return → master insert rack → master bus; cue stays PFL post-insert.
- Safety: input sanitize before dry copy (NaN/Inf→0, ±8 clamp), wet
  sanitize + runaway reset after process, bounded feedback, zero
  allocations inside render.
- `ParameterRegistry`: FX space at `0x1000|target<<8|slot<<4|kind` —
  same control path as the mixer for UI/MIDI/HID/automation.
- `FXPresetManager`: factory + user presets, native-param blobs,
  whole-engine `serializeAll`/`restoreAll` (kind+params, no pointers).

Tests: +25 `[fx]` cases (116 total, 1.59M assertions) — impulse/sine/
noise/silence through every effect, decay & pre-delay timing, ping-pong,
synced vs free divisions, bypass continuity, NaN/Inf sanitize + clamp,
presets/state round-trip, engine routing, registry coverage, mixer
insert/send/master integration, 500-block stress.

## Status — Phase 5 (professional mixer engine)

`core/mixer/` — modular mixer: sources render into channel strips,
strips feed master + headphone buses. Decks are now pure sources
(trackGain + transport mute); the channel path (trim → EQ → filter →
fader/mute/solo → xf assign) lives in `ChannelStrip`, so sampler /
external / FX-return / mic sources can feed the same strip later.

- `ChannelStrip`: smoothed trim, `EqProcessor` (3-band standard mode,
  4-band capable, per-band gain/freq/Q/bypass, ≤3 dB/block steps),
  `FilterProcessor` (position knob LP↔HP or typed cutoff), fader
  (Linear / AudioTaper / Custom curves), mute, solo, instant cut, PFL
  cue, crossfader assignment A/B/Thru, pre-fader meters.
- `CrossfaderEngine`: smoothed position, Linear / EqualPower / Cut /
  Custom curves.
- `MasterBus`: smoothed gain/mute → `LimiterProcessor` → meters
  (peak/RMS/hold/clip + GR) → out 0-1.
- `HeadphoneBus`: cue sum + master blend (cueVol × mix) → out 2-3;
  never feeds back into master.
- `MeteringEngine`/`ChannelMeters`: atomic peak/RMS/hold/clip per
  channel and bus — single-writer RT, read by UI anywhere.
- `RoutingEngine`: node/edge graph of the topology (introspection +
  extension point for FX sends/returns, recording, extra sources).
- `ParameterRegistry`: static metadata for every parameter
  (id/default/min/max/step/smoothMs); channel ids = `(ch<<5)|kind`,
  bus ids at 0x400+ — the single address space UI/MIDI/HID/automation
  and tests share.
- `MixerController` + `rt::SetParam` command: validated parameter
  writes to the audio thread; all legacy commands map to the same
  strip/bus setters, so nothing upstream changed.
- App unchanged on the outside; `engine.mixerCtl()` is the new
  validated entry point for future controller mappings.

Tests: +21 `[mixer]` cases (91 total, 1.59M assertions) — synthetic
sine/noise/impulse/silence through strips and buses, EQ/filter/fader/
crossfader/cue/solo/mute/limiter behavior, param routing, registry,
4-channel render, 500-block RT integrity run.

## Status — Phase 4 (performance engine)

`core/performance/` — non-RT layer translating musical actions to RT
commands; `SyncRtCore` + grid/slip/sync fields inside `DeckEngine` do
the continuous work in the callback.

- **Sync**: master/follower, Off/Bpm/Beat/Phrase modes. Follower rate =
  masterBpm/ownBpm slewed per block (user pitch ignored while synced);
  beat phase locked by predicted-phase comparison with deadband +
  bounded chase + one faded snap when far off. Deterministic statuses
  (NoGrid/OutOfRange/…) — failures are reported, not hidden.
- **Quantize**: target snapping on the BeatGrid (1..1/16 beat), not
  deferred triggering. Applies to cues, loops, jumps, slip actions.
- **Cues**: hot/memory/loop types, 8+ slots, frame+beat stored (survive
  grid re-edits), persisted in `cues` (migration v2) via
  `PerformanceStore`; `saved_loops` + `kv` too.
- **Loops**: beat loops of 1/32..32 beats, in/out/toggle/reloop,
  halve/double/move — all grid-aligned through the tempo map.
- **Beat jump**: ±beats preserving phase (or snapped with quantize).
- **Slip**: `slipShadow_` musical clock inside the deck — roll / hot cue
  hold / scratch restore the "where it would be" position on release.
  `loopRoll` = slip + temporary loop.
- **Events**: `PerfEvent` on actions + `poll()` emitting
  beat/bar/loop/slip transitions — ready for ControllerEngine/MIDI.
- App: SYNC/QNT buttons per deck; grid attaches when analysis finishes;
  `MasterClock` via `perf.masterClock()`.

Tests: 70 Catch2 cases — sync tempo convergence, phase lock, master
switch, out-of-range, no-grid rejection, quantize cue landing, hot vs
memory cue semantics, cue persistence roundtrip, beat loops,
halve/double/move, loop wrap, beat jump ±N, slip roll/cue recovery,
and a 30-op stress run with sync+loops+cues+jump+slip.

## Status — Phase 3 (music analysis engine)

Background analysis pipeline: decode → onset (spectral flux) → BPM
(autocorrelation + octave rescoring, ranked candidates, tempo map) →
key (chromagram + Krumhansl-Schmuckler, standard + Camelot) → silences
→ waveform (overview 4096 bins + detail 256-frame bins, cached POD
blobs) → beatgrid (piecewise-constant tempo segments, f64, editable).

- `core/analysis/`: OnsetDetector, BpmAnalyzer, BeatGrid + BeatGridEngine
  (setFirstBeat/move/nudge/adjustBpm/tempo segments/reset), KeyAnalyzer,
  WaveformGenerator + WaveformData (region queries), MusicAnalysisEngine
  (stage pipeline with cooperative cancel), AnalysisQueue (low-priority
  workers, priority heap, pause/resume/cancel/retry, events)
- `core/data/`: Database (SQLite wrapper, WAL, versioned migrations),
  AnalysisStore (fingerprint = size+mtime+head/tail hash; kAnalysisVersion;
  detected values in `analysis`, user corrections in `user_overrides` —
  never overwritten)
- `AnalysisService`: analyzeTrack (bg) / analyzeTrackSync / getAnalysis /
  getBeatGrid / getWaveform / getKey / getBpm (override-aware) /
  setUserBpm/setUserKey/saveUserGrid / invalidateStale
- App: `onda.db` opens beside `config.json`; deck LOAD enqueues analysis;
  state label shows BPM/key when done
- Tests: 51 Catch2 cases — BPM 70/128/174 + half/double candidates +
  tempo-change map, grid math/edits/downbeats/variable tempo, key C/Am,
  waveform levels + region query, store round-trip/override/fingerprint
  staleness, queue e2e/failure, cancellation, grid drift

Requires `third_party/sqlite` = SQLite amalgamation (gitignored, public
domain) and `third_party/ffmpeg` as before.

## Status — Phase 2 (audio engine + deck engine)

Full-track-in-RAM playback engine: worker `TrackLoader` (ffmpeg decode,
progress, cancel, errors) → atomic handoff → `DeckEngine` ×4 →
`MixerEngine` (faders, crossfader curves, cue bus, master, limiter,
meters) → out.

- `core/audio/TrackAudio`: whole track decoded to device-rate stereo float in RAM
- `core/engine/TrackLoader`: async decode/metadata/progress/cancel; never in RT
- `core/engine/DeckEngine`: transport, musical clock, varispeed cubic,
  pitch/range/bend, key lock (`ITimeStretcher`), loops, seek, reverse,
  quantize flag, EQ 3-band + DJ filter + gains; states
  Empty/Loading/Ready/Playing/Paused/Seeking/Error
- `core/engine/ITimeStretcher` + `core/dsp/WsolaStretcher`: replaceable
  WSOLA (zero-alloc circular buffers)
- `core/engine/MixerEngine` (superseded by `core/mixer/` in Phase 5):
  channel faders, crossfader (linear / equal-power / cut), PFL cue bus +
  cue vol/mix on outs 2-3, master gain, optional peak limiter,
  peak/RMS meters, clip counter
- `core/dsp/`: Biquad, ThreeBandEq, DjFilter, PeakLimiter,
  CrossfaderCurves, cubic resample
- UI shell: full functional controls per deck (seek, pitch+range, bend,
  KEY, loops IN/OUT//2/x2, REV, PFL, trim, EQ, filter, fader) + mixer
  column (master, cue vol/mix, crossfader + curve) + live master meters
- Tests: 31 Catch2 cases incl. RT zero-allocation probe and a ~110 s
  stress scenario (2 decks, loops, keylock, moving crossfader, cue)

Requires `third_party/ffmpeg` = gyan.dev `ffmpeg-release-full-shared.7z` (dev headers + import libs + DLLs).

## Build (Windows)

Requires VS BuildTools 2026 (MSVC 14.5x) with Windows SDK. CMake/Ninja ship with VS:

```bat
tools\cmake.cmd -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
tools\cmake.cmd --build build
build\src\onda.exe          :: app
build\tests\onda_tests.exe  :: tests
```

## macOS

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

## Layout

```
src/core/       pure C++20 engine primitives (no JUCE)
src/controller/ JUCE-free controller core: MIDI model, mapping,
                profiles, feedback, diagnostics, learn, clock, HID seam
src/platform/   JUCE-backed platform layer: audio HAL, log, config,
                MIDI device adapter (platform/midi/)
src/ui/         JUCE-free UI core: commands, shortcuts, snapshots,
                theme, waveform math, virtual table model, layout
src/app/        JUCE application + UI views (app/ui/)
tests/          Catch2 unit tests
docs/adr/       architecture decision records
```
