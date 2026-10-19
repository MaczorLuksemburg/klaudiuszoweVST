#pragma once

#include <juce_dsp/juce_dsp.h>

#include <array>

// Mid and side spectra of two stereo signals (the mix in the DAW and the loaded reference) side by side.
//
// The audio thread pushes samples into lock-free FIFOs; the message thread (editor timer) runs the FFTs.
// Levels are noise-normalised: power per bin divided by the window energy, so white noise of RMS 1 reads 0 dB
// whatever the FFT size, and changing the resolution never shifts the curves up or down. Mid = (L + R) / 2 and
// side = (L - R) / 2, so a mono signal has no side at all.
namespace waveref
{
    class SpectrumAnalyser
    {
    public:
        enum Channel { mixMid, mixSide, refMid, refSide, numChannels };

        static constexpr int minOrder = 11;
        static constexpr int maxOrder = 15;
        static constexpr int maxFft = 1 << maxOrder;
        static constexpr float floorDb = -140.0f;

        SpectrumAnalyser()
        {
            for (int c = 0; c < numChannels; ++c)
            {
                fifos[(size_t) c] = std::make_unique<juce::AbstractFifo> (fifoSize);
                fifoData[(size_t) c].assign ((size_t) fifoSize, 0.0f);
                history[(size_t) c].assign ((size_t) maxFft, 0.0f);
                average[(size_t) c].assign ((size_t) maxFft / 2 + 1, 0.0f);
                prefix[(size_t) c].assign ((size_t) maxFft / 2 + 2, 0.0);
            }

            fftData.assign ((size_t) maxFft * 2, 0.0f);
            setOrder (12);
        }

        void setSampleRate (double rate) { sampleRate.store (rate); }
        void setEnabled (bool shouldRun) { enabled.store (shouldRun); }

        // ---- audio thread --------------------------------------------------------------------------

        void push (bool isReference, const float* left, const float* right, int numSamples)
        {
            if (! enabled.load (std::memory_order_relaxed))
                return;

            const int midIndex = isReference ? refMid : mixMid;
            auto& midFifo = *fifos[(size_t) midIndex];
            auto& sideFifo = *fifos[(size_t) midIndex + 1];

            if (midFifo.getFreeSpace() < numSamples || sideFifo.getFreeSpace() < numSamples)
                return;   // the editor isn't keeping up: drop the block rather than skew mid against side

            write (midFifo, fifoData[(size_t) midIndex], left, right, numSamples, 0.5f);
            write (sideFifo, fifoData[(size_t) midIndex + 1], left, right, numSamples, -0.5f);
        }

        // ---- message thread ------------------------------------------------------------------------

        void setOrder (int order)
        {
            order = juce::jlimit (minOrder, maxOrder, order);
            if (order == currentOrder && fft != nullptr)
                return;

            currentOrder = order;
            fftSize = 1 << order;
            fft = std::make_unique<juce::dsp::FFT> (order);

            window.resize ((size_t) fftSize);
            juce::dsp::WindowingFunction<float>::fillWindowingTables (window.data(), (size_t) fftSize,
                                                                       juce::dsp::WindowingFunction<float>::hann, false);
            windowEnergy = 0.0;
            for (auto w : window)
                windowEnergy += (double) w * w;

            firstFrame.fill (true);
        }

        int getFftSize() const   { return fftSize; }
        int getOrder() const     { return currentOrder; }
        double getSampleRate() const { return sampleRate.load(); }

        // How fast the curves react, in seconds. A bigger FFT already looks further back in time (its window adds
        // about 0.54 x size / rate to the time a curve needs to follow a change), so the extra averaging is
        // shortened by that much: the response time stays about the same at every FFT size, down to the minimum
        // the window itself allows (about 0.37 s at 32768 samples and 48 kHz).
        void setAveragingSeconds (float seconds) { averaging = juce::jmax (0.001f, seconds); }
        void setFrozen (bool shouldFreeze)       { frozen = shouldFreeze; }

        void reset()
        {
            for (int c = 0; c < numChannels; ++c)
            {
                std::fill (history[(size_t) c].begin(), history[(size_t) c].end(), 0.0f);
                std::fill (average[(size_t) c].begin(), average[(size_t) c].end(), 0.0f);
                std::fill (prefix[(size_t) c].begin(), prefix[(size_t) c].end(), 0.0);
                fifos[(size_t) c]->reset();
            }

            firstFrame.fill (true);
        }

        // Drains the FIFOs and updates the averaged spectra. Returns true when something changed.
        bool process()
        {
            bool changed = false;

            for (int c = 0; c < numChannels; ++c)
            {
                auto& fifo = *fifos[(size_t) c];
                const int ready = fifo.getNumReady();

                // With no new audio there is nothing to do, unless the FFT size just changed: the old curves
                // don't fit the new bins, so redo them from the history we already hold.
                if (ready == 0 && ! firstFrame[(size_t) c])
                    continue;

                if (ready > 0)
                {
                    auto& ring = history[(size_t) c];
                    auto& pos = writePos[(size_t) c];
                    const auto scope = fifo.read (ready);

                    auto append = [&] (int start, int size)
                    {
                        for (int i = 0; i < size; ++i)
                        {
                            ring[(size_t) pos] = fifoData[(size_t) c][(size_t) (start + i)];
                            pos = (pos + 1) & (maxFft - 1);
                        }
                    };

                    append (scope.startIndex1, scope.blockSize1);
                    append (scope.startIndex2, scope.blockSize2);
                }

                if (frozen)
                    continue;

                updateSpectrum (c, (double) ready / juce::jmax (1.0, sampleRate.load()));
                changed = true;
            }

            return changed;
        }

        // Curve in dB for `numPoints` log-spaced frequencies from fMin to fMax. `smoothingDivisor` N averages the
        // power over 1/N octave (0 = no smoothing: the loudest bin between neighbouring points, so narrow peaks
        // like a sine never fall between two points). `tiltDbPerOct` is added around 1 kHz so a typical mix reads
        // roughly flat.
        void getCurve (int channel, int numPoints, float fMin, float fMax, int smoothingDivisor,
                       float tiltDbPerOct, float* out) const
        {
            const auto& avg = average[(size_t) channel];
            const auto& sums = prefix[(size_t) channel];
            const double binWidth = sampleRate.load() / (double) fftSize;
            const int lastBin = fftSize / 2;

            // Each point owns the stretch of spectrum up to its neighbours, whatever the smoothing.
            const double cellHalf = numPoints > 1 ? std::pow ((double) fMax / fMin, 0.5 / (numPoints - 1)) : 1.0;
            const double halfBand = smoothingDivisor > 0 ? juce::jmax (cellHalf, std::pow (2.0, 0.5 / smoothingDivisor)) : cellHalf;

            for (int i = 0; i < numPoints; ++i)
            {
                const double t = numPoints > 1 ? (double) i / (numPoints - 1) : 0.0;
                const double f = (double) fMin * std::pow ((double) fMax / fMin, t);

                const double lo = f / halfBand / binWidth;
                const double hi = f * halfBand / binWidth;
                const int k0 = juce::jlimit (0, lastBin, (int) std::ceil (lo));
                const int k1 = juce::jlimit (0, lastBin, (int) std::floor (hi));
                double power;

                if (k1 <= k0)
                {
                    // Less than two bins in reach (the bass): interpolate instead of drawing steps.
                    const double centre = juce::jlimit (0.0, (double) lastBin - 1.0, f / binWidth);
                    const int k = (int) centre;
                    const double frac = centre - k;
                    power = (double) avg[(size_t) k] * (1.0 - frac) + (double) avg[(size_t) k + 1] * frac;
                }
                else if (smoothingDivisor > 0)
                {
                    power = (sums[(size_t) k1 + 1] - sums[(size_t) k0]) / (double) (k1 - k0 + 1);
                }
                else
                {
                    power = 0.0;
                    for (int k = k0; k <= k1; ++k)
                        power = juce::jmax (power, (double) avg[(size_t) k]);
                }

                const float db = power > 1.0e-14 ? (float) (10.0 * std::log10 (power)) : floorDb;
                out[i] = juce::jmax (floorDb, db + tiltDbPerOct * (float) std::log2 (f / 1000.0));
            }
        }

    private:
        static constexpr int fifoSize = 1 << 16;

        static void write (juce::AbstractFifo& fifo, std::vector<float>& data, const float* left, const float* right,
                           int numSamples, float sideSign)
        {
            const auto scope = fifo.write (numSamples);

            for (int i = 0; i < scope.blockSize1; ++i)
                data[(size_t) (scope.startIndex1 + i)] = 0.5f * left[i] + sideSign * right[i];

            for (int i = 0; i < scope.blockSize2; ++i)
                data[(size_t) (scope.startIndex2 + i)] = 0.5f * left[scope.blockSize1 + i] + sideSign * right[scope.blockSize1 + i];
        }

        void updateSpectrum (int c, double seconds)
        {
            const auto& ring = history[(size_t) c];
            const int start = (writePos[(size_t) c] - fftSize + maxFft) & (maxFft - 1);

            for (int i = 0; i < fftSize; ++i)
                fftData[(size_t) i] = ring[(size_t) ((start + i) & (maxFft - 1))] * window[(size_t) i];

            std::fill (fftData.begin() + fftSize, fftData.begin() + 2 * fftSize, 0.0f);
            fft->performFrequencyOnlyForwardTransform (fftData.data());

            // Measured: the time to reach half of a step is about 0.69 tau + 0.54 N / rate.
            const double windowSeconds = (double) fftSize / juce::jmax (1.0, sampleRate.load());
            const double tau = juce::jmax (0.01, (double) averaging - 0.78 * windowSeconds);
            const double alpha = firstFrame[(size_t) c] ? 1.0 : 1.0 - std::exp (-seconds / tau);
            firstFrame[(size_t) c] = false;

            auto& avg = average[(size_t) c];
            auto& sums = prefix[(size_t) c];
            const int numBins = fftSize / 2 + 1;
            double running = 0.0;
            sums[0] = 0.0;

            for (int k = 0; k < numBins; ++k)
            {
                const double magnitude = (double) fftData[(size_t) k];
                const double power = magnitude * magnitude / windowEnergy;
                avg[(size_t) k] += (float) (alpha * (power - (double) avg[(size_t) k]));
                running += (double) avg[(size_t) k];
                sums[(size_t) k + 1] = running;
            }
        }

        std::array<std::unique_ptr<juce::AbstractFifo>, numChannels> fifos;
        std::array<std::vector<float>, numChannels> fifoData, history, average;
        std::array<std::vector<double>, numChannels> prefix;
        std::array<int, numChannels> writePos {};
        std::array<bool, numChannels> firstFrame {};

        std::unique_ptr<juce::dsp::FFT> fft;
        std::vector<float> window, fftData;
        double windowEnergy = 1.0;
        int currentOrder = 0, fftSize = 0;
        std::atomic<double> sampleRate { 44100.0 };
        float averaging = 0.3f;
        bool frozen = false;

        std::atomic<bool> enabled { false };
    };
}
