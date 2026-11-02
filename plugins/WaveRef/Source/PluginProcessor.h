#pragma once

#include "Loudness.h"
#include "Parameters.h"
#include "RefPlayer.h"
#include "Spectrum.h"
#include "Storage.h"
#include "StretchPlayer.h"
#include "Stereo.h"
#include "TrackData.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

// WaveRef - reference A/B with a mid/side spectrum comparison.
//
// The plugin sits on the master bus. "Mix" is the audio coming in from the DAW; "Ref" is a loaded file (one of
// eight slots) looping next to it. A/B crossfades between them in a few milliseconds, the reference can be
// loudness-matched to the mix, and the monitored signal can be folded to mono or reduced to a band. Both signals
// are always analysed, whichever one is audible, so the editor can draw their mid and side spectra on top of each other.
class WaveRefProcessor : public juce::AudioProcessor,
                         public juce::ChangeBroadcaster,
                         private juce::AsyncUpdater,
                         private juce::Timer
{
public:
    // Where a reference's loop sits, in seconds of the file (end 0 = to the end of the file).
    struct SlotInfo
    {
        juce::String path;
        double loopStart = 0.0, loopEnd = 0.0;
        bool loopOn = false;
        double cues[4] { -1.0, -1.0, -1.0, -1.0 };   // seconds, negative = not set
        double bpm = 0.0;                            // 0 = unknown
        double downbeat = 0.0;                       // seconds of the first beat of the grid
        bool loading = false;
        juce::String error;
    };

    WaveRefProcessor();
    ~WaveRefProcessor() override;

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

    // ---- reference slots (message thread) ----------------------------------------------------------

    // Decodes a file on a background thread and puts it in the slot when done. A new loop is kept if the slot
    // already had one for the same file.
    void loadFile (int slot, const juce::File& file);

    // Same, but waits for the result (tests and the offline renderer).
    bool loadFileBlocking (int slot, const juce::File& file, juce::String* error = nullptr);

    // Puts a ready-made track in a slot (what the loader does when it finishes).
    void installTrack (int slot, std::shared_ptr<waveref::TrackData> track);

    void clearSlot (int slot);

    std::shared_ptr<const waveref::TrackData> getTrack (int slot) const;
    SlotInfo getSlotInfo (int slot) const;

    // Loop in samples of the loaded track (the whole track when the loop is off).
    void getLoopSamples (int slot, int64_t& start, int64_t& end) const;
    void setLoop (int slot, double startSeconds, double endSeconds, bool loopOn);

    // Cue points (four per slot): seconds into the file, negative clears.
    void setCue (int slot, int index, double seconds);

    // Jump the reference to a position (free mode). The caller turns the reference's transport on if it should play.
    void seekReference (double seconds);

    // Makes the reference's current position line up with the host's now: by beats in beats mode, otherwise by time
    // (switching to host-time mode).
    bool alignToHost();

    // Beat grid of a reference: tempo (0 = unknown) and the time of a bar's first beat. Detected when a file loads.
    void setGrid (int slot, double bpm, double downbeatSeconds);
    void detectGridAgain (int slot);

    // Free the tracks that the audio thread no longer uses. Called from the editor's timer.
    void collectGarbage();

    // ---- remembered references ---------------------------------------------------------------------------
    // Every file loaded into a slot (with its loop, cues and tempo) is kept in a small file in the user's application
    // data folder, and a new instance starts with those slots filled. A project that was saved keeps its own slots.
    bool isRememberingReferences() const;
    void setRememberReferences (bool shouldRemember);   // turning it on keeps what is loaded now
    void forgetRememberedReferences();

    // Writes pending loop / cue / tempo edits to that file now (otherwise it happens a moment later).
    void flushLibrary();

    // Loads finish on a background thread and are applied on the message thread. Tests (which run no message
    // loop) wait with hasLoadsInFlight() and apply the results themselves.
    bool hasLoadsInFlight() const { return loader.getNumJobs() > 0; }
    void drainFinishedLoads();

    // ---- blind test ----------------------------------------------------------------------------------

    // True when the signal on "X" is the reference (re-rolled after every guess and when blind mode starts).
    bool isXTheReference() const { return xIsRef.load(); }
    void rerollBlind();

    // Records a guess ("X is the reference" or "Y is..."), re-rolls and returns whether it was right.
    bool submitGuess (bool guessedXIsRef);
    int getBlindCorrect() const { return blindCorrect.load(); }
    int getBlindTotal() const   { return blindTotal.load(); }
    void resetBlindScore()      { blindCorrect = 0; blindTotal = 0; }

    // ---- readouts for the editor ---------------------------------------------------------------------

    bool isReferenceAudible() const { return refAudible.load(); }
    float getMixLufs() const        { return mixLufs.load(); }
    float getRefLufs() const        { return refLufs.load(); }
    float getMatchDb() const        { return matchDb.load(); }
    int getPlayheadSlot() const     { return playheadSlot.load(); }
    int64_t getPlayhead() const     { return playhead.load(); }
    double getPlayheadSeconds() const { return (double) playhead.load() / currentRate.load(); }
    bool isHostTimeKnown() const    { return hostTimeKnown.load(); }
    bool isHostPlaying() const      { return hostIsPlaying.load(); }
    double getHostSeconds() const   { return hostSeconds.load(); }
    bool isHostBeatKnown() const    { return hostBeatKnown.load(); }
    double getHostBpm() const       { return hostBpmValue.load(); }
    double getCurrentSampleRate() const { return currentRate.load(); }

    juce::AudioProcessorValueTreeState apvts;
    waveref::SpectrumAnalyser analyser;
    waveref::StereoMeter stereo;

    // Editor settings that live in the project state (colours, FFT size...).
    juce::ValueTree getUiState();

private:
    static constexpr int maxChunk = 256;

    struct Slot
    {
        std::shared_ptr<waveref::TrackData> owner;                       // message thread
        std::atomic<waveref::TrackData*> pending { nullptr };            // message thread -> audio thread
        std::atomic<waveref::TrackData*> active { nullptr };             // what the audio thread uses
        std::atomic<int64_t> loopStart { 0 }, loopEnd { 0 };             // samples; 0 / 0 = whole track
        std::atomic<float> loopLufs { waveref::loudness::silenceLufs };           // loudness of the loop region
        std::atomic<double> gridBpm { 0.0 }, gridDownbeat { 0.0 };       // the beat grid
        SlotInfo info;                                                   // guarded by slotLock
        std::atomic<int> generation { 0 };                               // newest load request
    };

    struct FinishedLoad
    {
        int slot, generation;
        std::shared_ptr<waveref::TrackData> track;
        juce::String error;
    };

    struct Retired
    {
        std::shared_ptr<waveref::TrackData> track;
        juce::int64 retiredAt;
    };

    void applySlotState();
    void restoreLibrary();
    void updateLibrary (int slot, bool replaceEntry);
    void markLibraryDirty (int slot);
    void handleAsyncUpdate() override { startTimer (600); }
    void timerCallback() override { stopTimer(); flushLibrary(); }
    void writeSlotState (int slot);
    void publishLoop (int slot);
    void startLoad (int slot, const juce::File& file, bool blocking, juce::String* error);
    void finishLoad (int slot, int generation, std::shared_ptr<waveref::TrackData> track, const juce::String& error);
    void loadAllSlots();

    void effectiveLoop (const waveref::TrackData* track, int slot, int64_t& start, int64_t& end) const;
    bool isReferenceSelected() const;
    void updateFilterCutoffs (float freq, float widthOctaves);

    struct Params
    {
        std::atomic<float> *ab, *slot, *matchOn, *matchMode, *trim, *mono, *filterOn, *filterFreq, *filterWidth,
                           *restartSwitch, *restartPlay, *blind, *refPlay, *refMode, *syncOffset, *beatOffset, *nudge,
                           *keyLock, *side, *delta;
    } params {};

    // Slots and threads
    std::array<Slot, waveref::numSlots> slots;
    mutable juce::CriticalSection slotLock;
    std::vector<Retired> graveyard;
    std::vector<FinishedLoad> finishedLoads;   // guarded by slotLock
    juce::AudioFormatManager formats;
    juce::ThreadPool loader { 1 };
    std::atomic<double> currentRate { 44100.0 };
    bool rateKnown = false;

    // Audio thread state
    double sampleRate = 44100.0;
    std::array<waveref::TrackData*, waveref::numSlots> tracks {};
    int playingSlot = -1;
    waveref::RefPlayer player;
    waveref::StretchPlayer stretch;
    waveref::loudness::LiveMeter liveMeter;
    bool wasPlaying = false, wasRefAudible = false;
    float matchGainDb = 0.0f;

    using Linear = juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>;
    Linear abFade, monoFade, sideFade, deltaFade, filterFade, refGain;
    const waveref::TrackData* currentTrack = nullptr;   // what the player was last pointed at

    std::array<juce::dsp::StateVariableTPTFilter<float>, 4> bandFilters;   // HP, HP, LP, LP (24 dB/oct each side)
    float lastFilterFreq = -1.0f, lastFilterWidth = -1.0f;

    std::array<float, maxChunk> refL {}, refR {}, mixL {}, mixR {};

    // Readouts and blind test
    std::atomic<bool> refAudible { false };
    std::atomic<float> mixLufs { waveref::loudness::silenceLufs }, refLufs { waveref::loudness::silenceLufs }, matchDb { 0.0f };
    std::atomic<int> playheadSlot { -1 };
    std::atomic<int64_t> seekRequest { -1 };
    std::atomic<uint32_t> libraryDirty { 0 };
    std::atomic<bool> hostTimeKnown { false }, hostIsPlaying { false };
    std::atomic<double> hostSeconds { 0.0 }, hostPpq { 0.0 }, hostBpmValue { 0.0 };
    std::atomic<bool> hostBeatKnown { false };
    std::atomic<int64_t> playhead { 0 };
    std::atomic<bool> xIsRef { false };
    std::atomic<int> blindCorrect { 0 }, blindTotal { 0 };
    juce::Random blindRandom;

    JUCE_DECLARE_WEAK_REFERENCEABLE (WaveRefProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveRefProcessor)
};
