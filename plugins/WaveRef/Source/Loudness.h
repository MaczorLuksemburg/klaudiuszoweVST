#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <cmath>
#include <vector>

// ITU-R BS.1770 loudness: K-weighting, 100 ms sub-blocks, 400 ms blocks (4 sub-blocks, 75 % overlap) and the
// two-stage gate. The same code measures the live mix and the loop region of a loaded reference, so the two
// numbers can be subtracted to get the match gain.
namespace waveref::loudness
{
    inline constexpr float silenceLufs = -150.0f;

    inline float energyToLufs (double energy)
    {
        return energy > 1.0e-15 ? (float) (-0.691 + 10.0 * std::log10 (energy)) : silenceLufs;
    }

    // Two cascaded biquads (shelf + high-pass) per channel.
    class KWeighting
    {
    public:
        void prepare (double sampleRate)
        {
            // Coefficients from the BS.1770-4 filter parameters (same maths as libebur128).
            {
                const double f0 = 1681.974450955533, gain = 3.999843853973347, q = 0.7071752369554196;
                const double k = std::tan (juce::MathConstants<double>::pi * f0 / sampleRate);
                const double vh = std::pow (10.0, gain / 20.0);
                const double vb = std::pow (vh, 0.4996667741545416);
                const double a0 = 1.0 + k / q + k * k;

                shelf = { (vh + vb * k / q + k * k) / a0, 2.0 * (k * k - vh) / a0, (vh - vb * k / q + k * k) / a0,
                          2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0 };
            }
            {
                const double f0 = 38.13547087602444, q = 0.5003270373238773;
                const double k = std::tan (juce::MathConstants<double>::pi * f0 / sampleRate);
                const double a0 = 1.0 + k / q + k * k;

                highpass = { 1.0, -2.0, 1.0, 2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0 };
            }

            reset();
        }

        void reset() { state = {}; }

        inline float process (int channel, float x)
        {
            auto& s = state[(size_t) channel];
            return (float) run (highpass, s.b, run (shelf, s.a, (double) x));
        }

    private:
        struct Coeffs { double b0, b1, b2, a1, a2; };
        struct Biquad { double z1 = 0.0, z2 = 0.0; };
        struct Channel { Biquad a, b; };

        static inline double run (const Coeffs& c, Biquad& s, double x)
        {
            const double y = c.b0 * x + s.z1;
            s.z1 = c.b1 * x - c.a1 * y + s.z2;
            s.z2 = c.b2 * x - c.a2 * y;
            return y;
        }

        Coeffs shelf {}, highpass {};
        std::array<Channel, 2> state {};
    };

    // Mean of the sub-block energies that make up one 400 ms block starting at `first`.
    inline double blockEnergy (const float* sub, int count, int first)
    {
        const int n = juce::jmin (4, count - first);
        double sum = 0.0;
        for (int i = 0; i < n; ++i)
            sum += (double) sub[first + i];
        return n > 0 ? sum / n : 0.0;
    }

    // Gated loudness over `count` sub-block energies (each already the sum over channels of the K-weighted
    // mean square of 100 ms). Fewer than four sub-blocks are treated as one short block.
    inline float gatedLoudness (const float* sub, int count)
    {
        if (count <= 0)
            return silenceLufs;

        const int numBlocks = juce::jmax (1, count - 3);
        const double absoluteGate = std::pow (10.0, (-70.0 + 0.691) / 10.0);

        double sum = 0.0;
        int used = 0;

        for (int b = 0; b < numBlocks; ++b)
        {
            const double e = blockEnergy (sub, count, b);
            if (e > absoluteGate)
            {
                sum += e;
                ++used;
            }
        }

        if (used == 0)
            return silenceLufs;

        const double relativeGate = (sum / used) * std::pow (10.0, -1.0);   // -10 LU
        double gatedSum = 0.0;
        int gatedCount = 0;

        for (int b = 0; b < numBlocks; ++b)
        {
            const double e = blockEnergy (sub, count, b);
            if (e > absoluteGate && e > relativeGate)
            {
                gatedSum += e;
                ++gatedCount;
            }
        }

        return gatedCount > 0 ? energyToLufs (gatedSum / gatedCount) : silenceLufs;
    }

    inline int subBlockLength (double sampleRate) { return juce::jmax (1, juce::roundToInt (sampleRate * 0.1)); }

    // Sub-block energies of a whole buffer (offline, for loaded references).
    inline std::vector<float> subBlockEnergies (const juce::AudioBuffer<float>& audio, double sampleRate)
    {
        const int length = subBlockLength (sampleRate);
        const int numChannels = juce::jmin (2, audio.getNumChannels());
        const int numSub = audio.getNumSamples() / length;

        std::vector<float> result ((size_t) numSub, 0.0f);
        KWeighting weighting;
        weighting.prepare (sampleRate);

        for (int block = 0; block < numSub; ++block)
        {
            double energy = 0.0;

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float* data = audio.getReadPointer (ch) + (size_t) block * (size_t) length;
                double sum = 0.0;

                for (int i = 0; i < length; ++i)
                {
                    const double y = weighting.process (ch, data[i]);
                    sum += y * y;
                }

                energy += sum / length;
            }

            result[(size_t) block] = (float) energy;
        }

        return result;
    }

    // Streaming meter for the live mix. Audio thread only; results are read through the accessors after each
    // finished sub-block (every 100 ms).
    class LiveMeter
    {
    public:
        static constexpr int historySize = 300;   // 30 s of sub-blocks

        void prepare (double sampleRate)
        {
            weighting.prepare (sampleRate);
            subLength = subBlockLength (sampleRate);
            reset();
        }

        void reset()
        {
            weighting.reset();
            history.fill (0.0f);
            count = 0;
            writeIndex = 0;
            accum = 0.0;
            accumCount = 0;
            shortLufs = longLufs = silenceLufs;
        }

        void process (const float* left, const float* right, int numSamples)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                const double l = weighting.process (0, left[i]);
                const double r = weighting.process (1, right[i]);
                accum += l * l + r * r;

                if (++accumCount >= subLength)
                {
                    pushSubBlock ((float) (accum / subLength));
                    accum = 0.0;
                    accumCount = 0;
                }
            }
        }

        float getShortLufs() const { return shortLufs; }    // last 3 s, ungated
        float getLongLufs() const  { return longLufs; }     // last 30 s, gated

    private:
        void pushSubBlock (float energy)
        {
            history[(size_t) writeIndex] = energy;
            writeIndex = (writeIndex + 1) % historySize;
            count = juce::jmin (count + 1, historySize);

            // Short term: mean of the last 30 sub-blocks (fewer while the meter is still filling up).
            const int shortCount = juce::jmin (30, count);
            double sum = 0.0;
            for (int i = 0; i < shortCount; ++i)
                sum += (double) history[(size_t) ((writeIndex - 1 - i + historySize) % historySize)];
            shortLufs = energyToLufs (sum / shortCount);

            // Long term: gated over everything we kept, oldest first.
            std::array<float, historySize> ordered;
            for (int i = 0; i < count; ++i)
                ordered[(size_t) i] = history[(size_t) ((writeIndex - count + i + historySize) % historySize)];
            longLufs = gatedLoudness (ordered.data(), count);
        }

        KWeighting weighting;
        std::array<float, historySize> history {};
        int subLength = 4800, count = 0, writeIndex = 0, accumCount = 0;
        double accum = 0.0;
        float shortLufs = silenceLufs, longLufs = silenceLufs;
    };
}
