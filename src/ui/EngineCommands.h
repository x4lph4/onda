#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "CommandManager.h"
#include "core/library/MusicLibraryEngine.h"
#include "core/mixer/MixerController.h"
#include "core/model/DeckStats.h"
#include "core/performance/PerformanceEngine.h"
#include "core/rt/CommandQueue.h"

namespace onda::sampler { class SamplerEngine; }
namespace onda::rec     { class RecordingEngine; }
namespace onda::ui      { class FXSurface; }

namespace onda::ui {

/// Ports the command layer needs from the rest of the app — every
/// field is JUCE-free so tests wire fakes instead of a real
/// AudioEngine. The app fills them once at startup; commands then
/// execute identically from UI / keyboard / MIDI / HID.
struct EnginePorts {
    rt::CommandQueue*         queue  = nullptr;  // → audio thread
    mixer::MixerController*   mixer  = nullptr;  // validated params
    PerformanceEngine*        perf   = nullptr;
    library::MusicLibraryEngine* lib = nullptr;
    sampler::SamplerEngine*   sampler = nullptr;  // Phase 11
    rec::RecordingEngine*     recorder = nullptr; // Phase 12

    /// deck index → live stats (for toggle-state queries + cue trigger)
    std::function<DeckStats*(int)>            deckStats;
    /// (deck, path) → async load request
    std::function<void(int, const std::string&)> loadTrack;
    /// (deck) → unload
    std::function<void(int)>                  unloadTrack;
    /// → currently selected browser path, or "" (DeckLoad uses it)
    std::function<std::string()>              browserSelection;
    /// (id,state) → app-level toggles (settings/browser/layout/fx)
    std::function<void(uint32_t)>             appAction;
    /// (deck, AnalysisEdit action, value) → analysis edit surface;
    /// value < 0 lets the handler derive it (e.g. playhead frame)
    std::function<void(int, int, double)>     analysisEdit;
    /// (deck) → copyable sync-diagnostics text (SyncDiagCopy)
    std::function<void(int)>                  copySyncDiag;
    /// Phase 16 — FX focus surface (macros/presets/slot ops)
    FXSurface*                                fx = nullptr;
    /// () → copyable FX diagnostics text (FxDiagCopy)
    std::function<void()>                     copyFxDiag;
    /// Phase 17 — mixer scenes/presets/diagnostics
    /// (slot 0..7) → capture current mixer state into a scene bank
    std::function<void(int)>                  saveMixScene;
    /// (slot 0..7) → recall a stored scene
    std::function<void(int)>                  recallMixScene;
    /// (delta ±1) → cycle the mixer-preset bank and apply
    std::function<void(int)>                  applyMixPreset;
    /// () → copyable mixer diagnostics text (MixDiagCopy)
    std::function<void()>                     copyMixDiag;
};

/// Register the whole command surface. All commands are POD-safe:
/// they push rt::Command / call non-RT facades — never touch RT code.
void registerEngineCommands(CommandManager& cm,
                            const EnginePorts& ports);

} // namespace onda::ui
