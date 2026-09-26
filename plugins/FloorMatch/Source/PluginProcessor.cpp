#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace floormatch;

namespace
{
    const juce::Identifier profileId { "profile" };

    int toIndex (const std::atomic<float>* p) { return (int) std::lround (p->load()); }
}

FloorMatchProcessor::FloorMatchProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "FloorMatch", createParameterLayout())
{
    target       = apvts.getRawParameterValue (ids::target);
    match        = apvts.getRawParameterValue (ids::match);
    maxReduction = apvts.getRawParameterValue (ids::maxReduction);
    lookahead    = apvts.getRawParameterValue (ids::lookahead);
    fill         = apvts.getRawParameterValue (ids::fill);
    listen       = apvts.getRawParameterValue (ids::listen);

    apvts.addParameterListener (ids::lookahead, this);

    // A first guess for hosts that ask for the latency before preparing.
    engine.prepare (48000.0, 1, toIndex (lookahead));
    setLatencySamples (engine.getLatencySamples());
}

FloorMatchProcessor::~FloorMatchProcessor()
{
    apvts.removeParameterListener (ids::lookahead, this);
    cancelPendingUpdate();
}

bool FloorMatchProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    return in == layouts.getMainOutputChannelSet()
        && (in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo());
}

void FloorMatchProcessor::prepareToPlay (double sampleRate, int)
{
    currentSampleRate = sampleRate;
    currentChannels = std::max (1, getTotalNumInputChannels());
    prepareEngine (toIndex (lookahead));
}

void FloorMatchProcessor::prepareEngine (int lookaheadIndex)
{
    engine.prepare (currentSampleRate, currentChannels, lookaheadIndex);
    appliedProfileVersion = -1;   // hand the profile to the fresh engine again
    publishedSnapshot = -1;
    setLatencySamples (engine.getLatencySamples());
}

// Lookahead changes the latency and reallocates the engine: done on the message thread with
// the audio callback locked out.
void FloorMatchProcessor::parameterChanged (const juce::String&, float)
{
    triggerAsyncUpdate();
}

void FloorMatchProcessor::handleAsyncUpdate()
{
    if (currentSampleRate <= 0.0 || toIndex (lookahead) == engine.getLookahead())
        return;

    const juce::ScopedLock lock (getCallbackLock());
    prepareEngine (toIndex (lookahead));
}

void FloorMatchProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    if (currentSampleRate <= 0.0)
        return;

    const int version = profileVersion.load();
    if (version != appliedProfileVersion)
    {
        const juce::SpinLock::ScopedTryLockType lock (sharedLock);
        if (lock.isLocked())
        {
            engine.setProfile (sharedProfile);
            appliedProfileVersion = version;
        }
    }

    engine.setLearning (learnRequested.load());
    if (quietestResetRequested.exchange (false))
        engine.resetQuietest();

    dsp::Settings settings;
    settings.targetDb = target->load();
    settings.match = match->load() * 0.01f;
    settings.maxReductionDb = maxReduction->load();
    settings.fill = fill->load() > 0.5f;
    settings.listenRemoved = toIndex (listen) == listenRemoved;
    engine.setSettings (settings);

    const int channels = std::min (buffer.getNumChannels(), currentChannels);
    if (channels == currentChannels)
        engine.process (buffer.getArrayOfWritePointers(), channels, buffer.getNumSamples());

    if (engine.getSnapshotCounter() != publishedSnapshot)
    {
        const juce::SpinLock::ScopedTryLockType lock (sharedLock);
        if (lock.isLocked())
        {
            publishedSnapshot = engine.getSnapshotCounter();
            sharedSnapshot = engine.getSnapshot();
            sharedLearnedValid = engine.getLearnedProfile (sharedLearned);
            sharedQuietestValid = engine.getQuietest (sharedQuietest, sharedQuietestLevel);
        }
    }
}

//==============================================================================
void FloorMatchProcessor::setLearning (bool shouldLearn)
{
    if (shouldLearn == learnRequested.load())
        return;

    if (shouldLearn)
    {
        {
            const juce::SpinLock::ScopedLockType lock (sharedLock);
            sharedLearnedValid = false;
        }
        learnRequested = true;
        return;
    }

    learnRequested = false;

    dsp::Profile learned;
    {
        const juce::SpinLock::ScopedLockType lock (sharedLock);
        if (! sharedLearnedValid)
            return;
        learned = sharedLearned;
    }

    setProfile (learned, true);
}

bool FloorMatchProcessor::getQuietest (float& levelDb) const
{
    const juce::SpinLock::ScopedLockType lock (sharedLock);
    if (! sharedQuietestValid)
        return false;

    levelDb = dsp::profileLevelDb (sharedQuietest);
    return true;
}

bool FloorMatchProcessor::useQuietest()
{
    dsp::Profile quietest;
    {
        const juce::SpinLock::ScopedLockType lock (sharedLock);
        if (! sharedQuietestValid)
            return false;
        quietest = sharedQuietest;
    }

    setProfile (quietest, true);
    return true;
}

void FloorMatchProcessor::resetQuietest()
{
    {
        const juce::SpinLock::ScopedLockType lock (sharedLock);
        sharedQuietestValid = false;
    }
    quietestResetRequested = true;
}

bool FloorMatchProcessor::hasProfile() const
{
    const juce::SpinLock::ScopedLockType lock (sharedLock);
    return sharedProfile.valid;
}

dsp::Profile FloorMatchProcessor::getProfile() const
{
    const juce::SpinLock::ScopedLockType lock (sharedLock);
    return sharedProfile;
}

void FloorMatchProcessor::setProfile (const dsp::Profile& profile, bool alsoSetTarget)
{
    {
        const juce::SpinLock::ScopedLockType lock (sharedLock);
        sharedProfile = profile;
    }
    ++profileVersion;
    apvts.state.setProperty (profileId, profileToString (profile), nullptr);

    if (alsoSetTarget && profile.valid)
    {
        auto* param = apvts.getParameter (ids::target);
        param->beginChangeGesture();
        param->setValueNotifyingHost (param->convertTo0to1 (dsp::profileLevelDb (profile)));
        param->endChangeGesture();
    }
}

void FloorMatchProcessor::clearProfile()
{
    setProfile ({}, false);
}

bool FloorMatchProcessor::getSnapshot (dsp::Snapshot& result) const
{
    const juce::SpinLock::ScopedLockType lock (sharedLock);
    result = sharedSnapshot;
    return result.valid;
}

juce::String FloorMatchProcessor::profileToString (const dsp::Profile& profile)
{
    if (! profile.valid)
        return {};

    juce::StringArray values;
    for (float v : profile.psd)
        values.add (juce::String (10.0f * std::log10 (std::max (v, 1.0e-30f)), 3));   // dB per Hz

    return values.joinIntoString (" ");
}

dsp::Profile FloorMatchProcessor::profileFromString (const juce::String& text)
{
    dsp::Profile profile;
    const auto values = juce::StringArray::fromTokens (text, " ", {});

    if (values.size() != dsp::numProfileBands)
        return profile;

    for (int i = 0; i < values.size(); ++i)
        profile.psd[(size_t) i] = std::pow (10.0f, values[i].getFloatValue() / 10.0f);

    profile.valid = true;
    return profile;
}

//==============================================================================
juce::AudioProcessorEditor* FloorMatchProcessor::createEditor()
{
    return new FloorMatchEditor (*this);
}

void FloorMatchProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void FloorMatchProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
    {
        if (xml->hasTagName (apvts.state.getType()))
        {
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
            setProfile (profileFromString (apvts.state.getProperty (profileId).toString()), false);
        }
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new FloorMatchProcessor();
}
