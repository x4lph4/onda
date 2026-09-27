# ADR 0025 — FX engine completion (tails, stem FX, sampler routing, tempo sources)

## Status

Accepted — Phase 25.

## Context

Phase 16 already shipped most of the FX spec: `FXProcessor`/`FXBase`,
slots, racks, `FXEngine`, send/return buses, presets, macros,
automation, diagnostics, and thirteen effect types. Audit showed five
real gaps: bypass killed effect tails in ~10 ms, per-stem FX existed
only as routing intent (`StemFXRouter`), the sampler channel aliased
the SendA rack, there was no MASTER/DECK/MANUAL tempo-source select,
and parameters written between `loadEffect` and processor adoption
were silently dropped.

## Decisions

1. **Tail drain keeps the wet gate open, not a second bypass mode.**
   On disable, a tail-capable slot keeps `smEn_` at 1, feeds the
   processor zeros, and lets the tail decay until wet peak < −80 dBFS
   or an 8 s cap. A new dry gate (`smEnDry_`) ramps the dry path back
   to **unity** over the same 10 ms ramp — during the drain the output
   is `dry + wet·mix`, i.e. a true "echo out". Rejected: crossfading
   the whole output to dry (the old behaviour) — it truncates the tail
   and attenuates the program by `(1-mix)` instead of being a
   transparent bypass.

2. **Stem FX live in the deck, bound into the FX id space.** Stems are
   mixed inside `DeckEngine`, so per-stem inserts cannot be a rack.
   Each deck owns `FXSlot stemFx_[4]`; stems with an active slot are
   staged into preallocated per-stem block buffers, processed, and
   summed. `FXEngine::bindStemSlots` borrows the pointers so
   `target=deck, slot=4..7` params/presets/diagnostics reach them
   through the exact same path as rack slots — one id space, zero
   duplicated machinery. In the keylock path stems are FX'd in
   source-domain chunks *before* the WSOLA stretcher so the stretched
   output stays time-coherent.

3. **Sampler insert = `FXTarget::Spare`, never a send rack.**
   `channelFxTarget` maps strips 0..3 → deck racks and the sampler
   strip → `Spare` (the existing `SamplerInsert` route). Previously
   `processDeckInsert(4)` resolved to `racks_[SendA]` and processed
   that rack twice per block — once as sampler insert, once as send
   return.

4. **Sync context is stamped, tempo source is a slot param.**
   `SyncMode` widened to 0/1/2 (MANUAL/DECK/MASTER). Each block the
   slot copies either its target `ctx_` or the `masterCtx` (the
   sync-master deck's tempo) into the processor — modes, divisions and
   tempo changes all flow through the existing `FXClock`
   `divisionFrames()` path. Sends and the master rack always run on
   the master ctx.

5. **Pending params are stashed and replayed on adoption.**
   P1..P8 writes while `fx_ == nullptr` (pending swap) are kept in an
   8-entry stash with a bit mask and replayed in `adoptPending`. Fixes
   preset loads and any controller write that lands between a type
   change and the next block boundary.

6. **`hasTail()` and `FXParamType` are interface metadata, not
   flags on specific effects.** `hasTail()` lets the slot decide drain
   vs hibernate generically; `FXParamType` tags in `FXParamInfo` let
   UI/controllers render knobs vs selectors vs beat-divisions without
   per-effect knowledge.

## Consequences

- Bypass semantics changed deliberately: disabling a Delay/Echo/Reverb
  now yields `dry + tail` instead of a fast cut. The Phase-16 test was
  updated to assert the new (spec §42) behaviour.
- `busy()` now covers enabled-or-draining; the mixer, deck staging and
  tests all check `busy()`, not `enabled()`, when deciding whether a
  slot still needs input.
- Diagnostics carry a `stems[4]` row per deck target; `FXRackDiag`
  grew the array.
- `SyncMode` range 0..2 in `ParameterRegistry` — existing saved
  values 0/1 are unchanged.

## Validation

- 14 new `[fx25]` cases: tail drain (echo/reverb, slot + stem level),
  stem isolation (Vocals→Echo, Drums→HP, Bass→Phaser, Inst→Reverb
  simultaneously), sampler→Spare routing (no SendA double-processing),
  MANUAL/DECK/MASTER tempo sources, sync-vs-free, pending-param
  preservation, preset round-trip + duplicate, registry coverage,
  reorder, 4-deck + stems + sends + master stress.
- Full suite: all test cases and assertions pass; smoke test stable.
