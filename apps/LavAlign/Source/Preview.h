#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include <atomic>
#include <memory>
#include <vector>

// Listening to a take before and after alignment: boom and lavs summed (levels matched, as a mixer would),
// lavs alone or the boom alone. Switching original / aligned crossfades, so the change in tone is easy to hear.
namespace lavalign
{
    struct PreviewData
    {
        double sampleRate = 48000;
        std::vector<float> boom;                       // take timeline
        std::vector<std::vector<float>> lavOriginal;   // take timeline, level-matched to the boom
        std::vector<std::vector<float>> lavAligned;
        int64_t length() const { return (int64_t) boom.size(); }
    };

    class Preview : private juce::AudioSource
    {
    public:
        enum class Listen { sum, lavs, boom };

        Preview();
        ~Preview() override;

        void setData (std::shared_ptr<const PreviewData> data);  // message thread
        void play();
        void stop();
        bool isPlaying() const { return playing.load(); }
        void setPosition (int64_t samplePos);
        int64_t getPosition() const { return position.load(); }
        void setAligned (bool shouldUseAligned) { aligned = shouldUseAligned; }
        void setListen (Listen l) { listen = (int) l; }
        bool hasDevice() const { return deviceOk; }

        juce::AudioDeviceManager& getDeviceManager() { return deviceManager; }

    private:
        void prepareToPlay (int samplesPerBlock, double sampleRate) override;
        void releaseResources() override {}
        void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override;

        juce::AudioDeviceManager deviceManager;
        juce::AudioSourcePlayer player;
        bool deviceOk = false;

        std::shared_ptr<const PreviewData> pending;  // swapped in by the audio thread
        std::shared_ptr<const PreviewData> current;
        juce::SpinLock lock;

        std::atomic<bool> playing { false }, aligned { true };
        std::atomic<int> listen { 0 };
        std::atomic<int64_t> position { 0 };
        std::atomic<int64_t> seekTo { -1 };

        double deviceRate = 48000;
        float mixAligned = 1.0f;  // smoothed crossfade original -> aligned
        float gain = 0.0f;        // smoothed start/stop fade
        juce::LagrangeInterpolator interpolator;
        std::vector<float> takeBuffer, outBuffer;
        double readPos = 0;
    };
}
