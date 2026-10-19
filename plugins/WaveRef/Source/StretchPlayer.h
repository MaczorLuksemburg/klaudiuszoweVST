#pragma once

#include "TrackData.h"

#include <rubberband/RubberBandStretcher.h>

#include <array>
#include <memory>
#include <vector>

namespace waveref
{
    // Plays a reference at a different tempo than it was made at (Rubber Band, real-time mode), aimed at a
    // position that the caller works out from the host's beat position. Every jump starts a new voice and
    // fades the old one out. Audio thread only; everything is allocated in prepare().
    class StretchPlayer
    {
    public:
        static constexpr int numVoices = 3;

        void prepare (double newSampleRate, int)
        {
            sampleRate = newSampleRate;

            for (auto& v : voices)
            {
                v = {};
                v.stretcher = std::make_unique<RubberBand::RubberBandStretcher> (
                    (size_t) newSampleRate, 2,
                    RubberBand::RubberBandStretcher::OptionProcessRealTime
                        | RubberBand::RubberBandStretcher::OptionEngineFiner
                        | RubberBand::RubberBandStretcher::OptionThreadingNever,
                    1.0, 1.0);
                v.stretcher->setMaxProcessSize ((size_t) chunk);
            }

            for (auto& channel : input)
                channel.assign ((size_t) chunk, 0.0f);

            for (auto& channel : scratch)
                channel.assign ((size_t) chunk, 0.0f);

            for (auto& channel : mixed)
                channel.assign ((size_t) maxBlock, 0.0f);

            main = -1;
        }

        void reset()
        {
            for (auto& v : voices)
            {
                v.active = false;
                v.track = nullptr;
            }

            main = -1;
        }

        bool isActive() const { return main >= 0 || anyActive(); }

        // Where the main voice is in the track (input samples).
        double getPosition() const { return main >= 0 ? voices[(size_t) main].position : 0.0; }

        // Called once per block. `expected` is the track position that the first output sample of this block
        // should come from; `timeRatio` is output length / input length (reference tempo / host tempo).
        void update (const TrackData* track, double expected, double timeRatio, bool keyLock)
        {
            timeRatio = juce::jlimit (0.25, 4.0, timeRatio);
            const double pitch = keyLock ? 1.0 : 1.0 / timeRatio;

            if (main >= 0)
            {
                auto& v = voices[(size_t) main];
                const double error = v.position - expected;

                if (v.track == track && std::abs (error) < 0.04 * sampleRate)
                {
                    // Close enough: pull the position onto the target by bending the tempo a little.
                    const double correction = juce::jlimit (-0.03, 0.03, error / (0.5 * sampleRate));
                    v.ratio = timeRatio * (1.0 + correction);
                    v.stretcher->setTimeRatio (v.ratio);
                    v.stretcher->setPitchScale (pitch);
                    v.pitch = pitch;
                    return;
                }
            }

            startVoice (track, expected, timeRatio, pitch);
        }

        // Fade everything out (host stopped, left the mode, ...).
        void stop (int fadeSamples)
        {
            for (auto& v : voices)
            {
                if (v.active)
                {
                    v.target = 0.0f;
                    v.step = 1.0f / (float) juce::jmax (1, fadeSamples);
                }
            }

            main = -1;
        }

        // Adds the next `numSamples` to left / right.
        void render (float* left, float* right, int numSamples)
        {
            jassert (numSamples <= maxBlock);

            for (auto& v : voices)
            {
                if (! v.active)
                    continue;

                pull (v, numSamples);

                for (int i = 0; i < numSamples; ++i)
                {
                    left[i] += v.gain * mixed[0][(size_t) i];
                    right[i] += v.gain * mixed[1][(size_t) i];

                    if (v.gain < v.target)       v.gain = juce::jmin (v.target, v.gain + v.step);
                    else if (v.gain > v.target)  v.gain = juce::jmax (v.target, v.gain - v.step);
                }

                v.position += (double) numSamples / v.ratio;

                if (v.target == 0.0f && v.gain == 0.0f)
                    v.active = false;
            }

            if (main >= 0 && ! voices[(size_t) main].active)
                main = -1;
        }

    private:
        static constexpr int chunk = 2048;
        static constexpr int maxBlock = 1024;

        struct Voice
        {
            std::unique_ptr<RubberBand::RubberBandStretcher> stretcher;
            const TrackData* track = nullptr;
            double position = 0.0;     // input sample the next output sample comes from
            double inPosition = 0.0;   // input sample the stretcher is fed next
            double ratio = 1.0, pitch = 1.0;
            float gain = 0.0f, target = 0.0f, step = 1.0f;
            bool active = false;
        };

        bool anyActive() const
        {
            for (const auto& v : voices)
                if (v.active)
                    return true;

            return false;
        }

        // Fills the input buffers with the track from `v.inPosition` on (zeros outside the file) and feeds them.
        void feed (Voice& v, int count)
        {
            count = juce::jlimit (1, chunk, count);
            const int64_t start = (int64_t) std::llround (v.inPosition);
            const int64_t length = v.track->getLength();

            for (int ch = 0; ch < 2; ++ch)
            {
                const float* source = v.track->audio.getReadPointer (ch);
                auto& buffer = input[(size_t) ch];

                for (int i = 0; i < count; ++i)
                {
                    const int64_t index = start + i;
                    buffer[(size_t) i] = index >= 0 && index < length ? source[index] : 0.0f;
                }
            }

            const float* pointers[2] { input[0].data(), input[1].data() };
            v.stretcher->process (pointers, (size_t) count, false);
            v.inPosition += count;
        }

        void feedSilence (Voice& v, int count)
        {
            while (count > 0)
            {
                const int n = juce::jmin (count, chunk);
                std::fill_n (input[0].begin(), n, 0.0f);
                std::fill_n (input[1].begin(), n, 0.0f);
                const float* pointers[2] { input[0].data(), input[1].data() };
                v.stretcher->process (pointers, (size_t) n, false);
                count -= n;
            }
        }

        void startVoice (const TrackData* track, double position, double ratio, double pitch)
        {
            // Fade out what is playing now.
            for (auto& v : voices)
            {
                if (v.active)
                {
                    v.target = 0.0f;
                    v.step = 1.0f / 240.0f;
                }
            }

            // A free voice, or the quietest one.
            int chosen = -1;
            for (int i = 0; i < numVoices; ++i)
                if (! voices[(size_t) i].active) { chosen = i; break; }

            if (chosen < 0)
            {
                chosen = 0;
                for (int i = 1; i < numVoices; ++i)
                    if (voices[(size_t) i].gain < voices[(size_t) chosen].gain) chosen = i;
            }

            auto& v = voices[(size_t) chosen];
            v.track = track;
            v.ratio = ratio;
            v.pitch = pitch;
            v.stretcher->reset();
            v.stretcher->setTimeRatio (ratio);
            v.stretcher->setPitchScale (pitch);

            // Real-time mode doesn't compensate the start: pad the input, drop the start delay of the output.
            const int pad = (int) v.stretcher->getPreferredStartPad();
            int toDiscard = (int) v.stretcher->getStartDelay();

            feedSilence (v, pad);
            v.inPosition = position;

            for (int guard = 0; guard < 64 && toDiscard > 0; ++guard)
            {
                const int available = v.stretcher->available();

                if (available > 0)
                {
                    const int take = juce::jmin (available, toDiscard, chunk);
                    float* pointers[2] { scratch[0].data(), scratch[1].data() };
                    v.stretcher->retrieve (pointers, (size_t) take);
                    toDiscard -= take;
                }
                else
                {
                    feed (v, juce::jmax (64, (int) v.stretcher->getSamplesRequired()));
                }
            }

            v.position = position;
            v.gain = 1.0f;
            v.target = 1.0f;
            v.active = true;
            main = chosen;
        }

        void pull (Voice& v, int count)
        {
            for (int guard = 0; guard < 32 && (int) v.stretcher->available() < count; ++guard)
                feed (v, juce::jmax (64, (int) v.stretcher->getSamplesRequired()));

            float* pointers[2] { mixed[0].data(), mixed[1].data() };
            const int have = juce::jmin (count, (int) juce::jmax (0, v.stretcher->available()));

            if (have > 0)
                v.stretcher->retrieve (pointers, (size_t) have);

            for (int ch = 0; ch < 2; ++ch)
                for (int i = have; i < count; ++i)
                    mixed[(size_t) ch][(size_t) i] = 0.0f;
        }

        std::array<Voice, numVoices> voices;
        std::array<std::vector<float>, 2> input, scratch, mixed;
        double sampleRate = 44100.0;
        int main = -1;
    };
}
