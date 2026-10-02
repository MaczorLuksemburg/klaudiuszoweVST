#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace dynmap;

DynMapProcessor::DynMapProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",     juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output",    juce::AudioChannelSet::stereo(), true)
                          .withInput  ("Sidechain", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "DynMap", createParameterLayout()),
      engine (apvts),
      presets (apvts, engine.curves)
{
}

bool DynMapProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();

    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    if (in != juce::AudioChannelSet::stereo() && in != juce::AudioChannelSet::mono())
        return false;

    if (layouts.inputBuses.size() > 1)
    {
        const auto sc = layouts.getChannelSet (true, 1);
        if (! sc.isDisabled() && sc != juce::AudioChannelSet::stereo() && sc != juce::AudioChannelSet::mono())
            return false;
    }

    return true;
}

void DynMapProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate, samplesPerBlock);
    sidechain.setSize (2, juce::jmax (32, samplesPerBlock));
    setLatencySamples (engine.getLatency());
}

void DynMapProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();

    if (buffer.getNumChannels() < 2 || numSamples == 0)
        return;

    // Copy the sidechain first: with a mono main input its channels overlap the output's right channel.
    const float* scLeft = nullptr;
    const float* scRight = nullptr;

    if (getBusCount (true) > 1 && getBus (true, 1)->isEnabled()
        && buffer.getNumChannels() > getChannelIndexInProcessBlockBuffer (true, 1, 0))
    {
        const auto scBuffer = getBusBuffer (buffer, true, 1);

        if (scBuffer.getNumChannels() > 0 && numSamples <= sidechain.getNumSamples())
        {
            sidechain.copyFrom (0, 0, scBuffer, 0, 0, numSamples);
            sidechain.copyFrom (1, 0, scBuffer, scBuffer.getNumChannels() > 1 ? 1 : 0, 0, numSamples);
            scLeft = sidechain.getReadPointer (0);
            scRight = sidechain.getReadPointer (1);
        }
    }

    if (getMainBusNumInputChannels() == 1)
        buffer.copyFrom (1, 0, buffer, 0, 0, numSamples);

    engine.process (buffer.getWritePointer (0), buffer.getWritePointer (1), scLeft, scRight, numSamples);

    if (engine.getLatency() != getLatencySamples())
        setLatencySamples (engine.getLatency());
}

juce::AudioProcessorEditor* DynMapProcessor::createEditor()
{
    return new DynMapEditor (*this);
}

int DynMapProcessor::getNumPrograms()
{
    return presets.getNumFactoryPresets();
}

int DynMapProcessor::getCurrentProgram()
{
    const int index = presets.getCurrentPresetIndex();
    return juce::isPositiveAndBelow (index, presets.getNumFactoryPresets()) ? index : 0;
}

void DynMapProcessor::setCurrentProgram (int index)
{
    // Some hosts call this with the current program after restoring state; don't reset then.
    if (index != getCurrentProgram())
        presets.loadPreset (index);
}

const juce::String DynMapProcessor::getProgramName (int index)
{
    return presets.getPresetNames()[index];
}

void DynMapProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = presets.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void DynMapProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            presets.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DynMapProcessor();
}
