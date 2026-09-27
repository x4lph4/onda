# ADR 0022 — Key analysis + harmonic mixing engine

Phase 22. Status: implemented.

## Context

Harmonic mixing needs more than a detected key string: confidence,
alternatives when detection is ambiguous, five notation systems,
Camelot wheel math, descriptive BPM+key compatibility, key shifting,
related-track discovery, and persistence that survives algorithm
changes — all without touching the audio thread and without ever
making mix decisions for the DJ.

## Decisions

1. **One canonical key index, five display systems.** Major 0–11,
   minor 12–23, unknown −1 is stored in the DB, filters, decks and
   snapshots. Standard/Flat/Sharp/Camelot/OpenKey are pure
   representations via `KeyNotationConverter`; `KeyNormalizer` does
   enharmonic spelling at display time only. No duplicated key data
   anywhere (§9/§10).

2. **Detection is a swappable interface.** `KeyDetector` +
   `KeyAnalysisProvider` (quality→config for FAST/BALANCED/
   HIGH_QUALITY, detector factory). The shipping `KsKeyDetector`
   wraps the existing chromagram + Krumhansl–Schmuckler analyzer —
   kept because it is explainable and already validated. Everything
   downstream consumes `KeyAnalysisResult`, never the algorithm
   (§4/§36). `MusicAnalysisEngine` runs one chromagram pass that
   yields both the legacy `KeyResult` and the rich result —
   alternatives and windowed `TonalSegment`s come from the same
   correlation vector, not a second analysis.

3. **Confidence and ambiguity are first-class.** `confidence` is the
   best-vs-runner-up correlation margin. Ambiguous results keep
   `alternatives` and set `key_ambiguous`; low confidence renders as
   uncertain everywhere rather than as a wrong-but-certain key (§7/8).
   `TonalSegment`s with insufficient confidence are UNKNOWN — no
   invented key changes (§31/32).

4. **Camelot math is canonical, not tabular.** Position =
   `(pc*7 + 8|5) mod 12` (major +8 → B side, minor +5 → A side) —
   verified against the full 24-key table in tests. The strict
   "perfect match" neighbourhood (same + relative + number±1 same
   side) is `CamelotWheel::compatibleIndices` and the
   `key_camelot_compatible` filter. The pre-existing broader
   `key_compatible` semantics (semitone, dominant/subdominant
   neighbours) are deliberately preserved — existing playlists/tests
   depend on them.

5. **Compatibility is descriptive, never a verdict.**
   `HarmonicCompatibilityEngine` returns a `HarmonicRelation` kind
   (SameKey/RelativeMajorMinor/AdjacentCamelot/EnergyChange/
   DistantKey/Unknown) plus distances and a human explanation.
   `TempoCompatibility` returns diff/ratio/suggested pitch %.
   `MusicalCompatibilityEngine` combines both. Nothing auto-loads,
   auto-syncs, or ranks tracks (§13–17, §27).

6. **Key shift is math, not audio.** `KeyShiftEngine` computes
   semitone displacement, wraps across octaves preserving mode, and
   provides the original→shifted display model (§18/19). It never
   touches playback state itself.

7. **Cache is versioned; overrides are sacred.** Schema v8 adds
   algo/version/timestamp/alternatives/segments/ambiguous columns
   plus `key_analysis_history`. A cached result under different
   `{algorithm,version,quality}` is stale — invalidated, not reused
   (§21/§40). `user_overrides.key_index` stays independent of
   detected values forever (§38/§39).

8. **Multi-deck = shared model, zero duplication.** Key fields live
   in `DeckSnapshot`; `EngineBridge` fills them identically for all
   four decks. `HarmonicPanel` + `CamelotWheelView` are pure
   snapshot-driven views (§47/§49).

## Alternatives rejected

- **Second chromagram pass for rich results** — wasteful; one pass
  already produces the full correlation vector.
- **Replacing K–S with a heuristic/ML hybrid now** — no evidence to
  justify it; the provider seam makes it a drop-in later.
- **Subjective compatibility score** — the spec forbids ranking;
  descriptive relations + explanations keep the DJ in control.
- **Changing `key_compatible` semantics to strict Camelot** — would
  silently alter existing smart playlists; strict behaviour ships as
  a separate field instead.

## Consequences

- New headers only under `src/core/harmonic/` (10) — no CMake source
  changes for the core; `HarmonicPanel.h` joins `src/app/ui`.
- `analysis` schema v8 migration is additive (ALTER + new table).
- Search: `mode:`, `camelot:`, `keyconf:`, `compatible:` tokens.
- Filters: `key_mode`, `keyconf`, `key_camelot_compatible`.
- Browser gains Key/Camelot/Mode/Confidence columns.
- Deck badge shows Camelot key with confidence/ambiguity styling.
- DDJ-FLX4 gets only data-level prep (snapshot key fields already
  reach feedback); no invented controls.
- 24 test cases / 493 assertions; suite now 449 cases green.
