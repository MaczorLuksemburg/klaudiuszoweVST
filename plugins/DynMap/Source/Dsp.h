#pragma once

#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <vector>

// Small DSP building blocks used by the engine.
namespace dynmap::dsp
{
    //==============================================================================
    // Fast dB conversions for per-sample detector work (error well below 0.001 dB).
    inline float fastLog2 (float x) noexcept
    {
        x = x < 1.0e-30f ? 1.0e-30f : x;
        int exponent = 0;
        const float m = std::frexp (x, &exponent);   // x = m * 2^e, m in [0.5, 1)
        const float t = (m - 0.70710678f) / (m + 0.70710678f);
        const float t2 = t * t;
        // 2 * atanh(t) / ln 2 series.
        const float series = t * (2.8853900817779268f + t2 * (0.9617966939259756f + t2 * (0.5770780163555854f + t2 * 0.41219858311113245f)));
        return (float) exponent - 0.5f + series;
    }

    inline float gainToDb (float gain) noexcept { return 6.0205999132796239f * fastLog2 (gain); }

    inline float dbToGain (float db) noexcept
    {
        if (db < -160.0f)
            return 0.0f;

        return std::exp2 (db * 0.16609640474436813f);
    }

    //==============================================================================
    // Zero-delay-feedback one-pole (TPT). Coefficients are shared, state is per channel.
    struct OnePoleCoeffs
    {
        float G = 0.0f;

        void setCutoff (double freq, double sampleRate)
        {
            const double g = std::tan (juce::MathConstants<double>::pi * juce::jlimit (1.0, sampleRate * 0.49, freq) / sampleRate);
            G = (float) (g / (1.0 + g));
        }
    };

    struct OnePoleState
    {
        float s = 0.0f;

        float lowpass (float x, const OnePoleCoeffs& c) noexcept
        {
            const float v = (x - s) * c.G;
            const float lp = v + s;
            s = lp + v;
            return lp;
        }

        void reset() { s = 0.0f; }
    };

    // Zero-delay-feedback state variable filter (Simper/Zavalishin).
    struct SvfCoeffs
    {
        float k = 1.41421356f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;

        void setup (double freq, double q, double sampleRate)
        {
            const double g = std::tan (juce::MathConstants<double>::pi * juce::jlimit (1.0, sampleRate * 0.49, freq) / sampleRate);
            const double kd = 1.0 / q;
            const double d1 = 1.0 / (1.0 + g * (g + kd));
            k  = (float) kd;
            a1 = (float) d1;
            a2 = (float) (g * d1);
            a3 = (float) (g * g * d1);
        }
    };

    struct SvfState
    {
        float ic1 = 0.0f, ic2 = 0.0f;

        // Returns low-pass; band-pass in bp.
        inline float tick (float x, const SvfCoeffs& c, float& bp) noexcept
        {
            const float v3 = x - ic2;
            const float v1 = c.a1 * ic1 + c.a2 * v3;
            const float v2 = ic2 + c.a2 * ic1 + c.a3 * v3;
            ic1 = 2.0f * v1 - ic1;
            ic2 = 2.0f * v2 - ic2;
            bp = v1;
            return v2;
        }

        float lowpass (float x, const SvfCoeffs& c) noexcept  { float bp; return tick (x, c, bp); }
        float highpass (float x, const SvfCoeffs& c) noexcept { float bp; const float lp = tick (x, c, bp); return x - c.k * bp - lp; }
        float allpass (float x, const SvfCoeffs& c) noexcept  { float bp; tick (x, c, bp); return x - 2.0f * c.k * bp; }

        void reset() { ic1 = ic2 = 0.0f; }
    };

    //==============================================================================
    // Integer delay line for one channel. Length is set once in prepare (no audio-thread allocation).
    class DelayLine
    {
    public:
        void prepare (int maxDelay)
        {
            buffer.assign ((size_t) juce::nextPowerOfTwo (juce::jmax (2, maxDelay + 1)), 0.0f);
            mask = (int) buffer.size() - 1;
            writePos = 0;
        }

        void reset() { std::fill (buffer.begin(), buffer.end(), 0.0f); }

        // Writes x and returns the sample written `delay` samples ago.
        inline float process (float x, int delay) noexcept
        {
            buffer[(size_t) writePos] = x;
            const float out = buffer[(size_t) ((writePos - delay) & mask)];
            writePos = (writePos + 1) & mask;
            return out;
        }

        void process (float* data, int numSamples, int delay) noexcept
        {
            if (delay <= 0)
            {
                for (int i = 0; i < numSamples; ++i)
                {
                    buffer[(size_t) writePos] = data[i];
                    writePos = (writePos + 1) & mask;
                }
                return;
            }

            for (int i = 0; i < numSamples; ++i)
                data[i] = process (data[i], delay);
        }

        int getMaxDelay() const { return mask; }

    private:
        std::vector<float> buffer { 0.0f, 0.0f };
        int mask = 1, writePos = 0;
    };

    //==============================================================================
    // Saturation curves. All have unity slope at zero; drive g pushes the signal in and the
    // output is divided by g, so drive lowers the point where saturation starts instead of
    // making everything louder (except Crush, where drive lowers the bit depth).
    inline float fastTanh (float x) noexcept
    {
        if (x > 3.0f)  return 1.0f;
        if (x < -3.0f) return -1.0f;
        const float x2 = x * x;
        return x * (27.0f + x2) / (27.0f + 9.0f * x2);
    }

    struct Saturator
    {
        int type = 0;
        float drive = 1.0f, invDrive = 1.0f, quant = 32768.0f, invQuant = 1.0f / 32768.0f;
        float tubeOffset = 0.0f, tubeScale = 1.0f;

        static constexpr float tubeBias = 0.25f;

        void setup (int newType, float driveDb)
        {
            type = newType;
            drive = std::pow (10.0f, driveDb / 20.0f);
            invDrive = 1.0f / drive;

            const float bits = 16.0f - driveDb * (14.0f / 36.0f);
            quant = std::exp2 (bits - 1.0f);
            invQuant = 1.0f / quant;

            tubeOffset = std::tanh (tubeBias);
            tubeScale = 1.0f / (1.0f - tubeOffset * tubeOffset);
        }

        inline float process (float x) const noexcept
        {
            switch (type)
            {
                case 1: return fastTanh (x * drive) * invDrive;                                         // tape
                case 2: return (fastTanh (x * drive + tubeBias) - tubeOffset) * tubeScale * invDrive;   // tube (asymmetric)
                case 3: return juce::jlimit (-1.0f, 1.0f, x * drive) * invDrive;                        // hard
                case 4: return std::sin (juce::MathConstants<float>::halfPi * x * drive)
                               * (1.0f / juce::MathConstants<float>::halfPi) * invDrive;                // fold
                case 5: return std::round (x * quant) * invQuant;                                        // crush
                default: return x;
            }
        }
    };

    //==============================================================================
    // Sliding-window minimum (monotonic queue) over the last `length` values.
    class SlidingMin
    {
    public:
        void prepare (int maxLength)
        {
            values.assign ((size_t) maxLength + 2, 1.0f);
            indices.assign ((size_t) maxLength + 2, 0);
            capacity = maxLength + 2;
            reset (1);
        }

        void reset (int newLength)
        {
            length = juce::jlimit (1, capacity - 1, newLength);
            head = tail = 0;
            count = 0;
            time = 0;
        }

        // Changes the window without clearing it (older values expire on the next push).
        void setLength (int newLength) noexcept { length = juce::jlimit (1, capacity - 1, newLength); }

        float push (float v) noexcept
        {
            while (count > 0 && values[(size_t) back()] >= v)
            {
                tail = (tail - 1 + capacity) % capacity;
                --count;
            }

            values[(size_t) tail] = v;
            indices[(size_t) tail] = time;
            tail = (tail + 1) % capacity;
            ++count;

            while (indices[(size_t) head] <= time - length)
            {
                head = (head + 1) % capacity;
                --count;
            }

            ++time;
            return values[(size_t) head];
        }

    private:
        int back() const { return (tail - 1 + capacity) % capacity; }

        std::vector<float> values;
        std::vector<int64_t> indices;
        int capacity = 2, length = 1, head = 0, tail = 0, count = 0;
        int64_t time = 0;
    };

    // Sliding-window maximum.
    class SlidingMax
    {
    public:
        void prepare (int maxLength) { minimum.prepare (maxLength); }
        void reset (int length)      { minimum.reset (length); }
        void setLength (int length)  { minimum.setLength (length); }
        float push (float v) noexcept { return -minimum.push (-v); }

    private:
        SlidingMin minimum;
    };

    // Moving average over the last `length` values (running sum in double).
    class MovingAverage
    {
    public:
        void prepare (int maxLength) { history.assign ((size_t) maxLength + 1, 1.0f); reset (1, 1.0f); }

        void reset (int newLength, float value)
        {
            length = juce::jlimit (1, (int) history.size(), newLength);
            std::fill (history.begin(), history.end(), value);
            sum = (double) value * length;
            pos = 0;
        }

        float push (float v) noexcept
        {
            sum += (double) v - (double) history[(size_t) pos];
            history[(size_t) pos] = v;
            pos = (pos + 1) % length;
            return (float) (sum / length);
        }

    private:
        std::vector<float> history { 1.0f };
        double sum = 1.0;
        int length = 1, pos = 0;
    };

    //==============================================================================
    // Estimates inter-sample peaks with a 4x polyphase windowed-sinc interpolator
    // (the ITU BS.1770 true-peak idea). Output is delayed by `delay` samples.
    class TruePeakDetector
    {
    public:
        static constexpr int halfTaps = 8;
        static constexpr int delay = halfTaps;

        TruePeakDetector()
        {
            for (int phase = 1; phase < 4; ++phase)
            {
                const double frac = phase / 4.0;
                double sum = 0.0;

                for (int k = 0; k < 2 * halfTaps; ++k)
                {
                    const double t = (double) (k - halfTaps + 1) - frac;   // tap offset from the interpolated point
                    const double sinc = std::abs (t) < 1.0e-9 ? 1.0 : std::sin (juce::MathConstants<double>::pi * t) / (juce::MathConstants<double>::pi * t);
                    const double w = 0.5 + 0.5 * std::cos (juce::MathConstants<double>::pi * t / (halfTaps + 0.5));
                    coeffs[(size_t) phase - 1][(size_t) k] = (float) (sinc * w);
                    sum += sinc * w;
                }

                for (auto& c : coeffs[(size_t) phase - 1])
                    c = (float) (c / sum);
            }
        }

        void reset() { history.fill (0.0f); pos = 0; }

        // Returns the true-peak estimate of the sample `delay` samples ago.
        float process (float x) noexcept
        {
            history[(size_t) pos] = history[(size_t) pos + historySize] = x;
            pos = (pos + 1) % historySize;

            // history[pos .. pos + 2*halfTaps - 1] is oldest..newest of the last 16 samples.
            const float* h = history.data() + pos;
            float peak = std::abs (h[halfTaps - 1]);

            for (const auto& phase : coeffs)
            {
                float v = 0.0f;
                for (int k = 0; k < 2 * halfTaps; ++k)
                    v += phase[(size_t) k] * h[k];
                peak = juce::jmax (peak, std::abs (v));
            }

            return peak;
        }

    private:
        static constexpr int historySize = 2 * halfTaps;
        std::array<std::array<float, 2 * halfTaps>, 3> coeffs {};
        std::array<float, 2 * historySize> history {};
        int pos = 0;
    };

    //==============================================================================
    // BS.1770 loudness (K-weighting + mean square over a sliding window) for meters and auto gain.
    class KWeighting
    {
    public:
        void prepare (double sampleRate)
        {
            // Pre-filter (high shelf) and RLB high-pass, designed per sample rate (from libebur128).
            {
                const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
                const double K = std::tan (juce::MathConstants<double>::pi * f0 / sampleRate);
                const double Vh = std::pow (10.0, G / 20.0), Vb = std::pow (Vh, 0.4996667741545416);
                const double a0 = 1.0 + K / Q + K * K;
                shelf = { (Vh + Vb * K / Q + K * K) / a0, 2.0 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0,
                          2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0 };
            }
            {
                const double f0 = 38.13547087602444, Q = 0.5003270373238773;
                const double K = std::tan (juce::MathConstants<double>::pi * f0 / sampleRate);
                const double a0 = 1.0 + K / Q + K * K;
                highpass = { 1.0, -2.0, 1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0 };
            }
            reset();
        }

        void reset() { for (auto& s : states) s = {}; }

        // Mean-square contribution of one stereo sample.
        float process (float left, float right) noexcept
        {
            const double l = highpass.tick (shelf.tick (left, states[0]), states[1]);
            const double r = highpass.tick (shelf.tick (right, states[2]), states[3]);
            return (float) (l * l + r * r);
        }

    private:
        struct State { double x1 = 0, x2 = 0, y1 = 0, y2 = 0; };

        struct Biquad
        {
            double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;

            double tick (double x, State& s) const noexcept
            {
                const double y = b0 * x + b1 * s.x1 + b2 * s.x2 - a1 * s.y1 - a2 * s.y2;
                s.x2 = s.x1; s.x1 = x;
                s.y2 = s.y1; s.y1 = y;
                return y;
            }
        };

        Biquad shelf, highpass;
        std::array<State, 4> states {};
    };

    // Short-term loudness (3 s window) in LUFS, updated every 100 ms.
    class LoudnessMeter
    {
    public:
        void prepare (double sampleRate)
        {
            weighting.prepare (sampleRate);
            blockLength = juce::jmax (1, (int) (sampleRate * 0.1));
            reset();
        }

        void reset()
        {
            weighting.reset();
            blocks.fill (0.0);
            blockSum = 0.0;
            blockCount = 0;
            blockIndex = 0;
            lufs.store (-100.0f);
        }

        void process (const float* left, const float* right, int numSamples) noexcept
        {
            for (int i = 0; i < numSamples; ++i)
            {
                blockSum += weighting.process (left[i], right[i]);

                if (++blockCount >= blockLength)
                {
                    blocks[(size_t) blockIndex] = blockSum / blockLength;
                    blockIndex = (blockIndex + 1) % (int) blocks.size();
                    blockSum = 0.0;
                    blockCount = 0;

                    double mean = 0.0;
                    for (double b : blocks)
                        mean += b;
                    mean /= (double) blocks.size();

                    lufs.store (mean > 1.0e-12 ? (float) (-0.691 + 10.0 * std::log10 (mean)) : -100.0f);
                }
            }
        }

        float getLufs() const { return lufs.load (std::memory_order_relaxed); }

    private:
        KWeighting weighting;
        std::array<double, 30> blocks {};
        double blockSum = 0.0;
        int blockLength = 4800, blockCount = 0, blockIndex = 0;
        std::atomic<float> lufs { -100.0f };
    };
}

namespace dynmap
{
    using dsp::OnePoleCoeffs;
    using dsp::OnePoleState;
    using dsp::SvfCoeffs;
    using dsp::SvfState;
}
