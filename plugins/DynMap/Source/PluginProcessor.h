#pragma once

#include "Engine.h"
#include "Presets.h"

// DynMap - multiband dynamic mapping. Drawn input->output level curves (Maximus style) plus
// transient curves on an input stage, up to 12 bands and a master stage, followed by a
// clipper and a true-peak limiter.
class DynMapProcessor : public juce::AudioProcessor
{
public:
    DynMapProcessor();

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
    dynmap::Engine engine;
    dynmap::PresetManager presets;

private:
    juce::AudioBuffer<float> sidechain;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DynMapProcessor)
};
