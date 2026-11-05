#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <functional>
#include <map>
#include <optional>
#include <vector>

// Broadcast WAV reading and writing for production sound files.
//
// Recorder files (poly or mono BWF, RF64 for long takes) carry metadata that post needs: the bext chunk's
// timecode (TimeReference), scene/take and track names in iXML, and recorder-specific notes. Aligned
// files keep all of it: every chunk except the audio is copied unchanged, the sample format stays the
// same, and channels that weren't processed are copied byte for byte. Other formats (AIFF, FLAC, ...) are
// read through JUCE and written as plain WAV.
namespace lavalign
{
    struct AudioFileInfo
    {
        juce::File file;
        double  sampleRate = 0;
        int     numChannels = 0;
        int64_t numFrames = 0;
        int     bitsPerSample = 0;
        bool    isFloat = false;

        // WAV only
        bool    isWav = false;
        bool    isRf64 = false;
        int     blockAlign = 0;
        int64_t dataOffset = 0;
        int64_t dataSize = 0;
        juce::MemoryBlock fmtChunk;  // as stored in the file
        struct Chunk { juce::String id; int64_t offset = 0; int64_t size = 0; };
        std::vector<Chunk> otherChunks;  // in file order, without fmt, data and ds64

        // metadata
        std::optional<int64_t> timeReference;  // bext: samples since midnight
        juce::String scene, take, tape, description;
        juce::StringArray trackNames;           // one per channel (may be empty strings)

        double durationSeconds() const { return sampleRate > 0 ? (double) numFrames / sampleRate : 0.0; }
        juce::String timecodeString (double fps = 25.0) const;
    };

    using IoProgress = std::function<bool (float)>;

    // Reads the header and metadata. Returns an error message, or empty on success.
    juce::String readInfo (const juce::File& file, AudioFileInfo& info);

    // Reads the given channels (0-based) as float.
    juce::String readChannels (const AudioFileInfo& info, const std::vector<int>& channels,
                               std::vector<std::vector<float>>& out, const IoProgress& progress = {});

    // Writes a copy of 'source' to 'dest' with some channels replaced (channel -> samples, numFrames each).
    // Same sample format, all metadata chunks kept, other channels copied byte for byte.
    juce::String writeReplaced (const AudioFileInfo& source, const juce::File& dest,
                                const std::map<int, const float*>& replaced, const IoProgress& progress = {});

    // Writes one channel as a mono file in the source's sample format, keeping the bext chunk (timecode)
    // and the other metadata except iXML (its track list describes the poly file).
    juce::String writeMono (const AudioFileInfo& source, const juce::File& dest, const float* samples,
                            const IoProgress& progress = {});

    // Plain WAV writer (tests, CLI): float 32 or PCM 16/24, optional bext time reference.
    juce::String writeSimpleWav (const juce::File& dest, const std::vector<const float*>& channels, int64_t numFrames,
                                 double sampleRate, int bitsPerSample, bool isFloat,
                                 std::optional<int64_t> timeReference = {}, const juce::String& ixml = {});
}
