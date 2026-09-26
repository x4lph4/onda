#pragma once

#include <cstdint>

#include "core/adevice/AudioDeviceTypes.h"

namespace onda::adevice {

/// Recovery state machine (§19). The engine drives events; this tracks
/// state and decides what recovery is allowed. All transitions are
/// explicit — no impossible states.
class AudioRecoveryManager {
public:
    enum class Event {
        StreamOk,           // device opened & running
        StreamFailed,       // open/apply returned an error
        DeviceLost,         // current device vanished from enumeration
        DeviceBack,         // preferred device reappeared
        RecoverOk,          // fallback/reopen succeeded
        RecoverFailed,
        StreamStopped       // callback stopped while expected running
    };

    void on(Event e) {
        using E = Event;
        switch (s_) {
        case HealthState::Healthy:
            if (e == E::StreamFailed || e == E::StreamStopped)
                s_ = HealthState::Warning;
            else if (e == E::DeviceLost) s_ = HealthState::DeviceLost;
            break;
        case HealthState::Warning:
            if (e == E::StreamOk)        s_ = HealthState::Healthy;
            else if (e == E::DeviceLost) s_ = HealthState::DeviceLost;
            else if (e == E::StreamFailed) s_ = HealthState::Failed;
            break;
        case HealthState::DeviceLost:
            if (e == E::DeviceBack || e == E::RecoverOk)
                s_ = HealthState::Reconnecting;
            else if (e == E::RecoverFailed) s_ = HealthState::Failed;
            break;
        case HealthState::Reconnecting:
            if (e == E::StreamOk || e == E::RecoverOk)
                s_ = HealthState::Recovered;
            else if (e == E::RecoverFailed) s_ = HealthState::Failed;
            break;
        case HealthState::Recovered:
            if (e == E::StreamOk)   s_ = HealthState::Healthy;
            if (e == E::DeviceLost) s_ = HealthState::DeviceLost;
            break;
        case HealthState::Failed:
            // only an explicit successful reopen leaves Failed
            if (e == E::StreamOk || e == E::RecoverOk)
                s_ = HealthState::Reconnecting;
            break;
        }
    }

    HealthState state() const { return s_; }
    bool stable() const {
        return s_ == HealthState::Healthy || s_ == HealthState::Recovered;
    }
    /// recovery is worth attempting from DeviceLost/Reconnecting/Failed
    bool wantsRecovery() const {
        return s_ == HealthState::DeviceLost
            || s_ == HealthState::Reconnecting
            || s_ == HealthState::Failed;
    }
    static const char* name(HealthState s) {
        switch (s) {
            case HealthState::Healthy:      return "healthy";
            case HealthState::Warning:      return "warning";
            case HealthState::DeviceLost:   return "device lost";
            case HealthState::Reconnecting: return "reconnecting";
            case HealthState::Recovered:    return "recovered";
            case HealthState::Failed:       return "failed";
        }
        return "?";
    }
    const char* stateName() const { return name(s_); }

private:
    HealthState s_ = HealthState::Healthy;
};

} // namespace onda::adevice
