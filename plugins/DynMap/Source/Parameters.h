#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

// Parameter IDs are saved in DAW projects and presets: never rename or remove them.
//
// DynMap has "stages" that all work the same way: the input stage, up to 12 band stages and
// the master stage. Each stage parameter ID is "<stage prefix>_<name>", e.g. "in_att", "b3_att",
// "mst_att". Band slots 1-11 also own the crossover at their lower edge ("b3_on", "b3_freq").
namespace dynmap
{
    inline constexpr int maxBands = 12;
    inline constexpr int numStages = maxBands + 2;   // input, 12 bands, master
    inline constexpr int inputStage = 0;
    inline constexpr int masterStage = numStages - 1;

    inline constexpr int bandStage (int band) { return band + 1; }
    inline constexpr bool isBandStage (int stage) { return stage > inputStage && stage < masterStage; }

    juce::String stagePrefix (int stage);                        // "in", "b0".."b11", "mst"
    juce::String stageName (int stage);                          // "Input", "Band 1".."Band 12", "Master"
    juce::String stageParamId (int stage, const char* name);

    namespace ids
    {
        // Per-stage names (combine with stageParamId).
        inline constexpr const char* bypass    = "byp";
        inline constexpr const char* mode      = "mode";      // dynamics or waveshaper
        inline constexpr const char* pre       = "pre";
        inline constexpr const char* post      = "post";
        inline constexpr const char* mix       = "mix";
        inline constexpr const char* attack    = "att";
        inline constexpr const char* hold      = "hold";
        inline constexpr const char* release   = "rel";
        inline constexpr const char* relShape  = "relshape";
        inline constexpr const char* rms       = "rms";
        inline constexpr const char* lookahead = "la";
        inline constexpr const char* link      = "link";
        inline constexpr const char* stereo    = "stmode";
        inline constexpr const char* scFilter  = "schp";
        inline constexpr const char* scSource  = "scsrc";
        inline constexpr const char* trTime    = "trtime";
        inline constexpr const char* maxBoost  = "boost";
        inline constexpr const char* maxCut    = "cut";
        inline constexpr const char* smooth    = "smooth";
        inline constexpr const char* satType   = "sat";
        inline constexpr const char* drive     = "drive";
        inline constexpr const char* satPos    = "satpos";

        // Band stages only.
        inline constexpr const char* width = "width";
        inline constexpr const char* solo  = "solo";
        inline constexpr const char* mute  = "mute";

        // Band slots 1-11 only: the crossover at the band's lower edge.
        inline constexpr const char* bandOn = "on";
        inline constexpr const char* freq   = "freq";
        inline constexpr const char* slope  = "slope";

        // Global.
        inline constexpr const char* amount    = "g_amount";
        inline constexpr const char* time      = "g_time";
        inline constexpr const char* globalMix = "g_mix";
        inline constexpr const char* inGain    = "g_in";
        inline constexpr const char* outGain   = "out_gain";
        inline constexpr const char* clip      = "out_clip";
        inline constexpr const char* limiter   = "out_lim";
        inline constexpr const char* ceiling   = "out_ceil";
        inline constexpr const char* limRel    = "out_rel";
        inline constexpr const char* autoGain  = "g_autogain";
        inline constexpr const char* delta     = "g_delta";
        inline constexpr const char* scGain    = "g_scgain";    // level of the sidechain input into the detectors
        inline constexpr const char* scListen  = "g_sclisten";  // output plays the sidechain (to check routing)
        inline constexpr const char* quality   = "g_quality";
        inline constexpr const char* phase     = "g_phase";
    }

    enum Mode      { modeDynamics, modeWaveshaper };
    enum Stereo    { stereoLR, stereoMS };
    enum ScSource  { scInternal, scExternal, scExternalFull };   // bands: own band / same band of the sidechain / whole sidechain
    enum SatType   { satOff, satTape, satTube, satHard, satFold, satCrush };
    enum SatPos    { satAfter, satBefore };
    enum Slope     { slope6, slope12, slope24, slope48 };
    enum ClipMode  { clipOff, clipSoft, clipHard };
    enum Phase     { phaseMinimum, phaseLinear };

    inline constexpr float lookaheadMs[] { 0.0f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 20.0f };
    inline constexpr int numLookaheads = (int) std::size (lookaheadMs);
    inline constexpr float maxLookaheadMs = 20.0f;

    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // Default crossover frequency of band slot 1-11 (only used until the user places it).
    float defaultCrossoverHz (int band);

    juce::String formatHz (float hz);
    juce::String formatDb (float db, int decimals = 1);
}
