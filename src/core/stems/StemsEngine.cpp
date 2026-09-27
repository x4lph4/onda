#include "StemsEngine.h"

#include <chrono>
#include <filesystem>

namespace onda::stems {

StemsEngine::StemsEngine(Database& db, rt::CommandQueue& queue)
    : db_(db), queue_(queue), store_(db) {
    provider_ = std::make_unique<StemModelProvider>();
    sched_.setRunner([this](const StemJob& j, StemCancel& c,
                            const std::function<void(StemJobPhase, float)>& r) {
        return runJob(j, c, r);
    });
    sched_.addListener([this](const StemScheduler::Notification& n) {
        onJobEvent(n);
    });
}

StemsEngine::~StemsEngine() {
    // decks must not adopt buffers whose owner is dying
    for (auto& slot : slots_)
        if (slot.eng) slot.eng->pendingStems_.store(nullptr);
}

void StemsEngine::configure(const StemConfig& cfg) {
    cfg_ = cfg;
    cache_ = StemCache(cfg_.cacheDir);
    std::error_code ec;
    std::filesystem::create_directories(cfg_.cacheDir, ec);
}

void StemsEngine::setModelProvider(std::unique_ptr<StemModelProvider> p) {
    if (p) provider_ = std::move(p);
}

// ------------------------------------------------------------------ decks

void StemsEngine::bindDeck(int deck, DeckEngine* eng, DeckStats* stats) {
    if (deck < 0 || deck >= kDecks) return;
    slots_[deck].eng = eng;
    slots_[deck].stats = stats;
    mgr_[deck].reset();
}

void StemsEngine::onTrackLoading(int deck, int64_t trackId,
                                 uint64_t contentHash,
                                 const std::string& path,
                                 double deviceRate) {
    if (deck < 0 || deck >= kDecks) return;
    auto& slot = slots_[deck];
    slot.trackId = trackId; slot.contentHash = contentHash;
    slot.path = path;       slot.deviceRate = deviceRate;
    slot.jobId = -1;
    mgr_[deck].onCleared();
    if (slot.stats)
        slot.stats->stemsAvail.store((int)StemAvailability::Unavailable);

    if (contentHash == 0 || trackId <= 0) return;   // unknown track

    // Cache fast-path: a matching ready set → load it on a worker.
    const StemSetMeta meta = metaFor(slot, cfg_.defaultQuality);
    int st = -1;
    if (store_.findKey(contentHash, meta.key, const_cast<StemSetMeta&>(meta), &st)
        && st == StemStore::Ready && cache_.exists(meta.key)) {
        diag_.cacheHits++;
        mgr_[deck].setAvailability(StemAvailability::Loading);
        if (slot.stats)
            slot.stats->stemsAvail.store((int)StemAvailability::Loading);
        StemJob j;
        j.kind = 1;                     // load
        j.trackId = trackId; j.contentHash = contentHash;
        j.path = path; j.quality = meta.quality;
        j.targetRate = deviceRate;
        j.modelId = meta.modelId; j.modelVersion = meta.modelVersion;
        j.cacheKey = meta.key;
        slot.jobId = sched_.enqueue(j);
        mgr_[deck].setJobId(slot.jobId);
        diag_.jobsQueued++;
        return;
    }

    diag_.cacheMisses++;
    if (cfg_.autoSeparate && !path.empty()) {
        requestSeparation(trackId, contentHash, path,
                          cfg_.defaultQuality, 0);
        mgr_[deck].setAvailability(StemAvailability::Queued);
        if (slot.stats)
            slot.stats->stemsAvail.store((int)StemAvailability::Queued);
        return;
    }
    // §25/§32: nothing available — deck just plays the original.
}

void StemsEngine::onTrackUnloaded(int deck) {
    if (deck < 0 || deck >= kDecks) return;
    auto& slot = slots_[deck];
    slot.trackId = 0; slot.contentHash = 0; slot.jobId = -1;
    slot.path.clear();
    mgr_[deck].onCleared();
    mix_[deck].reset();
    if (slot.eng) slot.eng->pendingStems_.store(nullptr);
    if (slot.stats)
        slot.stats->stemsAvail.store((int)StemAvailability::Unavailable);
}

// ------------------------------------------------------------------ jobs

int64_t StemsEngine::requestSeparation(int64_t trackId, uint64_t contentHash,
                                       const std::string& path,
                                       int quality, int priority) {
    if (contentHash == 0) { diag_.rejects++; return -1; }
    StemJob j;
    j.kind = 0;                         // separate
    j.trackId = trackId; j.contentHash = contentHash;
    j.path = path;
    j.quality = quality >= 0 ? quality : cfg_.defaultQuality;
    j.priority = priority;
    const int64_t id = sched_.enqueue(j);
    diag_.jobsQueued++;
    for (int d = 0; d < kDecks; ++d)
        if (slots_[d].contentHash == contentHash) {
            slots_[d].jobId = id;
            mgr_[d].setJobId(id);
            mgr_[d].setAvailability(StemAvailability::Queued);
            if (slots_[d].stats)
                slots_[d].stats->stemsAvail.store(
                    (int)StemAvailability::Queued);
        }
    return id;
}

bool StemsEngine::cancelSeparation(int64_t jobId) {
    if (jobId < 0) return false;
    sched_.cancel(jobId);
    return true;
}

void StemsEngine::cancelForDeck(int deck) {
    if (deck < 0 || deck >= kDecks) return;
    if (slots_[deck].jobId >= 0) sched_.cancel(slots_[deck].jobId);
}

StemJobPhase StemsEngine::jobPhase(int64_t jobId) const {
    return (StemJobPhase)sched_.stateOf(jobId).phase;
}
float StemsEngine::jobProgress(int64_t jobId) const {
    return sched_.stateOf(jobId).progress;
}

// ------------------------------------------------------------------ state

StemAvailability StemsEngine::availability(int deck) const {
    if (deck < 0 || deck >= kDecks) return StemAvailability::Unavailable;
    return mgr_[deck].availability();
}
const Stem& StemsEngine::stemState(int deck, int stem) const {
    static Stem empty;
    if (deck < 0 || deck >= kDecks || stem < 0 || stem >= kStemMax)
        return empty;
    return mgr_[deck].stem(stem);
}
uint32_t StemsEngine::presentMask(int deck) const {
    if (deck < 0 || deck >= kDecks) return 0;
    return mgr_[deck].presentMask();
}

// ------------------------------------------------------------------ perf

void StemsEngine::setStemMode(int deck, int mode) {
    if (deck < 0 || deck >= kDecks) return;
    mgr_[deck].setMode(mode);
    postMode(deck);
    diag_.modeChanges++;
}
void StemsEngine::setStemGain(int deck, int stem, float g) {
    if (stem < 0 || stem >= kStemMax) return;
    mix_[deck].setGain(stem, g);
    autoEnableIfStems(deck);
    postParam(deck, stem);
    diag_.paramChanges++;
}
void StemsEngine::setStemPan(int deck, int stem, float p) {
    if (stem < 0 || stem >= kStemMax) return;
    mix_[deck].setPan(stem, p);
    postParam(deck, stem);
    diag_.paramChanges++;
}
void StemsEngine::setStemMute(int deck, int stem, bool m) {
    if (stem < 0 || stem >= kStemMax) return;
    mix_[deck].setMute(stem, m);
    autoEnableIfStems(deck);
    postParam(deck, stem);
    diag_.paramChanges++;
}
void StemsEngine::setStemSolo(int deck, int stem, bool s) {
    if (stem < 0 || stem >= kStemMax) return;
    mix_[deck].setSolo(stem, s);
    autoEnableIfStems(deck);
    // solo affects EVERY stem's resolved gain → repost all
    for (int i = 0; i < kStemMax; ++i) postParam(deck, i);
    diag_.paramChanges++;
}
void StemsEngine::clearStemMutes(int deck) {
    if (deck < 0 || deck >= kDecks) return;
    mix_[deck].clearMutes();
    mix_[deck].clearSolos();
    for (int i = 0; i < kStemMax; ++i) postParam(deck, i);
    diag_.paramChanges++;
}
void StemsEngine::stemGate(int deck, int stem, bool held) {
    if (stem < 0 || stem >= kStemMax) return;
    // momentary kill — toggles mute while held (documented §20
    // "trigger": gate = temporary mute of that stem)
    mix_[deck].setMute(stem, held);
    autoEnableIfStems(deck);
    postParam(deck, stem);
    diag_.paramChanges++;
}

void StemsEngine::stemAction(int deck, int stem, int kind, float v) {
    if (deck < 0 || deck >= kDecks) { diag_.rejects++; return; }
    switch (kind) {
        case 0: { const auto& mx = mix_[deck];
                  setStemMute(deck, stem, !mx.param(stem).muted); break; }
        case 1: { const auto& mx = mix_[deck];
                  setStemSolo(deck, stem, !mx.param(stem).solo); break; }
        case 2: setStemGain(deck, stem,
                            mix_[deck].param(stem).gain + 0.05f); break;
        case 3: setStemGain(deck, stem,
                            mix_[deck].param(stem).gain - 0.05f); break;
        case 4: setStemPan(deck, stem,
                           mix_[deck].param(stem).pan - 0.05f); break;
        case 5: setStemPan(deck, stem,
                           mix_[deck].param(stem).pan + 0.05f); break;
        case 6: stemGate(deck, stem, v > 0.5f); break;
        case 7: setStemGain(deck, stem, v * 2.0f); break;   // 0..1 → 0..2
        case 8: clearStemMutes(deck); break;
        default: diag_.rejects++; break;
    }
}

/// Stem actions imply stems mode — first action with a set present
/// flips the deck into stems render (documented §20 behaviour, same
/// as real products: touching a stem pad activates stems).
void StemsEngine::autoEnableIfStems(int deck) {
    if (mgr_[deck].mode() == 0 && mgr_[deck].presentMask() != 0)
        setStemMode(deck, 1);
}

StemRoutingPlan StemsEngine::routing(int deck) const {
    if (deck < 0 || deck >= kDecks) return {};
    return router_.plan(deck, mgr_[deck].mode() == 1,
                        mgr_[deck].presentMask(), fxR_[deck].wanted());
}

bool StemsEngine::applyPreset(const std::string& name, int deck) {
    if (deck < 0 || deck >= kDecks) return false;
    StemMixer m;
    if (!presets_.apply(name, m)) return false;
    mix_[deck] = m;
    for (int i = 0; i < kStemMax; ++i) postParam(deck, i);
    autoEnableIfStems(deck);
    return true;
}

int StemsEngine::trackStatus(uint64_t contentHash) {
    return store_.statusForTrack(contentHash);
}

bool StemsEngine::clearSet(uint64_t contentHash, const std::string& key) {
    const bool ok = cache_.clearSet(key);
    store_.deleteKey(contentHash, key);
    return ok;
}
void StemsEngine::clearAll() {
    cache_.clearAll();
    store_.deleteAll();
}

void StemsEngine::setPlaybackActive(bool active) {
    sched_.setMinActivePriority(active ? 1 : 0);  // §45 CPU gate
}

// ------------------------------------------------------------------ poll

void StemsEngine::poll() {
    // publish produced sets to waiting decks (hash-matched)
    std::vector<std::pair<int64_t, uint64_t>> ready;
    {
        std::lock_guard<std::mutex> lk(mu_);
        ready.swap(readyForHash_);
    }
    for (auto& [jobId, hash] : ready) {
        std::shared_ptr<StemSet> set;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = produced_.find(jobId);
            if (it != produced_.end()) { set = it->second; produced_.erase(it); }
        }
        if (set)
            for (int d = 0; d < kDecks; ++d)
                if (slots_[d].contentHash == hash &&
                    slots_[d].contentHash != 0)
                    publishSet(d, set);
        // finished the deck's tracking id
        for (int d = 0; d < kDecks; ++d)
            if (slots_[d].jobId == jobId) { slots_[d].jobId = -1;
                                            mgr_[d].setJobId(-1); }
    }
    for (int d = 0; d < kDecks; ++d) releaseRetired(d);
}

void StemsEngine::publishSet(int deck, std::shared_ptr<StemSet> set) {
    auto& slot = slots_[deck];
    if (!slot.eng || !set) return;
    if ((int)set->rate != (int)slot.deviceRate) return;   // wrong rate
    auto h = std::make_shared<StemHandle>();
    h->set = std::move(set);                             // share, no copy
    h->view.frames = h->set->numFrames;
    h->view.mask = h->set->mask;
    for (int i = 0; i < DeckStemBuffers::kMaxStems; ++i)
        h->view.samples[i] = h->set->samples[(size_t)i].data();
    {
        std::lock_guard<std::mutex> lk(mu_);
        owned_[deck][&h->view] = h;
    }
    slot.eng->pendingStems_.store(&h->view);             // RT adopts
    mgr_[deck].onSetLoaded(*h->set);
    if (slot.stats)
        slot.stats->stemsAvail.store((int)mgr_[deck].availability());
    diag_.bytesRead += (uint64_t)h->set->bytes();
}

void StemsEngine::releaseRetired(int deck) {
    auto& slot = slots_[deck];
    if (!slot.eng) return;
    for (;;) {
        const DeckStemBuffers* v =
            slot.eng->retiredStems_.exchange(nullptr);
        if (!v) break;
        std::lock_guard<std::mutex> lk(mu_);
        owned_[deck].erase(v);                           // frees buffers
    }
}

void StemsEngine::postParam(int deck, int stem) {
    if (!queue_.tryPush(rt::SetStemParam{
            (uint8_t)deck, (uint8_t)stem,
            mix_[deck].param(stem).gain,
            mix_[deck].param(stem).pan,
            (uint8_t)(mix_[deck].param(stem).muted ? 1 : 0),
            (uint8_t)(mix_[deck].param(stem).solo ? 1 : 0) }))
        diag_.rejects++;
}
void StemsEngine::postMode(int deck) {
    queue_.tryPush(rt::SetStemMode{ (uint8_t)deck,
                                    (uint8_t)mgr_[deck].mode() });
}

// ------------------------------------------------------------------ jobs

void StemsEngine::onJobEvent(const StemScheduler::Notification& n) {
    switch (n.event) {
        case StemScheduler::Event::Started:  diag_.jobsStarted++; break;
        case StemScheduler::Event::Completed: {
            diag_.jobsDone++;
            std::lock_guard<std::mutex> lk(mu_);
            readyForHash_.push_back({ n.jobId, n.contentHash });
            break;
        }
        case StemScheduler::Event::Failed:
            diag_.jobsFailed++;
            break;
        case StemScheduler::Event::Cancelled:
            diag_.jobsCancelled++;
            {
                std::lock_guard<std::mutex> lk(mu_);
                produced_.erase(n.jobId);
            }
            break;
        default: break;
    }
}

StemSetMeta StemsEngine::metaFor(const DeckSlot& d, int quality) const {
    StemSetMeta m;
    m.trackId = d.trackId; m.contentHash = d.contentHash;
    auto model = provider_->createModel(provider_->configFor(
        quality, cfg_.gpuMode));
    const auto info = model->info();
    m.modelId = info.id; m.modelVersion = info.version;
    m.quality = quality; m.rate = d.deviceRate; m.channels = 2;
    m.key = StemCache::keyFor(m);
    return m;
}

StemError StemsEngine::runJob(
    const StemJob& job, StemCancel& cancel,
    const std::function<void(StemJobPhase, float)>& report) {

    const auto t0 = std::chrono::steady_clock::now();

    // ---- LOAD job: read a validated set from the file cache -------
    if (job.kind == 1) {
        StemSetMeta m;
        m.contentHash = job.contentHash; m.modelId = job.modelId;
        m.modelVersion = job.modelVersion; m.quality = job.quality;
        m.rate = job.targetRate; m.channels = 2; m.key = job.cacheKey;
        auto set = std::make_shared<StemSet>();
        if (!cache_.read(job.cacheKey, m, *set)) {
            diag_.cacheReadFailures++;
            store_.markState(job.contentHash, job.cacheKey,
                             StemStore::Orphaned);
            return StemError::CacheReadFailed;
        }
        if (cancel.load()) return StemError::Cancelled;
        {
            std::lock_guard<std::mutex> lk(mu_);
            produced_[job.id] = set;
        }
        diag_.lastLoadMs.store(std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count());
        return StemError::Ok;
    }

    // ---- SEPARATE job: decode → model → validate → cache ----------
    if (cancel.load()) return StemError::Cancelled;
    report(StemJobPhase::Analyzing, 0.0f);
    const TrackAudio* src = decoder_ ? decoder_(job.path) : nullptr;
    if (!src || src->numFrames <= 0) {
        diag_.decodeFailures++;
        // honest record (§41/§48): a failed row lets the library show
        // STEMS FAILED instead of retrying forever / showing nothing
        DeckSlot t; t.trackId = job.trackId;
        t.contentHash = job.contentHash; t.deviceRate = job.targetRate;
        store_.save(metaFor(t, job.quality), StemStore::Failed);
        return StemError::DecodeFailed;
    }

    DeckSlot tmp;
    tmp.trackId = job.trackId; tmp.contentHash = job.contentHash;
    tmp.deviceRate = src->deviceRate;
    const StemSetMeta meta = metaFor(tmp, job.quality);

    const auto cfg = provider_->configFor(job.quality, cfg_.gpuMode);
    auto model = provider_->createModel(cfg);
    std::string merr;
    if (!model || !model->load(cfg, &merr)) {
        diag_.modelFailures++;
        store_.save(meta, StemStore::Failed);
        return StemError::ModelLoadFailed;
    }

    StemError e = model->analyze(*src, cancel,
        [&](StemJobPhase ph, float p) { report(ph, p); });
    if (e != StemError::Ok) {
        store_.save(meta, StemStore::Failed);
        if (e == StemError::InvalidAudio) diag_.decodeFailures++;
        else diag_.modelFailures++;
        return e;
    }
    if (cancel.load()) return StemError::Cancelled;

    auto set = std::make_shared<StemSet>();
    report(StemJobPhase::Separating, 0.0f);
    e = model->separate(*src, *set, cancel,
        [&](StemJobPhase ph, float p) { report(ph, p); });
    model->unload();
    if (e != StemError::Ok) {
        store_.save(meta, StemStore::Failed);
        if (e != StemError::Cancelled) diag_.modelFailures++;
        return e;
    }

    report(StemJobPhase::Caching, 0.0f);
    StemSetMeta full = meta;
    full.frames = set->numFrames; full.mask = set->mask;
    full.createdAt = (int64_t)::time(nullptr);
    const std::string key = cache_.write(*set, full, &cancel);
    if (key.empty()) {
        diag_.cacheWriteFailures++;
        store_.save(full, StemStore::Failed);
        return cancel.load() ? StemError::Cancelled
                             : StemError::CacheWriteFailed;
    }
    store_.save(full, StemStore::Ready);
    diag_.cacheWrites++;
    diag_.bytesWritten += (uint64_t)set->bytes();
    {
        std::lock_guard<std::mutex> lk(mu_);
        produced_[job.id] = set;
    }
    report(StemJobPhase::Caching, 1.0f);
    diag_.lastSeparateMs.store(std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count());
    return StemError::Ok;
}

} // namespace onda::stems
