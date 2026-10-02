#include "Engine.h"

namespace dynmap
{
namespace
{
    inline bool isOn (const std::atomic<float>* p) { return p->load() > 0.5f; }
    inline int toIndex (const std::atomic<float>* p) { return (int) std::lround (p->load()); }

    float coefFor (double samples) { return samples < 0.5 ? 0.0f : (float) std::exp (-1.0 / samples); }

    inline float softClip (float x, float ceiling) noexcept
    {
        const float u = x / ceiling;
        const float a = std::abs (u);
        if (a <= 0.5f)
            return x;
        const float y = 0.5f + 0.5f * std::tanh ((a - 0.5f) * 2.0f);
        return std::copysign (y * ceiling, x);
    }
}

//==============================================================================
const juce::Identifier CurveBank::treeId { "CURVES" };

CurveBank::CurveBank()
{
    resetAll();
}

Curve CurveBank::get (int stage, CurveKind kind) const
{
    const juce::ScopedLock sl (lock);
    return kind == CurveKind::level ? level[(size_t) stage] : transient[(size_t) stage];
}

void CurveBank::set (int stage, CurveKind kind, const Curve& curve)
{
    jassert (curve.getKind() == kind);

    {
        const juce::ScopedLock sl (lock);
        (kind == CurveKind::level ? level : transient)[(size_t) stage] = curve;
    }

    getSlot (stage, kind).publish (curve);
    changes.fetch_add (1);
}

void CurveBank::resetAll()
{
    for (int stage = 0; stage < numStages; ++stage)
    {
        set (stage, CurveKind::level, Curve (CurveKind::level));
        set (stage, CurveKind::transient, Curve (CurveKind::transient));
    }
}

void CurveBank::writeTo (juce::ValueTree& state) const
{
    state.removeChild (state.getChildWithName (treeId), nullptr);
    juce::ValueTree tree (treeId);

    const juce::ScopedLock sl (lock);

    for (int stage = 0; stage < numStages; ++stage)
    {
        if (! level[(size_t) stage].isNeutral())
            tree.setProperty ("l" + juce::String (stage), level[(size_t) stage].toString(), nullptr);
        if (! transient[(size_t) stage].isNeutral())
            tree.setProperty ("t" + juce::String (stage), transient[(size_t) stage].toString(), nullptr);
    }

    state.appendChild (tree, nullptr);
}

void CurveBank::readFrom (const juce::ValueTree& state)
{
    const auto tree = state.getChildWithName (treeId);

    for (int stage = 0; stage < numStages; ++stage)
    {
        const auto l = tree.getProperty ("l" + juce::String (stage)).toString();
        const auto t = tree.getProperty ("t" + juce::String (stage)).toString();
        set (stage, CurveKind::level, l.isEmpty() ? Curve (CurveKind::level) : Curve::fromString (CurveKind::level, l));
        set (stage, CurveKind::transient, t.isEmpty() ? Curve (CurveKind::transient) : Curve::fromString (CurveKind::transient, t));
    }
}

//==============================================================================
void Engine::Limiter::prepare (double newSampleRate)
{
    sampleRate = newSampleRate;
    window = juce::jmax (1, (int) std::round (0.002 * sampleRate));
    minimum.prepare (window + 1);
    average.prepare (window + 1);
    for (auto& d : delay)
        d.prepare (getLatency() + 4);
    fade.reset (sampleRate, 0.02);
    reset();
}

void Engine::Limiter::reset()
{
    minimum.reset (window + 1);
    average.reset (window + 1, 1.0f);
    for (auto& t : truePeak) t.reset();
    for (auto& d : delay) d.reset();
    released = 1.0f;
}

float Engine::Limiter::process (float* left, float* right, int numSamples, float ceiling, float releaseMs, bool enabled, bool snap)
{
    if (snap) fade.setCurrentAndTargetValue (enabled ? 1.0f : 0.0f);
    else      fade.setTargetValue (enabled ? 1.0f : 0.0f);
    const float releaseCoef = coefFor (releaseMs * 0.001 * sampleRate);
    const int lat = getLatency();
    float lowest = 1.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        // Gain each (true-peak) sample needs, held over the window and averaged over it, so the
        // gain is fully down when the peak comes out of the delay.
        const float peak = juce::jmax (truePeak[0].process (left[i]), truePeak[1].process (right[i]));
        const float required = peak > ceiling ? ceiling / peak : 1.0f;
        const float held = minimum.push (required);
        released = held < released ? held : held + releaseCoef * (released - held);
        const float g = average.push (released);

        const float l = delay[0].process (left[i], lat);
        const float r = delay[1].process (right[i], lat);
        const float f = fade.getNextValue();
        const float applied = 1.0f + f * (g - 1.0f);

        left[i] = l * applied;
        right[i] = r * applied;

        if (f > 0.0f)
        {
            // Safety net for rounding: sample peaks never pass the ceiling.
            left[i] = juce::jlimit (-ceiling, ceiling, left[i]);
            right[i] = juce::jlimit (-ceiling, ceiling, right[i]);
        }

        lowest = juce::jmin (lowest, applied);
    }

    return lowest;
}

//==============================================================================
Engine::Engine (juce::AudioProcessorValueTreeState& state) : apvts (state)
{
    auto get = [this] (const juce::String& id)
    {
        auto* p = apvts.getRawParameterValue (id);
        jassert (p != nullptr);
        return p;
    };

    for (int stage = 0; stage < numStages; ++stage)
    {
        auto& p = stageParams[(size_t) stage];
        auto id = [stage] (const char* name) { return stageParamId (stage, name); };

        p.bypass = get (id (ids::bypass));       p.mode = get (id (ids::mode));
        p.pre = get (id (ids::pre));             p.post = get (id (ids::post));
        p.mix = get (id (ids::mix));             p.attack = get (id (ids::attack));
        p.hold = get (id (ids::hold));           p.release = get (id (ids::release));
        p.relShape = get (id (ids::relShape));   p.rms = get (id (ids::rms));
        p.lookahead = get (id (ids::lookahead)); p.link = get (id (ids::link));
        p.stereo = get (id (ids::stereo));       p.scFilter = get (id (ids::scFilter));
        p.scSource = get (id (ids::scSource));   p.trTime = get (id (ids::trTime));
        p.maxBoost = get (id (ids::maxBoost));   p.maxCut = get (id (ids::maxCut));
        p.smooth = get (id (ids::smooth));       p.satType = get (id (ids::satType));
        p.drive = get (id (ids::drive));         p.satPos = get (id (ids::satPos));

        if (isBandStage (stage))
        {
            p.width = get (id (ids::width));
            p.solo = get (id (ids::solo));
            p.mute = get (id (ids::mute));

            if (stage != bandStage (0))
            {
                p.bandOn = get (id (ids::bandOn));
                p.freq = get (id (ids::freq));
                p.slope = get (id (ids::slope));
            }
        }
    }

    amountParam = get (ids::amount);     timeParam = get (ids::time);        mixParam = get (ids::globalMix);
    inGainParam = get (ids::inGain);     outGainParam = get (ids::outGain);  clipParam = get (ids::clip);
    limiterParam = get (ids::limiter);   ceilingParam = get (ids::ceiling);  limRelParam = get (ids::limRel);
    autoGainParam = get (ids::autoGain); deltaParam = get (ids::delta);      qualityParam = get (ids::quality);
    phaseParam = get (ids::phase);         scGainParam = get (ids::scGain);    scListenParam = get (ids::scListen);
}

Engine::~Engine() = default;

BandLayout Engine::getLayout() const
{
    BandLayout l;
    std::array<std::pair<float, int>, maxBands> used {};
    int count = 0;

    for (int slot = 1; slot < maxBands; ++slot)
    {
        const auto& p = stageParams[(size_t) bandStage (slot)];
        if (isOn (p.bandOn))
            used[(size_t) count++] = { p.freq->load(), slot };
    }

    std::sort (used.begin(), used.begin() + count);

    l.numBands = count + 1;
    l.slots[0] = 0;

    for (int i = 0; i < count; ++i)
    {
        const int slot = used[(size_t) i].second;
        l.slots[(size_t) i + 1] = slot;
        l.freqs[(size_t) i + 1] = used[(size_t) i].first;
        l.slopes[(size_t) i + 1] = toIndex (stageParams[(size_t) bandStage (slot)].slope);
    }

    return l;
}

void Engine::prepare (double newSampleRate, int maxBlockSize)
{
    sampleRate = newSampleRate;
    maxBlock = juce::jmax (32, maxBlockSize);

    for (auto& s : stages)
        s.prepare (sampleRate, maxBlock);

    bandBuffers.setSize (maxBands * 2, maxBlock);
    sidechainBandBuffers.setSize (maxBands * 2, maxBlock);
    scratch.setSize (4, maxBlock);
    sidechainDelayed.setSize (2, maxBlock);
    sidechainInput.setSize (2, maxBlock);

    for (int slot = 0; slot < maxBands; ++slot)
        for (int ch = 0; ch < 2; ++ch)
        {
            bandPointers[(size_t) slot][(size_t) ch] = bandBuffers.getWritePointer (slot * 2 + ch);
            sidechainBandPointers[(size_t) slot][(size_t) ch] = sidechainBandBuffers.getWritePointer (slot * 2 + ch);
        }

    layout = getLayout();
    splitter.prepare (sampleRate);
    sidechainSplitter.prepare (sampleRate);
    splitter.setLayout (layout, true);
    sidechainSplitter.setLayout (layout, true);
    linearSplitter.prepare (sampleRate, maxBlock, layout);

    clipLatency[0] = 0;
    for (int q = 1; q < Stage::numQualities; ++q)
    {
        auto& os = clipOversamplers[(size_t) q];
        os = std::make_unique<juce::dsp::Oversampling<float>> (2, (size_t) q, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, true);
        os->initProcessing ((size_t) maxBlock);
        clipLatency[(size_t) q] = juce::roundToInt (os->getLatencyInSamples());
    }

    limiter.prepare (sampleRate);

    const int maxOs = juce::jmax (*std::max_element (clipLatency.begin(), clipLatency.end()), stages[0].getOversamplingLatency (3));
    const int maxLookahead = (int) std::ceil (maxLookaheadMs * 0.001 * sampleRate);
    const int maxStage = maxLookahead + maxOs + 8;
    const int maxLatency = linearSplitter.getLatency() + 3 * maxStage + maxOs + limiter.getLatency() + 64;

    for (int ch = 0; ch < 2; ++ch)
    {
        dryDelay[(size_t) ch].prepare (maxLatency);
        sidechainBandDelay[(size_t) ch].prepare (linearSplitter.getLatency() + maxStage + 64);
        sidechainMasterDelay[(size_t) ch].prepare (maxLatency);
        clipDelay[(size_t) ch].prepare (maxOs + 4);
    }

    for (auto& g : bandGains)
        g.reset (sampleRate, 0.01);

    inGain.reset (sampleRate, 0.02);
    sidechainGain.reset (sampleRate, 0.02);
    outGain.reset (sampleRate, 0.02);
    globalMix.reset (sampleRate, 0.02);
    clipFade.reset (sampleRate, 0.02);
    autoGain.reset (sampleRate, 0.3);

    dryLoudness.prepare (sampleRate);
    wetLoudness.prepare (sampleRate);
    meters.outLoudness.prepare (sampleRate);

    preSpectrum.setSampleRate (sampleRate);
    postSpectrum.setSampleRate (sampleRate);
    sidechainSpectrum.setSampleRate (sampleRate);

    reset();
    readSettings();
}

void Engine::reset()
{
    for (auto& s : stages)
        s.reset();

    splitter.reset();
    sidechainSplitter.reset();
    linearSplitter.reset();

    for (auto* lines : { &dryDelay, &sidechainBandDelay, &sidechainMasterDelay, &clipDelay })
        for (auto& line : *lines)
            line.reset();

    for (auto& os : clipOversamplers)
        if (os != nullptr)
            os->reset();

    limiter.reset();
    dryLoudness.reset();
    wetLoudness.reset();
    meters.outLoudness.reset();
    for (auto& t : outTruePeak) t.reset();

    bandWasActive = {};
    autoGainDb = 0.0f;
    clipWasActive = false;
    firstBlock = true;
}

StageSettings Engine::readStage (int stage) const
{
    const auto& p = stageParams[(size_t) stage];
    StageSettings s;

    s.bypass = isOn (p.bypass);
    s.mode = toIndex (p.mode);
    s.preDb = p.pre->load();
    s.postDb = p.post->load();
    s.mix = p.mix->load() * 0.01f;
    s.attackMs = p.attack->load();
    s.holdMs = p.hold->load();
    s.releaseMs = p.release->load();
    s.relShape = p.relShape->load() * 0.01f;
    s.rmsMs = p.rms->load();
    s.lookaheadSamples = (int) std::round (lookaheadMs[(size_t) juce::jlimit (0, numLookaheads - 1, toIndex (p.lookahead))] * 0.001 * sampleRate);
    s.link = p.link->load() * 0.01f;
    s.stereo = toIndex (p.stereo);
    s.scFilterHz = p.scFilter->load();
    s.scSource = toIndex (p.scSource);
    s.trTimeMs = p.trTime->load();
    s.maxBoost = p.maxBoost->load();
    s.maxCut = p.maxCut->load();
    s.smoothMs = p.smooth->load();
    s.satType = toIndex (p.satType);
    s.driveDb = p.drive->load();
    s.satPos = toIndex (p.satPos);
    s.width = p.width != nullptr ? p.width->load() * 0.01f : 1.0f;
    return s;
}

void Engine::readSettings()
{
    global.amount = amountParam->load() * 0.01f;
    global.timeScale = timeParam->load() * 0.01f;
    global.quality = juce::jlimit (0, Stage::numQualities - 1, toIndex (qualityParam));
    phase = toIndex (phaseParam);
    clipMode = toIndex (clipParam);

    for (int stage = 0; stage < numStages; ++stage)
        settings[(size_t) stage] = readStage (stage);

    layout = getLayout();

    int maxBandLookahead = 0;
    for (int p = 0; p < layout.numBands; ++p)
        maxBandLookahead = juce::jmax (maxBandLookahead, settings[(size_t) bandStage (layout.slots[(size_t) p])].lookaheadSamples);

    const int osLat = stages[0].getOversamplingLatency (global.quality);
    const int inLat = settings[inputStage].lookaheadSamples + osLat;
    const int bandsLat = (phase == phaseLinear ? linearSplitter.getLatency() : 0) + maxBandLookahead + osLat;
    const int masterLat = settings[masterStage].lookaheadSamples + osLat;
    latency = inLat + bandsLat + masterLat + clipLatency[(size_t) global.quality] + limiter.getLatency();
}

void Engine::process (float* left, float* right, const float* scLeft, const float* scRight, int numSamples)
{
    for (int start = 0; start < numSamples; start += maxBlock)
    {
        const int n = juce::jmin (maxBlock, numSamples - start);
        processBlock (left + start, right + start,
                      scLeft != nullptr ? scLeft + start : nullptr,
                      scRight != nullptr ? scRight + start : nullptr, n);
    }
}

void Engine::processBlock (float* left, float* right, const float* scLeft, const float* scRight, int n)
{
    readSettings();

    // Dry copy for the global mix and delta, lined up with the processed signal.
    auto* dryL = scratch.getWritePointer (0);
    auto* dryR = scratch.getWritePointer (1);
    const int dryLat = juce::jmin (latency, dryDelay[0].getMaxDelay() - 1);
    float inPeakL = 0.0f, inPeakR = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        inPeakL = juce::jmax (inPeakL, std::abs (left[i]));
        inPeakR = juce::jmax (inPeakR, std::abs (right[i]));
        dryL[i] = dryDelay[0].process (left[i], dryLat);
        dryR[i] = dryDelay[1].process (right[i], dryLat);
    }

    meters.inPeak[0].store (juce::jmax (inPeakL, meters.inPeak[0].load()));
    meters.inPeak[1].store (juce::jmax (inPeakR, meters.inPeak[1].load()));

    // Sidechain: gain, level meter and spectrum. Everything after this uses the scaled copy.
    {
        const float target = juce::Decibels::decibelsToGain (scGainParam->load());
        if (firstBlock) sidechainGain.setCurrentAndTargetValue (target); else sidechainGain.setTargetValue (target);

        float peak = 0.0f;

        if (scLeft != nullptr && scRight != nullptr)
        {
            auto* l = sidechainInput.getWritePointer (0);
            auto* r = sidechainInput.getWritePointer (1);

            for (int i = 0; i < n; ++i)
            {
                const float g = sidechainGain.getNextValue();
                l[i] = scLeft[i] * g;
                r[i] = scRight[i] * g;
                peak = juce::jmax (peak, std::abs (l[i]), std::abs (r[i]));
            }

            scLeft = l;
            scRight = r;

            if (sidechainSpectrum.isEnabled())
            {
                auto* mono = scratch.getWritePointer (2);
                for (int i = 0; i < n; ++i)
                    mono[i] = 0.5f * (l[i] + r[i]);
                sidechainSpectrum.push (mono, n);
            }
        }

        const float fallen = meters.sidechainDb.load() - 60.0f * (float) n / (float) sampleRate;
        meters.sidechainDb.store (juce::jmax (-100.0f, fallen, juce::Decibels::gainToDecibels (peak, -100.0f)));
    }

    // Input gain.
    const float inTarget = juce::Decibels::decibelsToGain (inGainParam->load());
    if (firstBlock) inGain.setCurrentAndTargetValue (inTarget); else inGain.setTargetValue (inTarget);

    if (inGain.isSmoothing() || inTarget != 1.0f)
        for (int i = 0; i < n; ++i)
        {
            const float g = inGain.getNextValue();
            left[i] *= g;
            right[i] *= g;
        }

    // Input stage.
    auto& input = stages[inputStage];
    input.pullCurves (curves.getSlot (inputStage, CurveKind::level), curves.getSlot (inputStage, CurveKind::transient));
    input.setSettings (settings[inputStage], global, 0);
    input.process (left, right, scLeft, scRight, n);

    if (preSpectrum.isEnabled())
    {
        auto* mono = scratch.getWritePointer (2);
        for (int i = 0; i < n; ++i)
            mono[i] = 0.5f * (left[i] + right[i]);
        preSpectrum.push (mono, n);
    }

    processBands (left, right, scLeft, scRight, n);

    // Master stage; its sidechain is delayed like the audio that reaches it.
    const float* scMasterL = nullptr;
    const float* scMasterR = nullptr;

    if (scLeft != nullptr && scRight != nullptr)
    {
        const int osLat = stages[0].getOversamplingLatency (global.quality);
        const int toMaster = juce::jmin (latency - settings[masterStage].lookaheadSamples - osLat
                                             - clipLatency[(size_t) global.quality] - limiter.getLatency(),
                                         sidechainMasterDelay[0].getMaxDelay() - 1);
        auto* l = scratch.getWritePointer (2);
        auto* r = scratch.getWritePointer (3);
        for (int i = 0; i < n; ++i)
        {
            l[i] = sidechainMasterDelay[0].process (scLeft[i], toMaster);
            r[i] = sidechainMasterDelay[1].process (scRight[i], toMaster);
        }
        scMasterL = l;
        scMasterR = r;
    }

    auto& master = stages[masterStage];
    master.pullCurves (curves.getSlot (masterStage, CurveKind::level), curves.getSlot (masterStage, CurveKind::transient));
    master.setSettings (settings[masterStage], global, 0);
    master.process (left, right, scMasterL, scMasterR, n);

    processOutput (left, right, n);

    // Sidechain listen: hear what the detectors get, to check the routing.
    if (isOn (scListenParam))
    {
        if (scLeft != nullptr && scRight != nullptr)
        {
            std::copy (scLeft, scLeft + n, left);
            std::copy (scRight, scRight + n, right);
        }
        else
        {
            juce::FloatVectorOperations::clear (left, n);
            juce::FloatVectorOperations::clear (right, n);
        }
    }

    firstBlock = false;
}

void Engine::processBands (float* left, float* right, const float* scLeft, const float* scRight, int n)
{
    const int nb = layout.numBands;
    std::array<bool, maxBands> active {};
    bool anySolo = false;
    int maxBandLookahead = 0;

    for (int p = 0; p < nb; ++p)
    {
        const int slot = layout.slots[(size_t) p];
        active[(size_t) slot] = true;
        anySolo = anySolo || isOn (stageParams[(size_t) bandStage (slot)].solo);
        maxBandLookahead = juce::jmax (maxBandLookahead, settings[(size_t) bandStage (slot)].lookaheadSamples);
    }

    for (int slot = 0; slot < maxBands; ++slot)
    {
        const auto& p = stageParams[(size_t) bandStage (slot)];
        const float target = (! isOn (p.mute) && (! anySolo || isOn (p.solo))) ? 1.0f : 0.0f;
        auto& g = bandGains[(size_t) slot];

        if (active[(size_t) slot] && ! bandWasActive[(size_t) slot])
        {
            stages[(size_t) bandStage (slot)].reset();
            g.setCurrentAndTargetValue (target);
        }
        else if (firstBlock)
            g.setCurrentAndTargetValue (target);
        else
            g.setTargetValue (target);
    }

    bandWasActive = active;

    // Split.
    const float* in[2] { left, right };
    std::array<float* const*, maxBands> bands {};
    std::array<float* const*, maxBands> sidechainBands {};

    for (int slot = 0; slot < maxBands; ++slot)
    {
        bands[(size_t) slot] = bandPointers[(size_t) slot].data();
        sidechainBands[(size_t) slot] = sidechainBandPointers[(size_t) slot].data();
    }

    if (phase == phaseLinear)
    {
        linearSplitter.setLayout (layout);

        for (int p = 0; p < nb; ++p)
            for (int ch = 0; ch < 2; ++ch)
                juce::FloatVectorOperations::clear (bands[(size_t) layout.slots[(size_t) p]][ch], n);

        linearSplitter.process (in, bands.data(), n);
    }
    else
    {
        splitter.setLayout (layout, firstBlock);
        splitter.process (in, bands.data(), n);
    }

    // Sidechain bands (only split when a band listens to the sidechain).
    bool bandsUseSidechain = false;
    const bool hasSidechain = scLeft != nullptr && scRight != nullptr;

    if (hasSidechain)
    {
        const int osLat = stages[0].getOversamplingLatency (global.quality);
        const int toBands = juce::jmin (settings[inputStage].lookaheadSamples + osLat + (phase == phaseLinear ? linearSplitter.getLatency() : 0),
                                        sidechainBandDelay[0].getMaxDelay() - 1);
        auto* l = sidechainDelayed.getWritePointer (0);
        auto* r = sidechainDelayed.getWritePointer (1);

        for (int i = 0; i < n; ++i)
        {
            l[i] = sidechainBandDelay[0].process (scLeft[i], toBands);
            r[i] = sidechainBandDelay[1].process (scRight[i], toBands);
        }

        for (int p = 0; p < nb; ++p)
            bandsUseSidechain = bandsUseSidechain || settings[(size_t) bandStage (layout.slots[(size_t) p])].scSource == scExternal;

        if (bandsUseSidechain)
        {
            const float* scIn[2] { l, r };
            sidechainSplitter.setLayout (layout, firstBlock);
            sidechainSplitter.process (scIn, sidechainBands.data(), n);
        }
    }

    // Band stages, then the sum.
    juce::FloatVectorOperations::clear (left, n);
    juce::FloatVectorOperations::clear (right, n);

    for (int p = 0; p < nb; ++p)
    {
        const int slot = layout.slots[(size_t) p];
        const int stageIndex = bandStage (slot);
        auto& stage = stages[(size_t) stageIndex];
        auto* bandL = bands[(size_t) slot][0];
        auto* bandR = bands[(size_t) slot][1];

        stage.pullCurves (curves.getSlot (stageIndex, CurveKind::level), curves.getSlot (stageIndex, CurveKind::transient));
        stage.setSettings (settings[(size_t) stageIndex], global, maxBandLookahead - settings[(size_t) stageIndex].lookaheadSamples);
        // A band listens to the same band of the sidechain, or to all of it.
        const float* scBandL = nullptr;
        const float* scBandR = nullptr;
        const int source = settings[(size_t) stageIndex].scSource;

        if (hasSidechain && source == scExternal && bandsUseSidechain)
        {
            scBandL = sidechainBands[(size_t) slot][0];
            scBandR = sidechainBands[(size_t) slot][1];
        }
        else if (hasSidechain && source == scExternalFull)
        {
            scBandL = sidechainDelayed.getReadPointer (0);
            scBandR = sidechainDelayed.getReadPointer (1);
        }

        stage.process (bandL, bandR, scBandL, scBandR, n);

        auto& g = bandGains[(size_t) slot];

        if (! g.isSmoothing() && g.getTargetValue() == 1.0f)
        {
            juce::FloatVectorOperations::add (left, bandL, n);
            juce::FloatVectorOperations::add (right, bandR, n);
        }
        else if (g.isSmoothing())
        {
            for (int i = 0; i < n; ++i)
            {
                const float gain = g.getNextValue();
                left[i] += gain * bandL[i];
                right[i] += gain * bandR[i];
            }
        }
    }

    for (int slot = 0; slot < maxBands; ++slot)
        if (! active[(size_t) slot])
            stages[(size_t) bandStage (slot)].meter.active.store (false);
}

void Engine::processOutput (float* left, float* right, int n)
{
    const float* dryL = scratch.getReadPointer (0);
    const float* dryR = scratch.getReadPointer (1);

    // Output gain (drives the clipper and limiter).
    const float outTarget = juce::Decibels::decibelsToGain (outGainParam->load());
    if (firstBlock) outGain.setCurrentAndTargetValue (outTarget); else outGain.setTargetValue (outTarget);

    if (outGain.isSmoothing() || outTarget != 1.0f)
        for (int i = 0; i < n; ++i)
        {
            const float g = outGain.getNextValue();
            left[i] *= g;
            right[i] *= g;
        }

    // Clipper (oversampled), with a plain delay path of the same latency while it's off.
    const float ceiling = juce::Decibels::decibelsToGain (ceilingParam->load());
    const int q = global.quality;
    const int osLat = clipLatency[(size_t) q];
    const float clipTarget = clipMode != clipOff ? 1.0f : 0.0f;
    if (firstBlock) clipFade.setCurrentAndTargetValue (clipTarget); else clipFade.setTargetValue (clipTarget);
    const bool clipOn = clipFade.isSmoothing() || clipTarget > 0.5f;

    auto* clipL = scratch.getWritePointer (2);
    auto* clipR = scratch.getWritePointer (3);

    if (clipOn)
    {
        std::copy (left, left + n, clipL);
        std::copy (right, right + n, clipR);

        if (! clipWasActive)
            if (auto* os = clipOversamplers[(size_t) q].get())
                os->reset();

        const bool soft = clipMode != clipHard;
        auto clip = [soft, ceiling] (float x) { return soft ? softClip (x, ceiling) : juce::jlimit (-ceiling, ceiling, x); };

        float* channels[] { clipL, clipR };
        juce::dsp::AudioBlock<float> block (channels, 2, (size_t) n);

        if (auto* os = clipOversamplers[(size_t) q].get())
        {
            auto up = os->processSamplesUp (block);
            for (size_t ch = 0; ch < 2; ++ch)
            {
                auto* data = up.getChannelPointer (ch);
                for (size_t i = 0; i < up.getNumSamples(); ++i)
                    data[i] = clip (data[i]);
            }
            os->processSamplesDown (block);
        }
        else
        {
            for (int i = 0; i < n; ++i)
            {
                clipL[i] = clip (clipL[i]);
                clipR[i] = clip (clipR[i]);
            }
        }
    }

    clipWasActive = clipOn;

    for (int i = 0; i < n; ++i)
    {
        const float l = clipDelay[0].process (left[i], osLat);
        const float r = clipDelay[1].process (right[i], osLat);

        if (clipOn)
        {
            const float f = clipFade.getNextValue();
            left[i] = l + f * (clipL[i] - l);
            right[i] = r + f * (clipR[i] - r);
        }
        else
        {
            left[i] = l;
            right[i] = r;
        }
    }

    // True-peak limiter.
    const float lowest = limiter.process (left, right, n, ceiling, limRelParam->load(), isOn (limiterParam), firstBlock);
    meters.limiterGainDb.store (juce::jmin (meters.limiterGainDb.load(), juce::Decibels::gainToDecibels (lowest, -60.0f)));

    // Auto gain: match the short-term loudness of the output to the input.
    dryLoudness.process (dryL, dryR, n);
    wetLoudness.process (left, right, n);

    if (isOn (autoGainParam))
    {
        const float dryLufs = dryLoudness.getLufs(), wetLufs = wetLoudness.getLufs();
        if (dryLufs > -70.0f && wetLufs > -70.0f)
            autoGainDb = juce::jlimit (-24.0f, 24.0f, dryLufs - wetLufs);
    }
    else
    {
        autoGainDb = 0.0f;
    }

    meters.autoGainDb.store (autoGainDb);
    const float agTarget = juce::Decibels::decibelsToGain (autoGainDb);
    if (firstBlock) autoGain.setCurrentAndTargetValue (agTarget); else autoGain.setTargetValue (agTarget);

    // Global mix and delta.
    const float mixTarget = mixParam->load() * 0.01f;
    if (firstBlock) globalMix.setCurrentAndTargetValue (mixTarget); else globalMix.setTargetValue (mixTarget);
    const bool delta = isOn (deltaParam);
    const bool agActive = autoGain.isSmoothing() || agTarget != 1.0f;
    float peakL = 0.0f, peakR = 0.0f, truePeak = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        float l = left[i], r = right[i];

        if (agActive)
        {
            const float g = autoGain.getNextValue();
            l *= g;
            r *= g;
        }

        const float m = globalMix.getNextValue();
        if (m < 1.0f)
        {
            l = dryL[i] + m * (l - dryL[i]);
            r = dryR[i] + m * (r - dryR[i]);
        }

        if (delta)
        {
            l -= dryL[i];
            r -= dryR[i];
        }

        left[i] = l;
        right[i] = r;
        peakL = juce::jmax (peakL, std::abs (l));
        peakR = juce::jmax (peakR, std::abs (r));
        truePeak = juce::jmax (truePeak, outTruePeak[0].process (l), outTruePeak[1].process (r));
    }

    meters.outPeak[0].store (juce::jmax (peakL, meters.outPeak[0].load()));
    meters.outPeak[1].store (juce::jmax (peakR, meters.outPeak[1].load()));

    if (meters.resetTruePeak.exchange (false))
        meters.truePeakMax.store (0.0f);
    meters.truePeakMax.store (juce::jmax (truePeak, meters.truePeakMax.load()));
    meters.outLoudness.process (left, right, n);

    if (postSpectrum.isEnabled())
    {
        auto* mono = scratch.getWritePointer (2);
        for (int i = 0; i < n; ++i)
            mono[i] = 0.5f * (left[i] + right[i]);
        postSpectrum.push (mono, n);
    }
}
}
