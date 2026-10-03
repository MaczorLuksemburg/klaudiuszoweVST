#pragma once

// Black-box measurement of a dynamics processor from a rendered test signal, used to compare
// DynMap with plugins that can't be hosted here (e.g. FL Studio's Maximus):
//   DynMap_Tests --kit <file.wav>                         writes the test signal (48 kHz, 24 bit)
//   DynMap_Tests --compare <render.wav> [--preset <name>] [--out <dynmap.wav>]
// The render is the kit processed by the other plugin (any sample rate). The same kit goes through
// DynMap with the given preset (factory or user preset name, default Init) and both are measured:
// static curve at 100 Hz / 1 kHz / 8 kHz, attack/release on level jumps, and the frequency
// response at a low level.

#include <juce_audio_formats/juce_audio_formats.h>
#include <iostream>
#include <limits>
#include <map>

namespace measure
{
    // Kit layout, in seconds.
    constexpr double clickAt = 0.5;
    constexpr double stepLength = 0.4;
    constexpr int numSteps = 21;                        // -60 .. 0 dBFS in 3 dB steps (sine peak level)
    struct Staircase { const char* name; float freq; double start; };
    constexpr Staircase staircases[] { { "1 kHz", 1000.0f, 2.0 }, { "100 Hz", 100.0f, 11.4 }, { "8 kHz", 8000.0f, 20.8 },
                                       { "40 Hz", 40.0f, 50.6 } };   // added later, at the end, so older renders still line up
    constexpr double timingStart = 30.2;                // 3 x (1 s at -30 dB, 1 s at -6 dB), then 1 s at -30 dB, 1 kHz
    constexpr int timingCycles = 3;
    constexpr double sweepStart = 38.2, sweepLength = 6.0, sweepLevelDb = -40.0;
    constexpr double drumsStart = 45.2, drumsLength = 4.0;
    constexpr double kitLength = 60.0;

    inline float stepLevelDb (int step) { return -60.0f + 3.0f * (float) step; }

    inline double sweepFreqAt (double t)   // log sweep 20 Hz -> 20 kHz
    {
        return 20.0 * std::pow (1000.0, juce::jlimit (0.0, 1.0, t / sweepLength));
    }

    inline juce::AudioBuffer<float> makeKit (double sr)
    {
        const int total = (int) std::ceil (kitLength * sr);
        juce::AudioBuffer<float> kit (2, total);
        kit.clear();
        auto* d = kit.getWritePointer (0);
        auto at = [sr] (double seconds) { return (int) std::round (seconds * sr); };
        const double twoPi = juce::MathConstants<double>::twoPi;

        d[at (clickAt)] = juce::Decibels::decibelsToGain (-20.0f);

        // Level staircases, 2 ms ramps between steps.
        for (const auto& s : staircases)
        {
            for (int i = 0; i < numSteps * at (stepLength); ++i)
            {
                const double t = i / sr;
                const int step = juce::jmin (numSteps - 1, (int) (t / stepLength));
                const double inStep = t - step * stepLength;
                float level = juce::Decibels::decibelsToGain (stepLevelDb (step));
                if (inStep < 0.002 && step > 0)
                {
                    const float previous = juce::Decibels::decibelsToGain (stepLevelDb (step - 1));
                    level = previous + (level - previous) * (float) (inStep / 0.002);
                }
                d[at (s.start) + i] = level * (float) std::sin (twoPi * s.freq * t);
            }
        }

        // Level jumps for attack / release.
        for (int i = 0; i < at ((timingCycles * 2 + 1) * 1.0); ++i)
        {
            const double t = i / sr;
            const bool high = ((int) t) % 2 == 1 && t < timingCycles * 2.0;
            d[at (timingStart) + i] = juce::Decibels::decibelsToGain (high ? -6.0f : -30.0f) * (float) std::sin (twoPi * 1000.0 * t);
        }

        // Quiet log sweep for the frequency response.
        {
            const double k = std::log (1000.0);
            const float level = juce::Decibels::decibelsToGain ((float) sweepLevelDb);
            for (int i = 0; i < at (sweepLength); ++i)
            {
                const double t = i / sr;
                const double phase = twoPi * 20.0 * sweepLength / k * (std::exp (t / sweepLength * k) - 1.0);
                const double fade = juce::jmin (1.0, juce::jmin (t, sweepLength - t) / 0.01);
                d[at (sweepStart) + i] = level * (float) (fade * std::sin (phase));
            }
        }

        // Drum-like noise bursts for listening.
        {
            juce::Random random (7);
            for (int i = 0; i < at (drumsLength); ++i)
            {
                const double t = std::fmod (i / sr, 0.25);
                const double env = juce::jmin (1.0, t / 0.001) * std::exp (-t / 0.08);
                d[at (drumsStart) + i] = (float) (0.5 * env) * (random.nextFloat() * 2.0f - 1.0f);
            }
        }

        kit.copyFrom (1, 0, kit, 0, 0, total);
        return kit;
    }

    //==============================================================================
    struct Result
    {
        int offset = 0;                                                   // samples, render vs kit
        std::map<std::string, std::vector<float>> curves;                 // staircase name -> out peak dB per step
        float attackMs = 0.0f, releaseMs = 0.0f, overshootDb = 0.0f, undershootDb = 0.0f;
        float settledHighDb = 0.0f, settledLowDb = 0.0f;
        std::vector<std::pair<float, float>> response;                    // freq, gain dB
    };

    inline float rmsDb (const float* d, int from, int to)
    {
        double sum = 0.0;
        for (int i = from; i < to; ++i)
            sum += (double) d[i] * d[i];
        return juce::Decibels::gainToDecibels ((float) std::sqrt (sum / juce::jmax (1, to - from)), -200.0f);
    }

    inline Result analyse (const juce::AudioBuffer<float>& audio, double sr)
    {
        Result r;
        const auto* d = audio.getReadPointer (0);
        const int n = audio.getNumSamples();
        auto at = [sr] (double seconds) { return (int) std::round (seconds * sr); };

        // Align on the click: first sample above 30 % of the loudest one in the first 1.5 s.
        {
            const int search = juce::jmin (n, at (1.5));
            float peak = 0.0f;
            for (int i = 0; i < search; ++i)
                peak = juce::jmax (peak, std::abs (d[i]));
            int onset = 0;
            while (onset < search && std::abs (d[onset]) < 0.3f * peak)
                ++onset;
            r.offset = onset - at (clickAt);
        }

        auto index = [&] (double seconds) { return juce::jlimit (0, n, at (seconds) + r.offset); };

        // Static curves: steady second half of each step, as sine peak dB.
        for (const auto& s : staircases)
        {
            auto& curve = r.curves[s.name];
            for (int step = 0; step < numSteps; ++step)
            {
                const double t = s.start + step * stepLength;
                const int to = at (t + stepLength) + r.offset;
                // Renders of an older, shorter kit don't have every staircase.
                curve.push_back (to > n ? std::numeric_limits<float>::quiet_NaN()
                                        : rmsDb (d, index (t + stepLength * 0.5), index (t + stepLength)) + 3.0103f);
            }
        }

        // Timing: 1 ms peak envelope around each jump.
        {
            const int win = juce::jmax (1, at (0.001));
            auto envDb = [&] (double t)
            {
                float peak = 0.0f;
                for (int i = index (t); i < juce::jmin (n, index (t) + win); ++i)
                    peak = juce::jmax (peak, std::abs (d[i]));
                return juce::Decibels::gainToDecibels (peak, -200.0f);
            };

            auto settled = [&] (double from, double to)
            {
                return rmsDb (d, index (from), index (to)) + 3.0103f;
            };

            float attack = 0.0f, release = 0.0f, over = 0.0f, under = 0.0f, high = 0.0f, low = 0.0f;

            for (int c = 0; c < timingCycles; ++c)
            {
                const double up = timingStart + 1.0 + 2.0 * c, down = up + 1.0;
                const float settledHigh = settled (up + 0.7, up + 0.95);
                const float settledLow = settled (down + 0.7, down + 0.95);

                // Attack: how long the overshoot after the jump takes to come down to 37 %.
                float peak = -200.0f;
                for (double t = up; t < up + 0.3; t += 0.001)
                    peak = juce::jmax (peak, envDb (t));
                const float overshoot = juce::jmax (0.0f, peak - settledHigh);
                double tA = 0.0;
                if (overshoot > 0.3f)
                    for (tA = 0.0; tA < 0.7; tA += 0.0005)
                        if (envDb (up + tA) - settledHigh <= 0.37f * overshoot && envDb (up + tA) < peak - 0.01f)
                            break;

                // Release: how long the dip after the drop takes to recover to 37 %.
                float dip = 200.0f;
                for (double t = down + 0.002; t < down + 0.3; t += 0.001)
                    dip = juce::jmin (dip, envDb (t));
                const float undershoot = juce::jmax (0.0f, settledLow - dip);
                double tR = 0.0;
                if (undershoot > 0.3f)
                {
                    double tMin = 0.002;
                    for (double t = 0.002; t < 0.3; t += 0.001)
                        if (envDb (down + t) <= dip + 0.01f) { tMin = t; break; }
                    for (tR = tMin; tR < 0.95; tR += 0.001)
                        if (settledLow - envDb (down + tR) <= 0.37f * undershoot)
                            break;
                }

                attack += (float) tA * 1000.0f / timingCycles;
                release += (float) tR * 1000.0f / timingCycles;
                over += overshoot / timingCycles;
                under += undershoot / timingCycles;
                high += settledHigh / timingCycles;
                low += settledLow / timingCycles;
            }

            r.attackMs = attack; r.releaseMs = release; r.overshootDb = over; r.undershootDb = under;
            r.settledHighDb = high; r.settledLowDb = low;
        }

        // Frequency response at a low level: output RMS along the sweep against the known input level.
        for (float f : { 31.5f, 63.0f, 125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f })
        {
            const double t = sweepLength * std::log (f / 20.0) / std::log (1000.0);
            const double half = 0.5 * std::max (0.02, sweepLength * std::log (std::pow (2.0, 1.0 / 6.0)) / std::log (1000.0));
            const float out = rmsDb (d, index (sweepStart + t - half), index (sweepStart + t + half)) + 3.0103f;
            r.response.push_back ({ f, out - (float) sweepLevelDb });
        }

        return r;
    }

    //==============================================================================
    inline bool writeWav (const juce::AudioBuffer<float>& audio, double sr, const juce::File& file)
    {
        file.deleteFile();
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
        juce::WavAudioFormat wav;
        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (sr)
                                                                                    .withNumChannels (audio.getNumChannels())
                                                                                    .withBitsPerSample (24));
        return writer != nullptr && writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    }

    inline bool readAudio (const juce::File& file, juce::AudioBuffer<float>& audio, double& sr)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        if (reader == nullptr)
            return false;

        sr = reader->sampleRate;
        audio.setSize (2, (int) reader->lengthInSamples);
        reader->read (&audio, 0, (int) reader->lengthInSamples, 0, true, true);
        if (reader->numChannels == 1)
            audio.copyFrom (1, 0, audio, 0, 0, audio.getNumSamples());
        return true;
    }

    inline void printComparison (const Result& a, const char* nameA, const Result& b, const char* nameB)
    {
        auto f1 = [] (float v) { return (std::isnan (v) ? juce::String ("n/a") : juce::String (v, 1)).paddedLeft (' ', 8); };

        std::cout << "\nStatic curve (sine peak dBFS in -> out)" << std::endl;
        for (const auto& s : staircases)
        {
            std::cout << "  " << s.name << ":      in" << juce::String (nameA).paddedLeft (' ', 10) << juce::String (nameB).paddedLeft (' ', 10)
                      << "    diff" << std::endl;
            const auto& ca = a.curves.at (s.name);
            const auto& cb = b.curves.at (s.name);
            for (int step = 0; step < numSteps; step += 2)
                std::cout << "          " << f1 (stepLevelDb (step)) << "  " << f1 (ca[(size_t) step]) << "  " << f1 (cb[(size_t) step])
                          << "  " << f1 (cb[(size_t) step] - ca[(size_t) step]) << std::endl;
        }

        std::cout << "\nLevel jump -30 -> -6 dB at 1 kHz" << std::endl;
        std::cout << "                        " << juce::String (nameA).paddedLeft (' ', 10) << juce::String (nameB).paddedLeft (' ', 10) << std::endl;
        std::cout << "  settled at -6 in      " << f1 (a.settledHighDb) << "  " << f1 (b.settledHighDb) << " dB" << std::endl;
        std::cout << "  settled at -30 in     " << f1 (a.settledLowDb) << "  " << f1 (b.settledLowDb) << " dB" << std::endl;
        std::cout << "  overshoot on attack   " << f1 (a.overshootDb) << "  " << f1 (b.overshootDb) << " dB" << std::endl;
        std::cout << "  attack (to 37 %)      " << f1 (a.attackMs) << "  " << f1 (b.attackMs) << " ms" << std::endl;
        std::cout << "  dip after release     " << f1 (a.undershootDb) << "  " << f1 (b.undershootDb) << " dB" << std::endl;
        std::cout << "  release (to 37 %)     " << f1 (a.releaseMs) << "  " << f1 (b.releaseMs) << " ms" << std::endl;

        std::cout << "\nFrequency response at -40 dBFS (dB)" << std::endl;
        for (size_t i = 0; i < a.response.size(); ++i)
            std::cout << "  " << juce::String (a.response[i].first, 0).paddedLeft (' ', 7) << " Hz  " << f1 (a.response[i].second)
                      << "  " << f1 (b.response[i].second) << std::endl;

        std::cout << "\nAlignment offset: " << a.offset << " / " << b.offset << " samples" << std::endl;
    }
}
