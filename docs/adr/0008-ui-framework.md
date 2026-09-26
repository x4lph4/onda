# ADR-0008 — UI Framework (JUCE-free core + JUCE views)

Status: accepted (Phase 8)
Date: 2025

## Context

The app needs a professional live-performance UI on top of the audio,
mixer, performance, FX and library engines — without letting the UI
touch real-time objects or duplicate business logic per input source
(mouse, keyboard, MIDI/HID controllers later).

## Architecture — two layers

```
src/ui/            JUCE-free core — fully unit-testable, no JUCE types
├── CommandIds       stable id space: (category<<10)|index — the same
│                    address a MIDI/HID mapping table would use
├── CommandManager   registry + dispatch + state polling + listeners
├── EngineCommands   every command registered once against ports
│                    (CommandQueue / MixerController / Performance /
│                    Library / deckStats) — UI, keys, MIDI, HID share it
├── ShortcutManager  KeyDesc{mods,code} → cmd id; named profile,
│                    serialize/deserialize, JUCE-free key grammar
├── UiState          plain-value snapshots (DeckSnapshot, Channel-,
│                    Master-, FxSlot-, AudioStatus…); views are pure
│                    functions of (snapshot, theme, bounds)
├── NotificationCenter  bounded toast queue (dedup, ttl, level)
├── WaveformMapping  sec↔px, zoom-at-anchor, scroll clamp, seek,
│                    ensureVisible, grid-marker collection
├── TrackTableModel  paged virtual model: kPage=128 chunks, ≤8 pages
│                    resident (~1 k rows) regardless of library size
├── LayoutSpec       shell regions + collapse priorities
└── DesignTokens/ThemeEngine  palette/metrics/typography, density,
                    accent + per-deck waveform colors, serialize

src/app/ui/        JUCE views — render only from snapshots
├── OndaTheme        tokens → juce::LookAndFeel helpers
├── EngineBridge     THE ONLY gateway: builds UiSnapshot from atomics,
│                    translates engine state→snapshot, feeds notifications
├── AppShell         layout + 30 Hz tick + keyboard dispatch + overlays
├── TopBar           brand/master BPM/device/rate/buffer/latency/
│                    DSP%/XRUN/CLIP/UI-FPS + panel toggles
├── DeckView         info badges + transport + perf toggles + pitch +
│                    loop row + 8 hot-cue pads
├── WaveformView     overview + detail, playhead, grid (beat/bar/
│                    downbeat), cue markers, loop regions, zoom/scroll/seek
├── MixerView        2 channel strips (gain/EQ/filter/fader/PFL/meter/
│                    xf-assign) + master + crossfader
├── FXView           generic per-deck/send/master racks from FXSlot-
│                    Snapshots (kind selector, enable, mix, sync, params)
├── BrowserView      search field + rail (library/playlists/history)
│                    + virtualized table (ListBoxModel + TrackTableModel)
├── SettingsView     GENERAL/AUDIO/LIBRARY/DECKS/MIXER/FX/CONTROLLERS/
│                    SHORTCUTS/APPEARANCE/DIAGNOSTICS sections
└── controls/        StateButton (OFF/ON/ACTIVE/DISABLED/ERROR with
                    shape+text, not color only), Knob, MeterBar
```

## State separation

- **Audio state** — `DeckStats`/`MasterStats`/`FXStats` atomics, owned
  by the engines. The UI never mutates them.
- **Domain state** — library DB, playlists, analysis bundles.
- **UI state** — `UiSnapshot` (rebuilt at 30 Hz), `TrackTableModel`
  window, layout prefs, theme, notifications.

## Control flow — one path for every input

```
UI event / KeyPress / (future) MIDI / HID
        ↓  KeyDesc / CC→id mapping
CommandManager::invoke(id, source, value)
        ↓  exactly one registered handler
EngineCommands → CommandQueue::tryPush (deck/transport/master)
              → MixerController::set (validated, queued SetParam)
              → PerformanceEngine (cues/loops/sync — message thread)
              → MusicLibraryEngine (queries/post — non-RT)
```

A `PlayDeckA` executed from a button, `Q`, a MIDI pad or an HID button
runs identical code. `CommandManager::addListener` gives hardware
layers feedback (LED state) without touching engine internals.

## Rendering rules

- Views paint ONLY from snapshots + theme — never read engine state
  inside `paint()`.
- `WaveformView` queries `AnalysisBundle::waveform` (cached bins) once
  per loaded track; grid markers come from `WaveformMapping` math on
  the same bundle — zero per-frame audio work.
- The table renders visible rows only; page fetches are amortized and
  bounded (stress test: worst simulated frame 3.4 ms at 60 fps sweep).
- 30 Hz tick drives meters/playheads — enough for smooth motion at
  half the cost of 60 Hz repaints; `uiFps`/`uiFrameMs` are measured
  and shown in the TopBar.

## Safety contract

- No DSP in `src/ui` or `src/app/ui` — no `processBlock`, no filters,
  no buffers. Verified by code review + the layer split (ui targets
  don't link DSP units).
- Engine writes go through `tryPush` (bounded ring → drop-oldest
  cannot block) or non-RT facades; nothing in the UI locks a mutex
  the audio thread needs.
- `EngineBridge` reads only atomic snapshots; cue/loop ops call
  `PerformanceEngine` which is message-thread by design.

## Responsive policy (LayoutSpec)

Collapse order under pressure: browser shrinks to 140 px → fx strip
hides → browser hides → waveform strip shrinks → floor at min sizes.
Transport, waveform playhead, BPM/key and meters are never hidden —
verified in `ui_tests.cpp` for 960×560 … 3840×2160.

## Accessibility / error states

- `StateButton` encodes state by fill + edge + label, not hue alone.
- HC theme variant + density + accent customization.
- Every control reachable via `ShortcutManager` (remappable profile);
  `AppPanic` (Esc) stops all decks.
- Errors surface as edge-triggered toasts (`NotificationCenter`) with
  clear text — device loss, xruns, clip deltas, missing files,
  analysis failure — never modal for transient events.

## Measured

- 148 test cases / 1,594,204 assertions green (14 UI cases).
- Stress (JUCE-free, 600 frames): 50k-row scroll + waveform math +
  command dispatch → avg 1.0 ms/frame, worst 4.8 ms — under a
  16.6 ms budget with headroom; model memory bounded at ≤1024 rows.
- App smoke: clean start, engine @48 kHz/480 smp, UI-FPS readout
  live in TopBar.

## Limitations

- `DeckLoad` is wired (command + browser DnD target) but file-drop
  from the OS is marked TODO in `BrowserView`.
- `XYPad` is specified in the design system; the current FX set is
  knob-parametrized so it isn't built yet — `controls/` gains it when
  an FX kind needs XY mapping.
- Smart-playlist rules, MIDI learn and HID pages are placeholders in
  `SettingsView` — the command surface they will drive is complete.
- Waveform playhead smoothness is 30 Hz (timer), not vsync-locked —
  acceptable for meters/playheads; sub-frame interpolation can be
  added inside `WaveformView` if needed.
- Remote/HTML surface not in scope; `CommandManager` +
  `EngineCommands` is the integration seam for it.
