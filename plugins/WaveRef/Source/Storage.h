#pragma once

#include <juce_core/juce_core.h>

// Where WaveRef keeps what outlives a project: display settings, display presets and the remembered references.
// Tests point it at a scratch folder.
namespace waveref::storage
{
    inline juce::File& folderOverride()
    {
        static juce::File folder;
        return folder;
    }

    inline void setFolder (const juce::File& folder) { folderOverride() = folder; }

    inline juce::File folder()
    {
        if (folderOverride() != juce::File())
            return folderOverride();

        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("Maki plugins").getChildFile ("WaveRef");
    }
}
