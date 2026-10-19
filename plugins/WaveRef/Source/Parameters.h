#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

// Parameter IDs are saved in DAW projects and presets: never rename them.
namespace waveref
{
    namespace ids
    {
        inline constexpr const char* ab          = "ab";            // false = Mix (the session), true = Ref (the loaded file)
        inline constexpr const char* slot        = "slot";          // which of the 8 reference slots plays
        inline constexpr const char* matchOn     = "lm_on";         // auto loudness match of the reference to the mix
        inline constexpr const char* matchMode   = "lm_mode";       // measure the mix over 3 s or 30 s
        inline constexpr const char* trim        = "ref_trim";      // manual reference gain on top of the match
        inline constexpr const char* mono        = "mono";          // mono fold-down of the monitored signal
        inline constexpr const char* filterOn    = "lf_on";         // listening band
        inline constexpr const char* filterFreq  = "lf_freq";
        inline constexpr const char* filterWidth = "lf_width";      // octaves
        inline constexpr const char* restartSwitch = "restart_sw";  // jump to the loop start whenever Ref is switched in
        inline constexpr const char* restartPlay   = "restart_play"; // jump to the loop start when the host starts playing
        inline constexpr const char* blind       = "blind";         // blind test: A/B becomes X/Y in random order
        inline constexpr const char* refPlay     = "ref_play";      // the reference's own transport (free mode)
        inline constexpr const char* refMode     = "ref_mode";      // free / follows the host's time / follows its beats
        inline constexpr const char* syncOffset  = "sync_offset";   // seconds: reference time = host time + offset
        inline constexpr const char* beatOffset  = "beat_offset";   // beats, in beats mode
        inline constexpr const char* nudge       = "nudge_ms";      // fine offset in beats mode
        inline constexpr const char* keyLock     = "key_lock";      // keep the pitch when the tempo is matched
        inline constexpr const char* side        = "side";          // listen to the side signal only
        inline constexpr const char* delta       = "delta";         // listen to mix minus reference
    }

    inline constexpr int numSlots = 8;

    enum MatchMode { matchShort, matchLong };
    enum RefMode { modeFree, modeHostTime, modeBeats };

    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
}
