#pragma once

#include "Dsp.h"
#include "Parameters.h"

// Band splitting. Bands belong to slots: slot 0 is the lowest band, slots 1-11 each own the
// crossover at their lower edge and are only in use while that crossover is on. The layout
// sorts the used slots by frequency, so bands can be added anywhere without renumbering.
namespace dynmap
{
    struct BandLayout
    {
        int numBands = 1;
        std::array<int, maxBands> slots {};          // band slot at each position, low to high
        std::array<float, maxBands> freqs {};        // crossover below each position (position 0 unused)
        std::array<int, maxBands> slopes {};         // slope of that crossover

        bool sameTopology (const BandLayout& other) const
        {
            if (numBands != other.numBands)
                return false;

            for (int i = 0; i < numBands; ++i)
                if (slots[(size_t) i] != other.slots[(size_t) i] || (i > 0 && slopes[(size_t) i] != other.slopes[(size_t) i]))
                    return false;

            return true;
        }

        bool operator== (const BandLayout& other) const
        {
            if (! sameTopology (other))
                return false;

            for (int i = 1; i < numBands; ++i)
                if (freqs[(size_t) i] != other.freqs[(size_t) i])
                    return false;

            return true;
        }

        int positionOf (int slot) const
        {
            for (int i = 0; i < numBands; ++i)
                if (slots[(size_t) i] == slot)
                    return i;
            return -1;
        }
    };

    //==============================================================================
    // Linkwitz-Riley style crossovers built from zero-delay-feedback filters, so frequencies can
    // be dragged smoothly. Low + high always sums to an allpass; bands below a crossover get the
    // same allpass, so the whole split sums to a flat magnitude response.
    //   6 dB/oct:  one-pole, high = x - low (sums to exactly x)
    //  12 dB/oct:  LR2, high band inverted
    //  24 dB/oct:  LR4
    //  48 dB/oct:  LR8
    class MinimumPhaseSplitter
    {
    public:
        void prepare (double sampleRate);
        void reset();

        // Snap = jump to the layout's frequencies instead of gliding (on prepare and topology changes).
        void setLayout (const BandLayout&, bool snap);

        // bands[position][channel] must have room for numSamples.
        void process (const float* const* input, float* const* const* bands, int numSamples);

        static constexpr int maxChunk = 32;

    private:
        struct Coeffs
        {
            int slope = slope24;
            OnePoleCoeffs onePole;
            SvfCoeffs svf[2];

            void setup (int slopeIndex, double freq, double sampleRate);
        };

        struct SplitState
        {
            OnePoleState onePole[3];
            SvfState svf[7];
            void reset() { for (auto& s : onePole) s.reset(); for (auto& s : svf) s.reset(); }
        };

        struct AllpassState
        {
            OnePoleState onePole;
            SvfState svf[2];
            void reset() { onePole.reset(); for (auto& s : svf) s.reset(); }
        };

        static void split (const Coeffs&, SplitState&, float x, float& low, float& high) noexcept;
        static float allpass (const Coeffs&, AllpassState&, float x) noexcept;

        double sampleRate = 44100.0;
        BandLayout layout;
        std::array<float, maxBands> currentFreq {}, targetFreq {};
        std::array<Coeffs, maxBands> coeffs {};                                      // by crossover slot
        std::array<std::array<SplitState, 2>, maxBands> splitStates {};             // [crossover slot][channel]
        std::array<std::array<std::array<AllpassState, 2>, maxBands>, maxBands> allpassStates {};   // [band slot][crossover slot][channel]
        float glide = 0.1f;
        bool hasLayout = false;
    };

    //==============================================================================
    // Linear-phase split: each band is an FIR (zero-phase magnitude, windowed, centred), made on
    // a background thread and run with partitioned convolution. The magnitudes nest so the bands
    // sum to exactly a delayed impulse. Latency is half the FIR length.
    class LinearPhaseSplitter : private juce::Thread
    {
    public:
        LinearPhaseSplitter();
        ~LinearPhaseSplitter() override;

        // Not realtime: designs the current layout synchronously.
        void prepare (double sampleRate, int maxBlockSize, const BandLayout&);
        void reset();

        int getLatency() const { return firLength / 2 + convolvers[0]->getLatency(); }

        // Audio thread: requests a redesign when the layout changed and installs finished designs.
        void setLayout (const BandLayout&);

        void process (const float* const* input, float* const* const* bands, int numSamples);

        // Tests: true once the newest requested layout is installed.
        bool isUpToDate() const { return upToDate.load(); }

    private:
        void run() override;
        void design (const BandLayout&, std::array<juce::AudioBuffer<float>, maxBands>& irs) const;

        static constexpr int convolutionLatency = 1024;

        double sampleRate = 44100.0;
        int firLength = 8192;
        int maxBlock = 512;

        juce::dsp::ConvolutionMessageQueue queue;
        std::array<std::unique_ptr<juce::dsp::Convolution>, maxBands> convolvers;   // by band slot
        juce::AudioBuffer<float> scratch;

        juce::SpinLock requestLock, resultLock;
        BandLayout requested, current;
        bool hasRequest = false, hasResult = false;
        std::array<juce::AudioBuffer<float>, maxBands> results;
        BandLayout resultLayout;
        std::atomic<bool> upToDate { true };
        uint32_t requestCounter = 0, installedCounter = 0, resultCounter = 0;
        juce::WaitableEvent wakeUp;
    };
}
