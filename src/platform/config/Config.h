#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

namespace onda {

/// Persistent JSON configuration stored at <app-data>/Onda/config.json.
/// Versioned keys; evolve through explicit migration when schema changes.
class Config {
public:
    Config();

    juce::File file() const;
    void load();
    void save();

    /// AudioDeviceManager state XML (device selection, rate, buffer size).
    juce::String audioDeviceStateXml() const;
    void setAudioDeviceStateXml(const juce::String& xml);

    juce::var get(const juce::Identifier& key, const juce::var& defaultValue = {}) const;
    void set(const juce::Identifier& key, const juce::var& value);

private:
    juce::var root_;
    juce::CriticalSection lock_;
};

} // namespace onda
