#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace msc
{
    // Factory presets live in code; user presets are XML files in the user's app-data folder.
    class PresetManager
    {
    public:
        explicit PresetManager (juce::AudioProcessorValueTreeState&);

        static juce::File getUserPresetFolder();

        void rescanUserPresets();

        int getNumFactoryPresets() const;
        juce::StringArray getPresetNames() const;   // factory first, then user presets

        void loadPreset (int index);
        bool saveUserPreset (const juce::String& name);

        juce::String getCurrentPresetName() const;
        int getCurrentPresetIndex() const;          // -1 when the state doesn't match a known preset name

    private:
        void setCurrentPresetName (const juce::String&);

        juce::AudioProcessorValueTreeState& apvts;
        juce::Array<juce::File> userPresets;
    };
}
