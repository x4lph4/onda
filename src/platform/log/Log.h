#pragma once

#include <juce_core/juce_core.h>

namespace onda::log {

/// Installs a rotating FileLogger under <app-data>/Onda/logs.
/// Safe to call more than once. Never call log::* from the RT thread.
void init();

void info (const juce::String& message);
void warn (const juce::String& message);
void error(const juce::String& message);

} // namespace onda::log
