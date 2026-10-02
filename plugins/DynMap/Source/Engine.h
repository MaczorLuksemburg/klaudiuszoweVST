#pragma once

#include "Analyzer.h"
#include "Crossover.h"
#include "Stage.h"

namespace dynmap
{
    // All drawn curves (message thread), published to the audio thread through CurveSlots.
    class CurveBank
    {
    public:
        CurveBank();

        Curve get (int stage, CurveKind) const;
        void set (int stage, CurveKind, const Curve&);
        void resetAll();

        CurveSlot& getSlot (int stage, CurveKind kind) { return kind == CurveKind::level ? levelSlots[(size_t) stage] : transientSlots[(size_t) stage]; }

        // Curves live in a "CURVES" child of the plugin state (only non-neutral ones are written).
        void writeTo (juce::ValueTree& state) const;
        void readFrom (const juce::ValueTree& state);

        int getChangeCount() const { return changes.load(); }   // bumps on every edit, for displays

        static const juce::Identifier treeId;

    private:
        mutable juce::CriticalSection lock;
        std::array<Curve, numStages> level, transient;
        std::array<CurveSlot, numStages> levelSlots, transientSlots;
        std::atomic<int> changes { 0 };
    };

    //==============================================================================
    // Output peaks, loudness and limiter activity for the meters.
    struct OutputMeters
    {
        std::array<std::atomic<float>, 2> inPeak {}, outPeak {};
        std::atomic<float> truePeakMax { 0.0f };       // since the last reset (click the meter)
        std::atomic<float> limiterGainDb { 0.0f };
        std::atomic<float> autoGainDb { 0.0f };
        std::atomic<bool> resetTruePeak { false };
        std::atomic<float> sidechainDb { -100.0f };    // recent sidechain peak (falls 60 dB/s), -100 = none
        dsp::LoudnessMeter outLoudness;
    };

    //==============================================================================
    // Signal flow:
    //   in gain -> INPUT stage -> band split -> band stages (solo/mute) -> sum -> MASTER stage
    //   -> output gain -> clipper -> true-peak limiter -> auto gain -> delta
    // The global mix scales every stage's own dry/wet, so the dry part still goes through the
    // band split and stays in phase with the processed part.
    class Engine
    {
    public:
        explicit Engine (juce::AudioProcessorValueTreeState&);
        ~Engine();

        void prepare (double sampleRate, int maxBlockSize);
        void reset();

        // Stereo in place. Sidechain pointers may be null.
        void process (float* left, float* right, const float* scLeft, const float* scRight, int numSamples);

        // Latency for the current settings (the processor reports it to the host).
        int getLatency() const { return latency; }

        BandLayout getLayout() const;   // from the parameters (any thread)

        Stage& getStage (int stage) { return stages[(size_t) stage]; }

        // Tests: true when a linear-phase crossover redesign is still pending.
        bool isCrossoverDesignPending() const { return phase == phaseLinear && ! linearSplitter.isUpToDate(); }

        CurveBank curves;
        OutputMeters meters;
        SpectrumAnalyzer preSpectrum, postSpectrum, sidechainSpectrum;

    private:
        struct StageParams
        {
            std::atomic<float> *bypass, *mode, *pre, *post, *mix, *attack, *hold, *release, *relShape, *rms,
                               *lookahead, *link, *stereo, *scFilter, *scSource, *trTime, *maxBoost, *maxCut,
                               *smooth, *satType, *drive, *satPos;
            std::atomic<float> *width = nullptr, *solo = nullptr, *mute = nullptr;
            std::atomic<float> *bandOn = nullptr, *freq = nullptr, *slope = nullptr;
        };

        class Limiter
        {
        public:
            void prepare (double sampleRate);
            void reset();
            int getLatency() const { return window + dsp::TruePeakDetector::delay; }
            // Returns the lowest gain applied (linear).
            float process (float* left, float* right, int numSamples, float ceiling, float releaseMs, bool enabled, bool snap);

        private:
            double sampleRate = 44100.0;
            int window = 96;
            dsp::SlidingMin minimum;
            dsp::MovingAverage average;
            std::array<dsp::TruePeakDetector, 2> truePeak;
            std::array<dsp::DelayLine, 2> delay;
            float released = 1.0f;
            juce::SmoothedValue<float> fade;
        };

        void readSettings();
        StageSettings readStage (int stage) const;
        void processBlock (float* left, float* right, const float* scLeft, const float* scRight, int n);
        void processBands (float* left, float* right, const float* scLeft, const float* scRight, int n);
        void processOutput (float* left, float* right, int n);

        juce::AudioProcessorValueTreeState& apvts;
        std::array<StageParams, numStages> stageParams {};
        std::atomic<float> *amountParam, *timeParam, *mixParam, *inGainParam, *outGainParam, *clipParam,
                           *limiterParam, *ceilingParam, *limRelParam, *autoGainParam, *deltaParam,
                           *qualityParam, *phaseParam, *scGainParam, *scListenParam;

        double sampleRate = 44100.0;
        int maxBlock = 512;
        int latency = 0;

        std::array<Stage, numStages> stages;
        std::array<StageSettings, numStages> settings {};
        GlobalSettings global;
        BandLayout layout;
        int phase = phaseMinimum;
        bool firstBlock = true;
        std::array<bool, maxBands> bandWasActive {};

        MinimumPhaseSplitter splitter, sidechainSplitter;
        LinearPhaseSplitter linearSplitter;

        juce::AudioBuffer<float> bandBuffers, sidechainBandBuffers, scratch;
        std::array<std::array<float*, 2>, maxBands> bandPointers {}, sidechainBandPointers {};
        std::array<juce::SmoothedValue<float>, maxBands> bandGains;

        std::array<dsp::DelayLine, 2> dryDelay, sidechainBandDelay, sidechainMasterDelay;
        juce::AudioBuffer<float> sidechainDelayed, sidechainInput;
        juce::SmoothedValue<float> sidechainGain;

        juce::SmoothedValue<float> inGain, outGain;

        // Delta reference: the dry signal through a copy of the band split, so a neutral setup
        // gives silence even with minimum-phase crossovers.
        MinimumPhaseSplitter referenceSplitter;
        juce::AudioBuffer<float> referenceBands, deltaReference;
        std::array<std::array<float*, 2>, maxBands> referencePointers {};
        bool deltaWasOn = false;

        // Output section.
        std::array<std::unique_ptr<juce::dsp::Oversampling<float>>, Stage::numQualities> clipOversamplers;
        std::array<int, Stage::numQualities> clipLatency {};
        std::array<dsp::DelayLine, 2> clipDelay;
        juce::SmoothedValue<float> clipFade;
        int clipMode = clipOff;
        bool clipWasActive = false;
        Limiter limiter;
        dsp::LoudnessMeter dryLoudness, wetLoudness;
        float autoGainDb = 0.0f;
        juce::SmoothedValue<float> autoGain;
        std::array<dsp::TruePeakDetector, 2> outTruePeak;
    };
}
