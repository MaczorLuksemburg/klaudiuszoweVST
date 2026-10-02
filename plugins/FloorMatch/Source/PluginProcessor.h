#pragma once

#include "Engine.h"
#include "Parameters.h"

// FloorMatch - brings the background noise of dialogue takes to one target level (and colour).
// All DSP lives in floormatch::dsp::Engine; the processor handles parameters, state, latency and the
// hand-over of profiles and meters between the audio and message threads.
class FloorMatchProcessor : public juce::AudioProcessor,
                            private juce::AudioProcessorValueTreeState::Listener,
                            private juce::AsyncUpdater
{
public:
    FloorMatchProcessor();
    ~FloorMatchProcessor() override;

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
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

    // ---- message thread ------------------------------------------------------------------
    // Learn: average the noise while playing, then store it as the target profile and level.
    void setLearning (bool shouldLearn);
    bool isLearning() const { return learnRequested.load(); }

    // The quietest steady noise floor heard so far (level in dB), or false if none yet.
    bool getQuietest (float& levelDb) const;
    bool useQuietest();          // makes it the target profile and level
    void resetQuietest();

    bool hasProfile() const;
    floormatch::dsp::Profile getProfile() const;
    void setProfile (const floormatch::dsp::Profile&, bool alsoSetTarget);
    void clearProfile();

    bool getSnapshot (floormatch::dsp::Snapshot&) const;

    // The last ~20 s of timeline columns, oldest first; returns how many are filled.
    static constexpr int timelineLength = 400;
    using Timeline = std::array<floormatch::dsp::TimelineColumn, timelineLength>;
    int getTimeline (Timeline&) const;

    static juce::String profileToString (const floormatch::dsp::Profile&);
    static floormatch::dsp::Profile profileFromString (const juce::String&);

private:
    void parameterChanged (const juce::String& parameterID, float newValue) override;
    void handleAsyncUpdate() override;
    void prepareEngine (int lookahead);

    floormatch::dsp::Engine engine;
    double currentSampleRate = 0.0;
    int currentChannels = 1;

    std::atomic<float>* target = nullptr;
    std::atomic<float>* match = nullptr;
    std::atomic<float>* maxReduction = nullptr;
    std::atomic<float>* lookahead = nullptr;
    std::atomic<float>* fill = nullptr;
    std::atomic<float>* listen = nullptr;

    // Shared between threads; the audio thread only ever try-locks.
    juce::SpinLock sharedLock;
    floormatch::dsp::Profile sharedProfile, sharedLearned, sharedQuietest;
    floormatch::dsp::Snapshot sharedSnapshot;
    float sharedQuietestLevel = 0.0f;
    Timeline sharedTimeline {};
    int64_t sharedTimelineCount = 0, copiedColumns = 0;
    bool sharedLearnedValid = false, sharedQuietestValid = false;

    std::atomic<int> profileVersion { 0 };
    int appliedProfileVersion = -1, publishedSnapshot = -1;
    std::atomic<bool> learnRequested { false }, quietestResetRequested { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FloorMatchProcessor)
};
