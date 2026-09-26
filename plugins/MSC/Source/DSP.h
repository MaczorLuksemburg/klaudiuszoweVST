#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <cmath>
#include <complex>
#include <vector>

namespace msc::dsp
{
    // Butterworth Q per 2-pole stage for 12, 24 and 48 dB/oct.
    inline constexpr std::array<std::array<float, 4>, 3> butterworthQ {{
        { 0.7071068f, 0.0f, 0.0f, 0.0f },
        { 0.5411961f, 1.3065630f, 0.0f, 0.0f },
        { 0.5097956f, 0.6013449f, 0.8999762f, 2.5629154f },
    }};

    inline constexpr std::array<int, 3> stagesForSlope { 1, 2, 4 };

    // Blend of the SVF outputs: shape 0 = low-pass, 0.5 = band-pass (unity peak), 1 = high-pass.
    struct Morph { float lp, bp, hp; };

    inline Morph morphFor (float shape)
    {
        shape = juce::jlimit (0.0f, 1.0f, shape);

        if (shape < 0.5f)
        {
            const float t = shape * 2.0f;
            return { 1.0f - t, t, 0.0f };
        }

        const float t = (shape - 0.5f) * 2.0f;
        return { 0.0f, 1.0f - t, t };
    }

    // Magnitude of the analog prototype, for drawing the filter curve.
    inline float filterMagnitude (float freq, float cutoff, float shape, int slopeIndex)
    {
        slopeIndex = juce::jlimit (0, 2, slopeIndex);
        const auto m = morphFor (shape);
        const std::complex<float> s (0.0f, freq / cutoff);
        std::complex<float> h (1.0f, 0.0f);

        for (int i = 0; i < stagesForSlope[(size_t) slopeIndex]; ++i)
        {
            const float k = 1.0f / butterworthQ[(size_t) slopeIndex][(size_t) i];
            h *= (m.lp + m.bp * k * s + m.hp * s * s) / (s * s + k * s + 1.0f);
        }

        return std::abs (h);
    }

    // Cascade of TPT state-variable filters (Zavalishin / Cytomic) that morphs
    // continuously from low-pass through band-pass to high-pass.
    template <int NumChannels>
    class MorphFilter
    {
    public:
        void reset()
        {
            for (auto& channel : state)
                for (auto& s : channel)
                    s = {};
        }

        void setup (float cutoff, float shape, int slopeIndex, double sampleRate)
        {
            slopeIndex = juce::jlimit (0, 2, slopeIndex);
            numStages = stagesForSlope[(size_t) slopeIndex];
            morph = morphFor (shape);

            const float g = std::tan (juce::MathConstants<float>::pi
                                      * juce::jmin (cutoff, 0.49f * (float) sampleRate) / (float) sampleRate);

            for (int i = 0; i < numStages; ++i)
            {
                auto& c = coeffs[(size_t) i];
                c.k  = 1.0f / butterworthQ[(size_t) slopeIndex][(size_t) i];
                c.a1 = 1.0f / (1.0f + g * (g + c.k));
                c.a2 = g * c.a1;
                c.a3 = g * c.a2;
            }
        }

        float process (int channel, float x)
        {
            auto& channelState = state[(size_t) channel];

            for (int i = 0; i < numStages; ++i)
            {
                const auto& c = coeffs[(size_t) i];
                auto& s = channelState[(size_t) i];

                const float v3 = x - s.ic2;
                const float v1 = c.a1 * s.ic1 + c.a2 * v3;
                const float v2 = s.ic2 + c.a2 * s.ic1 + c.a3 * v3;
                s.ic1 = 2.0f * v1 - s.ic1;
                s.ic2 = 2.0f * v2 - s.ic2;

                const float hp = x - c.k * v1 - v2;
                x = morph.lp * v2 + morph.bp * c.k * v1 + morph.hp * hp;
            }

            return x;
        }

    private:
        struct Coeffs { float k = 1.4142136f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f; };
        struct State  { float ic1 = 0.0f, ic2 = 0.0f; };

        std::array<Coeffs, 4> coeffs {};
        std::array<std::array<State, 4>, NumChannels> state {};
        int numStages = 1;
        Morph morph { 1.0f, 0.0f, 0.0f };
    };

    // Power-of-two ring buffer with fractional reads.
    class DelayLine
    {
    public:
        void prepare (int maxDelaySamples)
        {
            const int size = juce::nextPowerOfTwo (maxDelaySamples + 4);
            buffer.assign ((size_t) size, 0.0f);
            mask = size - 1;
            writePos = 0;
        }

        void clear() { std::fill (buffer.begin(), buffer.end(), 0.0f); }

        void push (float x)
        {
            buffer[(size_t) writePos] = x;
            writePos = (writePos + 1) & mask;
        }

        // Delay in samples after the latest push; valid for any delay >= 0.
        float readLinear (float delay) const
        {
            const float pos = (float) (writePos - 1) - delay;
            const float floorPos = std::floor (pos);
            const int i = (int) floorPos;
            const float frac = pos - floorPos;
            const float a = at (i), b = at (i + 1);
            return a + frac * (b - a);
        }

        // 4-point Hermite interpolation for modulated delays; needs delay >= 2.
        float readHermite (float delay) const
        {
            const float pos = (float) (writePos - 1) - delay;
            const float floorPos = std::floor (pos);
            const int i = (int) floorPos;
            const float f = pos - floorPos;

            const float x0 = at (i - 1), x1 = at (i), x2 = at (i + 1), x3 = at (i + 2);
            const float c1 = 0.5f * (x2 - x0);
            const float c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
            const float c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
            return ((c3 * f + c2) * f + c1) * f + x1;
        }

    private:
        float at (int index) const { return buffer[(size_t) (index & mask)]; }

        std::vector<float> buffer;
        int mask = 0;
        int writePos = 0;
    };

    inline float clip (float x, int mode)
    {
        switch (mode)
        {
            case 1:  return juce::jlimit (-1.0f, 1.0f, x);   // hard
            case 2:  return std::tanh (x);                   // soft
            case 3:  return x / (1.0f + std::abs (x));       // extreme soft: bends from the first dB
            default: return x;
        }
    }
}
