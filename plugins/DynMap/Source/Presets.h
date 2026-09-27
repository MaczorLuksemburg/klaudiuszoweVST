#pragma once

#include "Engine.h"

namespace dynmap
{
    // Factory presets live in code; user presets are XML files (parameters + curves) in the
    // user's app-data folder.
    class PresetManager
    {
    public:
        PresetManager (juce::AudioProcessorValueTreeState&, CurveBank&);

        static juce::File getUserPresetFolder();

        void rescanUserPresets();

        int getNumFactoryPresets() const;
        juce::StringArray getPresetNames() const;   // factory first, then user presets

        void loadPreset (int index);
        bool saveUserPreset (const juce::String& name);

        juce::String getCurrentPresetName() const;
        int getCurrentPresetIndex() const;          // -1 when the state doesn't match a known preset name

        // Full state (parameters + curves), as saved in projects.
        juce::ValueTree copyState() const;
        void replaceState (const juce::ValueTree&);

        // Resets every parameter and curve to its default.
        void resetToDefaults();

    private:
        void setCurrentPresetName (const juce::String&);

        juce::AudioProcessorValueTreeState& apvts;
        CurveBank& curves;
        juce::Array<juce::File> userPresets;
    };
}
