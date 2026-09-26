#include "Config.h"
#include "../log/Log.h"

namespace onda {

static const juce::Identifier kAudioDeviceState { "audioDeviceState" };

Config::Config() : root_(new juce::DynamicObject()) {}

juce::File Config::file() const {
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("Onda")
        .getChildFile("config.json");
}

void Config::load() {
    const auto f = file();
    if (f.existsAsFile()) {
        auto parsed = juce::JSON::parse(f);
        if (parsed.isObject()) {
            const juce::ScopedLock sl(lock_);
            root_ = parsed;
            log::info("Config loaded from " + f.getFullPathName());
        } else {
            log::warn("Config file corrupt, starting fresh: " + f.getFullPathName());
        }
    }
}

void Config::save() {
    const auto f = file();
    f.getParentDirectory().createDirectory();
    juce::var snapshot;
    {
        const juce::ScopedLock sl(lock_);
        snapshot = root_;
    }
    if (!f.replaceWithText(juce::JSON::toString(snapshot)))
        log::error("Failed to write config: " + f.getFullPathName());
}

juce::var Config::get(const juce::Identifier& key, const juce::var& def) const {
    const juce::ScopedLock sl(lock_);
    if (auto* obj = root_.getDynamicObject())
        return obj->getProperty(key);
    return def;
}

void Config::set(const juce::Identifier& key, const juce::var& value) {
    const juce::ScopedLock sl(lock_);
    if (auto* obj = root_.getDynamicObject())
        obj->setProperty(key, value);
}

juce::String Config::audioDeviceStateXml() const {
    return get(kAudioDeviceState).toString();
}

void Config::setAudioDeviceStateXml(const juce::String& xml) {
    set(kAudioDeviceState, xml);
}

} // namespace onda
