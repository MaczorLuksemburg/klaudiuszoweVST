#pragma once

#include <juce_dsp/juce_dsp.h>

namespace msc
{
    // Lock-free tap from the audio thread plus a cheap FFT for the filter displays.
    // The audio thread only copies samples while a display is open and its module is on.
    class SpectrumAnalyzer
    {
    public:
        static constexpr int fftOrder = 11;
        static constexpr int fftSize  = 1 << fftOrder;
        static constexpr int numBins  = fftSize / 2 + 1;
        static constexpr float floorDb = -120.0f;

        SpectrumAnalyzer()
            : fft (fftOrder),
              window ((size_t) fftSize, juce::dsp::WindowingFunction<float>::hann, false)
        {
            fifoBuffer.resize ((size_t) fifo.getTotalSize());
            history.resize ((size_t) fftSize, 0.0f);
            fftData.resize ((size_t) fftSize * 2, 0.0f);
            levels.resize ((size_t) numBins, floorDb);
        }

        void setEnabled (bool shouldBeEnabled) { enabled.store (shouldBeEnabled); }
        void setSampleRate (double rate)        { sampleRate.store (rate); }

        // Audio thread.
        void push (const float* samples, int numSamples)
        {
            if (! enabled.load (std::memory_order_relaxed))
                return;

            const auto scope = fifo.write (numSamples);

            if (scope.blockSize1 > 0)
                std::copy (samples, samples + scope.blockSize1, fifoBuffer.begin() + scope.startIndex1);

            if (scope.blockSize2 > 0)
                std::copy (samples + scope.blockSize1, samples + scope.blockSize1 + scope.blockSize2,
                           fifoBuffer.begin() + scope.startIndex2);
        }

        // Message thread. Returns true if the levels changed and need a repaint.
        bool process()
        {
            const int ready = fifo.getNumReady();

            if (ready == 0)
            {
                bool changed = false;

                for (auto& level : levels)
                {
                    if (level > floorDb)
                    {
                        level = juce::jmax (floorDb, level - decayDbPerFrame);
                        changed = true;
                    }
                }

                return changed;
            }

            {
                const auto scope = fifo.read (ready);
                auto append = [this] (int start, int size)
                {
                    for (int i = 0; i < size; ++i)
                    {
                        history[(size_t) historyPos] = fifoBuffer[(size_t) (start + i)];
                        historyPos = (historyPos + 1) % fftSize;
                    }
                };

                append (scope.startIndex1, scope.blockSize1);
                append (scope.startIndex2, scope.blockSize2);
            }

            for (int i = 0; i < fftSize; ++i)
                fftData[(size_t) i] = history[(size_t) ((historyPos + i) % fftSize)];

            std::fill (fftData.begin() + fftSize, fftData.end(), 0.0f);
            window.multiplyWithWindowingTable (fftData.data(), (size_t) fftSize);
            fft.performFrequencyOnlyForwardTransform (fftData.data());

            constexpr float scale = 4.0f / (float) fftSize; // Hann coherent gain

            for (int bin = 0; bin < numBins; ++bin)
            {
                const float db = juce::Decibels::gainToDecibels (fftData[(size_t) bin] * scale, floorDb);
                auto& level = levels[(size_t) bin];
                level = juce::jmax (db, level - decayDbPerFrame);
            }

            return true;
        }

        // Smoothed level in dB at a frequency, tilted +4.5 dB/oct so a typical mix reads flat.
        float getLevelAt (float freq) const
        {
            const float binPos = freq * (float) fftSize / (float) sampleRate.load();
            const int bin = juce::jlimit (0, numBins - 2, (int) binPos);
            const float frac = juce::jlimit (0.0f, 1.0f, binPos - (float) bin);
            const float db = levels[(size_t) bin] + frac * (levels[(size_t) bin + 1] - levels[(size_t) bin]);
            return db + 4.5f * std::log2 (juce::jmax (freq, 20.0f) / 1000.0f);
        }

    private:
        static constexpr float decayDbPerFrame = 1.8f;

        juce::dsp::FFT fft;
        juce::dsp::WindowingFunction<float> window;

        juce::AbstractFifo fifo { 8192 };
        std::vector<float> fifoBuffer, history, fftData, levels;
        int historyPos = 0;

        std::atomic<bool> enabled { false };
        std::atomic<double> sampleRate { 44100.0 };
    };

    // Scrolling min/max history of the dynamic-pan modulation before and after the mod clipper.
    class ModScope
    {
    public:
        struct Column { float preMin = 0, preMax = 0, postMin = 0, postMax = 0; };

        static constexpr int numColumns = 320;
        static constexpr double secondsPerColumn = 0.008;   // ~2.5 s of history

        ModScope() { fifoColumns.resize ((size_t) fifo.getTotalSize()); }

        void setEnabled (bool shouldBeEnabled) { enabled.store (shouldBeEnabled); }
        bool isEnabled() const                 { return enabled.load (std::memory_order_relaxed); }

        // Audio thread.
        void prepare (double sampleRate)
        {
            samplesPerColumn = juce::jmax (1, (int) (sampleRate * secondsPerColumn));
            counter = 0;
            current = {};
        }

        void push (float pre, float post)
        {
            if (counter == 0)
                current = { pre, pre, post, post };

            current.preMin  = juce::jmin (current.preMin, pre);
            current.preMax  = juce::jmax (current.preMax, pre);
            current.postMin = juce::jmin (current.postMin, post);
            current.postMax = juce::jmax (current.postMax, post);

            if (++counter >= samplesPerColumn)
            {
                counter = 0;
                const auto scope = fifo.write (1);
                if (scope.blockSize1 > 0)
                    fifoColumns[(size_t) scope.startIndex1] = current;
            }
        }

        // Message thread. Returns true when new columns arrived.
        bool process()
        {
            const int ready = fifo.getNumReady();
            if (ready == 0)
                return false;

            const auto scope = fifo.read (ready);
            auto append = [this] (int start, int size)
            {
                for (int i = 0; i < size; ++i)
                {
                    history[(size_t) writeIndex] = fifoColumns[(size_t) (start + i)];
                    writeIndex = (writeIndex + 1) % numColumns;
                }
            };

            append (scope.startIndex1, scope.blockSize1);
            append (scope.startIndex2, scope.blockSize2);
            return true;
        }

        // 0 = oldest, numColumns - 1 = newest.
        const Column& getColumn (int index) const { return history[(size_t) ((writeIndex + index) % numColumns)]; }

    private:
        juce::AbstractFifo fifo { 512 };
        std::vector<Column> fifoColumns;
        std::array<Column, numColumns> history {};
        int writeIndex = 0;

        int samplesPerColumn = 384, counter = 0;
        Column current;
        std::atomic<bool> enabled { false };
    };
}
