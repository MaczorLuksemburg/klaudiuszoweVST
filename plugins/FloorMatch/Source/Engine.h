#pragma once

#include <juce_dsp/juce_dsp.h>

#include <complex>

// FloorMatch noise engine: brings the background noise of a dialogue track to a target
// level (and optionally a target colour) while leaving the dialogue alone.
//
//  1. STFT analysis (about 21 ms frames, 75 % overlap, sqrt-Hann windows).
//  2. Noise floor per bin from a window before AND after each frame (lookahead). Each side
//     gives a robust truncated mean of the smoothed power; per frequency band the engine
//     decides whether a cut between takes lies inside the window (the noise floor steps
//     up or down and stays there) and then only uses the side that belongs to the current
//     take, so a cut is followed on the exact frame instead of lagging behind. Only sharp
//     steps count as cuts: noise events inside a take (cloth rustle, a passing car) fade in
//     and out and are left alone. Frames well above the estimate count as speech and are
//     skipped, a long phrase or event holds the last estimate, and without a cut the floor
//     may only rise slowly.
//  3. Speech gain per bin: decision-directed a priori SNR, MMSE log-spectral amplitude
//     gain and a speech presence probability, opened slightly before onsets (lookahead)
//     and released slowly. In pure noise it is ~0, so the noise only sees a smooth,
//     slowly changing attenuation (no musical noise).
//  4. Final gain = sqrt(beta) + (1 - sqrt(beta)) * speech gain, where beta = target / noise
//     per bin (limited by the max reduction). Takes quieter than the target can be filled
//     with noise shaped like the target instead.
namespace floormatch::dsp
{

    inline constexpr int numProfileBands = 61;   // 1/6-octave bands from 20 Hz to 20.2 kHz

    inline float profileBandFrequency (int band) { return 20.0f * std::exp2 ((float) band / 6.0f); }

    // The display works in finer 1/12-octave bands (narrow hum lines stay visible).
    inline constexpr int numDisplayBands = 121;  // 20 Hz to 20.2 kHz
    inline float displayBandFrequency (int band) { return 20.0f * std::exp2 ((float) band / 12.0f); }

    // A-weighting as a power factor (1.0 at 1 kHz).
    float aWeighting (float hz);

    // A noise spectrum shape: mean-square power per Hz in each profile band (linear).
    struct Profile
    {
        std::array<float, numProfileBands> psd {};
        bool valid = false;
    };

    // Power per Hz at any frequency, log-interpolated between the band centres.
    float profilePsdAt (const Profile&, float hz);

    // A-weighted level of a profile between 20 Hz and 20 kHz, in dB where a full-scale sine reads 0 dB.
    float profileLevelDb (const Profile&);

    struct Settings
    {
        float targetDb = -60.0f;
        float match = 1.0f;             // 0 = level only (keeps each take's colour), 1 = match the profile's colour
        float maxReductionDb = 12.0f;
        bool fill = false;
        bool listenRemoved = false;
    };

    // What the engine did on its latest output frame, for meters and the spectrum display.
    struct Snapshot
    {
        bool valid = false;
        float noiseDb = -150.0f;        // A-weighted level of the noise floor in the input
        float outputDb = -150.0f;       // ... and after processing
        std::array<float, numDisplayBands> noise {}, target {}, output {};   // band levels in dB
        std::array<float, numDisplayBands> input {};                         // live input spectrum, dB
    };

    // One column of the timeline (about 50 ms): what happened to the background.
    struct TimelineColumn
    {
        float inputDb = -150.0f;        // A-weighted level of the whole input (dialogue included)
        float noiseDb = -150.0f;        // background in
        float outputDb = -150.0f;       // background out
        float targetDb = -150.0f;
        bool valid = false;             // false over digital silence
        bool cut = false;               // a cut between takes was detected here
        bool event = false;             // the background is being held through an event
        bool limited = false;           // max reduction stops the background reaching the target
    };

    class Engine
    {
    public:
        // How the speech gain is formed. The defaults are the tuned values; tests sweep them.
        struct Tuning
        {
            float ddAlpha = 0.98f;              // decision-directed a priori SNR smoothing (faster tracking keeps onsets and consonants)
            float minPrioriSnr = 0.001f;        // -30 dB
            float presenceLowDb = 2.0f;         // local SNR where speech presence starts ...
            float presenceHighDb = 8.0f;        // ... and where it is certain
            float attackStep = 0.8f;            // pre-opening per frame of lookahead before an onset
            float speechGainFloor = 0.12f;      // speech gains below this are noise: zeroed, or attack and release would hold them
            double releaseSeconds = 0.05;
            bool wienerSpeechGain = false;      // Wiener instead of MMSE log-spectral amplitude
            bool smoothGainAcrossBins = false;  // smoothing pulls harmonic peaks down towards the gaps between them
            float spikeGain = 0.5f;             // gains below this are only ever lowered to their neighbours' average (isolated noise spikes)
        };

        void setTuning (const Tuning&);
        const Tuning& getTuning() const noexcept { return tuning; }

        enum Lookahead { lookaheadOff, lookaheadShort, lookaheadNormal, lookaheadLong };
        static double lookaheadSeconds (int mode);

        void prepare (double sampleRate, int numChannels, int lookahead);
        void reset();

        int getLatencySamples() const noexcept { return latency; }
        int getLookahead() const noexcept      { return lookaheadMode; }
        int getFftSize() const noexcept        { return fftSize; }
        int getHopSize() const noexcept        { return hop; }

        void setSettings (const Settings& s) noexcept { settings = s; }
        void setProfile (const Profile&);
        void process (float* const* channels, int numChannels, int numSamples);

        const Snapshot& getSnapshot() const noexcept { return snapshot; }
        int getSnapshotCounter() const noexcept      { return snapshotCounter; }

        // Timeline columns written so far; the last timelineCapacity of them can be read. A cut is
        // marked up to timelineSettle columns after its column was written.
        static constexpr int timelineCapacity = 64, timelineSettle = 10;
        int64_t getTimelineCount() const noexcept                       { return timelineCount; }
        const TimelineColumn& getTimelineColumn (int64_t index) const   { return timeline[(size_t) (index % timelineCapacity)]; }

        // While learning, the estimated noise of every output frame is averaged into a profile.
        void setLearning (bool shouldLearn);
        bool getLearnedProfile (Profile& result) const;

        // The quietest noise floor that stayed stable for a while, since the last reset.
        void resetQuietest();
        bool getQuietest (Profile& result, float& levelDb) const;

        // Tests only: channels from this index on get the same gains but don't take part in the
        // analysis, so a clean component of the mix can be processed alongside it.
        void setAnalysisChannels (int channels) noexcept { analysisChannels = std::max (1, channels); }

        // What the estimator decided for its latest frame, for tracing real recordings. Tests only.
        struct Diagnostics
        {
            char sides[32] {};          // per decision band: '-' none, 'P' past, 'F' future, 'p'/'f' bounded, 'R' recent
            int rises = 0, falls = 0, compared = 0, holdingBins = 0;
            int probeBin = 200, probeCountPast = 0, probeCountFuture = 0, probeHold = 0;
            float probeNoise = 0.0f, probePast = 0.0f, probeFuture = 0.0f, probeTrack = 0.0f;
        };
        void setProbeBin (int bin) noexcept { diagnostics.probeBin = bin; }
        const Diagnostics& getDiagnostics() const noexcept { return diagnostics; }

        // Estimated noise per bin for the latest output frame (power, in FFT units). Tests only.
        const std::vector<float>& getLatestNoiseEstimate() const noexcept { return latestNoise; }
        bool isLatestNoiseValid() const noexcept                         { return latestNoiseValid; }
        float binToPsd() const noexcept                                   { return psdScale; }

    private:
        using Complex = std::complex<float>;

        enum Side { sideNone, sidePast, sideFuture, sideBoundedPast, sideBoundedFuture, sideRecent };

        void processFrame();
        void analyse (int64_t frame);
        void smooth (int64_t frame);
        bool estimateNoise (int64_t frame, float* noise);
        void computeSpeechGain (int64_t frame, const float* noise, bool noiseValid, float* speechGain);
        void synthesise (int64_t frame);

        float truncatedMean (const float* row, const uint8_t* labels, const std::vector<int>& slots, int& count) const;
        float weightedLevelDb (const float* binPower) const;
        void binsToProfile (const float* binPower, std::array<float, numProfileBands>& psd) const;
        void binsToDisplay (const float* binPower, std::array<float, numDisplayBands>& levelsDb) const;
        void addTimelineFrame (bool valid, float inputDb, float noiseDb, float outputDb);
        void updateTargetShape();

        static int slotOf (int64_t frame, int length) { return (int) (frame % length); }

        // Configuration
        double sampleRate = 48000.0;
        int numChannels = 1, lookaheadMode = lookaheadNormal, analysisChannels = 64;
        int fftOrder = 10, fftSize = 1024, hop = 256, numBins = 513;
        int smoothFrames = 4;           // S(t) averages P(t - 2 .. t + 2), or t - 4 .. t + 4 in the lowest bins
        static constexpr int lowBins = 8;
        int attackFrames = 4;           // speech gain opens up to 4 frames before an onset
        int pastFrames = 0, futureFrames = 0, decimation = 1;
        struct Window { int start = 0, end = 0; int length() const { return end - start; } };
        Window pastWindow, futureWindow;   // frame offsets (inclusive) of the two sides compared by the estimator
        int delayFrames = 0, latency = 0;
        int powerLength = 0, smoothLength = 0, gainLength = 0, spectrumLength = 0;
        float psdScale = 1.0f, releaseCoeff = 0.9f;

        std::unique_ptr<juce::dsp::FFT> fft;
        std::vector<float> window, fftBuffer, binFrequency, weightedBin;

        // Sample-rate side
        std::vector<std::vector<float>> inputRing, outputRing;
        int ringPos = 0, hopCounter = 0;
        int64_t frameCounter = 0;

        // Frame-rate history (ring buffers indexed by frame number)
        std::vector<std::vector<Complex>> spectra;   // [channel][slot * numBins + bin]
        std::vector<float> power;                    // [slot * numBins + bin], channel average of |Y|^2
        std::vector<uint8_t> powerValid;
        std::vector<float> smoothed;                 // [bin * smoothLength + slot]
        std::vector<float> bandSmoothed;             // [band * smoothLength + slot]
        std::vector<uint8_t> smoothedValid;
        std::vector<uint8_t> speechLabel;            // [bin * smoothLength + slot]: the frame looked like speech
        std::vector<float> referenceNoise;           // latest estimate, for labelling and holding
        std::vector<int> holdFrames;
        int heldBins = 0;                            // bins holding their estimate on the previous frame
        std::vector<float> riseTrack;                // slowly rising floor that limits rises without a cut
        bool referenceValid = false;
        std::vector<float> noiseHistory, speechHistory;   // [slot * numBins + bin], last attackFrames + 1 frames
        std::vector<uint8_t> noiseHistoryValid;

        // Decision bands (roughly critical bands) used to detect cuts between takes.
        std::vector<int> bandStart;                  // first bin of each band, plus numBins at the end
        struct BandState { int side = sideNone; float ratio = 1.0f; };
        std::vector<BandState> bands;
        std::vector<int> cutHold;                    // no lookahead: frames a detected cut still applies
        std::vector<float> scratchPast, scratchFuture;
        std::vector<float> spanLevels, spanReversed, sharpTrack, sharpWindow;   // cut sharpness test
        int cutTrackHalfWidth = 9, maxCutFrames = 28;
        std::vector<int> pastSlots, futureSlots, nearSlots;
        int nearFrames = 47;

        // Speech gain state
        std::vector<float> previousSpeech, releasedGain, tempGain, noiseScratch;

        Tuning tuning;

        // Target
        Settings settings;
        Profile profile;
        std::vector<float> targetShape;              // per bin, FFT power units, at the profile's own level
        float targetShapeLevelDb = 0.0f;
        std::vector<float> beta, fillPower;

        // Fill noise
        uint64_t randomState = 0x9e3779b97f4a7c15ull;
        float fillGate = 0.0f;
        float nextGaussian();

        // Profile band mapping: bins [start, end) or, for narrow bands, interpolation at the centre.
        struct BandMap { int start = 0, end = 0, lower = 0; float fraction = 0.0f, width = 1.0f; };
        std::array<BandMap, numProfileBands> bandMap {};
        std::array<BandMap, numDisplayBands> displayMap {};
        std::vector<float> targetPower;              // per bin, what the target asks for (before the max reduction)

        // Timeline
        std::array<TimelineColumn, timelineCapacity> timeline {};
        int64_t timelineCount = 0;
        int columnFrames = 9, framesInColumn = 0, validInColumn = 0;
        double columnInput = 0.0, columnNoise = 0.0, columnOutput = 0.0;
        bool columnCut = false, columnEvent = false;

        // Cut markers: recent background estimates per decision band, in dB.
        void markCuts (int64_t frame, const float* noise);
        std::vector<float> markHistory;
        int markLength = 56, markFrames = 0;
        float markPeak = 0.0f;
        int64_t markPeakFrame = 0, lastMarkFrame = -(1 << 30);

        Diagnostics diagnostics;

        // Metering, learning, quietest
        Snapshot snapshot;
        int snapshotCounter = 0, frameSinceSnapshot = 0;
        std::vector<float> latestNoise;
        bool latestNoiseValid = false;

        bool learning = false;
        std::array<double, numProfileBands> learnSum {};
        int learnCount = 0;

        std::array<float, numProfileBands> recentBands {}, quietestBands {};
        float recentLevel = 0.0f, recentDeviation = 10.0f, quietestLevel = 0.0f;
        int stableFrames = 0;
        bool hasQuietest = false;
    };
}
