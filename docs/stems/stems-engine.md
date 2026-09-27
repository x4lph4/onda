# Stems engine — separation, cache, and real-time stem performance

Phase 21 (`docs/adr/0021-stems-engine.md`). The stems system splits a
track into **VOCALS / DRUMS / BASS / INSTRUMENTS** on background
workers, caches the result on disk, and lets each deck render the
stem mix — or the original track — through the deck's existing
timeline (varispeed, WSOLA key-lock, sync, loops, slip, reverse all
apply identically: §36-38 of the spec).

```
Library track ──decode──▶ TrackAudio
        │
        ▼  StemScheduler worker (never UI/RT)          §9
StemSeparationModel (BandSplitModel today) ──▶ StemSet
        │                                        (4×f32 stereo @ device rate)
        ▼                                        §11 exact alignment
StemCache <key>/  wav+meta   ◀── atomic commit ──┘
        │
        ▼  StemsEngine::poll (control thread)
DeckStemBuffers ──pendingStems_──▶ DeckEngine RT adopt   (same
        │                                              handoff as
        ▼                                              TrackAudio)
renderVarispeed / renderStretch ── stemsMixAt(pos_) ──▶ channel
```

## Architecture

`StemsEngine` (non-RT, control thread) coordinates:

| Component | Role |
|---|---|
| `StemManager` | per-deck stem state model (present/duration/status) §3 |
| `StemScheduler` | priority job queue, cooperative cancel, CPU gate §9/§42/§45 |
| `StemModelProvider` | hw probe → `StemModelConfig` (quality/GPU) §6-8 |
| `StemSeparationModel` | abstract interface: load/analyze/separate §5 |
| `BandSplitModel` | reference DSP model — deterministic, exact reconstruction |
| `StemCache` | float32 WAV files, keyed/validated, atomic commits §27-31 |
| `StemStore` | `stem_sets` metadata rows in the library DB (schema v7) §29/§48 |
| `StemMixer` | gain/mute/solo/pan resolution model §12-16 |
| `StemRouter`/`StemFXRouter` | routing plans + per-stem FX intent §17-19 |
| `StemPresetManager` | named stem-state snapshots |
| `StemDiagnostics` | counters, timings, cache I/O §39/§52 |

## Model abstraction (§5)

The rest of the app only sees `StemSeparationModel`:

```cpp
bool      load(const StemModelConfig&, std::string* err);
void      unload();
StemError analyze (const TrackAudio&, StemCancel&, const StemProgress&);
StemError separate(const TrackAudio&, StemSet&, StemCancel&, const StemProgress&);
StemModelCaps capabilities();
StemModelInfo info();
```

`StemModelProvider::configFor(quality, gpuMode)` resolves a runtime
config from probed hardware (`StemHwCaps`: cpuWorkers, gpuAvailable,
vramBytes) and `createModel()` returns the best implementation.
Quality tiers (§8): `Fast` → filter order 1 / `Balanced` → 2 /
`HighQuality` → 4 + all workers. GPU policy (§46): `Off` forces CPU;
`Auto`/`On` resolve to `On` only when an accelerator exists — explicit
`On` on a GPU-less box falls back to CPU and never blocks.

A learned model (ONNX/Demucs-class) plugs into exactly this seam —
`DeckEngine`, `MixerEngine`, UI and pads never see it.

### BandSplitModel (reference)

Deterministic DSP, no ML, no model files to validate:

- BASS = stereo LP @ 150 Hz
- VOCALS = mid band 300 Hz–3.4 kHz (dual-mono, center content)
- DRUMS = mid residual after bass+vocal bands
- INSTRUMENTS = source − (bass + vocals + drums) → **exact residual**

Because instruments is the residual, the four stems sum back to the
source sample-for-sample (§51 tests verify RMS < 1e-4). Labels are
honest approximations of musical roles, not learned separation — the
point is the interface seam, deterministic tests, and zero external
dependencies.

## Pipeline (§4)

```
Track → Validation → Analysis → ModelSelection → Separation
      → Validation → Cache → Ready
```

Every stage runs on a `StemScheduler` worker. `StemJob::kind`: 0 =
separate, 1 = load-from-cache. Job phases (`StemJobPhase`): QUEUED →
ANALYZING → SEPARATING → VALIDATING → CACHING → READY / FAILED /
CANCELLED (§43 — surfaced verbatim to UI via notifications and
`DeckStats::stemsAvail`).

`StemsEngine::onTrackLoading` (called from `EngineBridge`'s load
port): instant store lookup → cached ready set ⇒ `kind=1` load job;
otherwise `autoSeparate` ⇒ separation job; either way playback starts
immediately on the original buffer (§25/§32 — separation is never a
precondition).

## Deck rendering (§25/§26/§36-38)

A finished `StemSet` becomes a POD `DeckStemBuffers` view (pointer
array + frame count + mask) published through `pendingStems_` — the
same lock-free handoff protocol as `TrackAudio`/`BeatGrid`. The RT
thread adopts at block top; retired views go back through
`retiredStems_` for the owner to free on its `poll()`.

Inside `renderVarispeed`/`renderStretch` the read head `pos_` samples
stems instead of the track buffer when `stemsActive()`:

```cpp
if (stemsActive())            // stemMode_==1 && set adopted
    stemsMixAt(pos_, sl, sr); // Σ audible stems, per-stem sm-gain/pan
else
    readCubicStereo(track, pos_, sl, sr);
```

One timeline, one position register — tempo/keylock/sync/loops/slip/
reverse apply identically to every stem (§36/§37/§38). The stretch
path stages a mixed chunk into `stemStage_` (preallocated in
`prepareDsp`) and feeds it to the same WSOLA stretcher.

**Fallback is unconditional (§25):** no set adopted, set retired,
stemMode off, or all stems muted-by-presence → the original buffer
renders. A model/decode failure never touches playback.

**Partial sets (§26):** `mask` marks which stems exist; absent stems
are skipped in the mix and shown Dim in UI/pads.

**Solo semantics (§14 — explicit):** if any stem is soloed, non-solo
stems are fully silenced; mute wins over solo on the same stem.

**Smoothing (§15/§34):** resolved `gL/gR` feed `smL/smR` one-pole
ramps (same k=0.001 as track gain) — mute/solo toggles are click-free
fades, never hard gates. Pan is constant-power on the stereo image
(§16).

## Commands

POD rt commands (`rt::Command` variant):

```cpp
struct SetStemMode  { uint8_t deck; uint8_t mode; };   // 0 orig,1 stems
struct SetStemParam { uint8_t deck; uint8_t stem;
                      float gain, pan; uint8_t mute, solo; };
```

`StemsEngine` setters update the `StemMixer` model AND post the
resolved `SetStemParam` — one call, both worlds consistent. Solo
reposts all 9 stem slots (resolution is global). UI command ids:
`DeckStemModeTgl/Analyze/Cancel/Unmute` (`0x0090-0x0093`) and
`DeckStemBase` (`0x00C0`) + stem×8 + kind (mute/solo/gain±/pan±/
gate/set).

Any stem action auto-enables stems mode when a set is present
(`autoEnableIfStems`) — touching a stem pad activates stems, same as
shipping products.

## Cache & storage (§27-31)

- **Key** = FNV-1a over `contentHash | modelId | modelVersion |
  quality | rate | channels` (hex16). Relocation-safe (content hash,
  not path) and fully versioned — any config change yields a
  different key (§28).
- **Layout** = `<cacheDir>/<key>/{voc.wav,drum.wav,bass.wav,inst.wav,
  meta}` — float32 WAVs via `rec::WavEncoder` + `meta` provenance
  sidecar.
- **Atomic commit** = write into `<key>.tmp/`, then rename. A crash
  mid-write leaves a `.tmp` dir that never validates as a set (§42
  cleanup removes it on cancel).
- **Validation on read** = meta k=v sidecar + per-file
  `WavEncoder::validate` (rate/channels/frames/bits all must match
  the expected `StemSetMeta`, else hard miss).
- **`readRange(key, stem, start, frames)`** = window read without
  materialising the set — the §31 streaming seam; full-set loading is
  per-deck-load only, never library-wide.
- **Storage manager** = `stats()` (bytes/sets/freeBytes/largest-8),
  `clearSet(key)`, `clearAll()` — audio files only; library rows are
  the store's call.

`stem_sets` (schema v7) holds only metadata: track_id, content_hash,
model_id/version, quality, format, mask, state
(Pending/Ready/Failed/Orphaned), cache_key, timestamps — no blobs.
`StemStore::statusForTrack` rolls up Ready>Pending>Failed for the
library column (§47/§48).

## Failure & cancellation (§41/§42)

- Model load/analyze/separate/decode failure → `stem_sets` row marked
  Failed, deck keeps playing the original, `stemsAvail` drops to
  Unavailable.
- `cancelSeparation`/`cancelForDeck` → queued jobs vanish, running
  jobs see `cancel=true` at their next 8192-frame poll, `.tmp` cache
  dirs are cleaned, nothing partial ever publishes.
- `setPlaybackActive(true)` raises the scheduler gate
  (`minActivePriority=1`) — background separation yields to playback
  (§45).

## Pads & UI (§21-23)

`PadMode::Stems` (mapping lives in `PadModeManager` only — §21):

| Pad | Action |
|---|---|
| 1-4 | stem mute toggle (Shift = solo) → VOC/DRM/BAS/INS |
| 5-8 | momentary gate (hold = kill, release = restore) |

`PadFeedbackEngine` reads `stemFlags` (present/muted/solo/loading) →
Set (active), Off (muted), Playing (solo), Dim (absent/loading).

`StemsPanel` (under each deck's performance cluster): 4 stem strips —
label, M/S buttons, drag-gain bar, status glyph — fed purely from
`DeckSnapshot.stemFlags/stemGain/stemsAvail/stemMode`; every click
goes through `CommandManager` to the stem ports.

## Threading contract

- **RT thread:** adopts `DeckStemBuffers` views, mixes at `pos_`,
  reads per-stem resolved params. No allocation (stage buffer is
  preallocated), no locks, no model code — ever.
- **Control thread:** `StemsEngine` setters, `poll()` (publishes
  finished sets, frees retired views), `EngineBridge` snapshot.
- **Worker threads:** decode → model → cache. Notifications fire on
  workers; listeners marshal appropriately.

Lifetime contract: **decks outlive `StemsEngine`** — its destructor
clears `pendingStems_` on bound decks, and `sched_` is declared last
so workers join before any state they touch dies.

## Diagnostics

`StemDiagnostics::line()` — jobs started/queued/done/failed/
cancelled, cache hits/misses/writes, bytes read/written, last
separate/load ms, param/mode changes, rejects, RT blocks. Counters
are atomics; safe to read any thread.

## Verified

- 32 Phase-21 tests, 1415 assertions: separation/alignment/
  reconstruction (RMS<1e-4, peak<2e-3), cache keys+round-trip+
  readRange+cancel+clear, store CRUD+rollup, mixer resolution, router
  legality, scheduler order/cancel/CPU-gate, deck RT (fallback, stem
  mode, mute→silence, solo, gain scale, partial set, keylock path),
  engine E2E (separate→cache→publish, two decks sharing a set, model
  failure → original plays, cancel, rt-command posting), pads +
  feedback, provider quality/GPU, 4-deck stem stress (~64 blocks × 4
  decks ≪ realtime).
- Full suite: **425 tests, 1,646,300 assertions, all green.**

## Limitations / future work

- `BandSplitModel` is a reference DSP split, not learned source
  separation — labels are honest approximations; a real model plugs
  into `StemSeparationModel`/`StemModelProvider` unchanged.
- Per-stem FX slots are resolved as *intent* (`StemFXRouter` →
  `StemRoutingPlan`); the plan is legal and routed, but per-stem DSP
  insert buses are the future consumer (§18 seam). Today stems reach
  the mixer through the deck's ChannelStrip for free.
- Stem waveforms use the deck waveform (stems share the timeline);
  per-stem `StemWaveform` sources are a documented seam (§24), not
  duplicated data.
- `stemStage_`/`StemSet` buffers are RAM-resident; `readRange` is the
  streaming path for oversized sets (§31), not yet wired to the deck.
- Quantize integration for stem actions (§35) is a future hook —
  actions resolve immediately today.
