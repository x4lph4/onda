# Pioneer DJ DDJ-FLX4 — integration notes

Status: **implemented against documented/verified public MIDI data — HARDWARE TEST PENDING**.
No physical unit was connected during development; all values below come
from the sources listed at the end and are cross-checked between them.
Anything not verified is marked UNKNOWN / assumption and is *not* mapped.

## Sources

| # | Source | What it provides |
|---|--------|------------------|
| S1 | AlphaTheta/Pioneer "MIDI message list DDJ-FLX4" (official PDF, downloads.support.alphatheta.com) | Channel/CC/note numbers per control |
| S2 | Mixxx `Pioneer-DDJ-FLX4` mapping (open source, github.com/mixxxdj/mixxx) | Byte values cross-checked against firmware 1.02, LED echo scheme |
| S3 | slipmate DDJ-FLX4 MIDI reference (community measurements) | 14-bit pair confirmation, pad bank layout, jog channels |

Where sources disagreed, S1/S2 agreement won; S3-only data is marked.

## Verified capabilities

- **Transport**: class-compliant USB MIDI + USB audio. No driver needed
  on Windows/macOS.
- **MIDI channels**: deck A = ch 1, deck B = ch 2 (0-based 0/1), pads on
  ch 8–11 (0-based 7–10, unshifted/shifted per deck), mixer on ch 7
  (0-based 6), FX on ch 5/6 (0-based 4/5).
- **14-bit controls** (MSB on cc<0x20 + LSB on cc+0x20): tempo slider,
  trim, EQ Hi/Mid/Low, channel fader, crossfader, head-mix, color-FX
  depth, FX level/depth — per S1/S3.
- **Jog wheels**: touch note 0x36; platter CC 0x21/0x22 vinyl-on,
  0x22/0x21 (S3), side 0x21 vinyl-off, search jog 0x1F/0x20 — rel-64
  delta encoding.
- **LEDs**: echo the input note back (note-on = lit). Pad-mode
  selectors light via echo of the selector note (S2 confirms).
- **Audio interface**: 4 out (ch 1-2 master RCA, ch 3-4 phones),
  44.1/48 kHz. Mic input is hardware-mixed to master only — it never
  reaches the USB input (documented limitation of the unit).

## Mapping decisions (this software)

- Tempo slider → `DECK_<n>_PITCH` param (0x8000 space) → `rt::SetPitch`.
- shift+SYNC (note 0x60) → cycles `DECK_<n>_PITCH_RANGE` through
  {±6, ±10, ±16, ±25} → `rt::SetPitchRange`. *Layout assumption —
  range cycling is our semantic, not a documented hw function.*
- Pads: mode = last-pressed selector (HOT CUE / BEAT JUMP / SAMPLER /
  BEAT LOOP primary; PAD FX / KEYBOARD / KEY SHIFT / PAD FX2 shifted —
  most shifted banks are reserved/unmapped pending the Phase-11
  sampler, consumed silently).
- Hot-cue pads: press = trigger, shift+press = delete (S2 convention).
- Beat-jump pads 1–4 = back, 5–8 = forward (layout assumption, S3
  layout diagram).
- Beat-loop pads → `deckX.loopToggle` (sized per-pad loops are an
  approximation; documented).
- Browse rotary (CC 0x40 ch 6, rel-64) → `browser.nextTrack` /
  `browser.prevTrack` per tick sign.
- Jog: vinyl-on + touch → `JogScratch`; vinyl-on no touch → `JogSeek`;
  wheel side → `JogPitchBend`; search jog → `JogSeek` ×3 gain.
- All transport/EQ/fader/FX buttons+knobs: static declarative mappings
  in `Flx4Profile` (generated, installed as `ddj-flx4.json`).

## Unsupported / UNKNOWN

| Function | Status |
|----------|--------|
| Smart Fader / Smart CFX auto features | hw-internal, not mapped |
| Pad banks: SAMPLER, KEYBOARD, KEY SHIFT, PAD FX1/2 | reserved — Phase 11 sampler |
| Deck 3/4 | FLX4 is a 2-deck controller — N/A |
| Firmware version readout | UNKNOWN — no documented query |
| MIDI clock | not emitted by hw (not needed) |
| HID | not needed — full surface is class-MIDI |

## Real-hardware checklist (run when a unit is attached)

    [ ] device detected as "DDJ-FLX4"    [ ] audio endpoints match
    [ ] play/cue/sync/PFL both decks     [ ] jog touch → scratch
    [ ] tempo slider 14-bit smooth       [ ] EQ/trim/fader/xf hires
    [ ] pad modes + LEDs repaint         [ ] shift layers
    [ ] browse rotary                    [ ] FX on/depth
    [ ] phones cue routing ch 3-4        [ ] disconnect → reconnect

## Safety

Every inbound message is validated by MappingEngine (range-clamped via
outMin/outMax, finite-checked by parameter layer). Deck params ride
`rt::CommandQueue` — nothing blocks the audio thread. Garbage-byte
fuzz test: 2000 mixed-type messages → all params stayed finite.
