#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "core/adevice/AudioBackend.h"
#include "core/adevice/AudioHealthMonitor.h"
#include "core/adevice/DjAudioProfile.h"
#include "core/adevice/AudioRecoveryManager.h"
#include "core/adevice/AudioRouting.h"
#include "core/adevice/BufferManager.h"
#include "core/adevice/DeviceProfile.h"
#include "core/adevice/LatencyManager.h"
#include "core/adevice/SampleRateManager.h"

namespace onda::adevice {

/// Phase-13 device layer (domain side, JUCE-free). Orchestrates
/// enumeration, selection, safe switching, sample-rate/buffer policy,
/// routing, recovery, health and diagnostics over a `Backend` seam.
/// Control/UI thread only — the RT callback lives in AudioEngine.
class AudioDeviceEngine {
public:
    /// App-injected services. `mixerMap` publishes a validated
    /// ChannelMapping to the mixer; `testSig` routes the routing-tool
    /// mode; `cfg` persists profiles; `notify` surfaces events.
    struct Ports {
        std::function<void(const ChannelMapping&)> mixerMap;
        std::function<void(TestSignal)> testSig;
        std::function<void(const std::string&,  // title
                           const std::string&)> notify;
        std::function<std::string(const std::string&)> cfgGet;
        std::function<void(const std::string&,  // key
                           const std::string&)> cfgSet;
        std::function<void(std::function<void(bool)>)> micPermission;
    };
    Ports ports;

    explicit AudioDeviceEngine(Backend& b) : backend_(b) {}

    // ---- enumeration (§3-6) --------------------------------------
    const std::vector<DeviceInfo>& devices() { return lastEnum_; }
    std::vector<DeviceInfo> enumerate() {
        lastEnum_ = DeviceCatalog::normalize(backend_.enumerate());
        return lastEnum_;
    }
    const DeviceInfo* find(const std::string& id) const {
        return DeviceCatalog::byId(lastEnum_, id);
    }
    const DeviceInfo* flx4() const { return DeviceCatalog::flx4(lastEnum_); }

    DeviceCapabilities capabilities(const std::string& deviceId) {
        const DeviceInfo* d = find(deviceId);
        if (!d) return {};
        return backend_.probe(d->name);
    }

    // ---- settings -------------------------------------------------
    const DeviceSettings& settings() const { return settings_; }
    const ChannelMapping& mapping() const { return settings_.map; }

    /// Validate + apply a settings change with rollback (§9-10).
    /// Legacy string view — failures are also typed via lastResult().
    std::string select(const DeviceSettings& s) {
        return selectEx(s).message;
    }

    /// §32 — typed result: AudioError code + actionable message.
    /// The returned path never leaves the audio stack half-configured:
    /// any failure either keeps the previous device or rolls back to
    /// the system default (§9 "never start partially").
    DeviceResult selectEx(const DeviceSettings& s) {
        auto fail = [&](AudioError c, std::string m) {
            lastResult_ = DeviceResult::fail(c, m);
            return lastResult_;
        };
        enumerate();
        DesiredSetup ds;
        ds.outChans = 4;

        // resolve output device
        std::string outErr;
        const DeviceInfo* out = resolveOut(s.outputId, outErr);
        if (!outErr.empty())
            return fail(AudioError::DeviceNotFound, outErr);
        ds.outName = out ? out->name : "";
        if (!s.inputId.empty()) {
            const DeviceInfo* in = find(s.inputId);
            if (!in) return fail(AudioError::DeviceNotFound,
                                 "input device not found: " + s.inputId);
            if (in->inCh <= 0)
                return fail(AudioError::ChannelConfigurationFailed,
                            "device '" + in->name +
                            "' has no input channels");
            ds.inName = in->name;
            ds.inChans = 2;
        }

        // capabilities → rate/buffer policy (§6-8)
        DeviceCapabilities caps;
        if (out) caps = backend_.probe(out->name);
        double wantRate = s.sampleRate;
        if (caps.known && !caps.sampleRates.empty()) {
            ds.rate = SampleRateManager::choose(caps.sampleRates,
                                                wantRate);
        } else {
            ds.rate = wantRate;   // can't probe → ask, backend decides
        }
        if (caps.known && !caps.bufferSizes.empty()) {
            ds.buffer = s.bufferSize > 0
                ? BufferManager::nearest(caps.bufferSizes, s.bufferSize)
                : BufferManager::forPreset(s.pref, caps.bufferSizes);
        } else {
            ds.buffer = s.bufferSize > 0 ? s.bufferSize
                : (s.pref == LatencyPref::Low ? 128
                 : s.pref == LatencyPref::Balanced ? 256 : 512);
        }
        ds.exclusive = s.exclusive && caps.exclusiveCapable;

        // routing must fit the resolved device (§16-17, §46)
        ChannelMapping map = s.map;
        // unset or untouched-default mapping → auto-pick for the device
        if (map.masterL < 0 || map == ChannelMapping()) {
            const int outs = caps.known ? caps.outCh
                            : (out ? out->outCh : 0);
            const auto choice = routeChoice(out, caps, outs);
            if (!choice.ok)
                return fail(AudioError::ManualConfigRequired,
                            choice.reason);
            map = choice.map;
            if (caps.known) {          // shrink to detected outs
                if (map.boothR >= caps.outCh) map.boothL = map.boothR = -1;
                if (map.cueR   >= caps.outCh) map.cueL   = map.cueR   = -1;
            }
        }
        if (caps.known) {
            const auto e = AudioRouting::check(map, caps.outCh);
            if (!e.empty())
                return fail(AudioError::ChannelConfigurationFailed, e);
        }

        // safe switch: snapshot → apply → verify → rollback (§9)
        const BackendStatus before = backend_.status();
        const std::string err = backend_.apply(ds);
        if (!err.empty()) {
            recovery_.on(RecEvent::StreamFailed);
            const std::string rb = backend_.reopenDefault();
            if (rb.empty()) recovery_.on(RecEvent::RecoverOk);
            else            recovery_.on(RecEvent::RecoverFailed);
            notify("Audio device switch failed",
                   err + (rb.empty() ? " — restored default output"
                                     : " — restore failed: " + rb));
            return fail(AudioError::InitializationFailed, err);
        }
        const BackendStatus now = backend_.status();
        if (!now.running) {
            recovery_.on(RecEvent::StreamFailed);
            notify("Audio device", "stream did not start on '" +
                   ds.outName + "'");
            return fail(AudioError::StreamStartFailed,
                        "stream did not start");
        }
        recovery_.on(RecEvent::StreamOk);

        DeviceSettings applied = s;
        applied.outputId  = out ? out->id : "";
        applied.sampleRate = now.rate;
        applied.bufferSize = now.buffer;
        applied.map = map;
        settings_ = applied;
        if (ports.mixerMap) ports.mixerMap(map);
        if (out) rememberProfile(*out, applied);
        const std::string rateNote =
            SampleRateManager::explain(wantRate, now.rate);
        if (!rateNote.empty()) notify("Sample rate adjusted", rateNote);
        lastResult_ = DeviceResult::success();
        return lastResult_;
    }

    /// Convenience: change only the routing on the live device.
    std::string setChannelMapping(const ChannelMapping& m) {
        return setChannelMappingEx(m).message;
    }
    DeviceResult setChannelMappingEx(const ChannelMapping& m) {
        const BackendStatus st = backend_.status();
        const auto e = AudioRouting::check(m, st.outCh);
        if (!e.empty())
            return (lastResult_ = DeviceResult::fail(
                        AudioError::ChannelConfigurationFailed, e));
        settings_.map = m;
        if (ports.mixerMap) ports.mixerMap(m);
        persistCurrentProfile();
        return (lastResult_ = DeviceResult::success());
    }

    void setTestSignal(TestSignal m) {
        testSig_ = m;
        if (ports.testSig) ports.testSig(m);
    }
    TestSignal testSignal() const { return testSig_; }

    // ---- periodic maintenance (§18-22) ----------------------------
    /// Call ~10-20 Hz from the app timer. `cbWorstMs` etc. come from
    /// rt::AudioStats — all atomic reads, nothing RT here.
    void tick(int64_t nowMs, double cbWorstMs, double cbAvgMs,
              uint64_t xruns, float cpu, int64_t underrunStampMs) {
        const BackendStatus st = backend_.status();
        health_.feed(nowMs, st.rate, st.buffer, cbWorstMs, cbAvgMs,
                     xruns, cpu, underrunStampMs);
        if (st.running && health_.hasWarnings())
            notify("Audio health", health_.popWarning());
        if (nowMs - lastEnumMs_ < kEnumEveryMs) return;
        lastEnumMs_ = nowMs;
        enumerate();
        hotplugCheck(nowMs, st);
    }

    // ---- health/recovery views ------------------------------------
    HealthState recoveryState() const { return recovery_.state(); }
    const char* recoveryName() const { return recovery_.stateName(); }
    HealthReport health() const { return health_.report(); }
    std::string backendName() const { return backend_.backendName(); }

    /// §31 — user-facing device state derived from the backend plus
    /// the recovery FSM. Cheap, control-thread only.
    AudioDeviceStatus deviceStatus() const {
        switch (recovery_.state()) {
            case HealthState::Reconnecting: return AudioDeviceStatus::Recovering;
            case HealthState::DeviceLost:   return AudioDeviceStatus::Disconnected;
            case HealthState::Failed:       return AudioDeviceStatus::Error;
            default: break;
        }
        const BackendStatus st = backend_.status();
        if (st.running)                 return AudioDeviceStatus::Active;
        if (!st.outName.empty())        return AudioDeviceStatus::Available;
        if (!lastResult_.ok())          return AudioDeviceStatus::Error;
        return AudioDeviceStatus::Unavailable;
    }
    /// §32 — last operation's typed result (code + message).
    const DeviceResult& lastResult() const { return lastResult_; }

    /// §27-28 — detected DJ-controller audio profile for a device id
    /// (nullptr when the device is gone or no profile matches). The
    /// returned pointer is owned by `djProfileCache_` — do not store.
    DjAudioProfile djProfile(const std::string& deviceId) {
        const DeviceInfo* d = find(deviceId);
        if (!d) return {};
        DeviceCapabilities caps;
        if (d->outCh <= 0 && d->inCh <= 0) caps = backend_.probe(d->name);
        else { caps.known = true; caps.outCh = d->outCh; caps.inCh = d->inCh; }
        return DjAudioProfile::detect(*d, caps);
    }

    LatencyReport latency() const {
        const BackendStatus st = backend_.status();
        return LatencyManager::estimate(st.rate, st.buffer,
                                        st.inLatSmp, st.outLatSmp);
    }

    // ---- profiles (§28) -------------------------------------------
    DeviceProfile profileFor(const std::string& deviceId) const {
        DeviceProfile p; p.deviceId = deviceId;
        const auto it = profiles_.find(deviceId);
        if (it != profiles_.end()) return it->second;
        if (ports.cfgGet) {
            DeviceProfile f;
            if (DeviceProfile::fromJson(
                    ports.cfgGet(profileKey(deviceId)), f))
                return f;
        }
        return p;
    }
    void saveProfile(const DeviceProfile& p) {
        profiles_[p.deviceId] = p;
        if (ports.cfgSet)
            ports.cfgSet(profileKey(p.deviceId), p.toJson());
    }

    /// Settings suggested for a device: persisted profile wins, then
    /// FLX4 topology, then defaults from its capabilities.
    DeviceSettings suggested(const DeviceInfo& d) {
        DeviceSettings s;
        s.outputId = d.id;
        const DeviceProfile p = profileFor(d.id);
        if (p.sampleRate > 0) {
            s.sampleRate = p.sampleRate; s.bufferSize = p.bufferSize;
            s.pref = p.pref; s.map = p.map;
            return s;
        }
        s.map.masterL = -1;   // sentinel → select() auto-picks
        return s;
    }

    // ---- diagnostics (§29) ----------------------------------------
    std::string diagnostics() const {
        const BackendStatus st = backend_.status();
        const HealthReport h = health_.report();
        const LatencyReport l = latency();
        char b[1800];
        std::snprintf(b, sizeof b,
            "DEVICE: %s\nBACKEND: %s\nSAMPLE RATE: %.0f Hz\n"
            "BUFFER: %d smp (%.1f ms)\n"
            "INPUT CH: %d\nOUTPUT CH: %d\n"
            "ROUTING: master->%d/%d cue->%d/%d booth->%d/%d\n"
            "ESTIMATED LATENCY: %.1f ms round trip%s\n"
            "CPU: %.0f%% of budget\n"
            "UNDERRUNS: %llu\nXRUNS: %llu\n"
            "STREAM STATUS: %s\nDEVICE STATUS: %s\n"
            "LAST ERROR: %s\nRECOVERY: %s\n",
            st.outName.empty() ? "(none)" : st.outName.c_str(),
            st.backend.c_str(), st.rate, st.buffer, l.bufferMs,
            st.inCh, st.outCh,
            settings_.map.masterL, settings_.map.masterR,
            settings_.map.cueL, settings_.map.cueR,
            settings_.map.boothL, settings_.map.boothR,
            l.roundTripMs, l.measured ? " (measured)" : " (estimated)",
            h.cpu * 100.0f,
            (unsigned long long)h.underruns,
            (unsigned long long)h.xruns,
            st.running ? "running" : "stopped",
            audioDeviceStatusName(deviceStatus()),
            lastResult_.codeName(),
            recovery_.stateName());
        return b;
    }

    /// OS permission seam (§33): real impl asks JUCE; default = granted.
    void requestMicPermission(std::function<void(bool)> cb) {
        if (ports.micPermission) ports.micPermission(std::move(cb));
        else cb(true);
    }

private:
    using RecEvent = AudioRecoveryManager::Event;

    const DeviceInfo* resolveOut(const std::string& id,
                                 std::string& err) {
        if (id.empty()) {                 // system default
            const DeviceInfo* d = DeviceCatalog::defaultOut(lastEnum_);
            return d;                     // may be null → backend default
        }
        const DeviceInfo* d = find(id);
        if (!d) err = "device not found (disconnected?): " + id;
        else if (d->outCh <= 0)
            err = "device '" + d->name + "' has no output channels";
        return d;
    }

    void hotplugCheck(int64_t nowMs, const BackendStatus& st) {
        const std::string cur =
            DeviceInfo::makeId(st.backend, st.outName);
        const DeviceInfo* live = find(cur);
        const bool streamDead = !st.outName.empty() && !st.running;
        if ((st.outName.empty() && st.running) || st.outName.empty())
            return;                       // nothing to lose
        if (live == nullptr || streamDead) {
            if (recovery_.state() == HealthState::Healthy
                || recovery_.state() == HealthState::Recovered) {
                recovery_.on(RecEvent::DeviceLost);
                lastResult_ = DeviceResult::fail(
                    AudioError::DeviceDisconnected,
                    "'" + st.outName + "' disconnected");
                notify("Audio device lost",
                       "'" + st.outName + "' disconnected — attempting "
                       "recovery on the default output");
            }
        }
        if (recovery_.wantsRecovery()
            && nowMs - lastRetryMs_ >= kRetryMs) {
            lastRetryMs_ = nowMs;
            const std::string err = settings_.outputId.empty()
                ? backend_.reopenDefault() : tryReopenPreferred();
            if (err.empty()) {
                recovery_.on(RecEvent::RecoverOk);
                recovery_.on(RecEvent::StreamOk);
                notify("Audio recovered",
                       "output restored on '" +
                       backend_.status().outName + "'");
            } else {
                recovery_.on(RecEvent::RecoverFailed);
            }
        }
    }

    std::string tryReopenPreferred() {
        const DeviceInfo* pref = find(settings_.outputId);
        if (!pref) return backend_.reopenDefault();
        DesiredSetup ds;
        ds.outName = pref->name; ds.outChans = 4;
        ds.rate = settings_.sampleRate;
        ds.buffer = settings_.bufferSize;
        const std::string e = backend_.apply(ds);
        if (e.empty() && ports.mixerMap) ports.mixerMap(settings_.map);
        return e;
    }

    void rememberProfile(const DeviceInfo& d, const DeviceSettings& s) {
        DeviceProfile p = profileFor(d.id);
        p.deviceId = d.id;
        p.sampleRate = s.sampleRate; p.bufferSize = s.bufferSize;
        p.pref = s.pref; p.map = s.map;
        saveProfile(p);
    }
    void persistCurrentProfile() {
        if (!settings_.outputId.empty()) {
            DeviceProfile p = profileFor(settings_.outputId);
            p.deviceId = settings_.outputId;
            p.sampleRate = settings_.sampleRate;
            p.bufferSize = settings_.bufferSize;
            p.pref = settings_.pref; p.map = settings_.map;
            saveProfile(p);
        }
    }
    static std::string profileKey(const std::string& id) {
        std::string k = "audio.profile.";
        for (char c : id) k += (c == '|' ? '/' : c);
        return k;
    }
    void notify(const std::string& t, const std::string& d) {
        if (ports.notify) ports.notify(t, d);
    }

    /// §46 auto-route: a detected DJ profile wins, then the generic
    /// capability-derived default. Returns ok=false (never a guess)
    /// when the device's outs cannot even host a master pair.
    AudioRouting::RouteChoice routeChoice(const DeviceInfo* out,
                                          const DeviceCapabilities& caps,
                                          int outs) {
        if (out != nullptr) {
            const auto prof = DjAudioProfile::detect(*out, caps);
            if (prof.role == DjAudioProfile::Role::DjController)
                return { prof.suggested, true, {} };
        }
        return AudioRouting::autoRoute(outs > 0 ? outs : 0);
    }

    Backend& backend_;
    AudioRecoveryManager recovery_;
    AudioHealthMonitor   health_;
    DeviceSettings       settings_;
    DeviceResult         lastResult_;
    TestSignal           testSig_ = TestSignal::Off;
    std::vector<DeviceInfo> lastEnum_;
    std::map<std::string, DeviceProfile> profiles_;
    int64_t lastEnumMs_ = -10000;
    int64_t lastRetryMs_ = -10000;
    static constexpr int64_t kEnumEveryMs = 2000;
    static constexpr int64_t kRetryMs = 3000;
};

} // namespace onda::adevice
