#pragma once

#include <atomic>
#include <functional>
#include <string>

#include "../audio/TrackAudio.h"
#include "StemTypes.h"

namespace onda::stems {

/// ------------------------------------------------------------------
/// §5 — separation-model abstraction. Everything the engine knows
/// about a separator lives behind this interface; swapping in a
/// learned model (Demucs-family, etc.) never touches DeckEngine,
/// MixerEngine or UI.
///
/// Calls run on StemScheduler workers — implementations may block,
/// allocate and use the GPU, but MUST poll `cancel` periodically in
/// long loops and MUST report progress for UI honesty.
/// ------------------------------------------------------------------
struct StemModelInfo {
    std::string id;          // "onda.bandsplit"
    std::string version;     // semantic — part of the cache key (§28)
    std::string displayName;
};

struct StemModelCaps {
    uint32_t stemMask     { 0 };   // StemType bits this model emits
    bool     gpuCapable   { false };
    bool     streaming    { false }; // can emit partial results
    bool     cancellable  { true };
    double   maxSampleRate{ 96000.0 };
};

struct StemModelConfig {
    int  quality    { (int)StemQuality::Balanced };
    int  gpuMode    { (int)GpuMode::Auto };
    int  workers    { 1 };          // intra-model parallelism
    int  filterOrder{ 2 };          // reference-model steepness
};

using StemCancel = std::atomic<bool>;
/// progress: 0..1 within the current stage; stage is a StemJobPhase.
using StemProgress = std::function<void(StemJobPhase stage, float p)>;

class StemSeparationModel {
public:
    virtual ~StemSeparationModel() = default;

    /// Prepare the model. `err` receives a user-facing reason on
    /// failure — NEVER throw. Model-file validation (§54) belongs
    /// here: load data, verify signatures, never execute anything.
    virtual bool load(const StemModelConfig& cfg,
                      std::string* err) = 0;
    virtual void unload() = 0;
    virtual bool loaded() const = 0;

    virtual StemModelInfo info() const = 0;
    virtual StemModelCaps capabilities() const = 0;

    /// §4 — cheap pre-pass: validate the input can be separated
    /// (channels, rate, duration). Returns a StemError code; Ok means
    /// proceed to separate().
    virtual StemError analyze(const TrackAudio& src,
                              StemCancel& cancel,
                              const StemProgress& progress) = 0;

    /// Produce the stems. On success `out` is fully populated
    /// (rate/channels/numFrames/mask/buffers, all aligned to `src`'s
    /// timeline — §11). A model may emit a PARTIAL mask (§26).
    /// Failure MUST NOT modify playback — just return the error.
    virtual StemError separate(const TrackAudio& src, StemSet& out,
                               StemCancel& cancel,
                               const StemProgress& progress) = 0;
};

} // namespace onda::stems
