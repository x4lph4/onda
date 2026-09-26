#pragma once

#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../rt/CommandQueue.h"
#include "SampleBank.h"
#include "SampleCache.h"
#include "SampleLoader.h"
#include "SampleRecorder.h"
#include "SampleRepository.h"
#include "SamplerRt.h"

namespace onda::sampler {

/// Domain façade for the whole sampler subsystem (task §1). Owns
/// banks, the async loader, the cache and persistence; mirrors every
/// performance action to the RT engine via rt::CommandQueue — the
/// same queue UI, keyboard, MIDI and HID use. Never blocks RT.
///
/// Threading: all public methods are app/control-thread. `tick()`
/// must be called periodically (~30 Hz) to drain loader results,
/// reclaim retired SampleData and persist dirty slots.
class SamplerEngine {
public:
    struct Ports {
        rt::CommandQueue* queue = nullptr;   // → audio thread
        /// notify(title, detail, level) — UI toasts
        std::function<void(const std::string&,
                           const std::string&, int)> notify;
    };

    SamplerEngine() = default;
    ~SamplerEngine() { if (loader_) loader_->stop(); }

    void init(DecoderFactory factory, double sampleRate = 48000.0);
    void setPorts(Ports p) { ports_ = std::move(p); }
    void attachRepository(SampleRepository* repo) { repo_ = repo; }

    // ---- banks (task §2) ----
    int  addBank(const std::string& name, int slots = SampleBank::kPadSlots);
    bool removeBank(int index);
    int  bankCount() const { return (int)banks_.size(); }
    int  activeBank() const { return activeBank_; }
    void selectBank(int i);
    SampleBank&       bank(int i)       { return banks_[(size_t)i]; }
    const SampleBank& bank(int i) const { return banks_[(size_t)i]; }
    /// active-bank slot = what pads/performance controls address
    SampleSlot&       slot(int pad)       {
        return banks_[(size_t)activeBank_].slot(pad);
    }
    const SampleSlot& slot(int pad) const {
        return banks_[(size_t)activeBank_].slot(pad);
    }

    // ---- loading (task §3/§4/§26) — always async ----
    /// returns request id (>0) or -1. Slot → Loading until tick().
    int64_t loadSample(int pad, const std::string& path,
                       double bpmHint = 0.0);
    void    loadFolder(int firstPad, const std::string& dir); // best-effort
    void    unloadSample(int pad);
    void    preload(int pad);        // pin in cache (task §26)
    bool    isLoading(int pad) const {
        return slot(pad).state.load() == (int)SlotState::Loading;
    }

    // ---- performance (task §5) — pushes rt commands ----
    void trigger(int pad, float vel = 1.0f);
    void release(int pad);
    void stop(int pad);
    void stopAll();

    // ---- parameters (domain mirror + rt push) ----
    void setSlotParam(int pad, uint8_t kind, float v);
    float slotParam(int pad, uint8_t kind) const;
    void setBankGain(float v);

    // ---- housekeeping (control thread, ~30 Hz) ----
    void tick();
    /// pending decode count (diagnostics)
    bool loaderBusy() const { return loader_ && loader_->busy(); }

    // ---- persistence (task §27) ----
    bool saveAll();
    bool loadAll();

    // ---- access ----
    SamplerRt&       rt()       noexcept { return rt_; }
    SampleCache&     cache()    noexcept { return cache_; }
    SampleRecorder&  recorder() noexcept { return recorder_; }
    int  activeVoices() const noexcept { return rt_.activeVoices(); }
    int64_t cacheBytes() const noexcept { return cache_.bytesUsed(); }

private:
    void applyResult(SampleLoader::Result&& r);
    void publishBank();          // push active bank params+data to RT
    void mirrorSlot(int pad);    // push all of one slot's params
    static void pullSlotRt(SamplerEngine& e, int pad,
                           SampleSlot& s);  // domain → rt param cmds

    Ports                    ports_;
    DecoderFactory           factory_;
    double                   rate_ = 48000.0;
    SamplerRt                rt_;

    std::vector<SampleBank>  banks_;
    int                      activeBank_ = 0;
    SampleCache              cache_;
    SampleRecorder           recorder_;
    SampleRepository*        repo_ = nullptr;
    /// unique_ptr: SampleLoader is immobile (thread/mutex/cv)
    std::unique_ptr<SampleLoader> loader_;        // created in init()

    std::mutex                      resMu_;
    std::deque<SampleLoader::Result> results_;
    bool                            dirty_ = false;
};

} // namespace onda::sampler
