#pragma once

#include <memory>
#include <string>
#include <thread>

#include "BandSplitModel.h"

namespace onda::stems {

/// ------------------------------------------------------------------
/// §6-8 — model registry + hardware-aware selection.
///
/// The provider probes the machine once (§7), then answers
/// "best model + config for (quality, gpuMode)". GPU is NEVER assumed
/// (§46): a probe function is injected (real backends can install a
/// CUDA/DirectML/Metal probe later); the default probe reports "no
/// accelerator" and every request resolves to a CPU configuration.
/// Choosing a GPU mode on a GPU-less machine silently degrades to
/// CPU — never blocks, never fails.
/// ------------------------------------------------------------------
struct StemHwCaps {
    int  cpuCores    { 0 };
    int  cpuWorkers  { 1 };   // recommended background workers
    bool gpuAvailable{ false };
    std::string gpuName;
};

class StemModelProvider {
public:
    /// `gpuProbe` — optional detector; default reports none.
    explicit StemModelProvider(
        std::function<bool(std::string* nameOut)> gpuProbe = nullptr) {
        caps_.cpuCores   = (int)std::thread::hardware_concurrency();
        caps_.cpuWorkers = caps_.cpuCores > 4 ? 2 : 1;
        if (gpuProbe) {
            std::string name;
            caps_.gpuAvailable = gpuProbe(&name);
            caps_.gpuName = std::move(name);
        }
    }

    const StemHwCaps& caps() const { return caps_; }

    /// Resolve an effective runtime config for a request (§8).
    /// `quality` picks steepness/worker budgets; `gpuMode` is honoured
    /// only when an accelerator exists.
    StemModelConfig configFor(int quality, int gpuMode) const {
        StemModelConfig c;
        c.quality = quality;
        switch ((StemQuality)quality) {
            case StemQuality::Fast:        c.filterOrder = 1;
                                           c.workers = 1; break;
            case StemQuality::Balanced:    c.filterOrder = 2;
                                           c.workers = 1; break;
            case StemQuality::HighQuality: c.filterOrder = 4;
                                           c.workers = caps_.cpuWorkers; break;
            default:                       c.filterOrder = 2;
                                           c.workers = 1; break;
        }
        // GPU request resolves to the actual capability — explicit On
        // with no GPU still runs (CPU), matching §7/§46 policy.
        const bool wantGpu = gpuMode != (int)GpuMode::Off;
        c.gpuMode = (wantGpu && caps_.gpuAvailable)
                        ? (int)GpuMode::On : (int)GpuMode::Off;
        return c;
    }

    /// Instantiate the best model for a config. Today: always the
    /// reference band-split (CPU). Future providers return learned
    /// models here — same interface (§5).
    std::unique_ptr<StemSeparationModel> createModel(
        const StemModelConfig& /*cfg*/) const {
        return std::make_unique<BandSplitModel>();
    }

    /// Does any installed model satisfy these requirements?
    bool canServe(int /*quality*/, int /*gpuMode*/) const { return true; }

private:
    StemHwCaps caps_;
};

} // namespace onda::stems
