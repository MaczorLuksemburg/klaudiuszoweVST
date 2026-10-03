#pragma once

#include "Curve.h"
#include "Dsp.h"
#include "Parameters.h"

namespace dynmap
{
    // Everything one stage reads from the parameters, gathered once per block.
    struct StageSettings
    {
        bool bypass = false;
        int mode = modeDynamics;
        float preDb = 0.0f, postDb = 0.0f, mix = 1.0f;
        float attackMs = 5.0f, holdMs = 0.0f, releaseMs = 120.0f, relShape = 0.0f, rmsMs = 0.0f;
        int relLaw = 0;              // 0 classic, 1-8 accelerating release curves
        int lookaheadSamples = 0;
        float link = 1.0f;
        int stereo = stereoLR;
        float scFilterHz = 10.0f;
        int scSource = scInternal;
        float trTimeMs = 40.0f;
        float maxBoost = 24.0f, maxCut = 96.0f, smoothMs = 0.5f;
        int satType = satOff;
        float driveDb = 0.0f;
        int satPos = satAfter;
        float width = 1.0f;          // bands only
    };

    struct GlobalSettings
    {
        float amount = 1.0f;         // scales every curve's gain (negative inverts)
        float timeScale = 1.0f;      // scales every attack, hold, release and transient time
        int quality = 1;             // oversampling: 0 off, 1 2x, 2 4x, 3 8x
    };

    // Live values for the curve displays and meters (written by the audio thread once per block).
    struct StageMeter
    {
        std::atomic<float> levelDb { -150.0f };    // detected level (x on the level curve)
        std::atomic<float> transientDb { 0.0f };   // x on the transient curve
        std::atomic<float> gainDb { 0.0f };        // applied gain with the biggest magnitude this block
        std::atomic<bool> active { false };
    };

    //==============================================================================
    // One dynamics "map": pre gain -> detector -> level curve + transient curve -> gain,
    // saturation / waveshaping (oversampled), post gain, width, dry/wet mix.
    class Stage
    {
    public:
        static constexpr int maxChunk = 64;
        static constexpr int numQualities = 4;

        void prepare (double sampleRate, int maxBlockSize);
        void reset();

        // Latency of the oversampler for a quality setting (same for every stage).
        int getOversamplingLatency (int q) const { return osLatency[(size_t) juce::jlimit (0, numQualities - 1, q)]; }

        // Audio delay = lookahead + extraDelay; the detector sees the signal extraDelay late, so
        // bands with less lookahead line up with the band that has the most.
        void setSettings (const StageSettings&, const GlobalSettings&, int extraDelay);
        int getLatency() const { return lookahead + extraDelay + osLatency[(size_t) quality]; }

        // In place on left/right; sidechain may be null (then the external source falls back to the input).
        void process (float* left, float* right, const float* scLeft, const float* scRight, int numSamples);

        void pullCurves (CurveSlot& levelSlot, CurveSlot& transientSlot);

        StageMeter meter;

    private:
        void processChunk (float* left, float* right, const float* scLeft, const float* scRight, int n);
        void runShaper (int n, bool gainInside);
        void updateCoefficients();

        double sampleRate = 44100.0;
        StageSettings settings;
        GlobalSettings global;
        int lookahead = 0, extraDelay = 0, quality = 1;
        bool snapNext = true;

        CurveTable levelTable, transientTable;
        uint32_t levelVersion = 0, transientVersion = 0;

        // Smoothed controls.
        juce::SmoothedValue<float> preGain, postGain, mixAmount, widthAmount, shaperFade;

        // Detector.
        float attackCoef = 0.0f, releaseCoef = 0.0f, rmsCoef = 0.0f, smoothCoef = 0.0f;
        float fastAttack = 0.0f, fastRelease = 0.0f, slowAttack = 0.0f, slowRelease = 0.0f;   // transient followers
        int holdSamples = 0;
        std::array<float, 2> meanSquare {}, envelope {}, fastEnv {}, slowEnv {}, sustainEnv {}, smoothedGain {};
        std::array<int, 2> holdCounter {};
        std::array<int, 2> releaseCount {};          // samples since an accelerating release started
        std::array<float, 2> releaseStartLog2 {};    // log2 of the envelope where it started
        float relLawScale = 0.0f, relLawPower = 1.0f, relLawInvLength = 0.0f;
        std::array<dsp::SlidingMax, 2> levelPeak, transientPeak;   // ripple-free level for the followers
        int levelPeakLength = 1;
        bool detectorPrimed = false;
        OnePoleCoeffs scFilterCoeffs;
        std::array<OnePoleState, 2> scFilterState {};
        bool scFilterOn = false;

        // Delays.
        std::array<dsp::DelayLine, 2> audioDelay, detectorDelay, dryDelay, shaperDelay;

        // Shaper.
        dsp::Saturator saturator;
        std::array<std::unique_ptr<juce::dsp::Oversampling<float>>, numQualities> oversamplers;
        std::array<int, numQualities> osLatency {};
        OnePoleCoeffs dcCoeffs;
        std::array<OnePoleState, 2> dcState {};
        bool shaperWasActive = false;

        // Chunk buffers.
        std::array<std::array<float, maxChunk>, 2> dry {}, audio {}, gain {}, transientGain {}, direct {};
        juce::AudioBuffer<float> shaperBuffer;

        float blockLevel = -150.0f, blockTransient = 0.0f, blockGain = 0.0f;
    };
}
