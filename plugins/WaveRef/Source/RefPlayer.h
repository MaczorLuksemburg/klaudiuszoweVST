#pragma once

#include "TrackData.h"

#include <array>

namespace waveref
{
    // Plays one reference track. Every jump (loop wrap, restart, cue, seek, slot change) starts a new voice and
    // fades the old ones out, so nothing ever clicks. The newest voice is the "main" one: its position is what
    // the editor shows as the playhead. While stopped the position is held and the playhead stays put.
    // Audio thread only.
    class RefPlayer
    {
    public:
        static constexpr int maxVoices = 6;

        void reset()
        {
            for (auto& v : voices)
                v = {};

            numVoices = 0;
            stopped = false;
            held = 0;
        }

        bool isPlaying() const { return numVoices > 0 && ! stopped; }
        bool isStopped() const { return stopped || numVoices == 0; }
        bool hasVoices() const { return numVoices > 0; }

        int64_t getPosition() const { return isPlaying() ? voices[0].position : held; }
        const TrackData* getTrack() const { return numVoices > 0 ? voices[0].track : nullptr; }

        // Where playback resumes from (and what the playhead shows) while nothing is playing.
        void setHeld (int64_t position) { held = position; }

        // Starts a voice at `position`; older voices fade out over `fadeOut` samples (0 = cut at once).
        void start (const TrackData* track, int64_t position, int fadeIn, int fadeOut)
        {
            stopped = false;

            for (int i = 0; i < numVoices; ++i)
            {
                voices[i].target = 0.0f;
                voices[i].step = fadeOut > 0 ? 1.0f / (float) fadeOut : 1.0f;
            }

            if (numVoices == maxVoices)
                --numVoices;   // drop the oldest

            for (int i = numVoices; i > 0; --i)
                voices[i] = voices[i - 1];

            Voice v;
            v.track = track;
            v.position = position;
            v.gain = fadeIn > 0 ? 0.0f : 1.0f;
            v.target = 1.0f;
            v.step = fadeIn > 0 ? 1.0f / (float) fadeIn : 1.0f;
            voices[0] = v;
            ++numVoices;
        }

        // Fades everything out and remembers where it was.
        void stop (int fadeOut)
        {
            if (stopped)
                return;

            if (numVoices > 0)
                held = voices[0].position;

            for (int i = 0; i < numVoices; ++i)
            {
                voices[i].target = 0.0f;
                voices[i].step = fadeOut > 0 ? 1.0f / (float) fadeOut : 1.0f;
            }

            stopped = true;
        }

        // Adds the next `numSamples` of the track to `left` / `right` (which the caller has cleared).
        // With `wrap`, the main voice loops over [loopStart, loopEnd) and the file as a whole; a position before
        // the loop plays on until it reaches it, and one after it plays to the end of the file, then comes back
        // to the loop start. Without `wrap` it just plays and falls silent at the end of the file.
        void render (float* left, float* right, int numSamples, int64_t loopStart, int64_t loopEnd, bool wrap)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                if (numVoices == 0)
                    return;

                if (wrap && ! stopped)
                {
                    auto& main = voices[0];

                    if (main.position >= loopStart && main.position < loopEnd)
                        main.inLoop = true;

                    if ((main.inLoop && main.position >= loopEnd) || main.position >= main.track->getLength())
                        start (main.track, loopStart, 0, wrapFade);   // main is now voices[1]
                }

                float l = 0.0f, r = 0.0f;

                for (int v = 0; v < numVoices; ++v)
                {
                    auto& voice = voices[v];

                    if (voice.position >= 0 && voice.position < voice.track->getLength())
                    {
                        l += voice.gain * voice.track->audio.getReadPointer (0)[voice.position];
                        r += voice.gain * voice.track->audio.getReadPointer (1)[voice.position];
                    }

                    ++voice.position;

                    if (voice.gain < voice.target)
                        voice.gain = juce::jmin (voice.target, voice.gain + voice.step);
                    else if (voice.gain > voice.target)
                        voice.gain = juce::jmax (voice.target, voice.gain - voice.step);
                }

                left[i] += l;
                right[i] += r;

                // Drop voices that have faded out or run off the end of the file.
                for (int v = numVoices - 1; v >= 0; --v)
                {
                    const bool faded = voices[v].target == 0.0f && voices[v].gain == 0.0f;
                    const bool ended = voices[v].position >= voices[v].track->getLength() && (v > 0 || ! wrap);

                    if (faded || ended)
                    {
                        if (v == 0 && ! stopped)
                            held = voices[0].position;

                        for (int k = v; k < numVoices - 1; ++k)
                            voices[k] = voices[k + 1];

                        --numVoices;
                    }
                }
            }
        }

    private:
        static constexpr int wrapFade = 32;   // ~0.7 ms: the tail past the loop end covers the join

        struct Voice
        {
            const TrackData* track = nullptr;
            int64_t position = 0;
            float gain = 0.0f, target = 0.0f, step = 1.0f;
            bool inLoop = false;
        };

        std::array<Voice, maxVoices + 1> voices {};
        int numVoices = 0;
        bool stopped = false;
        int64_t held = 0;
    };
}
