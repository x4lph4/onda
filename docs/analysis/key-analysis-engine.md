# Key Analysis + Harmonic Mixing Engine (Phase 22)

Professional tonal analysis, notation conversion, Camelot wheel,
BPM/key compatibility and related-track discovery. The engine
**informs** the DJ — it never makes mix decisions automatically
(no auto-load, no auto-sync on compatibility).

All headers under `src/core/harmonic/` (`namespace onda::harmonic`
abbreviated `H::` below).

## Architecture

```
KeyAnalysisEngine                 (coordinator, background worker)
├── KeyAnalysisProvider           (algorithm seam: detector factory +
│                                  quality→config mapping)
│   └── KeyDetector               (interface; impl: KsKeyDetector =
│                                  chromagram + Krumhansl–Schmuckler)
├── KeyAnalysisCache              (SQLite persistence, version/quality
│                                  invalidation, alternatives+segments,
│                                  analysis history)
├── KeyNormalizer                 (enharmonic spelling per notation)
├── KeyNotationConverter          (5 display systems, parse/format)
├── CamelotConverter/CamelotWheel (canonical 24-position math,
│                                  strict wheel neighbourhood)
├── HarmonicCompatibilityEngine   (key relationship + explanation)
├── TempoCompatibility            (BPM diff / ratio / pitch %)
├── MusicalCompatibilityEngine    (key+tempo combined report)
├── KeyShiftEngine                (semitone math, octave wrap,
│                                  original→shifted display model)
├── KeyDiagnostics                (counters: started/done/failed/
│                                  cache hits, avg ms)
└── RelatedTracksEngine           (criteria-driven suggestions with
                                   per-result explanations)
```

Real-time contract: **nothing here runs on the audio thread or the
UI thread.** Detection runs inside `MusicAnalysisEngine`'s existing
background analysis pipeline (one chromagram pass produces both the
legacy `KeyResult` and the rich `KeyAnalysisResult`), or on
`KeyAnalysisEngine`'s own worker for explicit re-analysis. Decks,
mixer and UI consume plain values only.

## Key model

`MusicalKey` (HarmonicTypes.h) = pitch class 0–11 + `KeyMode`
(Major/Minor, extensible to Dorian/Phrygian/… — modes are modelled,
not detected: the K–S detector only estimates major/minor and never
invents a mode it cannot support) + optional confidence.

**Canonical index** — one integer spans all 24 keys and is what the
database, filters, decks and snapshots carry:

```
major: 0..11  (C,C#,D,D#,E,F,F#,G,G#,A,A#,B)
minor: 12..23 (Cm,C#m,Dm,D#m,Em,Fm,F#m,Gm,G#m,Am,A#m,Bm)
unknown: -1
```

`KeyAnalysisResult` = primary key + confidence + `alternatives`
(secondary candidates with their own correlations, kept when the
detection is ambiguous) + `segments` (`TonalSegment{start,end,key,
confidence}` — windowed tonal estimates; future key-change detection
consumes these, segments with low confidence are marked UNKNOWN,
never fabricated) + algorithm id/version + timestamp.

## Detection pipeline

```
PCM → chromagram (windowed, hop-controlled)
    → Pearson correlation × 24 Krumhansl–Schmuckler profiles
    → rank: primary = argmax, alternatives = runners-up
    → confidence = margin(best, second), normalized 0..1
    → optional windowed segments (same profiles per window)
    → MusicalKey + KeyAnalysisResult → cache/store
```

`KeyAnalysisProvider::configFor(quality)` maps FAST / BALANCED /
HIGH_QUALITY to `{maxChromaFrames, windowSec, segments, …}` so cost
is bounded per tier; `createDetector(cfg)` returns the algorithm.
Everything downstream consumes `KeyAnalysisResult`, never the
detector — a different algorithm (incl. ML) plugs into the provider
without touching library/UI/deck code (§36).

## Confidence and ambiguity

`confidence ∈ [0,1]` is the correlation margin between the best and
second-best key — it measures *decisiveness*, not correctness. When
close candidates exist, `alternatives` is non-empty and the result
is flagged ambiguous (`key_ambiguous` persisted). Low-confidence or
unknown keys render as unknown/uncertain everywhere — deck badge,
browser column, HarmonicPanel — never as a certain key.

## Notation systems

`KeyNotationConverter` displays/parses the canonical index in five
systems — a representation, never duplicated data:

| System   | Example (idx 21) | Notes                              |
|----------|------------------|------------------------------------|
| Standard | `Am`             | sharps                             |
| Flat     | `Am` / `Bb`      | flat spelling (Bb, Eb, Ab, Db, Gb) |
| Sharp    | `Am` / `A#m`     | sharp spelling                     |
| Camelot  | `8A`             | 1A–12A minor, 1B–12B major         |
| OpenKey  | `8m` / `8d`      | `nd` major, `nm` minor             |

`KeyNormalizer` handles enharmonics (`C#↔Db`) purely at display —
the stored index never changes. Parsing accepts standard names
(`Am`, `F#`, `Bbm`) and `nA/nB/nd/nm` forms.

## Camelot wheel

`CamelotWheel` is the canonical 24-position model — position
`1..12` × side `A`/`B`:

```
pc → Camelot number:   major  n = (pc*7 + 8) mod 12   (0→12)
                       minor  n = (pc*7 + 5) mod 12
side: major=B, minor=A
```

`compatibleIndices(keyIndex)` = the strict "perfect match" set —
**same + relative major/minor + number±1 same side** — e.g.
8A(Am) → {8A, 8B(C), 7A(Dm), 9A(Em)}.

`HarmonicCompatibilityEngine::relate(current, candidate)` returns a
`HarmonicRelation{kind, semitoneDistance, camelotSteps, score,
explanation}` with kinds: `SameKey`, `RelativeMajorMinor`,
`AdjacentCamelot`, `EnergyChange` (wheel-adjacent raising/lowering
the number), `DistantKey`, `Unknown`. Relations are *descriptive* —
the engine classifies the musical relationship and explains it; it
never ranks tracks as "better".

The pre-existing broader `key_compatible` filter (semitone and
dominant/subdominant neighbours) is unchanged; strict wheel
behaviour is exposed as `key_camelot_compatible` and through
`RelatedTracksEngine::RelatedCriteria::strictCamelot`.

## BPM + key compatibility

`TempoCompatibility::relate(curBpm, candBpm)` → `{bpmDiff, ratio,
suggestedPitchPct}` — descriptive numbers only (a +2 BPM diff at
124→126 is a +1.6% pitch suggestion; it is **not applied**).

`MusicalCompatibilityEngine::relate()` combines both into one report:
key relationship + BPM diff + required tempo adjustment — e.g.
`128 BPM 8A` vs `126 BPM 9A` → "9A adjacent (+1 step), −2.0 BPM,
+1.59% pitch". No subjective score is attached to tempo.

`KeyShiftEngine` computes semitone displacement between two keys
(`shiftFor(from,to)`), normalizes across the octave (±12 wraps to
the same pitch class, mode preserved), and exposes an original→
shifted display model for decks that have key shifting (§18/19).
With key-lock engaged, tempo changes preserve perceived pitch to
the limits of the time-stretcher — never assumed perfect (§20).

## Caching and persistence

Schema v8 extends `analysis` with `key_algo`, `key_version`,
`key_at`, `key_alt_json`, `key_segments_json`, `key_ambiguous`,
plus a `key_analysis_history` table (algorithm/version/date/result/
confidence — §40).

`KeyAnalysisCache` reads/writes the rich result keyed by track +
`{algorithm, version, quality}`: a result stored under different
parameters is `stale`, not reused. Detected data and user overrides
stay separate — `user_overrides.key_index` is applied on top of the
detected value, never overwritten (§38); same philosophy as the
existing BPM override (§39).

## Library integration

`LibraryTrack` carries `keyIndex`, `keyConfidence`, `keyAmbiguous`.
Search tokens (`SearchQuery`): `key:`, `mode:major|minor`,
`camelot:8A`, `keyconf:>=0.5`, `compatible:8A`. Filter fields:
`key_index`, `key_mode`, `keyconf`, `key_confidence`,
`key_camelot_compatible`, `key_compatible` (broad). Browser has
Key/Camelot/Mode/Confidence columns + sort (§23/24/25).

`RelatedTracksEngine::suggest(refKey, refBpm, criteria, queryFn)`:
configurable criteria (`sameKeyOnly`, `strictCamelot`,
`minKeyConfidence`, `bpmTolerancePct`, `limit`); each result carries
the criteria that produced it (`explanation`). Criteria are shown,
never hidden (§26/27).

## Decks and UI

- `DeckSnapshot` carries `keyIndex`, `keyConfidence`, `keyMode`,
  ambiguity; `DeckView` renders the key badge (Camelot default) with
  confidence/uncertain styling — identical code across decks A–D.
- `HarmonicPanel` (browser right column): current track
  (title/key/Camelot/BPM) vs browser-selection candidate
  (key/Camelot/BPM) + relationship + tempo suggestion. Reference =
  master/first playing deck. Non-intrusive, snapshot-driven.
- `CamelotWheelView` renders the 24-position wheel with current +
  related keys highlighted (§12).
- Colour coding prepared for major/minor/confidence — always paired
  with text, never colour-only (§30).

## Hardware (DDJ-FLX4)

Conservative prep only: key data already reaches controller feedback
via deck snapshots (key text/LED-adjacent fields). No invented key-
shift or library-nav controls — mappings are added only where the
public FLX4 MIDI map documents a physical control (§48).

## Performance (measured, 100k-track DB)

| Query                              | Result        |
|------------------------------------|---------------|
| `key_index = 8A`                   | 4,166 rows    |
| `key_camelot_compatible = 8A`      | 16,666 rows   |
| `key_mode=minor AND bpm 120–128`   | 14,517 rows   |
| `keyconf >= 0.5`                   | 71,428 rows   |
| `RelatedTracks(8A,124,limit 32)`   | 32 in ~65 ms  |

All filters are indexed SQLite WHERE clauses (no row materializing);
100k seed ~3 s.

## Testing (24 cases / 493 assertions)

Synthetic key signals (C, Am, G, Em, F, Dm, …) → detection +
confidence; ambiguous-chord signals → alternatives + uncertainty;
all-24 Camelot round-trips; 5-notation names + enharmonics;
compatibility matrix (same/relative/adjacent/distant/unknown);
BPM relation math; key-shift ±0/1/2/12 wraps; cache round-trip +
validity + history; schema v8 migration; filter/SQL clause content;
related-track criteria + explanations; provider quality tiers;
100k-track performance; 4-deck shared model.

## Known limitations

- Detector is chromagram + Krumhansl–Schmuckler — honest, explainable,
  but not a learned model. Heavily modal/atonal content yields low
  confidence (which is surfaced, not hidden). `KeyAnalysisProvider`
  is the seam for a stronger detector.
- Segments are per-window estimates; true key-change detection is a
  consumer of `TonalSegment`, marked UNKNOWN under low confidence
  rather than guessed (§31/32).
- Only major/minor are detected; other modes exist in the model but
  are never reported by this detector (§3).
- `MusicalCompatibility` is descriptive — it deliberately avoids a
  subjective "mix quality" score (§16).
