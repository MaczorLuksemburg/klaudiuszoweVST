#pragma once

#include "PluginProcessor.h"

// Placeholder until the DynMap UI is written.
class DynMapEditor : public juce::GenericAudioProcessorEditor
{
public:
    explicit DynMapEditor (DynMapProcessor& p) : juce::GenericAudioProcessorEditor (p) {}
};
