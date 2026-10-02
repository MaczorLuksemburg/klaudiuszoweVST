#include "Engine.h"

namespace floormatch::dsp
{
namespace
{
    constexpr float silenceMeanSquare = 1.0e-12f;   // frames below -120 dBFS count as digital silence
    constexpr float tiny = 1.0e-30f;

    // Cut detection between takes, per decision band.
    constexpr float stepRatio = 2.0f;               // the two sides' floors differ by more than 3 dB
    constexpr float stationaryRatio = 5.0f;         // 30th percentile within 7 dB of the minimum: noise, not speech
    constexpr int minBandFrames = 8;

    // A cut between takes changes the background at once (a hard cut or a short crossfade); a
    // noise event inside a take (close cloth rustle, a passing car) fades in and out. Steps over
    // 6 dB only count as cuts when they complete within 150 ms, so events are left alone.
    constexpr float sharpTestMinStepDb = 6.0f;
    constexpr double maxCutSeconds = 0.15, cutTrackHalfSeconds = 0.05;
    constexpr int minBinFrames = 3;

    // Frames more than 6 dB over the current noise estimate count as speech and stay out of the
    // noise statistics; a bin whose window holds fewer than 6 speech-free frames per side (a long
    // phrase) keeps its last estimate for up to 4 s.
    constexpr float speechLabelRatio = 4.0f;
    constexpr int minNoiseFrames = 6;
    constexpr double maxHoldSeconds = 8.0, maxHoldSecondsNoLookahead = 4.0;

    // Without a detected cut a bin's floor may only rise slowly (it falls freely): speech that
    // slips through the labels can't pull it up far before the next pause pulls it back.
    constexpr double maxRiseDbPerSecond = 2.0, maxRiseDbPerSecondNoLookahead = 6.0;
    constexpr float riseTolerance = 1.41f;           // +1.5 dB

    // Robust mean of the noise-like frames of one side: everything within +8 dB of the minimum,
    // then everything within +4 dB of that first mean (speech peaks drop out).
    constexpr float firstThreshold = 6.3f;
    constexpr float secondThreshold = 2.5f;
    constexpr float biasCorrection = 1.035f;         // calibrated on white noise by the tests

    // Speech gain.

    constexpr float aes17 = 3.0103f;                // a full-scale sine reads 0 dB

    // Upper edges of the decision bands (roughly critical bands); the last band runs to Nyquist.
    constexpr float decisionBandEdges[] { 150, 250, 350, 450, 570, 700, 840, 1000, 1170, 1370, 1600, 1850,
                                          2150, 2500, 2900, 3400, 4000, 4800, 5800, 7000, 8500, 10500, 13500 };

    // Exponential integral E1(x) for x > 0 (Abramowitz & Stegun 5.1.53 and 5.1.56).
    float expIntegral (float x)
    {
        if (x < 1.0f)
        {
            const float poly = -0.57721566f + x * (0.99999193f + x * (-0.24991055f + x * (0.05519968f
                                 + x * (-0.00976004f + x * 0.00107857f))));
            return poly - std::log (std::max (x, 1.0e-30f));
        }

        const float num = x * x + 2.334733f * x + 0.250621f;
        const float den = x * x + 3.330657f * x + 1.681534f;
        return num / den * std::exp (-x) / x;
    }

    float smoothstep (float x)
    {
        x = juce::jlimit (0.0f, 1.0f, x);
        return x * x * (3.0f - 2.0f * x);
    }

    float toDb (float power) { return 10.0f * std::log10 (std::max (power, 1.0e-20f)); }

    // Did a level sequence (dB, in time order) step up from the part before `split` to the part
    // after it within maxFrames? Works on a short 20th-percentile track, which ignores speech
    // peaks. Falls are tested on the reversed sequence.
    bool isSharpRise (const float* levels, int n, int split, float minStepDb, int halfWidth, int maxFrames,
                      std::vector<float>& track, std::vector<float>& window)
    {
        if (split <= 0 || split >= n)
            return true;

        for (int i = 0; i < n; ++i)
        {
            const int a = std::max (0, i - halfWidth), b = std::min (n, i + halfWidth + 1);
            std::copy (levels + a, levels + b, window.begin());
            auto nth = window.begin() + (b - a) / 5;
            std::nth_element (window.begin(), nth, window.begin() + (b - a));
            track[(size_t) i] = *nth;
        }

        auto percentile = [&] (int from, int to, int percent)
        {
            std::copy (track.begin() + from, track.begin() + to, window.begin());
            auto nth = window.begin() + (to - from) * percent / 100;
            std::nth_element (window.begin(), nth, window.begin() + (to - from));
            return *nth;
        };

        // Both levels from the quiet end of each part: by the time a step is judged, most of the
        // first part may already be the new level, and speech may fill much of either part.
        const float before = percentile (0, split, 10), after = percentile (split, n, 25);
        const float step = after - before;
        if (step <= minStepDb)
            return true;

        // From the last frame still near the old level to the first frame near the new one. Speech
        // before a cut sits above the in-between band, so it can't stretch the step; a fade-in can.
        const float low = before + 0.25f * step, high = before + 0.9f * step;
        int lastOld = -1;
        for (int i = n - 1; i >= 0; --i)
            if (track[(size_t) i] <= low) { lastOld = i; break; }

        for (int i = lastOld + 1; i < n; ++i)
            if (track[(size_t) i] >= high)
                return i - lastOld <= maxFrames;

        return false;
    }

    double aWeightingAmplitude (double hz)
    {
        const double f2 = hz * hz;
        const double c1 = 20.6 * 20.6, c2 = 107.7 * 107.7, c3 = 737.9 * 737.9, c4 = 12194.0 * 12194.0;
        return c4 * f2 * f2 / ((f2 + c1) * std::sqrt ((f2 + c2) * (f2 + c3)) * (f2 + c4));
    }
}

//==============================================================================
float aWeighting (float hz)
{
    static const double reference = aWeightingAmplitude (1000.0);
    const double a = aWeightingAmplitude ((double) hz) / reference;
    return (float) (a * a);
}

float profilePsdAt (const Profile& profile, float hz)
{
    const float position = juce::jlimit (0.0f, (float) (numProfileBands - 1), 6.0f * std::log2 (std::max (hz, 1.0f) / 20.0f));
    const int lower = std::min ((int) position, numProfileBands - 2);
    const float fraction = position - (float) lower;
    const float a = std::log (std::max (profile.psd[(size_t) lower], tiny));
    const float b = std::log (std::max (profile.psd[(size_t) lower + 1], tiny));
    return std::exp (a + fraction * (b - a));
}

float profileLevelDb (const Profile& profile)
{
    // Integrate on a 1/48-octave grid, fine enough to match a per-bin sum.
    constexpr int stepsPerOctave = 48;
    const double gridRatio = std::exp2 (1.0 / stepsPerOctave);
    const double halfStep = std::exp2 (0.5 / stepsPerOctave);
    double sum = 0.0;

    for (double f = 20.0 * halfStep; f < 20000.0; f *= gridRatio)
        sum += aWeighting ((float) f) * profilePsdAt (profile, (float) f) * (f * halfStep - f / halfStep);

    return toDb ((float) sum) + aes17;
}

//==============================================================================
double Engine::lookaheadSeconds (int mode)
{
    constexpr double seconds[] { 0.0, 0.5, 1.0, 2.0 };
    return seconds[juce::jlimit (0, 3, mode)];
}

void Engine::prepare (double newSampleRate, int channels, int lookahead)
{
    sampleRate = newSampleRate;
    numChannels = std::max (1, channels);
    lookaheadMode = juce::jlimit (0, 3, lookahead);

    // About 21 ms frames at any sample rate (the bin spacing stays ~43-47 Hz).
    fftOrder = sampleRate <= 50000.0 ? 10 : sampleRate <= 100000.0 ? 11 : 12;
    fftSize = 1 << fftOrder;
    hop = fftSize / 4;
    numBins = fftSize / 2 + 1;
    fft = std::make_unique<juce::dsp::FFT> (fftOrder);

    window.resize ((size_t) fftSize);
    for (int n = 0; n < fftSize; ++n)
        window[(size_t) n] = std::sqrt (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) n / (float) fftSize));

    fftBuffer.assign ((size_t) fftSize * 2, 0.0f);

    const double framesPerSecond = sampleRate / hop;
    futureFrames = juce::roundToInt (lookaheadSeconds (lookaheadMode) * framesPerSecond);
    pastFrames = lookaheadMode == lookaheadOff ? juce::roundToInt (1.5 * framesPerSecond) : futureFrames;

    // The two windows each estimate compares: before and after the frame, or without lookahead
    // the older part of the past against its last 0.4 s (a cut is then followed 0.4 s late).
    if (lookaheadMode == lookaheadOff)
    {
        const int recent = juce::roundToInt (0.4 * framesPerSecond);
        pastWindow = { -pastFrames, -recent };
        futureWindow = { -recent, 0 };
    }
    else
    {
        pastWindow = { -pastFrames, 0 };
        futureWindow = { 0, futureFrames };
    }

    decimation = std::max (1, juce::roundToInt (std::max (pastWindow.length(), futureWindow.length()) / 48.0));

    delayFrames = smoothFrames + futureFrames + attackFrames;
    latency = fftSize + delayFrames * hop;

    spectrumLength = delayFrames + 1;
    powerLength = delayFrames + 2 * smoothFrames + 3;
    smoothLength = pastFrames + futureFrames + 1;
    gainLength = attackFrames + 1;

    // |Y|^2 of a frame -> mean-square power per Hz (one-sided; the window's energy is N / 2).
    psdScale = (float) (4.0 / ((double) fftSize * sampleRate));
    const float binWidth = (float) (sampleRate / fftSize);

    binFrequency.resize ((size_t) numBins);
    weightedBin.resize ((size_t) numBins);

    for (int k = 0; k < numBins; ++k)
    {
        const float f = (float) k * binWidth;
        binFrequency[(size_t) k] = f;
        weightedBin[(size_t) k] = (k > 0 && f >= 20.0f && f <= 20000.0f) ? aWeighting (f) * psdScale * binWidth : 0.0f;
    }

    bandStart.clear();
    bandStart.push_back (0);
    for (float edge : decisionBandEdges)
    {
        const int bin = std::min (numBins, (int) std::ceil (edge / binWidth));
        if (bin > bandStart.back())
            bandStart.push_back (bin);
    }
    if (bandStart.back() < numBins)
        bandStart.push_back (numBins);

    bands.assign (bandStart.size() - 1, {});
    markLength = 2 * std::max (2, juce::roundToInt (0.15 * framesPerSecond));
    markHistory.assign ((size_t) markLength * bands.size(), 0.0f);
    cutHold.assign (bands.size(), 0);

    for (int i = 0; i < numProfileBands; ++i)
    {
        const float centre = profileBandFrequency (i);
        const float low = centre * std::exp2 (-1.0f / 12.0f), high = centre * std::exp2 (1.0f / 12.0f);
        auto& map = bandMap[(size_t) i];
        map.start = std::max (1, (int) std::ceil (low / binWidth));
        map.end = std::min (numBins, (int) std::ceil (high / binWidth));
        map.width = high - low;

        // Bands below the first bin take its value (bin 0 holds DC and low-frequency leakage).
        const float position = juce::jlimit (1.0f, (float) (numBins - 2), centre / binWidth);
        map.lower = (int) position;
        map.fraction = position - (float) map.lower;
    }

    for (int i = 0; i < numDisplayBands; ++i)
    {
        const float centre = displayBandFrequency (i);
        const float low = centre * std::exp2 (-1.0f / 24.0f), high = centre * std::exp2 (1.0f / 24.0f);
        auto& map = displayMap[(size_t) i];
        map.start = std::max (1, (int) std::ceil (low / binWidth));
        map.end = std::min (numBins, (int) std::ceil (high / binWidth));
        map.width = high - low;
        const float position = juce::jlimit (1.0f, (float) (numBins - 2), centre / binWidth);
        map.lower = (int) position;
        map.fraction = position - (float) map.lower;
    }

    columnFrames = std::max (1, juce::roundToInt (0.05 * framesPerSecond));
    releaseCoeff = (float) std::exp (-hop / (tuning.releaseSeconds * sampleRate));

    inputRing.assign ((size_t) numChannels, std::vector<float> ((size_t) fftSize, 0.0f));
    outputRing.assign ((size_t) numChannels, std::vector<float> ((size_t) fftSize, 0.0f));
    spectra.assign ((size_t) numChannels, std::vector<Complex> ((size_t) (spectrumLength * numBins)));

    power.assign ((size_t) (powerLength * numBins), 0.0f);
    powerValid.assign ((size_t) powerLength, 0);
    smoothed.assign ((size_t) (smoothLength * numBins), 0.0f);
    bandSmoothed.assign ((size_t) smoothLength * bands.size(), 0.0f);
    smoothedValid.assign ((size_t) smoothLength, 0);
    speechLabel.assign ((size_t) (smoothLength * numBins), 0);
    referenceNoise.assign ((size_t) numBins, 0.0f);
    holdFrames.assign ((size_t) numBins, 0);
    riseTrack.assign ((size_t) numBins, 0.0f);
    noiseHistory.assign ((size_t) (gainLength * numBins), 0.0f);
    speechHistory.assign ((size_t) (gainLength * numBins), 1.0f);
    noiseHistoryValid.assign ((size_t) gainLength, 0);

    const int spanLength = futureWindow.end - pastWindow.start + 1;
    spanLevels.assign ((size_t) spanLength, 0.0f);
    spanReversed.assign ((size_t) spanLength, 0.0f);
    sharpTrack.assign ((size_t) spanLength, 0.0f);
    cutTrackHalfWidth = std::max (1, juce::roundToInt (cutTrackHalfSeconds * framesPerSecond));
    maxCutFrames = juce::roundToInt (maxCutSeconds * framesPerSecond);
    sharpWindow.assign ((size_t) std::max (spanLength, 2 * cutTrackHalfWidth + 1), 0.0f);
    scratchPast.assign ((size_t) pastWindow.length() + 1, 0.0f);
    scratchFuture.assign ((size_t) futureWindow.length() + 1, 0.0f);
    pastSlots.clear();
    pastSlots.reserve ((size_t) (pastWindow.length() / decimation + 2));
    nearFrames = std::max (2, juce::roundToInt (0.25 * framesPerSecond));
    nearSlots.clear();
    nearSlots.reserve (32);
    futureSlots.clear();
    futureSlots.reserve ((size_t) (futureWindow.length() / decimation + 2));

    previousSpeech.assign ((size_t) numBins, 0.0f);
    releasedGain.assign ((size_t) numBins, 0.0f);
    tempGain.assign ((size_t) numBins, 0.0f);
    noiseScratch.assign ((size_t) numBins, 0.0f);
    targetShape.assign ((size_t) numBins, 0.0f);
    beta.assign ((size_t) numBins, 1.0f);
    fillPower.assign ((size_t) numBins, 0.0f);
    latestNoise.assign ((size_t) numBins, 0.0f);
    targetPower.assign ((size_t) numBins, 0.0f);

    updateTargetShape();
    reset();
}

void Engine::reset()
{
    for (auto& ring : inputRing)  std::fill (ring.begin(), ring.end(), 0.0f);
    for (auto& ring : outputRing) std::fill (ring.begin(), ring.end(), 0.0f);
    for (auto& s : spectra)       std::fill (s.begin(), s.end(), Complex {});

    std::fill (power.begin(), power.end(), 0.0f);
    std::fill (powerValid.begin(), powerValid.end(), (uint8_t) 0);
    std::fill (smoothedValid.begin(), smoothedValid.end(), (uint8_t) 0);
    std::fill (speechLabel.begin(), speechLabel.end(), (uint8_t) 0);
    std::fill (holdFrames.begin(), holdFrames.end(), 0);
    std::fill (cutHold.begin(), cutHold.end(), 0);
    heldBins = 0;
    std::fill (riseTrack.begin(), riseTrack.end(), 0.0f);
    referenceValid = false;
    std::fill (noiseHistoryValid.begin(), noiseHistoryValid.end(), (uint8_t) 0);
    std::fill (speechHistory.begin(), speechHistory.end(), 1.0f);
    std::fill (previousSpeech.begin(), previousSpeech.end(), 0.0f);
    std::fill (releasedGain.begin(), releasedGain.end(), 0.0f);

    ringPos = 0;
    hopCounter = 0;
    frameCounter = 0;
    fillGate = 0.0f;
    stableFrames = 0;
    frameSinceSnapshot = 0;
    snapshot.valid = false;
    latestNoiseValid = false;

    timelineCount = 0;
    framesInColumn = validInColumn = 0;
    columnInput = columnNoise = columnOutput = 0.0;
    columnCut = columnEvent = false;
    markFrames = 0;
    markPeak = 0.0f;
    markPeakFrame = 0;
    lastMarkFrame = -(1 << 30);
}

void Engine::setTuning (const Tuning& newTuning)
{
    tuning = newTuning;
    releaseCoeff = (float) std::exp (-hop / (tuning.releaseSeconds * sampleRate));
}

void Engine::setProfile (const Profile& newProfile)
{
    profile = newProfile;
    updateTargetShape();
}

void Engine::updateTargetShape()
{
    if (! profile.valid || targetShape.empty())
        return;

    for (int k = 0; k < numBins; ++k)
        targetShape[(size_t) k] = profilePsdAt (profile, std::max (binFrequency[(size_t) k], 20.0f)) / psdScale;

    targetShapeLevelDb = weightedLevelDb (targetShape.data());
}

//==============================================================================
void Engine::process (float* const* channels, int numChannelsIn, int numSamples)
{
    jassert (numChannelsIn == numChannels);
    juce::ignoreUnused (numChannelsIn);

    const int mask = fftSize - 1;

    for (int i = 0; i < numSamples; ++i)
    {
        for (int ch = 0; ch < numChannels; ++ch)
        {
            inputRing[(size_t) ch][(size_t) ringPos] = channels[ch][i];
            auto& out = outputRing[(size_t) ch][(size_t) ringPos];
            channels[ch][i] = out;
            out = 0.0f;
        }

        ringPos = (ringPos + 1) & mask;

        if (++hopCounter == hop)
        {
            hopCounter = 0;
            processFrame();
        }
    }
}

// Pipeline for the newest frame f: analyse f, smooth f - 2, estimate the noise and the speech
// gain of c = f - 2 - lookahead, and synthesise o = c - 4 (so the speech gain can open early).
void Engine::processFrame()
{
    const int64_t newest = frameCounter++;
    analyse (newest);

    const int64_t centre = newest - smoothFrames;
    if (centre >= 0)
        smooth (centre);

    const int64_t current = centre - futureFrames;
    if (current >= 0)
    {
        const int slot = slotOf (current, gainLength);
        float* noise = noiseHistory.data() + slot * numBins;
        const bool valid = estimateNoise (current, noise);
        noiseHistoryValid[(size_t) slot] = valid ? 1 : 0;
        computeSpeechGain (current, noise, valid, speechHistory.data() + slot * numBins);
    }

    const int64_t output = current - attackFrames;
    if (output >= 0)
        synthesise (output);
}

void Engine::analyse (int64_t frame)
{
    const int mask = fftSize - 1;
    const int slot = slotOf (frame, spectrumLength);
    const int powerSlot = slotOf (frame, powerLength);
    float* p = power.data() + powerSlot * numBins;
    std::fill (p, p + numBins, 0.0f);

    for (int ch = 0; ch < numChannels; ++ch)
    {
        const auto& in = inputRing[(size_t) ch];
        for (int n = 0; n < fftSize; ++n)
            fftBuffer[(size_t) n] = in[(size_t) ((ringPos + n) & mask)] * window[(size_t) n];

        std::fill (fftBuffer.begin() + fftSize, fftBuffer.end(), 0.0f);
        fft->performRealOnlyForwardTransform (fftBuffer.data(), true);

        Complex* dst = spectra[(size_t) ch].data() + slot * numBins;
        const bool analysed = ch < analysisChannels;
        for (int k = 0; k < numBins; ++k)
        {
            dst[k] = { fftBuffer[(size_t) (2 * k)], fftBuffer[(size_t) (2 * k + 1)] };
            if (analysed)
                p[k] += std::norm (dst[k]);
        }
    }

    double sum = 0.0;
    const float channelScale = 1.0f / (float) std::min (numChannels, analysisChannels);
    for (int k = 0; k < numBins; ++k)
    {
        p[k] *= channelScale;
        sum += p[k];
    }

    // Parseval with the window's energy (N / 2): mean square = sum over both sides / (N * N / 2).
    const double meanSquare = 2.0 * sum / ((double) fftSize * fftSize * 0.5);
    powerValid[(size_t) powerSlot] = meanSquare > silenceMeanSquare ? 1 : 0;
}

// S(t) = average of P over frames t - 2 .. t + 2 and a [1/4 1/2 1/4] kernel across bins. The lowest
// bins, where spectra are steep and bins few, skip the kernel and average t - 4 .. t + 4 instead,
// which gives them about the same statistics.
void Engine::smooth (int64_t frame)
{
    constexpr int innerFrames = 2;

    auto frameValid = [this] (int64_t t) { return t >= 0 && powerValid[(size_t) slotOf (t, powerLength)] != 0; };

    bool valid = true;
    for (int j = -innerFrames; j <= innerFrames; ++j)
        valid = valid && frameValid (frame + j);

    const int slot = slotOf (frame, smoothLength);
    smoothedValid[(size_t) slot] = valid ? 1 : 0;

    if (! valid)
        return;

    std::fill (tempGain.begin(), tempGain.end(), 0.0f);
    std::fill (noiseScratch.begin(), noiseScratch.begin() + lowBins, 0.0f);
    int lowCount = 0;

    for (int j = -smoothFrames; j <= smoothFrames; ++j)
    {
        if (! frameValid (frame + j))
            continue;

        const float* p = power.data() + slotOf (frame + j, powerLength) * numBins;
        for (int k = 0; k < lowBins; ++k)
            noiseScratch[(size_t) k] += p[k];
        ++lowCount;

        if (std::abs (j) <= innerFrames)
            for (int k = 0; k < numBins; ++k)
                tempGain[(size_t) k] += p[k];
    }

    const float scale = 1.0f / (float) (2 * innerFrames + 1);

    for (size_t b = 0; b < bands.size(); ++b)
    {
        float bandSum = 0.0f;

        for (int k = bandStart[b]; k < bandStart[b + 1]; ++k)
        {
            float s;
            if (k < lowBins)
            {
                s = noiseScratch[(size_t) k] / (float) lowCount;
            }
            else
            {
                const float left  = tempGain[(size_t) (k - 1)];
                const float right = tempGain[(size_t) std::min (numBins - 1, k + 1)];
                s = (0.25f * left + 0.5f * tempGain[(size_t) k] + 0.25f * right) * scale;
            }

            smoothed[(size_t) (k * smoothLength + slot)] = s;
            speechLabel[(size_t) (k * smoothLength + slot)] = referenceValid && s > speechLabelRatio * referenceNoise[(size_t) k] ? 1 : 0;
            bandSum += s;
        }

        bandSmoothed[b * (size_t) smoothLength + (size_t) slot] = bandSum;
    }
}

float Engine::truncatedMean (const float* row, const uint8_t* labels, const std::vector<int>& slots, int& count) const
{
    float minimum = std::numeric_limits<float>::max();
    count = 0;

    for (int s : slots)
    {
        if (labels == nullptr || labels[s] == 0)
        {
            minimum = std::min (minimum, row[s]);
            ++count;
        }
    }

    if (count == 0)
        return 0.0f;

    auto meanBelow = [&] (float threshold)
    {
        double sum = 0.0;
        int n = 0;
        for (int s : slots)
        {
            if (row[s] < threshold && (labels == nullptr || labels[s] == 0))
            {
                sum += row[s];
                ++n;
            }
        }
        return n > 0 ? (float) (sum / n) : threshold;
    };

    const float first = meanBelow (std::max (minimum, tiny) * firstThreshold);
    return biasCorrection * meanBelow (first * secondThreshold);
}

bool Engine::estimateNoise (int64_t frame, float* noise)
{
    // Frames of each side, decimated for the per-bin means (always including the frame itself).
    pastSlots.clear();
    futureSlots.clear();

    for (int64_t t = frame + pastWindow.end; t >= frame + pastWindow.start && t >= 0; t -= decimation)
        if (smoothedValid[(size_t) slotOf (t, smoothLength)] != 0)
            pastSlots.push_back (slotOf (t, smoothLength));

    for (int64_t t = frame + futureWindow.start; t <= frame + futureWindow.end; t += decimation)
        if (t >= 0 && smoothedValid[(size_t) slotOf (t, smoothLength)] != 0)
            futureSlots.push_back (slotOf (t, smoothLength));

    // Frames right around this one (0.25 s each way, or just before it without lookahead).
    nearSlots.clear();
    const int nearStep = std::max (1, nearFrames / 12);
    for (int64_t t = frame - nearFrames; t <= frame + std::min (nearFrames, futureWindow.end); t += nearStep)
        if (t >= 0 && smoothedValid[(size_t) slotOf (t, smoothLength)] != 0)
            nearSlots.push_back (slotOf (t, smoothLength));

    const bool hasPast = (int) pastSlots.size() >= minBinFrames;
    const bool hasFuture = (int) futureSlots.size() >= minBinFrames;

    if (! hasPast && ! hasFuture)
    {
        markFrames = 0;
        markPeak = 0.0f;
        return false;
    }

    // ---- cut detection per band, at full frame resolution ----------------------------------
    int rises = 0, falls = 0, compared = 0;

    for (size_t b = 0; b < bands.size(); ++b)
    {
        auto& band = bands[b];
        band = {};

        if (! hasPast || ! hasFuture)
            continue;

        const float* row = bandSmoothed.data() + b * (size_t) smoothLength;
        int numPast = 0, numFuture = 0, numSpan = 0;
        bool spanHasGap = false;

        // Both windows in time order, for the sharpness test (a digital-silence gap counts as a cut).
        for (int64_t t = std::max<int64_t> (0, frame + pastWindow.start); t <= frame + futureWindow.end; ++t)
        {
            if (smoothedValid[(size_t) slotOf (t, smoothLength)] != 0)
                spanLevels[(size_t) numSpan++] = toDb (row[slotOf (t, smoothLength)]);
            else
                spanHasGap = true;
        }

        for (int64_t t = std::max<int64_t> (0, frame + pastWindow.start); t <= frame + pastWindow.end; ++t)
            if (smoothedValid[(size_t) slotOf (t, smoothLength)] != 0)
                scratchPast[(size_t) numPast++] = row[slotOf (t, smoothLength)];

        for (int64_t t = std::max<int64_t> (0, frame + futureWindow.start); t <= frame + futureWindow.end; ++t)
            if (smoothedValid[(size_t) slotOf (t, smoothLength)] != 0)
                scratchFuture[(size_t) numFuture++] = row[slotOf (t, smoothLength)];

        // A cut decision needs most of both windows: a few frames of one vowel before digital
        // silence would otherwise look like a steady new floor.
        if (numPast < std::max (minBandFrames, pastWindow.length() / 2) || numFuture < std::max (minBandFrames, futureWindow.length() / 2))
            continue;

        auto floorOf = [] (std::vector<float>& values, int n, float& minimum, float& percentile30)
        {
            minimum = std::max (tiny, *std::min_element (values.begin(), values.begin() + n));
            auto nth = values.begin() + (n * 3) / 10;
            std::nth_element (values.begin(), nth, values.begin() + n);
            percentile30 = *nth;
        };

        float minPast, p30Past, minFuture, p30Future;
        floorOf (scratchPast, numPast, minPast, p30Past);
        floorOf (scratchFuture, numFuture, minFuture, p30Future);

        band.ratio = minFuture / minPast;
        ++compared;

        // A cut: the floor steps and the side after the step looks like steady noise (speech
        // would leave a much lower minimum than its 30th percentile).
        auto sharp = [&] (bool rise)
        {
            if (spanHasGap)
                return true;

            const int split = std::clamp (futureWindow.start - pastWindow.start, 1, numSpan - 1);
            if (rise)
                return isSharpRise (spanLevels.data(), numSpan, split, sharpTestMinStepDb, cutTrackHalfWidth, maxCutFrames, sharpTrack, sharpWindow);

            std::reverse_copy (spanLevels.begin(), spanLevels.begin() + numSpan, spanReversed.begin());
            return isSharpRise (spanReversed.data(), numSpan, numSpan - split, sharpTestMinStepDb, cutTrackHalfWidth, maxCutFrames, sharpTrack, sharpWindow);
        };

        if (band.ratio > stepRatio && p30Future < stationaryRatio * minFuture && sharp (true))
        {
            band.side = sideFuture;
            ++rises;
        }
        else if (band.ratio < 1.0f / stepRatio && p30Past < stationaryRatio * minPast && sharp (false))
        {
            band.side = sidePast;
            ++falls;
        }
    }

    // A cut between takes changes the background across most bands at once: every band then keeps
    // to the current take's side, bounded by the typical step where that side may hold continuous
    // speech. A step in only a few bands is an event (a crackle of cloth rustle, a hum starting):
    // those bands stay on the normal path, which holds the background through it.
    float riseRatio = 1.0f, fallRatio = 1.0f;

    auto medianRatio = [this] (int side)
    {
        std::array<float, 32> values {};
        int n = 0;
        for (const auto& band : bands)
            if (band.side == side && n < (int) values.size())
                values[(size_t) n++] = band.ratio;

        std::nth_element (values.begin(), values.begin() + n / 2, values.begin() + n);
        return values[(size_t) (n / 2)];
    };

    // While the background is being held through an event, steps are the event's own ups and
    // downs; taking one as a cut would adopt the event as the background.
    const bool insideEvent = heldBins > numBins / 4;

    if (insideEvent)
    {
        for (auto& band : bands)
            band.side = sideNone;
    }
    else if (rises >= 3 && rises * 2 >= compared)
    {
        riseRatio = medianRatio (sideFuture);
        for (auto& band : bands)
            if (band.side != sideFuture)
                band.side = band.ratio > 1.0f ? sideBoundedFuture : sideFuture;
    }
    else if (falls >= 3 && falls * 2 >= compared)
    {
        fallRatio = medianRatio (sidePast);
        for (auto& band : bands)
            if (band.side != sidePast)
                band.side = band.ratio < 1.0f ? sideBoundedPast : sidePast;
    }
    else if (rises < std::max (4, compared / 4))
    {
        // A step up in fewer than a quarter of the bands is an event: those bands stay on the normal
        // path. (Between a quarter and a half it is still a cut whose new take differs only in part
        // of the spectrum, like hiss giving way to rumble.) Steps down keep their decision: until
        // the cut, those bands still belong to the louder take.
        for (auto& band : bands)
            if (band.side == sideFuture)
                band.side = sideNone;
    }

    // With lookahead a cut up is seen while the new take fills the future window, but speech in
    // that window soon hides the step. Keep to the new take until the past window holds no frames
    // from before the cut, or the "lower side" rule would fall back to the old take's background.
    if (lookaheadMode != lookaheadOff)
    {
        const bool cutUp = ! insideEvent && rises >= std::max (4, compared / 4) && rises >= falls;


        for (size_t b = 0; b < bands.size(); ++b)
        {
            auto& band = bands[b];

            if (cutUp)
            {
                cutHold[b] = pastWindow.length();
                if (band.side == sideNone)
                    band.side = band.ratio > 1.0f ? sideFuture : sideRecent;
            }
            else if (cutHold[b] > 0 && band.side == sideNone)
            {
                band.side = sideRecent;
            }

            cutHold[b] = std::max (0, cutHold[b] - 1);
        }
    }

    // Without lookahead the frame is the newest one, so after a cut in either direction the recent
    // window is the current take: keep using it until the older window has no frames from before.
    if (lookaheadMode == lookaheadOff)
    {
        if (falls >= 3 && falls * 2 >= compared)
            riseRatio = fallRatio;

        for (size_t b = 0; b < bands.size(); ++b)
        {
            auto& band = bands[b];
            if (band.side == sidePast)        band.side = sideFuture;
            if (band.side == sideBoundedPast) band.side = sideBoundedFuture;

            if (band.side != sideNone)
                cutHold[b] = pastWindow.length();
            else if (cutHold[b] > 0)
                band.side = sideRecent;

            cutHold[b] = std::max (0, cutHold[b] - 1);
        }
    }

    diagnostics.rises = rises;
    diagnostics.falls = falls;
    diagnostics.compared = compared;
    diagnostics.holdingBins = 0;
    for (size_t b = 0; b < bands.size() && b + 1 < sizeof (diagnostics.sides); ++b)
        diagnostics.sides[b] = "-PFpfR"[bands[b].side];

    // ---- per-bin noise floor ------------------------------------------------------------------
    // Without lookahead a cut up is only seen late, so holding and slow rises cost more there.
    const bool noLookahead = lookaheadMode == lookaheadOff;
    const int maxHoldFrames = (int) ((noLookahead ? maxHoldSecondsNoLookahead : maxHoldSeconds) * sampleRate / hop);
    const float riseLimit = std::pow (10.0f, (float) ((noLookahead ? maxRiseDbPerSecondNoLookahead : maxRiseDbPerSecond) * hop / sampleRate / 10.0));

    // Normal jitter passes (up to +1.5 dB over a slowly rising track); beyond that the rise is slow.
    auto limitRise = [&] (int k, float n)
    {
        auto& track = riseTrack[(size_t) k];
        if (! referenceValid || track <= 0.0f)
        {
            track = n;
            return n;
        }

        n = std::min (n, track * riseTolerance);
        track = n < track ? n : std::min (n, track * riseLimit);
        return n;
    };

    for (size_t b = 0; b < bands.size(); ++b)
    {
        const int side = bands[b].side;

        for (int k = bandStart[b]; k < bandStart[b + 1]; ++k)
        {
            const float* row = smoothed.data() + (size_t) k * (size_t) smoothLength;
            const uint8_t* labels = speechLabel.data() + (size_t) k * (size_t) smoothLength;
            int countPast = 0, countFuture = 0, unused = 0;
            bool holding = false, limited = false;
            float n;

            // After a cut up, the new take's frames all sit above the old estimate: ignore the labels there.
            auto mean = [&] (bool withLabels, const std::vector<int>& slots, int& count)
            {
                return truncatedMean (row, withLabels ? labels : nullptr, slots, count);
            };
            auto past   = [&] (bool withLabels) { return mean (withLabels, pastSlots, countPast); };
            auto future = [&] (bool withLabels) { return mean (withLabels, futureSlots, countFuture); };

            if (! hasPast)
            {
                n = future (false);
            }
            else if (! hasFuture)
            {
                n = past (true);
                if (countPast < minNoiseFrames)
                {
                    holding = referenceValid && holdFrames[(size_t) k] < maxHoldFrames;
                    n = holding ? referenceNoise[(size_t) k] : past (false);
                }
                n = limitRise (k, n);
                limited = true;
            }
            else if (side == sideFuture)
            {
                n = future (false);
            }
            else if (side == sideRecent)
            {
                n = future (true);
                if (countFuture < minNoiseFrames)
                    n = referenceValid ? referenceNoise[(size_t) k] : future (false);
            }
            else if (side == sideBoundedFuture)
            {
                n = std::min (future (false), mean (false, pastSlots, unused) * riseRatio);
            }
            else if (side == sidePast || side == sideBoundedPast)
            {
                n = past (true);
                if (countPast < minNoiseFrames)
                    n = past (false);
                if (side == sideBoundedPast)
                    n = std::min (n, mean (false, futureSlots, unused) / fallRatio);
            }
            else
            {
                // No cut: the speech-free frames of both sides, averaged when they agree. When they
                // don't, the side that matches the speech-free frames right around this one (a take
                // change that wasn't detected as a cut lies on the other side); failing that, the
                // lower one (speech can only raise a floor).
                const float p = past (true), f = future (true);
                const bool okPast = countPast >= minNoiseFrames, okFuture = countFuture >= minNoiseFrames;

                if (okPast && okFuture)
                {
                    if (f < p * 2.0f && p < f * 2.0f)
                    {
                        n = (p * (float) countPast + f * (float) countFuture) / (float) (countPast + countFuture);
                    }
                    else
                    {
                        int countNear = 0;
                        const float here = truncatedMean (row, labels, nearSlots, countNear);
                        const bool nearPast = here < p * 2.0f && p < here * 2.0f;
                        const bool nearFuture = here < f * 2.0f && f < here * 2.0f;
                        if (k >= lowBins && countNear >= minNoiseFrames / 2 && nearPast != nearFuture)   // the lowest bins are too noisy for this
                            n = nearPast ? p : f;
                        else
                            n = std::min (p, f);
                    }
                }
                else if (okPast)   n = p;
                else if (okFuture) n = f;
                else
                {
                    // Inside a long phrase: keep the last estimate for a while.
                    holding = referenceValid && holdFrames[(size_t) k] < maxHoldFrames;
                    n = holding ? referenceNoise[(size_t) k] : std::min (past (false), future (false));
                }

                n = limitRise (k, n);
                limited = true;
            }

            if (! limited)
                riseTrack[(size_t) k] = n;   // a cut: the track jumps with the estimate

            holdFrames[(size_t) k] = holding ? holdFrames[(size_t) k] + 1 : 0;
            diagnostics.holdingBins += holding ? 1 : 0;
            if (k == diagnostics.probeBin)
            {
                int cp = 0, cf = 0;
                diagnostics.probePast = truncatedMean (row, labels, pastSlots, cp);
                diagnostics.probeFuture = truncatedMean (row, labels, futureSlots, cf);
                diagnostics.probeCountPast = cp;
                diagnostics.probeCountFuture = cf;
                diagnostics.probeHold = holdFrames[(size_t) k];
                diagnostics.probeNoise = n;
                diagnostics.probeTrack = riseTrack[(size_t) k];
            }
            noise[k] = std::max (n, tiny);
        }
    }

    heldBins = diagnostics.holdingBins;

    markCuts (frame, noise);
    std::copy (noise, noise + numBins, referenceNoise.begin());
    referenceValid = true;

    return true;
}

void Engine::computeSpeechGain (int64_t frame, const float* noise, bool noiseValid, float* speechGain)
{
    const float* current = power.data() + slotOf (frame, powerLength) * numBins;

    if (! noiseValid)
    {
        std::fill (speechGain, speechGain + numBins, 1.0f);
        std::copy (current, current + numBins, previousSpeech.begin());
        return;
    }

    const float* previous = frame > 0 ? power.data() + slotOf (frame - 1, powerLength) * numBins : current;
    const float* next = power.data() + slotOf (frame + 1, powerLength) * numBins;

    for (int k = 0; k < numBins; ++k)
    {
        const float n = noise[k];
        const float gamma = current[k] / n;

        float xi = tuning.ddAlpha * previousSpeech[(size_t) k] / n + (1.0f - tuning.ddAlpha) * std::max (gamma - 1.0f, 0.0f);
        xi = std::max (xi, tuning.minPrioriSnr);

        // MMSE log-spectral amplitude gain (Ephraim & Malah).
        const float v = std::max (xi * gamma / (1.0f + xi), 1.0e-8f);
        const float g = tuning.wienerSpeechGain ? xi / (1.0f + xi)
                                                : std::min (1.0f, xi / (1.0f + xi) * std::exp (0.5f * expIntegral (v)));
        previousSpeech[(size_t) k] = g * g * current[k];

        // Speech presence from the power in a 3 x 3 neighbourhood (bins k-1..k+1, frames -1..+1),
        // which catches weak speech while random noise peaks average out.
        const int k0 = std::max (0, k - 1), k1 = std::min (numBins - 1, k + 1);
        float local = 0.0f;
        for (int kk = k0; kk <= k1; ++kk)
            local += previous[kk] + current[kk] + next[kk];
        local /= (float) (3 * (k1 - k0 + 1));

        const float presence = smoothstep ((toDb (local / n) - tuning.presenceLowDb) / (tuning.presenceHighDb - tuning.presenceLowDb));
        tempGain[(size_t) k] = presence * g;
    }

    for (int k = 0; k < numBins; ++k)
    {
        const float left  = tempGain[(size_t) std::max (0, k - 1)];
        const float right = tempGain[(size_t) std::min (numBins - 1, k + 1)];
        const float neighbourMean = 0.25f * left + 0.5f * tempGain[(size_t) k] + 0.25f * right;
        float g = tuning.smoothGainAcrossBins ? neighbourMean : tempGain[(size_t) k];
        if (g < tuning.spikeGain)
            g = std::min (g, neighbourMean);
        speechGain[k] = std::max (0.0f, (g - tuning.speechGainFloor) / (1.0f - tuning.speechGainFloor));
    }
}

float Engine::nextGaussian()
{
    // xorshift64* + Box-Muller (the second value of each pair is thrown away for simplicity).
    auto uniform = [this]
    {
        randomState ^= randomState >> 12;
        randomState ^= randomState << 25;
        randomState ^= randomState >> 27;
        return (float) (((randomState * 2685821657736338717ull) >> 40) + 1) / 16777217.0f;   // (0, 1]
    };

    const float u1 = uniform(), u2 = uniform();
    return std::sqrt (-2.0f * std::log (u1)) * std::cos (juce::MathConstants<float>::twoPi * u2);
}

void Engine::synthesise (int64_t frame)
{
    const int slot = slotOf (frame, gainLength);
    const float* noise = noiseHistory.data() + slot * numBins;
    const bool noiseValid = noiseHistoryValid[(size_t) slot] != 0;
    const bool inputAlive = powerValid[(size_t) slotOf (frame, powerLength)] != 0;

    // Speech gain: open ahead of onsets (lookahead), release slowly.
    for (int k = 0; k < numBins; ++k)
    {
        float g = speechHistory[(size_t) (slot * numBins + k)];
        float weight = 1.0f;

        for (int j = 1; j <= attackFrames; ++j)
        {
            weight *= tuning.attackStep;
            g = std::max (g, weight * speechHistory[(size_t) (slotOf (frame + j, gainLength) * numBins + k)]);
        }

        releasedGain[(size_t) k] = std::max (g, releasedGain[(size_t) k] * releaseCoeff);
    }

    // Target per bin and the attenuation (beta) that reaches it.
    float noiseDb = -150.0f;

    if (noiseValid)
    {
        noiseDb = weightedLevelDb (noise);
        const float match = profile.valid ? juce::jlimit (0.0f, 1.0f, settings.match) : 0.0f;
        const float levelLog = (settings.targetDb - noiseDb) * 0.23025851f;           // ln (power ratio)
        const float profileLog = (settings.targetDb - targetShapeLevelDb) * 0.23025851f;
        const float minBeta = std::pow (10.0f, -settings.maxReductionDb / 10.0f);

        for (int k = 0; k < numBins; ++k)
        {
            const float n = noise[k];
            float targetLog = levelLog;   // log (target / noise) for level-only matching

            if (match > 0.0f)
                targetLog = (1.0f - match) * levelLog + match * (std::log (std::max (targetShape[(size_t) k], tiny) / n) + profileLog);

            const float ratio = std::exp (juce::jlimit (-60.0f, 60.0f, targetLog));
            targetPower[(size_t) k] = ratio * n;
            beta[(size_t) k] = juce::jlimit (minBeta, 1.0f, ratio);
            fillPower[(size_t) k] = (settings.fill && ratio > 1.0f) ? (ratio - 1.0f) * n : 0.0f;
        }
    }
    else
    {
        std::fill (beta.begin(), beta.end(), 1.0f);
        std::fill (fillPower.begin(), fillPower.end(), 0.0f);
    }

    fillGate += ((inputAlive && noiseValid ? 1.0f : 0.0f) - fillGate) * 0.25f;
    const bool addFill = settings.fill && ! settings.listenRemoved && fillGate > 1.0e-4f;

    // Fill spectrum, shared by all channels: complex Gaussian with E|Z|^2 = 4 * power, which
    // after the synthesis window and overlap-add analyses back to the requested power.
    if (addFill)
    {
        for (int k = 0; k < numBins; ++k)
        {
            const float amplitude = (k == 0 || k == numBins - 1) ? 0.0f : std::sqrt (2.0f * fillPower[(size_t) k]) * fillGate;
            const float re = nextGaussian() * amplitude;
            const float im = nextGaussian() * amplitude;
            tempGain[(size_t) k] = re;
            noiseScratch[(size_t) k] = im;
        }
    }

    const int mask = fftSize - 1;
    const int spectrumSlot = slotOf (frame, spectrumLength);

    for (int ch = 0; ch < numChannels; ++ch)
    {
        const Complex* y = spectra[(size_t) ch].data() + spectrumSlot * numBins;

        for (int k = 0; k < numBins; ++k)
        {
            float gain = 1.0f;
            if (noiseValid)
            {
                const float floorGain = std::sqrt (beta[(size_t) k]);
                gain = floorGain + (1.0f - floorGain) * releasedGain[(size_t) k];
            }

            Complex z = y[k] * (settings.listenRemoved ? 1.0f - gain : gain);
            if (addFill)
                z += Complex (tempGain[(size_t) k], noiseScratch[(size_t) k]);

            fftBuffer[(size_t) (2 * k)] = z.real();
            fftBuffer[(size_t) (2 * k + 1)] = z.imag();
        }

        fft->performRealOnlyInverseTransform (fftBuffer.data());

        auto& out = outputRing[(size_t) ch];
        for (int n = 0; n < fftSize; ++n)
            out[(size_t) ((ringPos + n) & mask)] += fftBuffer[(size_t) n] * window[(size_t) n] * 0.5f;
    }

    // ---- metering, learning and the quietest floor -------------------------------------------
    const float* inputPower = power.data() + slotOf (frame, powerLength) * numBins;
    const float inputDb = inputAlive ? weightedLevelDb (inputPower) : -150.0f;

    latestNoiseValid = noiseValid;
    if (! noiseValid)
    {
        stableFrames = 0;
        snapshot.valid = false;
        addTimelineFrame (false, inputDb, -150.0f, -150.0f);
        return;
    }

    // Expected noise floor after processing: attenuated noise plus fill.
    for (int k = 0; k < numBins; ++k)
        noiseScratch[(size_t) k] = noise[k] * beta[(size_t) k] + (addFill ? fillPower[(size_t) k] : 0.0f);
    const float outputNoiseDb = weightedLevelDb (noiseScratch.data());
    addTimelineFrame (true, inputDb, noiseDb, outputNoiseDb);

    std::copy (noise, noise + numBins, latestNoise.begin());

    std::array<float, numProfileBands> noiseBands;
    binsToProfile (noise, noiseBands);

    if (learning)
    {
        for (size_t i = 0; i < noiseBands.size(); ++i)
            learnSum[i] += noiseBands[i];
        ++learnCount;
    }

    const float smoothing = 1.0f - std::exp (-(float) hop / (float) sampleRate);   // 1 s
    if (stableFrames == 0)
    {
        recentLevel = noiseDb;
        recentDeviation = 10.0f;
        recentBands = noiseBands;
    }
    else
    {
        recentLevel += smoothing * (noiseDb - recentLevel);
        recentDeviation += smoothing * (std::abs (noiseDb - recentLevel) - recentDeviation);
        for (size_t i = 0; i < noiseBands.size(); ++i)
            recentBands[i] += smoothing * (noiseBands[i] - recentBands[i]);
    }

    ++stableFrames;
    if (stableFrames > (int) (2.0 * sampleRate / hop) && recentDeviation < 0.5f
        && (! hasQuietest || recentLevel < quietestLevel - 0.05f))
    {
        quietestLevel = recentLevel;
        quietestBands = recentBands;
        hasQuietest = true;
    }

    if (++frameSinceSnapshot >= 4 || ! snapshot.valid)
    {
        frameSinceSnapshot = 0;

        binsToDisplay (noise, snapshot.noise);
        binsToDisplay (targetPower.data(), snapshot.target);
        binsToDisplay (noiseScratch.data(), snapshot.output);
        binsToDisplay (inputPower, snapshot.input);

        snapshot.noiseDb = noiseDb;
        snapshot.outputDb = outputNoiseDb;
        snapshot.valid = true;
        ++snapshotCounter;
    }
}

//==============================================================================
float Engine::weightedLevelDb (const float* binPower) const
{
    double sum = 0.0;
    for (int k = 0; k < numBins; ++k)
        sum += (double) weightedBin[(size_t) k] * binPower[k];

    return toDb ((float) sum) + aes17;
}

void Engine::binsToProfile (const float* binPower, std::array<float, numProfileBands>& psd) const
{
    for (int i = 0; i < numProfileBands; ++i)
    {
        const auto& map = bandMap[(size_t) i];
        float value;

        if (map.end > map.start)
        {
            double sum = 0.0;
            for (int k = map.start; k < map.end; ++k)
                sum += binPower[k];
            value = (float) (sum / (map.end - map.start));
        }
        else
        {
            value = binPower[map.lower] + map.fraction * (binPower[map.lower + 1] - binPower[map.lower]);
        }

        psd[(size_t) i] = value * psdScale;
    }
}

void Engine::binsToDisplay (const float* binPower, std::array<float, numDisplayBands>& levelsDb) const
{
    for (int i = 0; i < numDisplayBands; ++i)
    {
        const auto& map = displayMap[(size_t) i];
        float value;

        if (map.end > map.start)
        {
            double sum = 0.0;
            for (int k = map.start; k < map.end; ++k)
                sum += binPower[k];
            value = (float) (sum / (map.end - map.start));
        }
        else
        {
            value = binPower[map.lower] + map.fraction * (binPower[map.lower + 1] - binPower[map.lower]);
        }

        levelsDb[(size_t) i] = toDb (value * psdScale * map.width) + aes17;
    }
}

// Timeline markers: when the estimator moves to a new take, its background estimate steps across
// most of the spectrum and stays there (events are held and speech is skipped, so neither does).
// The step is measured between the 0.15 s before and after each frame and marked at its peak.
void Engine::markCuts (int64_t frame, const float* noise)
{
    const size_t numBands = bands.size();
    const int slot = slotOf (frame, markLength);
    for (size_t b = 0; b < numBands; ++b)
    {
        double sum = 0.0;
        for (int k = bandStart[b]; k < bandStart[b + 1]; ++k)
            sum += noise[k];
        markHistory[(size_t) slot * numBands + b] = toDb ((float) sum);
    }

    // Wait until the windows hold real audio (after starting or after digital silence).
    const int span = markLength / 2;
    if (++markFrames < markLength + pastWindow.length() + futureWindow.length())
        return;

    float step = 0.0f;
    for (size_t b = 0; b < numBands; ++b)
    {
        double before = 0.0, after = 0.0;
        for (int j = 0; j < span; ++j)
        {
            before += markHistory[(size_t) slotOf (frame - markLength + 1 + j, markLength) * numBands + b];
            after  += markHistory[(size_t) slotOf (frame - span + 1 + j, markLength) * numBands + b];
        }
        step += (float) std::abs (after - before) / (float) span;
    }
    step /= (float) numBands;

    constexpr float markThresholdDb = 3.0f;
    if (step > markThresholdDb && step > markPeak)
    {
        markPeak = step;
        markPeakFrame = frame - span;
    }
    else if (markPeak > 0.0f && (step < markThresholdDb || step < 0.7f * markPeak))
    {
        // The estimate at frame c is output with frame c - attackFrames.
        const int64_t outputFrame = markPeakFrame - attackFrames;
        if (outputFrame - lastMarkFrame > (int64_t) (1.0 * sampleRate / hop))   // at most one per second
        {
            lastMarkFrame = outputFrame;
            const int64_t column = outputFrame / columnFrames;
            if (column >= timelineCount)
                columnCut = true;
            else if (column > timelineCount - timelineCapacity && column >= 0)
                timeline[(size_t) (column % timelineCapacity)].cut = true;
        }
        markPeak = 0.0f;
    }
}

// Collects output frames into ~50 ms timeline columns.

void Engine::addTimelineFrame (bool valid, float inputDb, float noiseDb, float outputDb)
{
    columnInput += std::pow (10.0, inputDb / 10.0);
    if (valid)
    {
        columnNoise += std::pow (10.0, noiseDb / 10.0);
        columnOutput += std::pow (10.0, outputDb / 10.0);
        ++validInColumn;
    }
    columnEvent = columnEvent || heldBins > numBins / 4;
    if (++framesInColumn < columnFrames)
        return;

    auto& column = timeline[(size_t) (timelineCount % timelineCapacity)];
    column.valid = validInColumn * 2 >= framesInColumn;
    column.inputDb = toDb ((float) (columnInput / framesInColumn));
    column.noiseDb = validInColumn > 0 ? toDb ((float) (columnNoise / validInColumn)) : -150.0f;
    column.outputDb = validInColumn > 0 ? toDb ((float) (columnOutput / validInColumn)) : -150.0f;
    column.targetDb = settings.targetDb;
    column.cut = columnCut;   // markCuts may still set it on recent columns
    column.event = columnEvent && column.valid;
    column.limited = column.valid && column.noiseDb > settings.targetDb + 1.0f && column.outputDb > settings.targetDb + 1.0f;
    ++timelineCount;

    framesInColumn = validInColumn = 0;
    columnInput = columnNoise = columnOutput = 0.0;
    columnCut = columnEvent = false;
}

void Engine::setLearning (bool shouldLearn)
{
    if (shouldLearn && ! learning)
    {
        learnSum.fill (0.0);
        learnCount = 0;
    }

    learning = shouldLearn;
}

bool Engine::getLearnedProfile (Profile& result) const
{
    if (learnCount == 0)
        return false;

    for (size_t i = 0; i < learnSum.size(); ++i)
        result.psd[i] = (float) (learnSum[i] / learnCount);

    result.valid = true;
    return true;
}

void Engine::resetQuietest()
{
    hasQuietest = false;
}

bool Engine::getQuietest (Profile& result, float& levelDb) const
{
    if (! hasQuietest)
        return false;

    result.psd = quietestBands;
    result.valid = true;
    levelDb = quietestLevel;
    return true;
}
}
