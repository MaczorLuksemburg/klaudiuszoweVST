#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include "BeatAnalysis.h"
#include "Loudness.h"

namespace waveref
{
    // A decoded reference file, ready for playback: stereo at the host's sample rate, plus the things the editor
    // and the loudness match need (waveform peaks, 100 ms energies). Built on a background thread and never
    // modified afterwards, so the audio thread can read it without locks.
    struct TrackData
    {
        static constexpr int peakBlock = 64;
        static constexpr double maxSeconds = 20.0 * 60.0;

        juce::String name, path;
        juce::AudioBuffer<float> audio;       // 2 channels, empty for an empty slot
        double sampleRate = 44100.0;
        std::vector<float> subEnergies;       // loudness::subBlockEnergies
        std::vector<float> peakMin, peakMax;  // per peakBlock samples, of (L + R) / 2
        TempoGuess tempo;                     // filled in by decodeTrack

        int64_t getLength() const    { return (int64_t) audio.getNumSamples(); }
        bool isEmpty() const         { return audio.getNumSamples() == 0; }
        double getSeconds() const    { return sampleRate > 0.0 ? (double) getLength() / sampleRate : 0.0; }

        // Min and max of the mono sum between two sample positions (for drawing the waveform).
        void getPeaks (int64_t start, int64_t end, float& low, float& high) const;

        // Gated loudness of a region, the number the match gain is built from.
        float getLoopLoudness (int64_t start, int64_t end) const;
    };

    // Reads WAV, AIFF, FLAC, OGG and MP3 (whatever the registered formats can open) and resamples to
    // `targetRate`. Returns null and sets `error` when the file can't be used. Blocking: call off the message thread.
    std::unique_ptr<TrackData> decodeTrack (juce::AudioFormatManager& formats, const juce::File& file,
                                            double targetRate, juce::String& error);

    // An empty slot.
    std::unique_ptr<TrackData> makeEmptyTrack (double sampleRate);

    // Builds a track from audio already in memory (tests, and decodeTrack's last step).
    std::unique_ptr<TrackData> makeTrack (juce::AudioBuffer<float>&& stereoAudio, double sampleRate,
                                          const juce::String& name, const juce::String& path);

    // Windowed-sinc resampler (offline quality).
    juce::AudioBuffer<float> resampleSinc (const juce::AudioBuffer<float>& input, double sourceRate, double targetRate);
}
