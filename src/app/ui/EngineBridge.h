#pragma once

#include <functional>
#include <string>

#include <juce_core/juce_core.h>

#include "core/analysis/AnalysisService.h"
#include "core/library/MusicLibraryEngine.h"
#include "core/performance/PerformanceEngine.h"
#include "core/rec/RecordingEngine.h"
#include "core/sampler/SamplerEngine.h"
#include "platform/audio/AudioEngine.h"
#include "ui/EngineCommands.h"
#include "ui/NotificationCenter.h"
#include "ui/UiState.h"
#include "ui/pads/PadLayer.h"

namespace onda::ui {

/// The single gateway between UI and engines. Views NEVER hold
/// engine pointers — they read UiSnapshot and invoke commands.
///
/// Read side:  snapshot() pulls atomic stats into plain structs
///             (call on the UI thread, ~30 Hz).
/// Write side: EnginePorts wires CommandManager → rt::CommandQueue /
///             MixerController / PerformanceEngine / loaders.
class EngineBridge {
public:
    EngineBridge(audio::AudioEngine& engine,
                 PerformanceEngine& perf,
                 AnalysisService& analysis,
                 library::MusicLibraryEngine& lib,
                 CommandManager& cmds,
                 NotificationCenter& notes,
                 sampler::SamplerEngine* sampler = nullptr,
                 rec::RecordingEngine*   recorder = nullptr);

    const EnginePorts& ports() const { return ports_; }
    EnginePorts&       mutablePorts()  { return ports_; }

    /// Pull the current engine state. Cheap: atomic loads only —
    /// called every UI frame (30 Hz) plus on-demand.
    void snapshot(UiSnapshot& out);

    /// Convenience: deck snapshot only.
    void snapshotDeck(int deck, DeckSnapshot& out);

    /// Track path loaded in a deck (for waveform/analysis lookups).
    std::string deckTrackPath(int deck) const;

    // ---- notification feeds (called from app listeners) ----
    void notifyAnalysis(const std::string& path, bool ok);
    void notifyTrackLoaded(int deck, const std::string& path);
    void notifyTrackMissing(const std::string& path);
    void notifyDeviceError(const std::string& what);

    /// Called on deck metadata refresh — title/artist/bpm/key from
    /// library row when the path matches a library track.
    void refreshDeckMeta(int deck, DeckSnapshot& out);

    /// Phase 11: domain pad layer (pad.press / pad.mode commands).
    pads::PadLayer&          pads()    noexcept { return pads_; }
    /// Phase 12: session recorder — may be null in minimal builds.
    rec::RecordingEngine*    recorder() noexcept { return recorder_; }

private:
    audio::AudioEngine&          engine_;
    PerformanceEngine&           perf_;
    AnalysisService&             analysis_;
    library::MusicLibraryEngine& lib_;
    NotificationCenter&          notes_;

    pads::PadLayer  pads_;
    rec::RecordingEngine* recorder_ = nullptr;
    EnginePorts ports_;
    std::string deckPath_[4];
    bool        pfl_[4] = { false, false, false, false };
    uint64_t    lastClipCount_ = 0;
};

} // namespace onda::ui
