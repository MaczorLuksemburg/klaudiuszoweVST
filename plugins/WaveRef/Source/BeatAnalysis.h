#pragma once

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <vector>

// Tempo and first-beat estimate for a loaded track: spectral-flux onset curve, autocorrelation for a rough
// tempo, a fine search over tempo and phase on the whole track, then the beat positions are nudged onto the
// actual attacks. It is made for steady, four-on-the-floor style material; for anything else the user can type,
// tap or halve/double the tempo.
namespace waveref
{
    struct TempoGuess
    {
        double bpm = 0.0;        // 0 = no estimate
        double downbeat = 0.0;   // seconds: a beat that starts a bar (the first one in the file)
        float confidence = 0.0f; // 0..1, rough
    };

    namespace beats
    {
        // Onset strength (one value per hop) of the mono mix.
        inline std::vector<float> onsetCurve (const juce::AudioBuffer<float>& audio, double sampleRate, int hop, int fftOrder, int maxSamples)
        {
            const int fftSize = 1 << fftOrder;
            const int numFrames = (maxSamples - fftSize) / hop;
            std::vector<float> flux ((size_t) juce::jmax (0, numFrames), 0.0f);

            if (numFrames < 8)
                return flux;

            juce::dsp::FFT fft (fftOrder);
            std::vector<float> window ((size_t) fftSize), data ((size_t) fftSize * 2);
            juce::dsp::WindowingFunction<float>::fillWindowingTables (window.data(), (size_t) fftSize,
                                                                       juce::dsp::WindowingFunction<float>::hann, false);

            const int maxBin = juce::jmin (fftSize / 2, (int) (5000.0 * fftSize / sampleRate));
            std::vector<float> previous ((size_t) maxBin + 1, 0.0f);
            const float* left = audio.getReadPointer (0);
            const float* right = audio.getReadPointer (juce::jmin (1, audio.getNumChannels() - 1));

            for (int frame = 0; frame < numFrames; ++frame)
            {
                const int start = frame * hop;

                for (int i = 0; i < fftSize; ++i)
                    data[(size_t) i] = 0.5f * (left[start + i] + right[start + i]) * window[(size_t) i];

                std::fill (data.begin() + fftSize, data.end(), 0.0f);
                fft.performFrequencyOnlyForwardTransform (data.data());

                float sum = 0.0f;

                for (int k = 1; k <= maxBin; ++k)
                {
                    const float level = std::log1p (data[(size_t) k] * 0.05f);
                    sum += juce::jmax (0.0f, level - previous[(size_t) k]);
                    previous[(size_t) k] = level;
                }

                flux[(size_t) frame] = sum;
            }

            // Remove the slow trend and keep what rises above it.
            const int span = 50;
            std::vector<float> trend (flux.size(), 0.0f);
            double running = 0.0;

            for (int i = 0; i < (int) flux.size(); ++i)
            {
                running += flux[(size_t) i];
                if (i >= span) running -= flux[(size_t) (i - span)];
                trend[(size_t) i] = (float) (running / juce::jmin (i + 1, span));
            }

            for (size_t i = 0; i < flux.size(); ++i)
                flux[i] = juce::jmax (0.0f, flux[i] - trend[i]);

            return flux;
        }

        inline float sampleAt (const std::vector<float>& values, double position)
        {
            if (position < 0.0 || position >= (double) values.size() - 1.0)
                return 0.0f;

            const int i = (int) position;
            const float frac = (float) (position - i);
            return values[(size_t) i] + frac * (values[(size_t) i + 1] - values[(size_t) i]);
        }
    }

    inline TempoGuess detectTempo (const juce::AudioBuffer<float>& audio, double sampleRate)
    {
        TempoGuess result;
        const int length = audio.getNumSamples();

        if (audio.getNumChannels() < 1 || length < (int) (sampleRate * 8.0))
            return result;

        const int fftOrder = sampleRate > 64000.0 ? 11 : 10;
        const int fftSize = 1 << fftOrder;
        const int hop = juce::jmax (64, juce::roundToInt (sampleRate * 0.010));
        const double hopSeconds = (double) hop / sampleRate;
        const int maxSamples = juce::jmin (length, (int) (sampleRate * 300.0));

        const auto onset = beats::onsetCurve (audio, sampleRate, hop, fftOrder, maxSamples);
        const int numFrames = (int) onset.size();

        if (numFrames < 600)
            return result;

        // Rough tempo: autocorrelation of the onset curve with a prior around 125 BPM, adding the lags of two and
        // four beats so that a beat is preferred over its subdivisions.
        const int minLag = juce::jmax (2, (int) std::floor (60.0 / 200.0 / hopSeconds));
        const int maxLag = (int) std::ceil (60.0 / 60.0 / hopSeconds);
        const int maxAcfLag = juce::jmin (numFrames / 2, maxLag * 4);

        std::vector<double> acf ((size_t) maxAcfLag + 1, 0.0);

        for (int lag = minLag; lag <= maxAcfLag; ++lag)
        {
            double sum = 0.0;
            for (int i = 0; i + lag < numFrames; ++i)
                sum += (double) onset[(size_t) i] * (double) onset[(size_t) (i + lag)];

            acf[(size_t) lag] = sum / (double) (numFrames - lag);
        }

        double bestScore = 0.0, meanScore = 0.0;
        int bestLag = 0, counted = 0;

        for (int lag = minLag; lag <= maxLag; ++lag)
        {
            double score = acf[(size_t) lag];
            if (2 * lag <= maxAcfLag) score += 0.5 * acf[(size_t) (2 * lag)];
            if (4 * lag <= maxAcfLag) score += 0.25 * acf[(size_t) (4 * lag)];

            const double bpm = 60.0 / ((double) lag * hopSeconds);
            const double octaves = std::log2 (bpm / 125.0);
            score *= std::exp (-0.5 * (octaves / 0.9) * (octaves / 0.9));

            meanScore += score;
            ++counted;

            if (score > bestScore)
            {
                bestScore = score;
                bestLag = lag;
            }
        }

        if (bestLag == 0 || bestScore <= 0.0)
            return result;

        meanScore /= juce::jmax (1, counted);
        const double roughBpm = 60.0 / ((double) bestLag * hopSeconds);

        // Fine search over tempo and phase on the whole curve.
        double bestBpm = roughBpm, bestPhase = 0.0, bestGrid = -1.0;

        for (double bpm = roughBpm - 1.5; bpm <= roughBpm + 1.5; bpm += 0.01)
        {
            const double period = 60.0 / bpm / hopSeconds;

            for (double phase = 0.0; phase < period; phase += 0.5)
            {
                double sum = 0.0;
                for (double position = phase; position < numFrames - 1; position += period)
                    sum += beats::sampleAt (onset, position);

                if (sum > bestGrid)
                {
                    bestGrid = sum;
                    bestBpm = bpm;
                    bestPhase = phase;
                }
            }
        }

        const double period = 60.0 / bestBpm / hopSeconds;

        // Which beat starts a bar: the one of four whose onsets are strongest.
        double barSum[4] {};
        int index = 0;

        for (double position = bestPhase; position < numFrames - 1; position += period, ++index)
            barSum[index % 4] += beats::sampleAt (onset, position);

        int barPhase = 0;
        for (int i = 1; i < 4; ++i)
            if (barSum[i] > barSum[barPhase]) barPhase = i;

        // The onset curve peaks a little before the middle of its window.
        const double windowDelay = 0.42 * fftSize;
        double downbeatSamples = (bestPhase + barPhase * period) * hop + windowDelay;

        // Nudge the grid onto the real attacks: the attack of the loud low end near every beat, median offset.
        {
            const float* left = audio.getReadPointer (0);
            const float* right = audio.getReadPointer (juce::jmin (1, audio.getNumChannels() - 1));
            const double beatSamples = 60.0 / bestBpm * sampleRate;
            const int searchBefore = (int) (0.030 * sampleRate), searchAfter = (int) (0.030 * sampleRate);
            const int slope = juce::jmax (1, (int) (0.002 * sampleRate));
            const float coefficient = 1.0f - std::exp (-2.0f * juce::MathConstants<float>::pi * 300.0f / (float) sampleRate);

            std::vector<double> residuals;
            std::vector<float> envelope;
            const int span = searchBefore + searchAfter + slope + 1;

            for (int beat = 0; beat < 400; ++beat)
            {
                const double centre = downbeatSamples + (double) beat * beatSamples;
                const int first = (int) centre - searchBefore - slope;

                if (first < 0 || first + span + slope >= maxSamples)
                    continue;

                envelope.assign ((size_t) span + (size_t) slope, 0.0f);
                float state = 0.0f;

                for (int i = 0; i < (int) envelope.size(); ++i)
                {
                    const float x = std::abs (0.5f * (left[first + i] + right[first + i]));
                    state += coefficient * (x - state);
                    envelope[(size_t) i] = state;
                }

                float bestRise = 0.0f;
                int bestIndex = -1;

                for (int i = slope; i < (int) envelope.size(); ++i)
                {
                    const float rise = envelope[(size_t) i] - envelope[(size_t) (i - slope)];
                    if (rise > bestRise) { bestRise = rise; bestIndex = i; }
                }

                if (bestIndex >= 0 && bestRise > 0.01f)
                    residuals.push_back ((double) (first + bestIndex - slope / 2) - centre);
            }

            if (residuals.size() >= 8)
            {
                std::sort (residuals.begin(), residuals.end());
                const double median = residuals[residuals.size() / 2];

                if (std::abs (median) < 0.03 * sampleRate)
                    downbeatSamples += median;
            }
        }

        // The first bar in the file, not the first one found by the phase search.
        const double barSamples = 4.0 * 60.0 / bestBpm * sampleRate;
        downbeatSamples = std::fmod (downbeatSamples, barSamples);
        if (downbeatSamples < 0.0) downbeatSamples += barSamples;

        result.bpm = std::round (bestBpm * 100.0) / 100.0;
        result.downbeat = downbeatSamples / sampleRate;
        result.confidence = (float) juce::jlimit (0.0, 1.0, (bestScore / juce::jmax (1.0e-12, meanScore) - 1.0) / 6.0);
        return result;
    }
}
