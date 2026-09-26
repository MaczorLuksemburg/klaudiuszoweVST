#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

// Parameter IDs are saved in DAW projects and presets: never rename them.
namespace msc
{
    namespace ids
    {
        // Module 0: input band split and source selection.
        inline constexpr const char* inOn     = "in_on";
        inline constexpr const char* inShape  = "in_shape";
        inline constexpr const char* inCutoff = "in_cutoff";
        inline constexpr const char* inSlope  = "in_slope";
        inline constexpr const char* inWetSrc = "in_wet_src";
        inline constexpr const char* inDrySrc = "in_dry_src";

        // Module 1: dynamic pan (the signal pans itself).
        inline constexpr const char* dpOn     = "dp_on";
        inline constexpr const char* dpAmount = "dp_amount";
        inline constexpr const char* dpMax    = "dp_max";
        inline constexpr const char* dpShape  = "dp_shape";
        inline constexpr const char* dpCutoff = "dp_cutoff";
        inline constexpr const char* dpSlope  = "dp_slope";
        inline constexpr const char* dpClip   = "dp_clip";    // clips the modulation (pan position), not the audio
        inline constexpr const char* dpSource = "dp_src";
        inline constexpr const char* dpComp   = "dp_comp";
        inline constexpr const char* dpThresh = "dp_thresh";
        inline constexpr const char* dpMakeup = "dp_makeup";

        // Module 2: Haas delay and polarity.
        inline constexpr const char* hsOn    = "hs_on";
        inline constexpr const char* hsLeft  = "hs_left";
        inline constexpr const char* hsRight = "hs_right";
        inline constexpr const char* hsInvL  = "hs_inv_l";
        inline constexpr const char* hsInvR  = "hs_inv_r";

        // Module 3: Juno-style chorus.
        inline constexpr const char* chOn    = "ch_on";
        inline constexpr const char* chMode  = "ch_mode";
        inline constexpr const char* chDepth = "ch_depth";
        inline constexpr const char* chWidth = "ch_width";
        inline constexpr const char* chTone  = "ch_tone";
        inline constexpr const char* chMix   = "ch_mix";

        // Module 4: balance and mid/side image.
        inline constexpr const char* imOn      = "im_on";
        inline constexpr const char* imBalance = "im_balance";
        inline constexpr const char* imMid     = "im_mid";
        inline constexpr const char* imSide    = "im_side";
    }

    enum Source   { sourceStereo, sourceMono, sourceLeft, sourceRight };
    enum ClipMode  { clipOff, clipHard, clipSoft, clipExtreme };
    enum ModSource { modSum, modLeft, modRight };

    // Compressor ratios for the dynamic-pan modulator, matching the "Comp" choices.
    inline constexpr float modCompRatios[] { 1.0f, 2.0f, 4.0f, 8.0f };

    // Share of the full auto makeup (the gain that keeps 0 dBFS at 0 dBFS), matching the "Makeup" choices.
    inline constexpr float modMakeupAmounts[] { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };

    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    juce::String formatHz (float hz);
}
