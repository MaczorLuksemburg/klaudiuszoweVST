#include "TrackData.h"

namespace waveref
{
void TrackData::getPeaks (int64_t start, int64_t end, float& low, float& high) const
{
    start = juce::jlimit<int64_t> (0, getLength(), start);
    end   = juce::jlimit<int64_t> (start, getLength(), end);
    low = 0.0f;
    high = 0.0f;

    if (end <= start)
        return;

    if (end - start < 4 * peakBlock)
    {
        const float* l = audio.getReadPointer (0);
        const float* r = audio.getReadPointer (1);
        low = high = 0.5f * (l[start] + r[start]);

        for (int64_t i = start; i < end; ++i)
        {
            const float v = 0.5f * (l[i] + r[i]);
            low = juce::jmin (low, v);
            high = juce::jmax (high, v);
        }

        return;
    }

    const auto first = (size_t) (start / peakBlock);
    const auto last = juce::jmin (peakMin.size() - 1, (size_t) ((end - 1) / peakBlock));
    low = peakMin[first];
    high = peakMax[first];

    for (size_t i = first + 1; i <= last; ++i)
    {
        low = juce::jmin (low, peakMin[i]);
        high = juce::jmax (high, peakMax[i]);
    }
}

float TrackData::getLoopLoudness (int64_t start, int64_t end) const
{
    if (subEnergies.empty())
        return loudness::silenceLufs;

    const int64_t subLength = loudness::subBlockLength (sampleRate);
    const int64_t count = (int64_t) subEnergies.size();

    int64_t first = juce::jlimit<int64_t> (0, count - 1, start / subLength);
    int64_t last  = juce::jlimit<int64_t> (first + 1, count, end / subLength);

    return loudness::gatedLoudness (subEnergies.data() + first, (int) (last - first));
}

std::unique_ptr<TrackData> makeEmptyTrack (double sampleRate)
{
    auto track = std::make_unique<TrackData>();
    track->sampleRate = sampleRate;
    return track;
}

std::unique_ptr<TrackData> makeTrack (juce::AudioBuffer<float>&& stereoAudio, double sampleRate,
                                      const juce::String& name, const juce::String& path)
{
    jassert (stereoAudio.getNumChannels() == 2);

    auto track = std::make_unique<TrackData>();
    track->name = name;
    track->path = path;
    track->sampleRate = sampleRate;
    track->audio = std::move (stereoAudio);
    track->subEnergies = loudness::subBlockEnergies (track->audio, sampleRate);

    const int64_t length = track->getLength();
    const auto numBlocks = (size_t) ((length + TrackData::peakBlock - 1) / TrackData::peakBlock);
    track->peakMin.assign (numBlocks, 0.0f);
    track->peakMax.assign (numBlocks, 0.0f);

    const float* l = track->audio.getReadPointer (0);
    const float* r = track->audio.getReadPointer (1);

    for (size_t block = 0; block < numBlocks; ++block)
    {
        const int64_t from = (int64_t) block * TrackData::peakBlock;
        const int64_t to = juce::jmin (length, from + TrackData::peakBlock);
        float low = 0.5f * (l[from] + r[from]), high = low;

        for (int64_t i = from + 1; i < to; ++i)
        {
            const float v = 0.5f * (l[i] + r[i]);
            low = juce::jmin (low, v);
            high = juce::jmax (high, v);
        }

        track->peakMin[block] = low;
        track->peakMax[block] = high;
    }

    return track;
}

juce::AudioBuffer<float> resampleSinc (const juce::AudioBuffer<float>& input, double sourceRate, double targetRate)
{
    const double ratio = sourceRate / targetRate;
    const int inLength = input.getNumSamples();
    const int outLength = (int) ((double) inLength / ratio);
    const double cutoff = juce::jmin (1.0, 1.0 / ratio);

    constexpr int halfWidth = 12;     // zero crossings on each side of the kernel (at the cutoff frequency)
    constexpr int resolution = 128;   // table entries per zero crossing

    std::vector<float> table ((size_t) (halfWidth * resolution + 2), 0.0f);

    for (size_t j = 0; j < table.size(); ++j)
    {
        const double x = (double) j / resolution;
        const double u = x / halfWidth;
        const double pix = juce::MathConstants<double>::pi * x;
        const double sinc = x < 1.0e-9 ? 1.0 : std::sin (pix) / pix;
        const double window = u >= 1.0 ? 0.0
                                       : 0.35875 + 0.48829 * std::cos (juce::MathConstants<double>::pi * u)
                                                 + 0.14128 * std::cos (2.0 * juce::MathConstants<double>::pi * u)
                                                 + 0.01168 * std::cos (3.0 * juce::MathConstants<double>::pi * u);
        table[j] = (float) (sinc * window);
    }

    const int tapsEachSide = (int) std::ceil (halfWidth / cutoff);
    juce::AudioBuffer<float> output (input.getNumChannels(), outLength);

    for (int ch = 0; ch < input.getNumChannels(); ++ch)
    {
        const float* in = input.getReadPointer (ch);
        float* out = output.getWritePointer (ch);

        for (int i = 0; i < outLength; ++i)
        {
            const double position = (double) i * ratio;
            const int centre = (int) position;
            const int from = juce::jmax (0, centre - tapsEachSide + 1);
            const int to = juce::jmin (inLength - 1, centre + tapsEachSide);

            double accumulator = 0.0, weightSum = 0.0;

            for (int k = from; k <= to; ++k)
            {
                const double x = std::abs (((double) k - position) * cutoff);
                if (x >= halfWidth)
                    continue;

                const double index = x * resolution;
                const int i0 = (int) index;
                const float frac = (float) (index - i0);
                const double w = (double) table[(size_t) i0] + frac * (double) (table[(size_t) i0 + 1] - table[(size_t) i0]);

                accumulator += w * (double) in[k];
                weightSum += w;
            }

            out[i] = weightSum > 1.0e-9 ? (float) (accumulator / weightSum) : 0.0f;
        }
    }

    return output;
}

std::unique_ptr<TrackData> decodeTrack (juce::AudioFormatManager& formats, const juce::File& file,
                                        double targetRate, juce::String& error)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
    {
        error = "Can't read " + file.getFileName();
        return nullptr;
    }

    if ((double) reader->lengthInSamples / reader->sampleRate > TrackData::maxSeconds)
    {
        error = file.getFileName() + " is longer than 20 minutes";
        return nullptr;
    }

    const int length = (int) reader->lengthInSamples;
    juce::AudioBuffer<float> raw (2, length);

    // Mono files fill both channels; anything beyond two channels is ignored.
    if (reader->numChannels == 1)
    {
        reader->read (&raw, 0, length, 0, true, false);
        raw.copyFrom (1, 0, raw, 0, 0, length);
    }
    else
    {
        reader->read (&raw, 0, length, 0, true, true);
    }

    if (std::abs (reader->sampleRate - targetRate) > 0.5)
        raw = resampleSinc (raw, reader->sampleRate, targetRate);

    if (raw.getNumSamples() < 64)
    {
        error = file.getFileName() + " is too short";
        return nullptr;
    }

    auto track = makeTrack (std::move (raw), targetRate, file.getFileNameWithoutExtension(), file.getFullPathName());
    track->tempo = detectTempo (track->audio, targetRate);
    return track;
}
}
