#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    std::unique_ptr<juce::AudioParameterFloat> makePercentParam (const juce::String& id, const juce::String& name)
    {
        auto attributes = juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction ([] (float value, int) { return juce::String (juce::roundToInt (value)) + " %"; })
            .withValueFromStringFunction ([] (const juce::String& text) { return text.retainCharacters ("0123456789.-").getFloatValue(); });

        return std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { id, 1 }, name,
                                                            juce::NormalisableRange<float> (0.0f, 200.0f, 0.1f),
                                                            100.0f, attributes);
    }
}

StereoScaleProcessor::StereoScaleProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "StereoScale", createParameterLayout())
{
    leftParam  = apvts.getRawParameterValue ("left");
    rightParam = apvts.getRawParameterValue ("right");
    midParam   = apvts.getRawParameterValue ("mid");
    sideParam  = apvts.getRawParameterValue ("side");
}

juce::AudioProcessorValueTreeState::ParameterLayout StereoScaleProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add (makePercentParam ("left",  "Left"));
    layout.add (makePercentParam ("right", "Right"));
    layout.add (makePercentParam ("mid",   "Mid"));
    layout.add (makePercentParam ("side",  "Side"));
    return layout;
}

void StereoScaleProcessor::prepareToPlay (double sampleRate, int)
{
    constexpr double rampSeconds = 0.02;

    for (auto* gain : { &leftGain, &rightGain, &midGain, &sideGain })
        gain->reset (sampleRate, rampSeconds);

    leftGain .setCurrentAndTargetValue (leftParam ->load() * 0.01f);
    rightGain.setCurrentAndTargetValue (rightParam->load() * 0.01f);
    midGain  .setCurrentAndTargetValue (midParam  ->load() * 0.01f);
    sideGain .setCurrentAndTargetValue (sideParam ->load() * 0.01f);
}

bool StereoScaleProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainInputChannelSet()  == juce::AudioChannelSet::stereo()
        && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void StereoScaleProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    leftGain .setTargetValue (leftParam ->load() * 0.01f);
    rightGain.setTargetValue (rightParam->load() * 0.01f);
    midGain  .setTargetValue (midParam  ->load() * 0.01f);
    sideGain .setTargetValue (sideParam ->load() * 0.01f);

    if (buffer.getNumChannels() < 2)
        return;

    auto* left  = buffer.getWritePointer (0);
    auto* right = buffer.getWritePointer (1);

    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        const float mid  = 0.5f * (left[i] + right[i]) * midGain.getNextValue();
        const float side = 0.5f * (left[i] - right[i]) * sideGain.getNextValue();

        left[i]  = (mid + side) * leftGain.getNextValue();
        right[i] = (mid - side) * rightGain.getNextValue();
    }
}

juce::AudioProcessorEditor* StereoScaleProcessor::createEditor()
{
    return new StereoScaleEditor (*this);
}

void StereoScaleProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void StereoScaleProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new StereoScaleProcessor();
}
