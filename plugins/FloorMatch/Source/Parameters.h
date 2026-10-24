#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

// Parameter IDs are saved in DAW projects: never rename or remove them.
namespace floormatch
{
    namespace ids
    {
        inline constexpr const char* target       = "target";      // noise floor target, dB (A-weighted)
        inline constexpr const char* match        = "match";       // colour match to the learned profile, %
        inline constexpr const char* maxReduction = "max_red";     // dB
        inline constexpr const char* lookahead    = "lookahead";   // Off / 0.5 / 1 / 2 s
        inline constexpr const char* fill         = "fill";        // room tone fill for takes below the target
        inline constexpr const char* listen       = "listen";      // Output / Removed
    }

    enum Listen { listenOutput, listenRemoved };

    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    juce::String formatDb (float db, int decimals = 1);
}
