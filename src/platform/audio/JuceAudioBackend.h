#pragma once

#include <memory>

#include <juce_audio_devices/juce_audio_devices.h>

#include "core/adevice/AudioBackend.h"

namespace onda::audio {

/// Phase-13 platform adapter: exposes juce::AudioDeviceManager through
/// the JUCE-free adevice::Backend seam. All calls are control/UI thread
/// only — none of this runs on the audio callback.
///
/// JUCE notes:
///  - Channel counts are NOT enumerable without opening a device, so
///    enumerate() reports inCh/outCh as presence flags (>=1 = has
///    channels of that direction); probe() fills real counts.
///  - probe() briefly opens the endpoint on the control thread — it may
///    legitimately fail on a busy/exclusive device (caps.known=false).
class JuceAudioBackend final : public adevice::Backend {
public:
    explicit JuceAudioBackend(juce::AudioDeviceManager& m) : mgr_(m) {}

    std::string backendName() const override {
        const auto t = mgr_.getCurrentAudioDeviceType(); // String
        return t.isEmpty() ? "JUCE" : t.toStdString();
    }

    std::vector<adevice::RawDevice> enumerate() override {
        std::vector<adevice::RawDevice> out;
        for (auto* t : mgr_.getAvailableDeviceTypes()) {
            t->scanForDevices();
            const auto inNames  = t->getDeviceNames(true);
            const auto outNames = t->getDeviceNames(false);
            const int defIn  = t->getDefaultDeviceIndex(true);
            const int defOut = t->getDefaultDeviceIndex(false);
            for (int i = 0; i < outNames.size(); ++i) {
                adevice::RawDevice d;
                d.backend = t->getTypeName().toStdString();
                d.name    = outNames[i].toStdString();
                d.outCh   = 1;                    // presence, not count
                d.inCh    = inNames.contains(outNames[i]) ? 1 : 0;
                d.isDefaultOut = (i == defOut);
                d.isDefaultIn  = inNames.contains(outNames[i])
                                 && inNames.indexOf(outNames[i]) == defIn;
                out.push_back(std::move(d));
            }
            for (int i = 0; i < inNames.size(); ++i) {
                if (outNames.contains(inNames[i])) continue; // merged above
                adevice::RawDevice d;
                d.backend = t->getTypeName().toStdString();
                d.name    = inNames[i].toStdString();
                d.inCh    = 1;
                d.isDefaultIn = (i == defIn);
                out.push_back(std::move(d));
            }
        }
        return out;
    }

    adevice::BackendStatus status() override {
        adevice::BackendStatus s;
        s.backend = backendName();
        if (auto* d = mgr_.getCurrentAudioDevice()) {
            s.outName  = d->getName().toStdString();
            s.rate     = d->getCurrentSampleRate();
            s.buffer   = d->getCurrentBufferSizeSamples();
            s.outCh    = d->getActiveOutputChannels()
                             .countNumberOfSetBits();
            s.inCh     = d->getActiveInputChannels()
                             .countNumberOfSetBits();
            s.inLatSmp  = d->getInputLatencyInSamples();
            s.outLatSmp = d->getOutputLatencyInSamples();
            s.running   = d->isOpen();
            juce::AudioDeviceManager::AudioDeviceSetup cur;
            mgr_.getAudioDeviceSetup(cur);
            s.inName = cur.inputDeviceName.toStdString();
        }
        return s;
    }

    adevice::DeviceCapabilities probe(const std::string& name) override {
        adevice::DeviceCapabilities c;
        const juce::String jname(name);
        // already live → read the open device, no reopen
        if (auto* cur = mgr_.getCurrentAudioDevice();
            cur != nullptr && cur->getName() == jname) {
            c.known  = true;
            c.outCh  = cur->getOutputChannelNames().size();
            c.inCh   = cur->getInputChannelNames().size();
            for (auto r : cur->getAvailableSampleRates())
                c.sampleRates.push_back(r);
            for (auto b : cur->getAvailableBufferSizes())
                c.bufferSizes.push_back(b);
            finish(c);
            return c;
        }
        for (auto* t : mgr_.getAvailableDeviceTypes()) {
            t->scanForDevices();
            if (!t->getDeviceNames(false).contains(jname)) continue;
            std::unique_ptr<juce::AudioIODevice> d(
                t->createDevice(jname, {}));
            if (!d) return c;
            const juce::String err = d->open(
                juce::BigInteger(), juce::BigInteger().setBit(0),
                44100.0, 512);
            if (!err.isEmpty()) return c;   // busy/denied → unknown
            c.known = true;
            c.outCh = d->getOutputChannelNames().size();
            c.inCh  = d->getInputChannelNames().size();
            for (auto r : d->getAvailableSampleRates())
                c.sampleRates.push_back(r);
            for (auto b : d->getAvailableBufferSizes())
                c.bufferSizes.push_back(b);
            d->close();
            finish(c);
            return c;
        }
        return c;
    }

    std::string apply(const adevice::DesiredSetup& ds) override {
        juce::AudioDeviceManager::AudioDeviceSetup prev;
        mgr_.getAudioDeviceSetup(prev);
        juce::AudioDeviceManager::AudioDeviceSetup s = prev;
        s.outputDeviceName = juce::String(ds.outName);
        s.inputDeviceName  = juce::String(ds.inName);
        s.useDefaultInputChannels  = (ds.inChans <= 0);
        s.useDefaultOutputChannels = false;
        s.inputChannels  = ds.inChans > 0
            ? juce::BigInteger().setRange(0, ds.inChans, true)
            : juce::BigInteger();
        s.outputChannels = ds.outChans > 0
            ? juce::BigInteger().setRange(0, ds.outChans, true)
            : juce::BigInteger();
        if (ds.rate > 0.0)   s.sampleRate = ds.rate;
        if (ds.buffer > 0)   s.bufferSize = ds.buffer;
        const juce::String err =
            mgr_.setAudioDeviceSetup(s, /*treatAsChosenDevice*/false);
        if (!err.isEmpty()) {
            // contract: leave the previous working device running
            mgr_.setAudioDeviceSetup(prev, false);
            return err.toStdString();
        }
        return {};
    }

    std::string reopenDefault() override {
        juce::AudioDeviceManager::AudioDeviceSetup s;
        mgr_.getAudioDeviceSetup(s);
        s.outputDeviceName.clear();   // empty → system default
        s.useDefaultOutputChannels = true;
        const juce::String err = mgr_.setAudioDeviceSetup(s, false);
        return err.toStdString();
    }

private:
    static void finish(adevice::DeviceCapabilities& c) {
        c.duplex = c.inCh > 0 && c.outCh > 0;
        for (int b : c.bufferSizes)
            if (b > 0 && b < 128) { c.lowLatency = true; break; }
        // WASAPI-exclusive / ASIO isn't surfaced per-device by JUCE —
        // exclusiveCapable stays false; apply() ignores exclusive then.
    }

    juce::AudioDeviceManager& mgr_;
};

} // namespace onda::audio
