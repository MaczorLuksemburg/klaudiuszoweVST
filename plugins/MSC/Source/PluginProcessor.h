#pragma once

#include "Analyzer.h"
#include "DSP.h"
#include "Parameters.h"
#include "Presets.h"

// MSC - multistage stereo control.
// Chain: input band split -> dynamic pan -> Haas -> chorus -> dimension -> image, then the
// untouched band is added back. Modules that are off are skipped entirely.
class MscProcessor : public juce::AudioProcessor
{
public:
    MscProcessor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.5; }

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;
    msc::PresetManager presets;
    msc::SpectrumAnalyzer inputAnalyzer, dynPanAnalyzer;
    msc::ModScope modScope;

    // Dimension LFO phase (radians) for the editor's tap display.
    std::atomic<float> dimensionPhase { 0.0f };

private:
    static constexpr int maxChunk = 256;

    struct Module
    {
        juce::SmoothedValue<float> fade;   // 0 = bypassed, 1 = fully in
        bool wasActive = false;
    };

    using Linear = juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>;
    using Multiplicative = juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative>;

    void updateTargets (bool snap);
    void processChunk (float* left, float* right, int numSamples);
    static bool beginModule (Module&, bool& justStarted);

    struct Params
    {
        std::atomic<float> *inOn, *inShape, *inCutoff, *inSlope, *inWetSrc, *inDrySrc;
        std::atomic<float> *dpOn, *dpAmount, *dpMax, *dpShape, *dpCutoff, *dpSlope, *dpClip, *dpSource, *dpComp, *dpThresh, *dpMakeup;
        std::atomic<float> *hsOn, *hsLeft, *hsRight, *hsInvL, *hsInvR;
        std::atomic<float> *chOn, *chMode, *chDepth, *chWidth, *chTone, *chMix, *chRate;
        std::atomic<float> *imOn, *imBalance, *imMid, *imSide;
        std::atomic<float> *dmOn, *dmSize, *dmMix;
    } params {};

    double sampleRate = 44100.0;

    Module input, dynPan, haas, chorus, dimension, image;

    // Input
    msc::dsp::MorphFilter<2> inputFilter;
    Multiplicative inCutoff;
    Linear inShape;
    int inSlope = 1, wetSource = 0, drySource = 0;

    // Dynamic pan
    msc::dsp::MorphFilter<1> dynPanFilter;
    Multiplicative dpCutoff;
    Linear dpShape, dpDepth, dpThreshold;
    int dpSlope = 0, clipMode = msc::clipSoft, modSource = msc::modSum;
    bool compNormalize = false;
    float compRatio = 1.0f, compMakeup = 0.5f, compEnvelope = 0.0f, compAttack = 0.0f, compRelease = 0.0f;

    // Haas
    msc::dsp::DelayLine haasLeft, haasRight;
    Linear hsDelayL, hsDelayR, hsPolarityL, hsPolarityR;

    // Chorus
    msc::dsp::DelayLine chorusDelay;
    Linear chCentre, chSwing, chWidth, chMix;
    Multiplicative chTone;
    float chRate = 0.5f, chPhase = 0.0f, chToneA = 0.0f, chToneB = 0.0f;

    // Dimension
    msc::dsp::DelayLine dimensionDelay;
    // Tap delays sit on whole samples (like the original: interpolating between samples would dull
    // the highs); a Size change glides each tap from its old to its new whole-sample delay.
    std::array<float, 4> dmFrom {}, dmTo {};
    Linear dmGlide, dmMix;   // glide 0..1 between dmFrom and dmTo, wet amount 0..1
    float dmPhase = 0.0f;    // LFO phase in cycles

    // Image
    Linear imMid, imSide, imGainL, imGainR;

    std::array<float, maxChunk> wetL {}, wetR {}, dryL {}, dryR {}, scratch {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MscProcessor)
};
