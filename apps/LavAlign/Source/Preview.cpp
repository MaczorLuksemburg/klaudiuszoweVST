#include "Preview.h"

namespace lavalign
{
Preview::Preview()
{
    const auto err = deviceManager.initialiseWithDefaultDevices (0, 2);
    deviceOk = err.isEmpty() && deviceManager.getCurrentAudioDevice() != nullptr;
    player.setSource (this);
    deviceManager.addAudioCallback (&player);
}

Preview::~Preview()
{
    deviceManager.removeAudioCallback (&player);
    player.setSource (nullptr);
}

void Preview::setData (std::shared_ptr<const PreviewData> data)
{
    const juce::SpinLock::ScopedLockType sl (lock);
    pending = std::move (data);
    if (pending == nullptr) playing = false;
}

void Preview::play()
{
    playing = true;
}

void Preview::stop()
{
    playing = false;
}

void Preview::setPosition (int64_t samplePos)
{
    seekTo = std::max<int64_t> (0, samplePos);
    position = std::max<int64_t> (0, samplePos);
}

void Preview::prepareToPlay (int samplesPerBlock, double sampleRate)
{
    deviceRate = sampleRate;
    takeBuffer.assign ((size_t) samplesPerBlock * 8 + 16, 0.0f);
    outBuffer.assign ((size_t) samplesPerBlock, 0.0f);
    interpolator.reset();
}

void Preview::getNextAudioBlock (const juce::AudioSourceChannelInfo& info)
{
    info.clearActiveBufferRegion();
    {
        const juce::SpinLock::ScopedTryLockType sl (lock);
        if (sl.isLocked() && pending != current)
        {
            current = pending;
            interpolator.reset();
            readPos = (double) position.load();
        }
    }
    const auto data = current;
    if (data == nullptr || data->length() == 0)
        return;

    const auto seek = seekTo.exchange (-1);
    if (seek >= 0) { readPos = (double) seek; interpolator.reset(); }

    const bool wantPlay = playing.load();
    if (! wantPlay && gain <= 0.0f)
        return;

    const int numOut = info.numSamples;
    if ((int) outBuffer.size() < numOut) outBuffer.assign ((size_t) numOut, 0.0f);
    const double ratio = data->sampleRate / deviceRate;
    const int numIn = (int) std::ceil (numOut * ratio) + 8;
    if ((int) takeBuffer.size() < numIn) takeBuffer.assign ((size_t) numIn, 0.0f);

    const int nLav = (int) data->lavOriginal.size();
    const float lavScale = nLav > 0 ? 1.0f / std::sqrt ((float) nLav) : 0.0f;
    const int mode = listen.load();
    const float target = aligned.load() ? 1.0f : 0.0f;
    const float fadeCoef = (float) (1.0 / (0.015 * data->sampleRate));
    const auto start = (int64_t) readPos;
    for (int i = 0; i < numIn; ++i)
    {
        const int64_t k = start + i;
        float v = 0.0f;
        if (k >= 0 && k < data->length())
        {
            mixAligned += (target - mixAligned) * fadeCoef;
            float lo = 0.0f, la = 0.0f;
            for (int j = 0; j < nLav; ++j)
            {
                lo += data->lavOriginal[(size_t) j][(size_t) k];
                la += j < (int) data->lavAligned.size() && ! data->lavAligned[(size_t) j].empty()
                          ? data->lavAligned[(size_t) j][(size_t) k] : data->lavOriginal[(size_t) j][(size_t) k];
            }
            const float l = (lo + mixAligned * (la - lo)) * lavScale;
            const float b = data->boom[(size_t) k];
            v = mode == 0 ? 0.5f * (b + l) : mode == 1 ? l : b;
        }
        takeBuffer[(size_t) i] = v;
    }
    const int used = interpolator.process (ratio, takeBuffer.data(), outBuffer.data(), numOut, numIn, 0);
    readPos += used;
    position = (int64_t) readPos;

    const float gainCoef = (float) (1.0 / (0.005 * deviceRate));
    auto* left = info.buffer->getWritePointer (0, info.startSample);
    auto* right = info.buffer->getNumChannels() > 1 ? info.buffer->getWritePointer (1, info.startSample) : nullptr;
    for (int i = 0; i < numOut; ++i)
    {
        gain += ((wantPlay ? 1.0f : 0.0f) - gain) * gainCoef;
        const float v = juce::jlimit (-1.0f, 1.0f, outBuffer[(size_t) i] * gain);
        left[i] = v;
        if (right != nullptr) right[i] = v;
    }
    if (gain < 1e-4f && ! wantPlay) gain = 0.0f;
    if (readPos >= (double) data->length()) { playing = false; readPos = 0; position = 0; }
}
}
