#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace msc;

namespace
{
    constexpr int coeffInterval = 32;   // filter coefficients follow smoothed values every 32 samples

    inline void selectSource (int source, float l, float r, float& outL, float& outR)
    {
        switch (source)
        {
            case sourceMono:  outL = outR = 0.5f * (l + r); break;
            case sourceLeft:  outL = outR = l; break;
            case sourceRight: outL = outR = r; break;
            default:          outL = l; outR = r; break;
        }
    }

    // Roland Juno-60 chorus: triangle LFO sweeping a BBD delay around ~3.5 ms,
    // with the right channel's LFO inverted for width.
    struct ChorusMode { float rateHz, centreMs, swingMs; };
    constexpr ChorusMode chorusModes[] { { 0.513f, 3.5f, 1.85f },    // I
                                         { 0.863f, 3.5f, 1.85f },    // II
                                         { 9.75f,  3.5f, 0.25f } };  // I+II

    inline bool isOn (const std::atomic<float>* p) { return p->load() > 0.5f; }
    inline int  toIndex (const std::atomic<float>* p) { return (int) std::lround (p->load()); }
}

MscProcessor::MscProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "MSC", createParameterLayout()),
      presets (apvts)
{
    auto get = [this] (const char* id)
    {
        auto* p = apvts.getRawParameterValue (id);
        jassert (p != nullptr);
        return p;
    };

    params.inOn = get (ids::inOn);         params.inShape = get (ids::inShape);   params.inCutoff = get (ids::inCutoff);
    params.inSlope = get (ids::inSlope);   params.inWetSrc = get (ids::inWetSrc); params.inDrySrc = get (ids::inDrySrc);

    params.dpOn = get (ids::dpOn);         params.dpAmount = get (ids::dpAmount); params.dpMax = get (ids::dpMax);
    params.dpShape = get (ids::dpShape);   params.dpCutoff = get (ids::dpCutoff); params.dpSlope = get (ids::dpSlope);
    params.dpClip = get (ids::dpClip);     params.dpSource = get (ids::dpSource); params.dpComp = get (ids::dpComp);
    params.dpThresh = get (ids::dpThresh); params.dpMakeup = get (ids::dpMakeup);

    params.hsOn = get (ids::hsOn);         params.hsLeft = get (ids::hsLeft);     params.hsRight = get (ids::hsRight);
    params.hsInvL = get (ids::hsInvL);     params.hsInvR = get (ids::hsInvR);

    params.chOn = get (ids::chOn);         params.chMode = get (ids::chMode);     params.chDepth = get (ids::chDepth);
    params.chWidth = get (ids::chWidth);   params.chTone = get (ids::chTone);     params.chMix = get (ids::chMix);

    params.imOn = get (ids::imOn);         params.imBalance = get (ids::imBalance);
    params.imMid = get (ids::imMid);       params.imSide = get (ids::imSide);
}

bool MscProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
        && (in == juce::AudioChannelSet::stereo() || in == juce::AudioChannelSet::mono());
}

void MscProcessor::prepareToPlay (double newSampleRate, int)
{
    sampleRate = newSampleRate;

    for (auto* module : { &input, &dynPan, &haas, &chorus, &image })
    {
        module->fade.reset (sampleRate, 0.015);
        module->wasActive = false;
    }

    for (auto* s : { &inShape, &dpShape, &dpDepth, &dpThreshold, &chWidth, &chMix, &imMid, &imSide, &imGainL, &imGainR })
        s->reset (sampleRate, 0.02);

    for (auto* s : { &hsPolarityL, &hsPolarityR })
        s->reset (sampleRate, 0.01);

    for (auto* s : { &hsDelayL, &hsDelayR, &chCentre, &chSwing })
        s->reset (sampleRate, 0.05);

    for (auto* s : { &inCutoff, &dpCutoff, &chTone })
        s->reset (sampleRate, 0.03);

    haasLeft.prepare ((int) std::ceil (0.5 * sampleRate) + 8);
    haasRight.prepare ((int) std::ceil (0.5 * sampleRate) + 8);
    chorusDelay.prepare ((int) std::ceil (0.02 * sampleRate));

    // Modulator compressor: moderate 10 ms attack, 150 ms release.
    compAttack  = std::exp (-1.0f / (0.010f * (float) sampleRate));
    compRelease = std::exp (-1.0f / (0.150f * (float) sampleRate));
    compEnvelope = 0.0f;

    inputAnalyzer.setSampleRate (sampleRate);
    dynPanAnalyzer.setSampleRate (sampleRate);
    modScope.prepare (sampleRate);

    updateTargets (true);
}

void MscProcessor::updateTargets (bool snap)
{
    auto set = [snap] (auto& smoothed, float value)
    {
        if (snap) smoothed.setCurrentAndTargetValue (value);
        else      smoothed.setTargetValue (value);
    };

    set (input.fade,  isOn (params.inOn) ? 1.0f : 0.0f);
    set (dynPan.fade, isOn (params.dpOn) ? 1.0f : 0.0f);
    set (haas.fade,   isOn (params.hsOn) ? 1.0f : 0.0f);
    set (chorus.fade, isOn (params.chOn) ? 1.0f : 0.0f);
    set (image.fade,  isOn (params.imOn) ? 1.0f : 0.0f);

    set (inCutoff, params.inCutoff->load());
    set (inShape,  params.inShape->load());
    inSlope   = toIndex (params.inSlope);
    wetSource = toIndex (params.inWetSrc);
    drySource = toIndex (params.inDrySrc);

    set (dpCutoff, params.dpCutoff->load());
    set (dpShape,  params.dpShape->load());
    set (dpDepth,  params.dpAmount->load() * params.dpMax->load() * 0.01f);
    set (dpThreshold, params.dpThresh->load());
    dpSlope   = toIndex (params.dpSlope);
    clipMode  = toIndex (params.dpClip);
    modSource = toIndex (params.dpSource);
    compRatio = modCompRatios[juce::jlimit (0, 3, toIndex (params.dpComp))];
    compMakeup = modMakeupAmounts[juce::jlimit (0, 4, toIndex (params.dpMakeup))];

    const float msToSamples = (float) sampleRate * 0.001f;
    set (hsDelayL, params.hsLeft->load() * msToSamples);
    set (hsDelayR, params.hsRight->load() * msToSamples);
    set (hsPolarityL, isOn (params.hsInvL) ? -1.0f : 1.0f);
    set (hsPolarityR, isOn (params.hsInvR) ? -1.0f : 1.0f);

    const auto& mode = chorusModes[juce::jlimit (0, 2, toIndex (params.chMode))];
    chRate = mode.rateHz;
    set (chCentre, mode.centreMs * msToSamples);
    set (chSwing,  mode.swingMs * msToSamples * params.chDepth->load() * 0.01f);
    set (chWidth,  params.chWidth->load() * 0.01f);
    set (chTone,   params.chTone->load());
    set (chMix,    params.chMix->load() * 0.01f);

    const float balance = params.imBalance->load() * 0.01f;
    set (imMid,   params.imMid->load() * 0.01f);
    set (imSide,  params.imSide->load() * 0.01f);
    set (imGainL, balance > 0.0f ? 1.0f - balance : 1.0f);
    set (imGainR, balance < 0.0f ? 1.0f + balance : 1.0f);
}

bool MscProcessor::beginModule (Module& module, bool& justStarted)
{
    const bool active = module.fade.isSmoothing() || module.fade.getTargetValue() > 0.5f;
    justStarted = active && ! module.wasActive;
    module.wasActive = active;
    return active;
}

void MscProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();

    if (buffer.getNumChannels() < 2)
        return;

    if (getTotalNumInputChannels() == 1)
        buffer.copyFrom (1, 0, buffer, 0, 0, numSamples);

    updateTargets (false);

    auto* left  = buffer.getWritePointer (0);
    auto* right = buffer.getWritePointer (1);

    for (int start = 0; start < numSamples; start += maxChunk)
        processChunk (left + start, right + start, std::min (maxChunk, numSamples - start));
}

void MscProcessor::processChunk (float* left, float* right, int n)
{
    bool justStarted = false;

    // ---- 0: input band split and source selection --------------------------------------
    // The processed band is filter(x) and the rest is x - filter(x), so both always sum back
    // to the original signal.
    const bool splitActive = beginModule (input, justStarted);

    if (splitActive)
    {
        if (justStarted)
            inputFilter.reset();

        for (int i = 0; i < n; ++i)
        {
            if (i % coeffInterval == 0)
            {
                const int steps = std::min (coeffInterval, n - i);
                inputFilter.setup (inCutoff.skip (steps), inShape.skip (steps), inSlope, sampleRate);
            }

            const float l = left[i], r = right[i];
            scratch[(size_t) i] = 0.5f * (l + r);

            const float bandL = inputFilter.process (0, l);
            const float bandR = inputFilter.process (1, r);

            float wl, wr, dl, dr;
            selectSource (wetSource, bandL, bandR, wl, wr);
            selectSource (drySource, l - bandL, r - bandR, dl, dr);

            const float g = input.fade.getNextValue();
            wetL[(size_t) i] = l + g * (wl - l);
            wetR[(size_t) i] = r + g * (wr - r);
            dryL[(size_t) i] = g * dl;
            dryR[(size_t) i] = g * dr;
        }

        inputAnalyzer.push (scratch.data(), n);
    }
    else
    {
        std::copy (left, left + n, wetL.begin());
        std::copy (right, right + n, wetR.begin());
    }

    // ---- 1: dynamic pan -------------------------------------------------------------------
    // The signal drives its own pan position at audio rate:
    //   modulator = filter(source taken from the plugin input) -> optional compressor -> * amount
    //   p = modClip(modulator), then side += mid * p (for a mono input: L = x(1-p), R = x(1+p)).
    //   Mid is untouched, so L + R is exactly the input: the effect cancels perfectly in mono.
    //   The clipper shapes p (+-1 = hard left/right), never the audio, since clipping the
    //   channels would break that cancellation.
    if (beginModule (dynPan, justStarted))
    {
        if (justStarted)
        {
            dynPanFilter.reset();
            compEnvelope = 0.0f;
        }

        const bool feedScope = modScope.isEnabled();
        const bool compress = compRatio > 1.0f;
        const float compSlope = 1.0f - 1.0f / compRatio;
        constexpr float dbToLog = 0.11512925f;   // ln(10) / 20

        for (int i = 0; i < n; ++i)
        {
            if (i % coeffInterval == 0)
            {
                const int steps = std::min (coeffInterval, n - i);
                dynPanFilter.setup (dpCutoff.skip (steps), dpShape.skip (steps), dpSlope, sampleRate);
            }

            const float l = wetL[(size_t) i], r = wetR[(size_t) i];

            // The modulator reads the plugin's own input (left/right are untouched until the
            // recombine step), not the input module's band: e.g. the bass can pan the highs.
            const float inL = left[i], inR = right[i];
            const float source = modSource == modLeft ? inL : modSource == modRight ? inR : 0.5f * (inL + inR);
            scratch[(size_t) i] = source;

            float mod = dynPanFilter.process (0, source);
            const float threshold = dpThreshold.getNextValue();

            if (compress)
            {
                const float level = std::abs (mod);
                const float coeff = level > compEnvelope ? compAttack : compRelease;
                compEnvelope = level + coeff * (compEnvelope - level);

                // Gain reduction above the threshold plus a share of the makeup that would keep
                // 0 dBFS at 0 dBFS; the makeup lifts everything below it, so quiet parts pan harder.
                const float envelopeDb = juce::Decibels::gainToDecibels (compEnvelope, -120.0f);
                const float gainDb = -threshold * compSlope * compMakeup - juce::jmax (0.0f, envelopeDb - threshold) * compSlope;
                mod *= std::exp (gainDb * dbToLog);
            }

            const float pre = dpDepth.getNextValue() * mod;
            const float p = dsp::clip (pre, clipMode);

            if (feedScope)
                modScope.push (pre, p);

            const float sideAdd = 0.5f * (l + r) * p;
            const float pl = l - sideAdd;
            const float pr = r + sideAdd;

            const float g = dynPan.fade.getNextValue();
            wetL[(size_t) i] = l + g * (pl - l);
            wetR[(size_t) i] = r + g * (pr - r);
        }

        dynPanAnalyzer.push (scratch.data(), n);
    }

    // ---- 2: Haas delay and polarity -------------------------------------------------------
    if (beginModule (haas, justStarted))
    {
        if (justStarted)
        {
            haasLeft.clear();
            haasRight.clear();
            hsDelayL.setCurrentAndTargetValue (hsDelayL.getTargetValue());
            hsDelayR.setCurrentAndTargetValue (hsDelayR.getTargetValue());
        }

        for (int i = 0; i < n; ++i)
        {
            const float l = wetL[(size_t) i], r = wetR[(size_t) i];
            haasLeft.push (l);
            haasRight.push (r);

            const float hl = haasLeft.readLinear (hsDelayL.getNextValue()) * hsPolarityL.getNextValue();
            const float hr = haasRight.readLinear (hsDelayR.getNextValue()) * hsPolarityR.getNextValue();

            const float g = haas.fade.getNextValue();
            wetL[(size_t) i] = l + g * (hl - l);
            wetR[(size_t) i] = r + g * (hr - r);
        }
    }

    // ---- 3: Juno-style chorus -------------------------------------------------------------
    // Mono sum into one delay line with two taps; the right tap's LFO is inverted by Width.
    if (beginModule (chorus, justStarted))
    {
        if (justStarted)
        {
            chorusDelay.clear();
            chToneA = chToneB = 0.0f;
            chCentre.setCurrentAndTargetValue (chCentre.getTargetValue());
            chSwing.setCurrentAndTargetValue (chSwing.getTargetValue());
        }

        const float toneCoeff = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * chTone.skip (n) / (float) sampleRate);
        const float phaseStep = chRate / (float) sampleRate;

        for (int i = 0; i < n; ++i)
        {
            chPhase += phaseStep;
            if (chPhase >= 1.0f)
                chPhase -= 1.0f;

            const float tri = 1.0f - 4.0f * std::abs (chPhase - 0.5f);   // -1..1
            const float centre = chCentre.getNextValue();
            const float swing = chSwing.getNextValue() * tri;
            const float width = chWidth.getNextValue();

            const float l = wetL[(size_t) i], r = wetR[(size_t) i];
            chorusDelay.push (0.5f * (l + r));

            const float tapA = chorusDelay.readHermite (juce::jmax (2.0f, centre + swing));
            const float tapB = chorusDelay.readHermite (juce::jmax (2.0f, centre + swing * (1.0f - 2.0f * width)));
            chToneA += toneCoeff * (tapA - chToneA);
            chToneB += toneCoeff * (tapB - chToneB);

            const float mix = chMix.getNextValue();
            const float norm = 1.0f / (1.0f + 0.5f * mix);
            const float cl = (l + mix * chToneA) * norm;
            const float cr = (r + mix * chToneB) * norm;

            const float g = chorus.fade.getNextValue();
            wetL[(size_t) i] = l + g * (cl - l);
            wetR[(size_t) i] = r + g * (cr - r);
        }
    }

    // ---- 4: image (mid/side + balance) ----------------------------------------------------
    if (beginModule (image, justStarted))
    {
        for (int i = 0; i < n; ++i)
        {
            const float l = wetL[(size_t) i], r = wetR[(size_t) i];
            const float mid  = 0.5f * (l + r) * imMid.getNextValue();
            const float side = 0.5f * (l - r) * imSide.getNextValue();
            const float il = (mid + side) * imGainL.getNextValue();
            const float ir = (mid - side) * imGainR.getNextValue();

            const float g = image.fade.getNextValue();
            wetL[(size_t) i] = l + g * (il - l);
            wetR[(size_t) i] = r + g * (ir - r);
        }
    }

    // ---- recombine ------------------------------------------------------------------------
    if (splitActive)
    {
        for (int i = 0; i < n; ++i)
        {
            left[i]  = wetL[(size_t) i] + dryL[(size_t) i];
            right[i] = wetR[(size_t) i] + dryR[(size_t) i];
        }
    }
    else
    {
        std::copy (wetL.begin(), wetL.begin() + n, left);
        std::copy (wetR.begin(), wetR.begin() + n, right);
    }
}

juce::AudioProcessorEditor* MscProcessor::createEditor()
{
    return new MscEditor (*this);
}

int MscProcessor::getNumPrograms()
{
    return presets.getNumFactoryPresets();
}

int MscProcessor::getCurrentProgram()
{
    const int index = presets.getCurrentPresetIndex();
    return juce::isPositiveAndBelow (index, presets.getNumFactoryPresets()) ? index : 0;
}

void MscProcessor::setCurrentProgram (int index)
{
    // Some hosts call this with the current program after restoring state; don't reset then.
    if (index != getCurrentProgram())
        presets.loadPreset (index);
}

const juce::String MscProcessor::getProgramName (int index)
{
    return presets.getPresetNames()[index];
}

void MscProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void MscProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MscProcessor();
}
