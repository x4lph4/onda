#include "Log.h"

#include <mutex>

namespace onda::log {

void init() {
    static std::once_flag once;
    std::call_once(once, [] {
        juce::Logger::setCurrentLogger(
            juce::FileLogger::createDefaultAppLogger(
                "Onda", "onda.log", "=== ONDA log ===", 4 * 1024 * 1024));
    });
}

void info (const juce::String& m) { juce::Logger::writeToLog("[INF] " + m); }
void warn (const juce::String& m) { juce::Logger::writeToLog("[WRN] " + m); }
void error(const juce::String& m) { juce::Logger::writeToLog("[ERR] " + m); }

} // namespace onda::log
