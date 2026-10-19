#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <cmath>

// Stereo width and correlation of the mix and the reference, and the recent samples for a goniometer.
// The audio thread feeds it (only while an editor is open); the editor reads the results.
namespace waveref
{
    class StereoMeter
    {
    public:
        static constexpr int numPoints = 4096;

        void setEnabled (bool shouldRun) { enabled.store (shouldRun); }
        void setSampleRate (double rate) { sampleRate = rate; }

        // Audio thread.
        void push (bool isReference, const float* left, const float* right, int numSamples)
        {
            if (! enabled.load (std::memory_order_relaxed))
                return;

            auto& s = sources[isReference ? 1 : 0];
            const double a = 1.0 - std::exp (-1.0 / (0.4 * sampleRate));   // 400 ms
            constexpr int decimation = 4;

            for (int i = 0; i < numSamples; ++i)
            {
                const double l = left[i], r = right[i];
                const double m = 0.5 * (l + r), sd = 0.5 * (l - r);

                s.ll += a * (l * l - s.ll);
                s.rr += a * (r * r - s.rr);
                s.lr += a * (l * r - s.lr);
                s.mm += a * (m * m - s.mm);
                s.ss += a * (sd * sd - s.ss);

                if (++s.counter >= decimation)
                {
                    s.counter = 0;
                    const int w = s.writeIndex.load (std::memory_order_relaxed);
                    constexpr float root = 0.70710678f;
                    s.points[(size_t) w * 2] = (left[i] - right[i]) * root;        // x: side
                    s.points[(size_t) w * 2 + 1] = (left[i] + right[i]) * root;    // y: mid
                    s.writeIndex.store ((w + 1) % numPoints, std::memory_order_release);
                }
            }

            const double denominator = std::sqrt (s.ll * s.rr);
            s.correlation.store (denominator > 1.0e-12 ? (float) juce::jlimit (-1.0, 1.0, s.lr / denominator) : 0.0f);
            const double total = s.mm + s.ss;
            s.width.store (total > 1.0e-12 ? (float) std::sqrt (s.ss / total) : 0.0f);
            s.level.store ((float) (s.ll + s.rr));
        }

        // Editor thread. Correlation: +1 mono ... 0 unrelated ... -1 out of phase.
        float getCorrelation (bool isReference) const { return sources[isReference ? 1 : 0].correlation.load(); }

        // Share of the signal that is side, 0 (mono) to 1 (side only).
        float getWidth (bool isReference) const       { return sources[isReference ? 1 : 0].width.load(); }
        bool hasSignal (bool isReference) const       { return sources[isReference ? 1 : 0].level.load() > 1.0e-9f; }

        // The newest `count` points as x, y pairs (oldest first); returns how many were copied.
        int readPoints (bool isReference, float* xy, int count) const
        {
            const auto& s = sources[isReference ? 1 : 0];
            count = juce::jmin (count, numPoints);
            const int end = s.writeIndex.load (std::memory_order_acquire);

            for (int i = 0; i < count; ++i)
            {
                const int index = (end - count + i + numPoints) % numPoints;
                xy[i * 2] = s.points[(size_t) index * 2];
                xy[i * 2 + 1] = s.points[(size_t) index * 2 + 1];
            }

            return count;
        }

    private:
        struct Source
        {
            double ll = 0.0, rr = 0.0, lr = 0.0, mm = 0.0, ss = 0.0;
            int counter = 0;
            std::array<float, numPoints * 2> points {};
            std::atomic<int> writeIndex { 0 };
            std::atomic<float> correlation { 0.0f }, width { 0.0f }, level { 0.0f };
        };

        std::array<Source, 2> sources;
        double sampleRate = 44100.0;
        std::atomic<bool> enabled { false };
    };
}
