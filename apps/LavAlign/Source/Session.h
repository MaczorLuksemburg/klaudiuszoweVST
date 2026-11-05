#pragma once

#include "Align.h"
#include "WavFile.h"

#include <map>
#include <memory>

// Takes, channel roles, processing and export (no GUI).
//
// A take is one moment of recording: a poly file from the recorder, or several files whose timecode
// overlaps (mono AAF media of the same clip, separate recorders, stems rendered from a DAW). Every channel
// of a take has a role: the boom (the reference, never changed), a lav (aligned to the boom) or off.
// Roles belong to the track name ("key"), so setting them once applies to every take of the shoot.
namespace lavalign
{
    enum class Role { off, boom, lav };

    struct TakeFile
    {
        AudioFileInfo info;
        int64_t start = 0;  // samples from the take's start
    };

    struct TakeChannel
    {
        int fileIndex = 0;
        int channel = 0;
        juce::String name;  // track name or file name
        juce::String key;   // role key shared across takes
        Role role = Role::off;
    };

    struct LavOutcome
    {
        int channelIndex = -1;  // into Take::channels
        LavResult result;       // 'aligned' is kept only while the take is selected / exported
        std::vector<float> original;  // the lav as recorded (kept with the audio, for the before/after preview)
    };

    struct Take
    {
        juce::String name;
        double sampleRate = 0;
        int64_t length = 0;
        std::vector<TakeFile> files;
        std::vector<TakeChannel> channels;

        int boomChannel() const;            // index into channels, -1 if none
        std::vector<int> lavChannels() const;
        juce::String problem() const;       // why it can't be processed, empty if it can
        juce::String signature() const;     // identifies the files (for caching results)
    };

    struct ExportOptions
    {
        juce::File folder;            // empty: an "Aligned" folder next to each source file
        bool splitPoly = false;       // poly files: write each aligned lav as its own mono file
        juce::String suffix;          // added to file names (empty: same names, for relinking)
    };

    class Session
    {
    public:
        // Adds files (folders are scanned for audio files); returns messages about files that were skipped.
        juce::StringArray addFiles (const juce::Array<juce::File>& files);
        void removeTake (int index);
        void clear();

        int numTakes() const { return (int) takes.size(); }
        Take& take (int i) { return *takes[(size_t) i]; }
        const Take& take (int i) const { return *takes[(size_t) i]; }

        // Sets a channel's role; every channel with the same key (in all takes) follows.
        void setRole (int takeIndex, int channelIndex, Role role);
        void setRoleForKey (const juce::String& key, Role role);

        static juce::String keyFor (const juce::String& name, bool isFileName);

    private:
        void regroup();
        void applyRoles (Take& t);

        std::vector<AudioFileInfo> files;
        std::vector<int> batchOf;  // files added together without timecode stay together
        int nextBatch = 0;
        std::vector<std::unique_ptr<Take>> takes;
        std::map<juce::String, Role> userRoles;
    };

    // Loads the take's audio and aligns every lav to the boom (lavs run in parallel).
    // 'keepAudio' keeps the aligned samples in the outcomes (for preview and export).
    // 'boomOut' (optional) receives the boom on the take's timeline.
    juce::String processTake (const Take& take, const Settings& settings, std::vector<LavOutcome>& outcomes,
                              const Progress& progress, bool keepAudio = true, bool measure = true,
                              std::vector<float>* boomOut = nullptr);

    // Writes the aligned lavs. Returns the files written (or an error in 'error').
    juce::Array<juce::File> exportTake (const Take& take, const std::vector<LavOutcome>& outcomes,
                                        const ExportOptions& options, juce::String& error, const IoProgress& progress = {});

}
