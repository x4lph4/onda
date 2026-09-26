#include "SamplerEngine.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

#include "SampleSync.h"

namespace fs = std::filesystem;

namespace onda::sampler {

namespace {
void pushq(rt::CommandQueue* q, const rt::Command& c) {
    if (q) q->tryPush(c);
}
} // namespace

void SamplerEngine::init(DecoderFactory factory, double rate) {
    factory_ = std::move(factory);
    rate_ = rate;
    rt_.prepare(rate);
    loader_ = std::make_unique<SampleLoader>(factory_, rate_);
    loader_->start([this](SampleLoader::Result&& r) {
        std::lock_guard<std::mutex> lk(resMu_);
        results_.push_back(std::move(r));
    });
    if (banks_.empty()) addBank("Bank 1");
}

// ---------------- banks ----------------

int SamplerEngine::addBank(const std::string& name, int n) {
    SampleBank b(n);
    b.name = name;
    if (repo_) b.id = repo_->upsertBank(-1, name, (int)banks_.size(), n);
    banks_.push_back(std::move(b));
    return (int)banks_.size() - 1;
}

bool SamplerEngine::removeBank(int index) {
    if (index <= 0 || index >= (int)banks_.size()) return false;
    if (repo_ && banks_[(size_t)index].id >= 0)
        repo_->deleteBank(banks_[(size_t)index].id);
    banks_.erase(banks_.begin() + index);
    if (activeBank_ >= (int)banks_.size())
        activeBank_ = (int)banks_.size() - 1;
    publishBank();
    return true;
}

void SamplerEngine::selectBank(int i) {
    if (i == activeBank_ || i < 0 || i >= (int)banks_.size()) return;
    stopAll();
    activeBank_ = i;
    publishBank();
    dirty_ = true;
}

// ---------------- loading ----------------

int64_t SamplerEngine::loadSample(int pad, const std::string& path,
                                  double bpmHint) {
    if (path.empty()) return -1;
    SampleSlot& s = slot(pad);
    s.path = path;
    if (s.name.empty()) {
        s.name = fs::path(path).stem().string();
        if (s.name.empty()) s.name = path;
    }
    s.state.store((int)SlotState::Loading);
    return loader_ ? loader_->load(pad, path, bpmHint) : -1;
}

void SamplerEngine::loadFolder(int firstPad, const std::string& dir) {
    std::error_code ec;
    int pad = firstPad;
    for (auto& de : fs::directory_iterator(dir, ec)) {
        if (!de.is_regular_file() || pad >= bank(activeBank_).slotCount())
            break;
        const auto ext = de.path().extension().string();
        if (ext == ".wav" || ext == ".mp3" || ext == ".flac" ||
            ext == ".aiff" || ext == ".aif" || ext == ".ogg")
            loadSample(pad++, de.path().string());
    }
}

void SamplerEngine::unloadSample(int pad) {
    SampleSlot& s = slot(pad);
    stop(pad);
    rt_.clearSlot(pad);
    if (s.sampleId > 0) cache_.remove(s.sampleId);
    s.sampleId = -1;
    s.path.clear(); s.name.clear();
    s.durationFrames.store(0); s.bpm.store(0.0);
    s.state.store((int)SlotState::Empty);
    dirty_ = true;
}

void SamplerEngine::preload(int pad) {
    const SampleSlot& s = slot(pad);
    if (s.sampleId > 0) cache_.pin(s.sampleId, true);
}

// ---------------- performance ----------------

void SamplerEngine::trigger(int pad, float vel) {
    const SampleSlot& s = slot(pad);
    if (s.state.load() == (int)SlotState::Empty) return;
    pushq(ports_.queue, rt::SampleTrigger{(uint8_t)pad, vel});
}
void SamplerEngine::release(int pad) {
    pushq(ports_.queue, rt::SampleRelease{(uint8_t)pad});
}
void SamplerEngine::stop(int pad) {
    pushq(ports_.queue, rt::SampleStop{(uint8_t)pad});
}
void SamplerEngine::stopAll() {
    pushq(ports_.queue, rt::SampleStop{255});
}

// ---------------- parameters ----------------

void SamplerEngine::setSlotParam(int pad, uint8_t kind, float v) {
    SampleSlot& s = slot(pad);
    switch (kind) {
        case SP_Volume:   s.volume = std::clamp(v, 0.f, 1.f); break;
        case SP_Pan:      s.pan = std::clamp(v, -1.f, 1.f); break;
        case SP_Gain:     s.gain = std::clamp(v, 0.f, 2.f); break;
        case SP_Pitch:    s.pitch = std::clamp(v, -48.f, 48.f); break;
        case SP_Mode:     s.mode = (PlaybackMode)(int)v; break;
        case SP_LoopOn:   s.loopOn = v > 0.5f; break;
        case SP_SyncOn:   s.syncOn = v > 0.5f; break;
        case SP_Quantize: s.quant = (QuantizeMode)(int)v; break;
        case SP_Mute:     s.mute = v > 0.5f; break;
        case SP_Solo:     s.solo = v > 0.5f; break;
        case SP_Filter:   s.filterPos = std::clamp(v, -1.f, 1.f); break;
        default: break;
    }
    pushq(ports_.queue, rt::SampleSlotParam{(uint8_t)pad, kind, v});
    dirty_ = true;
}

float SamplerEngine::slotParam(int pad, uint8_t kind) const {
    const SampleSlot& s = slot(pad);
    switch (kind) {
        case SP_Volume:   return s.volume;
        case SP_Pan:      return s.pan;
        case SP_Gain:     return s.gain;
        case SP_Pitch:    return s.pitch;
        case SP_Mode:     return (float)(int)s.mode;
        case SP_LoopOn:   return s.loopOn ? 1.f : 0.f;
        case SP_SyncOn:   return s.syncOn ? 1.f : 0.f;
        case SP_Quantize: return (float)(int)s.quant;
        case SP_Mute:     return s.mute ? 1.f : 0.f;
        case SP_Solo:     return s.solo ? 1.f : 0.f;
        case SP_Filter:   return s.filterPos;
        default:          return 0.f;
    }
}

void SamplerEngine::setBankGain(float v) {
    pushq(ports_.queue, rt::SamplerParam{0, std::clamp(v, 0.f, 2.f)});
}

// ---------------- housekeeping ----------------

void SamplerEngine::tick() {
    // drain loader completions (worker → app thread handoff)
    for (;;) {
        SampleLoader::Result r;
        {
            std::lock_guard<std::mutex> lk(resMu_);
            if (results_.empty()) break;
            r = std::move(results_.front());
            results_.pop_front();
        }
        applyResult(std::move(r));
    }
    rt_.retireTrash();
}

void SamplerEngine::applyResult(SampleLoader::Result&& r) {
    SampleSlot& s = slot(r.job.slot);
    if (!r.data) {
        s.state.store((int)SlotState::Error);
        if (ports_.notify)
            ports_.notify("Sample load failed",
                          s.name + " — " + r.error, 2);
        return;
    }
    // register in library DB (sample row) + cache + RT publish
    if (repo_) {
        s.sampleId = repo_->upsertSample(
            s.path, s.name,
            (double)r.data->frames / r.data->sampleRate,
            r.data->peak, r.data->bpm,
            r.data->sampleRate, r.data->channels);
    }
    if (s.sampleId > 0) cache_.put(s.sampleId, r.data, false);
    rt_.publishSlot(r.job.slot, r.data);
    s.durationFrames.store(r.data->frames);
    s.bpm.store(r.data->bpm);
    s.state.store((int)SlotState::Ready);
    dirty_ = true;
}

// publish active-bank params + cached audio into RT slots 0..7
void SamplerEngine::publishBank() {
    SampleBank& b = banks_[(size_t)activeBank_];
    const int n = std::min(b.slotCount(), SampleBank::kPadSlots);
    for (int i = 0; i < n; ++i) {
        SampleSlot& s = b.slot(i);
        mirrorSlot(i);
        auto data = s.sampleId > 0 ? cache_.get(s.sampleId) : nullptr;
        if (data) {
            rt_.publishSlot(i, std::move(data));
            s.state.store((int)SlotState::Ready);
        } else if (!s.path.empty()) {
            // sample not cached → queue a reload from disk
            s.state.store((int)SlotState::Loading);
            if (loader_) loader_->load(i, s.path, s.bpm.load());
        } else {
            rt_.clearSlot(i);
            s.state.store((int)SlotState::Empty);
        }
    }
}

void SamplerEngine::mirrorSlot(int pad) {
    static const uint8_t kinds[] = {
        SP_Volume, SP_Pan, SP_Gain, SP_Pitch, SP_Mode,
        SP_LoopOn, SP_SyncOn, SP_Quantize, SP_Mute, SP_Solo };
    for (uint8_t k : kinds)
        pushq(ports_.queue,
              rt::SampleSlotParam{(uint8_t)pad, k,
                                  slotParam(pad, k)});
}

// ---------------- persistence ----------------

bool SamplerEngine::saveAll() {
    if (!repo_) return false;
    for (auto& b : banks_) {
        if (b.id < 0)
            b.id = repo_->upsertBank(-1, b.name, activeBank_,
                                   b.slotCount());
        for (auto& s : b.slots)
            repo_->saveSlot(b.id, s);
    }
    dirty_ = false;
    return true;
}

bool SamplerEngine::loadAll() {
    if (!repo_) return false;
    auto loaded = repo_->loadAll();
    if (loaded.empty()) return false;
    stopAll();
    banks_ = std::move(loaded);
    activeBank_ = 0;
    publishBank();
    dirty_ = false;
    return true;
}

} // namespace onda::sampler
