#include "Stage.h"

namespace dynmap
{
namespace
{
    // One-pole coefficient for a time constant given in samples (0 = instant).
    float coefFor (double samples)
    {
        return samples < 0.5 ? 0.0f : (float) std::exp (-1.0 / samples);
    }

    constexpr float floorDb = -150.0f;

    // The followers read the peak of the last few milliseconds, so a steady tone reads its true
    // peak instead of sagging between waveform peaks. The level window follows the release
    // (short releases can still follow the waveform, like Maximus), capped at 10 ms.
    constexpr double maxPeakHoldMs = 10.0;
    constexpr double transientPeakMs = 5.0;
}

void Stage::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;

    osLatency[0] = 0;
    for (int q = 1; q < numQualities; ++q)
    {
        auto& os = oversamplers[(size_t) q];
        os = std::make_unique<juce::dsp::Oversampling<float>> (2, (size_t) q, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, true);
        os->initProcessing ((size_t) maxChunk);
        osLatency[(size_t) q] = juce::roundToInt (os->getLatencyInSamples());
    }

    const int maxOsLatency = *std::max_element (osLatency.begin(), osLatency.end());
    const int maxLookahead = (int) std::ceil (maxLookaheadMs * 0.001 * sampleRate);

    for (int ch = 0; ch < 2; ++ch)
    {
        audioDelay[(size_t) ch].prepare (maxLookahead + 4);
        detectorDelay[(size_t) ch].prepare (maxLookahead + 4);
        dryDelay[(size_t) ch].prepare (maxLookahead + maxOsLatency + 4);
        shaperDelay[(size_t) ch].prepare (maxOsLatency + 4);
    }

    shaperBuffer.setSize (2, maxChunk);

    const int maxPeakWindow = (int) std::ceil (maxPeakHoldMs * 0.001 * sampleRate) + 2;
    for (int ch = 0; ch < 2; ++ch)
    {
        levelPeak[(size_t) ch].prepare (maxPeakWindow);
        transientPeak[(size_t) ch].prepare (maxPeakWindow);
    }

    for (auto* s : { &preGain, &postGain, &mixAmount, &widthAmount, &shaperFade })
        s->reset (sampleRate, 0.02);

    dcCoeffs.setCutoff (5.0, sampleRate);
    snapNext = true;
    reset();
}

void Stage::reset()
{
    for (auto* lines : { &audioDelay, &detectorDelay, &dryDelay, &shaperDelay })
        for (auto& line : *lines)
            line.reset();

    for (auto& os : oversamplers)
        if (os != nullptr)
            os->reset();

    for (auto& s : scFilterState) s.reset();
    for (auto& s : dcState) s.reset();

    for (int ch = 0; ch < 2; ++ch)
    {
        levelPeak[(size_t) ch].reset (levelPeakLength);
        transientPeak[(size_t) ch].reset ((int) std::round (transientPeakMs * 0.001 * sampleRate));
    }

    meanSquare = {};
    smoothedGain = {};
    holdCounter = {};
    detectorPrimed = false;
    shaperWasActive = false;
    snapNext = true;
}

void Stage::pullCurves (CurveSlot& levelSlot, CurveSlot& transientSlot)
{
    levelSlot.pull (levelTable, levelVersion);
    transientSlot.pull (transientTable, transientVersion);
}

void Stage::setSettings (const StageSettings& s, const GlobalSettings& g, int extra)
{
    const int newQuality = juce::jlimit (0, numQualities - 1, g.quality);

    if (newQuality != quality && oversamplers[(size_t) newQuality] != nullptr)
        oversamplers[(size_t) newQuality]->reset();

    settings = s;
    global = g;
    quality = newQuality;
    lookahead = juce::jlimit (0, audioDelay[0].getMaxDelay() / 2, s.lookaheadSamples);
    extraDelay = juce::jlimit (0, detectorDelay[0].getMaxDelay() - 1, extra);

    auto set = [this] (juce::SmoothedValue<float>& v, float target)
    {
        if (snapNext) v.setCurrentAndTargetValue (target);
        else          v.setTargetValue (target);
    };

    set (preGain, juce::Decibels::decibelsToGain (s.preDb));
    set (postGain, juce::Decibels::decibelsToGain (s.postDb));
    set (mixAmount, s.bypass ? 0.0f : s.mix);
    set (widthAmount, s.width);
    set (shaperFade, (s.satType != satOff || s.mode == modeWaveshaper) ? 1.0f : 0.0f);
    snapNext = false;

    updateCoefficients();
}

void Stage::updateCoefficients()
{
    const double msToSamples = sampleRate * 0.001 * (double) global.timeScale;

    attackCoef  = coefFor (settings.attackMs * msToSamples);
    releaseCoef = coefFor (settings.releaseMs * msToSamples);
    holdSamples = (int) (settings.holdMs * msToSamples);
    rmsCoef     = coefFor (settings.rmsMs * msToSamples);

    levelPeakLength = juce::jlimit (1, (int) (maxPeakHoldMs * 0.001 * sampleRate), (int) std::round (settings.releaseMs * msToSamples));
    for (auto& peak : levelPeak)
        peak.setLength (levelPeakLength);
    smoothCoef  = coefFor (settings.smoothMs * sampleRate * 0.001);

    // Transient detection (like a transient designer): the attack side compares a fast and a
    // slow-attack follower with the same release; the sustain side compares a slow-release
    // follower with a fast-release one. Attacks read positive, decaying tails negative.
    const double tr = settings.trTimeMs * msToSamples;
    fastAttack  = coefFor (0.3 * sampleRate * 0.001);
    fastRelease = coefFor (tr * 0.5);
    slowAttack  = coefFor (tr);
    slowRelease = coefFor (tr * 4.0);

    scFilterOn = settings.scFilterHz > 10.5f;
    scFilterCoeffs.setCutoff (settings.scFilterHz, sampleRate);

    saturator.setup (settings.satType, settings.driveDb);
}

void Stage::process (float* left, float* right, const float* scLeft, const float* scRight, int numSamples)
{
    blockLevel = floorDb;
    blockTransient = 0.0f;
    blockGain = 0.0f;

    for (int start = 0; start < numSamples; start += maxChunk)
    {
        const int n = juce::jmin (maxChunk, numSamples - start);
        processChunk (left + start, right + start,
                      scLeft != nullptr ? scLeft + start : nullptr,
                      scRight != nullptr ? scRight + start : nullptr, n);
    }

    meter.levelDb.store (blockLevel);
    meter.transientDb.store (blockTransient);
    meter.gainDb.store (blockGain);
    meter.active.store (true);
}

void Stage::processChunk (float* left, float* right, const float* scLeft, const float* scRight, int n)
{
    const bool midSide = settings.stereo == stereoMS;
    const bool useSidechain = settings.scSource != scInternal && scLeft != nullptr && scRight != nullptr;
    const bool waveshaper = settings.mode == modeWaveshaper;
    const bool levelActive = ! waveshaper && ! levelTable.neutral;
    const bool dynamicsActive = levelActive || ! transientTable.neutral;
    const int latency = getLatency();
    const int audioLatency = lookahead + extraDelay;
    const float amount = global.amount;
    const float maxBoost = settings.maxBoost, maxCut = settings.maxCut;
    const float link = settings.link;

    if (! dynamicsActive)
        detectorPrimed = false;

    for (int i = 0; i < n; ++i)
    {
        dry[0][(size_t) i] = dryDelay[0].process (left[i], latency);
        dry[1][(size_t) i] = dryDelay[1].process (right[i], latency);

        const float pre = preGain.getNextValue();
        float a0 = left[i] * pre, a1 = right[i] * pre;
        float d0, d1;

        if (useSidechain)
        {
            d0 = scLeft[i] * pre;
            d1 = scRight[i] * pre;
        }
        else
        {
            d0 = a0;
            d1 = a1;
        }

        if (midSide)
        {
            const float m = 0.5f * (a0 + a1), s = 0.5f * (a0 - a1);
            a0 = m; a1 = s;
            const float dm = 0.5f * (d0 + d1), ds = 0.5f * (d0 - d1);
            d0 = dm; d1 = ds;
        }

        audio[0][(size_t) i] = audioDelay[0].process (a0, audioLatency);
        audio[1][(size_t) i] = audioDelay[1].process (a1, audioLatency);

        if (! dynamicsActive)
        {
            detectorDelay[0].process (d0, extraDelay);
            detectorDelay[1].process (d1, extraDelay);
            gain[0][(size_t) i] = gain[1][(size_t) i] = 1.0f;
            continue;
        }

        if (scFilterOn)
        {
            d0 -= scFilterState[0].lowpass (d0, scFilterCoeffs);
            d1 -= scFilterState[1].lowpass (d1, scFilterCoeffs);
        }

        d0 = detectorDelay[0].process (d0, extraDelay);
        d1 = detectorDelay[1].process (d1, extraDelay);

        // Level per channel (peak, or RMS over the RMS time). The followers run on linear
        // amplitude so zero crossings don't drag them down; the curves are read in dB.
        std::array<float, 2> level;
        const float d[2] { d0, d1 };

        for (int c = 0; c < 2; ++c)
        {
            if (rmsCoef > 0.0f)
            {
                auto& msq = meanSquare[(size_t) c];
                const float sq = d[c] * d[c];
                msq = sq + rmsCoef * (msq - sq);
                level[(size_t) c] = std::sqrt (msq);
            }
            else
            {
                level[(size_t) c] = std::abs (d[c]);
            }
        }

        const float loudest = juce::jmax (level[0], level[1]);
        level[0] += link * (loudest - level[0]);
        level[1] += link * (loudest - level[1]);

        std::array<float, 2> transientLevel;
        for (int c = 0; c < 2; ++c)
        {
            transientLevel[(size_t) c] = transientPeak[(size_t) c].push (level[(size_t) c]);
            level[(size_t) c] = levelPeak[(size_t) c].push (level[(size_t) c]);
        }

        if (! detectorPrimed)
        {
            for (int c = 0; c < 2; ++c)
            {
                envelope[(size_t) c] = level[(size_t) c];
                fastEnv[(size_t) c] = slowEnv[(size_t) c] = sustainEnv[(size_t) c] = transientLevel[(size_t) c];
                holdCounter[(size_t) c] = 0;
            }
        }

        for (int c = 0; c < 2; ++c)
        {
            const float x = level[(size_t) c];
            auto& env = envelope[(size_t) c];

            if (x > env)
            {
                env = x + attackCoef * (env - x);
                holdCounter[(size_t) c] = holdSamples;
            }
            else if (holdCounter[(size_t) c] > 0)
            {
                --holdCounter[(size_t) c];
            }
            else
            {
                // Release curve 0 %: one-pole on amplitude (steady dB per second).
                // 100 %: one-pole in dB (fast at first, then slowing down); the pull towards
                // quiet samples is capped at 24 dB so zero crossings don't yank it.
                const float steady = x + releaseCoef * (env - x);

                if (settings.relShape > 0.0f)
                {
                    const float envDb = dsp::gainToDb (env);
                    const float xDb = juce::jmax (dsp::gainToDb (x), envDb - 24.0f);
                    const float settling = dsp::dbToGain (xDb + releaseCoef * (envDb - xDb));
                    env = steady + settings.relShape * (settling - steady);
                }
                else
                {
                    env = steady;
                }
            }

            auto& fast = fastEnv[(size_t) c];          // fast attack, fast release
            auto& slow = slowEnv[(size_t) c];          // slow attack, fast release
            auto& sustain = sustainEnv[(size_t) c];    // fast attack, slow release
            const float tx = transientLevel[(size_t) c];
            fast    = tx + (tx > fast    ? fastAttack : fastRelease) * (fast - tx);
            slow    = tx + (tx > slow    ? slowAttack : fastRelease) * (slow - tx);
            sustain = tx + (tx > sustain ? fastAttack : slowRelease) * (sustain - tx);

            const float envDb = juce::jmax (floorDb, dsp::gainToDb (env));
            const float transient = dsp::gainToDb ((fast + 1.0e-9f) * (fast + 1.0e-9f) / ((slow + 1.0e-9f) * (sustain + 1.0e-9f)));

            float gainDb = transientTable.lookup (transient);
            if (levelActive)
                gainDb += levelTable.lookup (envDb);

            gainDb = juce::jlimit (-maxCut, maxBoost, gainDb * amount);

            auto& smoothed = smoothedGain[(size_t) c];
            smoothed = detectorPrimed ? gainDb + smoothCoef * (smoothed - gainDb) : gainDb;
            gain[(size_t) c][(size_t) i] = dsp::dbToGain (smoothed);

            blockLevel = juce::jmax (blockLevel, envDb);
            if (std::abs (transient) > std::abs (blockTransient)) blockTransient = juce::jlimit (-24.0f, 24.0f, transient);
            if (std::abs (smoothed) > std::abs (blockGain))       blockGain = smoothed;
        }

        detectorPrimed = true;
    }

    // Direct path (no shaper): gain, then the oversampler's latency so both paths line up.
    const int osLat = osLatency[(size_t) quality];
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < n; ++i)
            direct[(size_t) c][(size_t) i] = shaperDelay[(size_t) c].process (audio[(size_t) c][(size_t) i] * gain[(size_t) c][(size_t) i], osLat);

    const bool shaperOn = shaperFade.getTargetValue() > 0.5f || shaperFade.isSmoothing();

    if (shaperOn)
    {
        if (! shaperWasActive)
        {
            if (auto* os = oversamplers[(size_t) quality].get())
                os->reset();
            for (auto& s : dcState) s.reset();
        }

        runShaper (n, ! waveshaper && settings.satPos == satBefore);
    }

    shaperWasActive = shaperOn;

    const bool widthActive = std::abs (widthAmount.getTargetValue() - 1.0f) > 1.0e-6f || widthAmount.isSmoothing();

    for (int i = 0; i < n; ++i)
    {
        float y0 = direct[0][(size_t) i], y1 = direct[1][(size_t) i];

        if (shaperOn)
        {
            const float fade = shaperFade.getNextValue();
            y0 += fade * (shaperBuffer.getSample (0, i) - y0);
            y1 += fade * (shaperBuffer.getSample (1, i) - y1);
        }

        const float post = postGain.getNextValue();
        y0 *= post;
        y1 *= post;

        if (midSide)
        {
            const float l = y0 + y1, r = y0 - y1;
            y0 = l; y1 = r;
        }

        const float width = widthAmount.getNextValue();

        if (widthActive)
        {
            const float m = 0.5f * (y0 + y1), s = 0.5f * (y0 - y1) * width;
            y0 = m + s;
            y1 = m - s;
        }

        const float mix = mixAmount.getNextValue();

        if (mix >= 1.0f)
        {
            left[i] = y0;
            right[i] = y1;
        }
        else
        {
            left[i]  = dry[0][(size_t) i] + mix * (y0 - dry[0][(size_t) i]);
            right[i] = dry[1][(size_t) i] + mix * (y1 - dry[1][(size_t) i]);
        }
    }
}

void Stage::runShaper (int n, bool gainInside)
{
    const bool waveshaper = settings.mode == modeWaveshaper;
    const bool satFirst = settings.satPos == satBefore;
    const bool tube = settings.satType == satTube;
    const float amount = global.amount;
    const float maxBoost = settings.maxBoost, maxCut = settings.maxCut;
    const auto& sat = saturator;
    const auto& table = levelTable;

    for (int c = 0; c < 2; ++c)
    {
        auto* out = shaperBuffer.getWritePointer (c);
        for (int i = 0; i < n; ++i)
            out[i] = gainInside ? audio[(size_t) c][(size_t) i] : audio[(size_t) c][(size_t) i] * gain[(size_t) c][(size_t) i];
    }

    auto shapeSample = [&] (float x, float g)
    {
        if (waveshaper)
        {
            auto waveshape = [&] (float v)
            {
                const float db = juce::jlimit (-maxCut, maxBoost, amount * table.lookup (dsp::gainToDb (std::abs (v))));
                return v * dsp::dbToGain (db);
            };

            return satFirst ? waveshape (sat.process (x)) : sat.process (waveshape (x));
        }

        return gainInside ? sat.process (x) * g : sat.process (x);
    };

    auto* os = oversamplers[(size_t) quality].get();

    if (os == nullptr)
    {
        for (int c = 0; c < 2; ++c)
        {
            auto* data = shaperBuffer.getWritePointer (c);
            for (int i = 0; i < n; ++i)
                data[i] = shapeSample (data[i], gain[(size_t) c][(size_t) i]);
        }
    }
    else
    {
        juce::dsp::AudioBlock<float> block (shaperBuffer.getArrayOfWritePointers(), 2, (size_t) n);
        auto up = os->processSamplesUp (block);
        const int factor = (int) os->getOversamplingFactor();

        for (int c = 0; c < 2; ++c)
        {
            auto* data = up.getChannelPointer ((size_t) c);
            const int length = n * factor;

            for (int j = 0; j < length; ++j)
                data[j] = shapeSample (data[j], gain[(size_t) c][(size_t) (j / factor)]);
        }

        os->processSamplesDown (block);
    }

    if (tube)
    {
        for (int c = 0; c < 2; ++c)
        {
            auto* data = shaperBuffer.getWritePointer (c);
            for (int i = 0; i < n; ++i)
                data[i] -= dcState[(size_t) c].lowpass (data[i], dcCoeffs);
        }
    }
}
}
