# ADR 0026 — Audio device engine completion (booth routing, typed errors, DJ profiles, output stage)

## Status

Accepted — Phase 26.

## Context

Phase 13 built `onda::adevice` (Backend seam, stable ids, caps probing,
safe switch + rollback, recovery FSM, health monitor, profiles) and
Phase 24 added the mixer booth bus on `outMap[4..5]`. Audit showed the
remaining gaps: the domain `ChannelMapping` never modelled the booth
pair, failures were bare strings (§32), there was no device-status view
(§31), no DJ-controller *audio* profile separate from the controller
layer (§27-30), no explicit auto-route failure mode (§46), and the
physical output stage lacked mono fold-down (§15/§24), a stream-start
fade (§38-39), and sanitize/DC protection (§40-41).

## Decisions

1. **Booth is part of `ChannelMapping`, not a mixer-side extra.**
   `boothL/boothR` join `masterL/R` and `cueL/R`; validation requires
   complete non-overlapping pairs; `defaults()` maps booth on 4/5 only
   with ≥6 detected outs. `DeviceProfile` JSON gained `bL/bR` — v1
   files read back as unmapped. The `mixerMap` port now carries all
   three pairs to `MixerEngine::setOutputMap` (6 atomics).
2. **`defaults()` never leaks the sentinel.** Pairs below the output
   threshold are written `-1`, not left at `ChannelMapping{}`'s
   `cueL=2` — a 2-out device must not "default" into a cue pair it
   cannot host. Same rule inside `DjAudioProfile`: the suggestion is
   derived from detected outs before any model matching.
3. **`DeviceResult` carries `AudioError`, strings stay as a view.**
   `selectEx()`/`setChannelMappingEx()` return typed results
   (`DEVICE_NOT_FOUND`, `CHANNEL_CONFIGURATION_FAILED`,
   `INITIALIZATION_FAILED`, `STREAM_START_FAILED`,
   `DEVICE_DISCONNECTED`, `MANUAL_CONFIGURATION_REQUIRED`, …); the
   old `select()` string API wraps them so callers/tests don't churn.
   `AudioDeviceStatus` is derived from backend status + recovery FSM —
   a view, not a second state machine.
4. **DJ profiles are detected, never assumed.** `DjAudioProfile::
   detect(info, caps)` → `DjController` only for an FLX4-named endpoint
   with ≥4 probed outs; `DjInterface` for any ≥4-out device (booth iff
   ≥6). Name alone yields nothing — an FLX4 reporting 2 outs gets no
   cue suggestion. Controller detection (`ctrl::`) stays a separate
   system (§29-30).
5. **`autoRoute` fails explicitly.** `AudioRouting::autoRoute()` returns
   `{map, ok, reason}`; `outs<=0` or no safe pair →
   `ManualConfigRequired` — the engine refuses rather than guesses (§46).
   Mono devices (`outs==1`) get a legal collapsed master pair.
6. **Physical outs go through `emitPair` — one choke point.** Each
   mapped pair (master/cue/booth) passes: mono fold-down when
   collapsed onto one channel (§15), the stream-start ramp (`fadeG_`,
   ~40 ms, armed by `onStreamRestart()` from
   `audioDeviceAboutToStart`), then `OutputGuard` (NaN/Inf/over-clamp
   sanitize + over-full counter + ~200 ms DC estimator). Buses keep
   writing to preallocated stage buffers; nothing allocates or locks.
   Headphone mono is a *bus* feature (`smMono_` fold-down + `0x40C`
   registry id) — distinct from routing collapse.
7. **Limiter stays the gain-staging boundary.** `OutputGuard` clamps
   at 4.0 — it only engages on broken signal; program headroom is the
   (still optional) master limiter's job (§42).

## Consequences

- `MixerBusDiag` gains `headphoneMono`, `outSanitized`, `outOverFull`,
  `masterDc/cueDc/boothDc`; `adevice` diagnostics print ROUTING /
  DEVICE STATUS / LAST ERROR lines.
- Recovery semantics unchanged; `hotplugCheck` also stamps
  `DEVICE_DISCONNECTED` into `lastResult()`.
- `emitPair` adds one memcpy per mapped pair per block vs the old
  direct write — ~4 KB worst case, negligible.

## Validation

- 17 new `[adev26]` cases: booth mapping/validation, autoRoute +
  manual-required, DJ profiles (incl. "don't invent"), typed error
  codes + status, disconnect→typed error→recovery (success and
  failure), profile v2 JSON, cue xf-independence (zero-crossing),
  mix sweep, 4-deck cue combos, master cue, headphone mono,
  OutputGuard sanitize/DC, stream fade, mono master, mock-FLX4
  end-to-end, state preservation, 4-deck stress.
- `[adev] [mixer] [fx25]` regression: all green; full suite green;
  app smoke stable.
