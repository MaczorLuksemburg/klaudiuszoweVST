#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace waveref;

namespace
{
    constexpr double abFadeSeconds     = 0.005;   // short enough that a kick keeps its attack
    constexpr double monoFadeSeconds   = 0.015;
    constexpr double filterFadeSeconds = 0.020;
    constexpr double gainRampSeconds   = 0.25;    // reference gain follows the loudness match slowly
    constexpr int slotFadeSamples      = 240;
    constexpr int restartFadeSamples   = 240;
    constexpr juce::int64 graveyardMs  = 2000;
    constexpr int64_t syncToleranceSamples = 64;   // following the host: re-aim the reference when it drifts further

    const juce::Identifier slotsId { "SLOTS" }, slotId { "SLOT" }, uiId { "UI" };
    const juce::Identifier indexProp { "index" }, pathProp { "path" }, loopStartProp { "loopStart" },
                           loopEndProp { "loopEnd" }, loopOnProp { "loopOn" }, bpmProp { "bpm" }, downbeatProp { "downbeat" };

    const juce::Identifier& cueProp (int i)
    {
        static const juce::Identifier ids[] { "cue0", "cue1", "cue2", "cue3" };
        return ids[i];
    }

    inline bool isOn (const std::atomic<float>* p) { return p->load() > 0.5f; }
}

WaveRefProcessor::WaveRefProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "WAVEREF", createParameterLayout())
{
    formats.registerBasicFormats();

    auto get = [this] (const char* id)
    {
        auto* p = apvts.getRawParameterValue (id);
        jassert (p != nullptr);
        return p;
    };

    params.ab = get (ids::ab);                     params.slot = get (ids::slot);
    params.matchOn = get (ids::matchOn);           params.matchMode = get (ids::matchMode);
    params.trim = get (ids::trim);                 params.mono = get (ids::mono);
    params.filterOn = get (ids::filterOn);         params.filterFreq = get (ids::filterFreq);
    params.filterWidth = get (ids::filterWidth);   params.restartSwitch = get (ids::restartSwitch);
    params.restartPlay = get (ids::restartPlay);   params.blind = get (ids::blind);
    params.refPlay = get (ids::refPlay);           params.refMode = get (ids::refMode);
    params.syncOffset = get (ids::syncOffset);     params.beatOffset = get (ids::beatOffset);
    params.nudge = get (ids::nudge);               params.keyLock = get (ids::keyLock);
    params.side = get (ids::side);                 params.delta = get (ids::delta);

    blindRandom.setSeedRandomly();
    xIsRef = blindRandom.nextBool();

    for (auto& s : slots)
        s.info = {};
}

WaveRefProcessor::~WaveRefProcessor()
{
    loader.removeAllJobs (true, 10000);
}

bool WaveRefProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
        && (in == juce::AudioChannelSet::stereo() || in == juce::AudioChannelSet::mono());
}

bool WaveRefProcessor::isReferenceSelected() const
{
    const bool ab = isOn (params.ab);

    // In a blind test the buttons are X and Y, and which of them carries the reference is hidden.
    return isOn (params.blind) ? ab != xIsRef.load() : ab;
}

void WaveRefProcessor::prepareToPlay (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    const bool rateChanged = ! rateKnown || std::abs (currentRate.load() - newSampleRate) > 0.5;
    currentRate = newSampleRate;
    rateKnown = true;

    liveMeter.prepare (newSampleRate);
    player.reset();
    stretch.prepare (newSampleRate, maxChunk);
    playingSlot = -1;
    wasPlaying = false;
    wasRefAudible = isReferenceSelected();
    matchGainDb = 0.0f;
    analyser.setSampleRate (newSampleRate);
    stereo.setSampleRate (newSampleRate);

    abFade.reset (newSampleRate, abFadeSeconds);
    monoFade.reset (newSampleRate, monoFadeSeconds);
    sideFade.reset (newSampleRate, monoFadeSeconds);
    deltaFade.reset (newSampleRate, monoFadeSeconds);
    filterFade.reset (newSampleRate, filterFadeSeconds);
    refGain.reset (newSampleRate, gainRampSeconds);
    abFade.setCurrentAndTargetValue (wasRefAudible ? 1.0f : 0.0f);
    monoFade.setCurrentAndTargetValue (isOn (params.mono) && ! isOn (params.side) ? 1.0f : 0.0f);
    sideFade.setCurrentAndTargetValue (isOn (params.side) ? 1.0f : 0.0f);
    deltaFade.setCurrentAndTargetValue (isOn (params.delta) ? 1.0f : 0.0f);
    currentTrack = nullptr;
    filterFade.setCurrentAndTargetValue (isOn (params.filterOn) ? 1.0f : 0.0f);
    refGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (params.trim->load()));

    const juce::dsp::ProcessSpec spec { newSampleRate, (juce::uint32) maxChunk, 2 };
    const juce::dsp::StateVariableTPTFilterType types[] { juce::dsp::StateVariableTPTFilterType::highpass,
                                                          juce::dsp::StateVariableTPTFilterType::highpass,
                                                          juce::dsp::StateVariableTPTFilterType::lowpass,
                                                          juce::dsp::StateVariableTPTFilterType::lowpass };
    for (size_t i = 0; i < bandFilters.size(); ++i)
    {
        bandFilters[i].prepare (spec);
        bandFilters[i].setType (types[i]);
        bandFilters[i].setResonance (0.7071f);
    }

    lastFilterFreq = lastFilterWidth = -1.0f;

    if (rateChanged)
        loadAllSlots();   // the decoded copies are at the old rate
}

void WaveRefProcessor::updateFilterCutoffs (float freq, float widthOctaves)
{
    if (freq == lastFilterFreq && widthOctaves == lastFilterWidth)
        return;

    lastFilterFreq = freq;
    lastFilterWidth = widthOctaves;

    const float half = std::pow (2.0f, widthOctaves * 0.5f);
    const float low = juce::jlimit (20.0f, 20000.0f, freq / half);
    const float high = juce::jlimit (low * 1.1f, (float) sampleRate * 0.45f, freq * half);

    bandFilters[0].setCutoffFrequency (low);
    bandFilters[1].setCutoffFrequency (low);
    bandFilters[2].setCutoffFrequency (high);
    bandFilters[3].setCutoffFrequency (high);
}

void WaveRefProcessor::effectiveLoop (const TrackData* track, int slot, int64_t& start, int64_t& end) const
{
    const int64_t length = track->getLength();
    start = slots[(size_t) slot].loopStart.load();
    end = slots[(size_t) slot].loopEnd.load();

    if (end <= 0 || end > length)
        end = length;

    start = juce::jlimit<int64_t> (0, juce::jmax<int64_t> (0, length - 64), start);

    if (end - start < 64)
    {
        start = 0;
        end = length;
    }
}

//==============================================================================
void WaveRefProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();

    if (buffer.getNumChannels() < 2 || numSamples == 0)
        return;

    if (getTotalNumInputChannels() == 1)
        buffer.copyFrom (1, 0, buffer, 0, 0, numSamples);

    // Tracks the loader finished since the last block.
    const int selectedSlot = juce::jlimit (0, numSlots - 1, (int) std::lround (params.slot->load()));
    bool trackReplaced = false;

    for (int i = 0; i < numSlots; ++i)
    {
        if (auto* fresh = slots[(size_t) i].pending.exchange (nullptr))
        {
            tracks[(size_t) i] = fresh;
            slots[(size_t) i].active.store (fresh);
            trackReplaced = trackReplaced || i == selectedSlot;
        }
    }

    // What the host tells us about its transport.
    bool hostPlaying = false, timeKnown = false, beatKnown = false;
    double hostTime = 0.0, ppq = 0.0, hostTempo = 0.0;

    if (auto* head = getPlayHead())
    {
        if (auto position = head->getPosition())
        {
            hostPlaying = position->getIsPlaying();

            if (auto seconds = position->getTimeInSeconds())
            {
                hostTime = *seconds;
                timeKnown = true;
            }
            else if (auto samples = position->getTimeInSamples())
            {
                hostTime = (double) *samples / sampleRate;
                timeKnown = true;
            }

            if (auto quarters = position->getPpqPosition())
                if (auto bpm = position->getBpm())
                {
                    ppq = *quarters;
                    hostTempo = *bpm;
                    beatKnown = hostTempo > 1.0;
                }
        }
    }

    hostTimeKnown.store (timeKnown);
    hostIsPlaying.store (hostPlaying);
    hostSeconds.store (hostTime);
    hostPpq.store (ppq);
    hostBpmValue.store (hostTempo);
    hostBeatKnown.store (beatKnown);

    const bool playStarted = hostPlaying && ! wasPlaying;
    wasPlaying = hostPlaying;

    const bool refSelected = isReferenceSelected();
    const bool switchedIn = refSelected && ! wasRefAudible;
    wasRefAudible = refSelected;

    const TrackData* track = tracks[(size_t) selectedSlot];
    const bool hasRef = track != nullptr && ! track->isEmpty();
    const int modeIndex = juce::jlimit (0, 2, (int) std::lround (params.refMode->load()));
    const bool freeMode = modeIndex == modeFree;
    const bool beatsMode = modeIndex == modeBeats;
    bool stretched = false;
    const bool playParam = isOn (params.refPlay);
    const int64_t seek = seekRequest.exchange (-1);
    int64_t loopStart = 0, loopEnd = 0;

    if (hasRef)
    {
        const int64_t length = track->getLength();

        if (freeMode)
        {
            effectiveLoop (track, selectedSlot, loopStart, loopEnd);

            if (stretch.isActive())
                stretch.stop (restartFadeSamples);

            if (selectedSlot != playingSlot || trackReplaced || currentTrack != track)
            {
                currentTrack = track;

                if (playParam)
                {
                    player.start (track, loopStart, slotFadeSamples, slotFadeSamples);
                }
                else
                {
                    player.stop (slotFadeSamples);
                    player.setHeld (loopStart);
                }
            }
            else if (seek >= 0)
            {
                const int64_t position = juce::jlimit<int64_t> (0, length - 1, seek);

                if (playParam)
                {
                    player.start (track, position, 0, restartFadeSamples);
                }
                else
                {
                    player.stop (restartFadeSamples);
                    player.setHeld (position);
                }
            }
            else if ((playStarted && isOn (params.restartPlay)) || (switchedIn && isOn (params.restartSwitch)))
            {
                if (playParam)
                {
                    player.start (track, loopStart, 0, restartFadeSamples);
                }
                else
                {
                    player.stop (restartFadeSamples);
                    player.setHeld (loopStart);
                }
            }
            else if (playParam && ! player.isPlaying())
            {
                player.start (track, player.getPosition(), 48, restartFadeSamples);   // resume where it was stopped
            }
            else if (! playParam && player.isPlaying())
            {
                player.stop (restartFadeSamples);
            }
        }
        else
        {
            // Following the host, no looping.
            currentTrack = track;
            loopStart = 0;
            loopEnd = length;

            double expected = 0.0, ratio = 1.0;
            bool canFollow = false;

            if (beatsMode)
            {
                const double refBpm = slots[(size_t) selectedSlot].gridBpm.load();
                const double downbeat = slots[(size_t) selectedSlot].gridDownbeat.load();

                if (beatKnown && refBpm > 1.0)
                {
                    // Reference beat = host beat + offset, so the grids line up whatever the two tempos are.
                    const double seconds = downbeat + (ppq + (double) params.beatOffset->load()) * 60.0 / refBpm
                                           + 0.001 * (double) params.nudge->load();
                    expected = seconds * sampleRate;
                    ratio = refBpm / hostTempo;
                    canFollow = true;
                }
            }
            else if (timeKnown)
            {
                expected = (hostTime + (double) params.syncOffset->load()) * sampleRate;
                canFollow = true;
            }

            if (canFollow)
            {
                const int64_t position = (int64_t) std::llround (expected);
                const bool inside = position >= 0 && position < length;
                const bool sameTempo = std::abs (ratio - 1.0) < 1.0e-4;

                if (hostPlaying && inside && sameTempo)
                {
                    if (stretch.isActive())
                        stretch.stop (restartFadeSamples);

                    if (! player.isPlaying() || trackReplaced || std::abs (player.getPosition() - position) > syncToleranceSamples)
                        player.start (track, position, 0, restartFadeSamples);
                }
                else if (hostPlaying && inside)
                {
                    if (player.isPlaying())
                        player.stop (restartFadeSamples);

                    stretch.update (track, expected, ratio, isOn (params.keyLock));
                    stretched = true;
                }
                else
                {
                    if (player.isPlaying())
                        player.stop (restartFadeSamples);

                    if (stretch.isActive())
                        stretch.stop (restartFadeSamples);
                }

                if (! player.isPlaying() && ! stretched)
                    player.setHeld (juce::jlimit<int64_t> (0, length, position));
            }
            else
            {
                if (player.isPlaying())
                    player.stop (restartFadeSamples);

                if (stretch.isActive())
                    stretch.stop (restartFadeSamples);
            }
        }
    }
    else
    {
        if (player.isPlaying())
            player.reset();

        stretch.reset();
        currentTrack = nullptr;
    }

    playingSlot = selectedSlot;

    const bool matchOn = isOn (params.matchOn);
    const bool longWindow = (int) std::lround (params.matchMode->load()) == matchLong;
    const float trimDb = params.trim->load();
    const float refLoopLufs = hasRef ? slots[(size_t) selectedSlot].loopLufs.load() : loudness::silenceLufs;

    abFade.setTargetValue (refSelected ? 1.0f : 0.0f);
    monoFade.setTargetValue (isOn (params.mono) && ! isOn (params.side) ? 1.0f : 0.0f);
    sideFade.setTargetValue (isOn (params.side) ? 1.0f : 0.0f);
    deltaFade.setTargetValue (isOn (params.delta) ? 1.0f : 0.0f);
    filterFade.setTargetValue (isOn (params.filterOn) ? 1.0f : 0.0f);

    float* const left = buffer.getWritePointer (0);
    float* const right = buffer.getWritePointer (1);

    for (int start = 0; start < numSamples; start += maxChunk)
    {
        const int n = juce::jmin (maxChunk, numSamples - start);
        float* l = left + start;
        float* r = right + start;

        // Mix loudness, and the gain that brings the reference to it.
        liveMeter.process (l, r, n);
        const float mixDb = longWindow ? liveMeter.getLongLufs() : liveMeter.getShortLufs();

        if (matchOn && hasRef && mixDb > -70.0f && refLoopLufs > -70.0f)
            matchGainDb = juce::jlimit (-40.0f, 40.0f, mixDb - refLoopLufs);

        refGain.setTargetValue (juce::Decibels::decibelsToGain ((matchOn && hasRef ? matchGainDb : 0.0f) + trimDb));

        // Reference, at the matched level.
        std::fill_n (refL.begin(), n, 0.0f);
        std::fill_n (refR.begin(), n, 0.0f);

        if (hasRef && player.hasVoices())
            player.render (refL.data(), refR.data(), n, loopStart, loopEnd, freeMode);

        if (stretch.isActive())
            stretch.render (refL.data(), refR.data(), n);

        for (int i = 0; i < n; ++i)
        {
            const float g = refGain.getNextValue();
            refL[(size_t) i] *= g;
            refR[(size_t) i] *= g;
        }

        analyser.push (false, l, r, n);
        analyser.push (true, refL.data(), refR.data(), n);
        stereo.push (false, l, r, n);
        stereo.push (true, refL.data(), refR.data(), n);

        const bool needMixCopy = deltaFade.isSmoothing() || isOn (params.delta);

        if (needMixCopy)
        {
            std::copy_n (l, n, mixL.begin());
            std::copy_n (r, n, mixR.begin());
        }

        // A/B: untouched mix when A is selected and settled, so the plugin is transparent.
        if (abFade.isSmoothing() || refSelected)
        {
            for (int i = 0; i < n; ++i)
            {
                const float f = abFade.getNextValue();
                l[i] += f * (refL[(size_t) i] - l[i]);
                r[i] += f * (refR[(size_t) i] - r[i]);
            }
        }

        // Delta: what the mix has that the reference doesn't (both at their matched levels).
        if (needMixCopy)
        {
            for (int i = 0; i < n; ++i)
            {
                const float f = deltaFade.getNextValue();
                l[i] += f * ((mixL[(size_t) i] - refL[(size_t) i]) - l[i]);
                r[i] += f * ((mixR[(size_t) i] - refR[(size_t) i]) - r[i]);
            }
        }

        if (sideFade.isSmoothing() || isOn (params.side))
        {
            for (int i = 0; i < n; ++i)
            {
                const float f = sideFade.getNextValue();
                const float s = 0.5f * (l[i] - r[i]);
                l[i] += f * (s - l[i]);
                r[i] += f * (s - r[i]);
            }
        }

        if (monoFade.isSmoothing() || (isOn (params.mono) && ! isOn (params.side)))
        {
            for (int i = 0; i < n; ++i)
            {
                const float f = monoFade.getNextValue();
                const float m = 0.5f * (l[i] + r[i]);
                l[i] += f * (m - l[i]);
                r[i] += f * (m - r[i]);
            }
        }

        if (filterFade.isSmoothing() || isOn (params.filterOn))
        {
            updateFilterCutoffs (params.filterFreq->load(), params.filterWidth->load());

            for (int i = 0; i < n; ++i)
            {
                const float f = filterFade.getNextValue();
                float bandL = l[i], bandR = r[i];

                for (auto& filter : bandFilters)
                {
                    bandL = filter.processSample (0, bandL);
                    bandR = filter.processSample (1, bandR);
                }

                l[i] += f * (bandL - l[i]);
                r[i] += f * (bandR - r[i]);
            }
        }
        else
        {
            for (auto& filter : bandFilters)
                filter.reset();
        }
    }

    refAudible.store (refSelected);
    mixLufs.store (longWindow ? liveMeter.getLongLufs() : liveMeter.getShortLufs());
    refLufs.store (refLoopLufs);
    matchDb.store (matchOn && hasRef ? matchGainDb : 0.0f);
    playheadSlot.store (hasRef ? selectedSlot : -1);
    playhead.store (stretch.isActive() && beatsMode ? (int64_t) std::llround (stretch.getPosition()) : player.getPosition());
}

//==============================================================================
// Reference slots

void WaveRefProcessor::loadAllSlots()
{
    for (int i = 0; i < numSlots; ++i)
    {
        juce::String path;
        {
            const juce::ScopedLock lock (slotLock);
            path = slots[(size_t) i].info.path;
        }

        if (path.isNotEmpty())
            startLoad (i, juce::File (path), false, nullptr);
    }
}

void WaveRefProcessor::loadFile (int slot, const juce::File& file)
{
    jassert (juce::isPositiveAndBelow (slot, numSlots));

    {
        const juce::ScopedLock lock (slotLock);
        auto& info = slots[(size_t) slot].info;

        if (info.path != file.getFullPathName())
        {
            info.loopStart = info.loopEnd = 0.0;
            info.loopOn = false;
            info.bpm = info.downbeat = 0.0;

            for (auto& cue : info.cues)
                cue = -1.0;
        }

        info.path = file.getFullPathName();
    }

    writeSlotState (slot);
    startLoad (slot, file, false, nullptr);
}

bool WaveRefProcessor::loadFileBlocking (int slot, const juce::File& file, juce::String* error)
{
    {
        const juce::ScopedLock lock (slotLock);
        auto& info = slots[(size_t) slot].info;

        if (info.path != file.getFullPathName())
        {
            info.loopStart = info.loopEnd = 0.0;
            info.loopOn = false;
            info.bpm = info.downbeat = 0.0;

            for (auto& cue : info.cues)
                cue = -1.0;
        }

        info.path = file.getFullPathName();
    }

    writeSlotState (slot);

    juce::String message;
    startLoad (slot, file, true, &message);

    if (error != nullptr)
        *error = message;

    return message.isEmpty();
}

void WaveRefProcessor::startLoad (int slot, const juce::File& file, bool blocking, juce::String* error)
{
    const int generation = ++slots[(size_t) slot].generation;
    const double rate = currentRate.load();

    {
        const juce::ScopedLock lock (slotLock);
        slots[(size_t) slot].info.loading = true;
        slots[(size_t) slot].info.error = {};
    }

    sendChangeMessage();

    if (blocking)
    {
        juce::String message;
        std::shared_ptr<TrackData> track (decodeTrack (formats, file, rate, message));
        finishLoad (slot, generation, std::move (track), message);

        if (error != nullptr)
            *error = message;

        return;
    }

    loader.addJob ([this, slot, generation, file, rate]
    {
        juce::String message;
        std::shared_ptr<TrackData> track (decodeTrack (formats, file, rate, message));

        {
            const juce::ScopedLock lock (slotLock);
            finishedLoads.push_back ({ slot, generation, std::move (track), message });
        }

        juce::MessageManager::callAsync ([weak = juce::WeakReference<WaveRefProcessor> (this)]
        {
            if (weak != nullptr)
                weak->drainFinishedLoads();
        });
    });
}

void WaveRefProcessor::drainFinishedLoads()
{
    std::vector<FinishedLoad> ready;
    {
        const juce::ScopedLock lock (slotLock);
        ready.swap (finishedLoads);
    }

    for (auto& load : ready)
        finishLoad (load.slot, load.generation, std::move (load.track), load.error);
}

void WaveRefProcessor::finishLoad (int slot, int generation, std::shared_ptr<TrackData> track, const juce::String& error)
{
    if (generation != slots[(size_t) slot].generation.load())
        return;   // a newer request replaced this one

    {
        const juce::ScopedLock lock (slotLock);
        slots[(size_t) slot].info.loading = false;
        slots[(size_t) slot].info.error = error;
    }

    // On failure keep the path (the file may only be missing on this machine) but stop playing the old audio.
    installTrack (slot, track != nullptr ? std::move (track) : std::shared_ptr<TrackData> (makeEmptyTrack (currentRate.load())));
}

void WaveRefProcessor::installTrack (int slot, std::shared_ptr<TrackData> shared)
{
    jassert (juce::isPositiveAndBelow (slot, numSlots));

    {
        const juce::ScopedLock lock (slotLock);
        auto& s = slots[(size_t) slot];

        if (s.owner != nullptr)
            graveyard.push_back ({ s.owner, juce::Time::getMillisecondCounter() });

        s.owner = shared;
    }

    if (shared != nullptr && shared->tempo.bpm > 0.0)
    {
        bool adopted = false;
        {
            const juce::ScopedLock lock (slotLock);
            auto& info = slots[(size_t) slot].info;

            if (info.bpm <= 0.0)
            {
                info.bpm = shared->tempo.bpm;
                info.downbeat = shared->tempo.downbeat;
                adopted = true;
            }
        }

        if (adopted)
            writeSlotState (slot);
    }

    publishLoop (slot);
    slots[(size_t) slot].pending.store (shared.get());
    collectGarbage();   // the editor's timer does this too, but it may not be open
    sendChangeMessage();
}

void WaveRefProcessor::clearSlot (int slot)
{
    ++slots[(size_t) slot].generation;

    {
        const juce::ScopedLock lock (slotLock);
        slots[(size_t) slot].info = {};
    }

    writeSlotState (slot);
    installTrack (slot, makeEmptyTrack (currentRate.load()));
}

void WaveRefProcessor::publishLoop (int slot)
{
    std::shared_ptr<TrackData> track;
    SlotInfo info;
    {
        const juce::ScopedLock lock (slotLock);
        track = slots[(size_t) slot].owner;
        info = slots[(size_t) slot].info;
    }

    auto& s = slots[(size_t) slot];
    s.gridBpm = info.bpm;
    s.gridDownbeat = info.downbeat;

    if (track == nullptr || track->isEmpty())
    {
        s.loopStart = 0;
        s.loopEnd = 0;
        s.loopLufs = loudness::silenceLufs;
        return;
    }

    const int64_t length = track->getLength();
    int64_t start = 0, end = 0;

    if (info.loopOn)
    {
        start = juce::jlimit<int64_t> (0, length - 1, (int64_t) std::llround (info.loopStart * track->sampleRate));
        end = info.loopEnd > 0.0 ? juce::jmin (length, (int64_t) std::llround (info.loopEnd * track->sampleRate)) : length;

        if (end - start < 64)
            start = end = 0;
    }

    s.loopLufs = track->getLoopLoudness (start, end > 0 ? end : length);
    s.loopStart = start;
    s.loopEnd = end;
}

void WaveRefProcessor::setLoop (int slot, double startSeconds, double endSeconds, bool loopOn)
{
    {
        const juce::ScopedLock lock (slotLock);
        auto& info = slots[(size_t) slot].info;
        info.loopStart = juce::jmax (0.0, startSeconds);
        info.loopEnd = juce::jmax (0.0, endSeconds);
        info.loopOn = loopOn;
    }

    writeSlotState (slot);
    publishLoop (slot);
    sendChangeMessage();
}

void WaveRefProcessor::setCue (int slot, int index, double seconds)
{
    if (! juce::isPositiveAndBelow (slot, numSlots) || ! juce::isPositiveAndBelow (index, 4))
        return;

    {
        const juce::ScopedLock lock (slotLock);
        slots[(size_t) slot].info.cues[index] = seconds < 0.0 ? -1.0 : seconds;
    }

    writeSlotState (slot);
    sendChangeMessage();
}

void WaveRefProcessor::seekReference (double seconds)
{
    seekRequest.store ((int64_t) std::llround (juce::jmax (0.0, seconds) * currentRate.load()));
}

bool WaveRefProcessor::alignToHost()
{
    const int mode = juce::jlimit (0, 2, (int) std::lround (params.refMode->load()));
    const int slot = juce::jlimit (0, numSlots - 1, (int) std::lround (params.slot->load()));
    const double bpm = slots[(size_t) slot].gridBpm.load();

    if (mode == modeBeats && hostBeatKnown.load() && bpm > 1.0)
    {
        // Whole beats, so the grids stay musically aligned; the nudge handles what the grid gets wrong.
        const double refBeat = (getPlayheadSeconds() - slots[(size_t) slot].gridDownbeat.load()) * bpm / 60.0;
        const double offset = juce::jlimit (-16.0, 16.0, std::round (refBeat - hostPpq.load()));

        if (auto* p = apvts.getParameter (ids::beatOffset))
            p->setValueNotifyingHost (p->convertTo0to1 ((float) offset));

        if (auto* p = apvts.getParameter (ids::nudge))
            p->setValueNotifyingHost (p->convertTo0to1 (0.0f));

        return true;
    }

    if (! hostTimeKnown.load())
        return false;

    const double offset = juce::jlimit (-60.0, 60.0, getPlayheadSeconds() - hostSeconds.load());

    if (auto* p = apvts.getParameter (ids::syncOffset))
        p->setValueNotifyingHost (p->convertTo0to1 ((float) offset));

    if (auto* p = apvts.getParameter (ids::refMode))
        p->setValueNotifyingHost (p->convertTo0to1 ((float) modeHostTime));

    return true;
}

void WaveRefProcessor::setGrid (int slot, double bpm, double downbeatSeconds)
{
    if (! juce::isPositiveAndBelow (slot, numSlots))
        return;

    {
        const juce::ScopedLock lock (slotLock);
        auto& info = slots[(size_t) slot].info;
        info.bpm = juce::jlimit (0.0, 400.0, bpm);
        info.downbeat = juce::jmax (0.0, downbeatSeconds);
    }

    writeSlotState (slot);
    publishLoop (slot);
    sendChangeMessage();
}

void WaveRefProcessor::detectGridAgain (int slot)
{
    if (auto track = getTrack (slot))
        if (track->tempo.bpm > 0.0)
            setGrid (slot, track->tempo.bpm, track->tempo.downbeat);
}

void WaveRefProcessor::getLoopSamples (int slot, int64_t& start, int64_t& end) const
{
    start = end = 0;

    if (auto track = getTrack (slot))
    {
        if (! track->isEmpty())
        {
            start = slots[(size_t) slot].loopStart.load();
            end = slots[(size_t) slot].loopEnd.load();

            if (end <= 0)
                end = track->getLength();
        }
    }
}

std::shared_ptr<const TrackData> WaveRefProcessor::getTrack (int slot) const
{
    const juce::ScopedLock lock (slotLock);
    return slots[(size_t) slot].owner;
}

WaveRefProcessor::SlotInfo WaveRefProcessor::getSlotInfo (int slot) const
{
    const juce::ScopedLock lock (slotLock);
    return slots[(size_t) slot].info;
}

void WaveRefProcessor::collectGarbage()
{
    const juce::ScopedLock lock (slotLock);
    const auto now = juce::Time::getMillisecondCounter();

    graveyard.erase (std::remove_if (graveyard.begin(), graveyard.end(), [&] (const Retired& item)
    {
        if ((juce::int64) (now - (juce::uint32) item.retiredAt) < graveyardMs)
            return false;

        // The audio thread might still be fading out of it: keep it until the audio thread has moved on.
        for (auto& s : slots)
            if (s.active.load() == item.track.get() || s.pending.load() == item.track.get())
                return false;

        return true;
    }), graveyard.end());
}

//==============================================================================
// Blind test

void WaveRefProcessor::rerollBlind()
{
    xIsRef = blindRandom.nextBool();
}

bool WaveRefProcessor::submitGuess (bool guessedXIsRef)
{
    const bool correct = guessedXIsRef == xIsRef.load();
    ++blindTotal;

    if (correct)
        ++blindCorrect;

    rerollBlind();
    return correct;
}

//==============================================================================
// State

juce::ValueTree WaveRefProcessor::getUiState()
{
    return apvts.state.getOrCreateChildWithName (uiId, nullptr);
}

void WaveRefProcessor::writeSlotState (int slot)
{
    auto slotsNode = apvts.state.getOrCreateChildWithName (slotsId, nullptr);
    auto node = slotsNode.getChildWithProperty (indexProp, slot);

    if (! node.isValid())
    {
        node = juce::ValueTree (slotId);
        node.setProperty (indexProp, slot, nullptr);
        slotsNode.appendChild (node, nullptr);
    }

    const auto info = getSlotInfo (slot);
    node.setProperty (pathProp, info.path, nullptr);
    node.setProperty (loopStartProp, info.loopStart, nullptr);
    node.setProperty (loopEndProp, info.loopEnd, nullptr);
    node.setProperty (loopOnProp, info.loopOn, nullptr);
    node.setProperty (bpmProp, info.bpm, nullptr);
    node.setProperty (downbeatProp, info.downbeat, nullptr);

    for (int i = 0; i < 4; ++i)
        node.setProperty (cueProp (i), info.cues[i], nullptr);
}

void WaveRefProcessor::applySlotState()
{
    const auto slotsNode = apvts.state.getChildWithName (slotsId);

    for (int i = 0; i < numSlots; ++i)
    {
        const auto node = slotsNode.getChildWithProperty (indexProp, i);
        SlotInfo wanted;

        if (node.isValid())
        {
            wanted.path = node[pathProp].toString();
            wanted.loopStart = (double) node[loopStartProp];
            wanted.loopEnd = (double) node[loopEndProp];
            wanted.loopOn = (bool) node[loopOnProp];
            wanted.bpm = (double) node[bpmProp];
            wanted.downbeat = (double) node[downbeatProp];

            for (int k = 0; k < 4; ++k)
                wanted.cues[k] = node.hasProperty (cueProp (k)) ? (double) node[cueProp (k)] : -1.0;
        }
        else
        {
            for (auto& cue : wanted.cues)
                cue = -1.0;
        }

        juce::String loadedPath;
        bool hasAudio = false;
        {
            const juce::ScopedLock lock (slotLock);
            auto& s = slots[(size_t) i];
            s.info.path = wanted.path;
            s.info.loopStart = wanted.loopStart;
            s.info.loopEnd = wanted.loopEnd;
            s.info.loopOn = wanted.loopOn;
            s.info.bpm = wanted.bpm;
            s.info.downbeat = wanted.downbeat;
            std::copy (std::begin (wanted.cues), std::end (wanted.cues), std::begin (s.info.cues));
            hasAudio = s.owner != nullptr && ! s.owner->isEmpty();
            loadedPath = hasAudio ? s.owner->path : juce::String();
        }

        if (wanted.path.isEmpty())
        {
            if (hasAudio)
                installTrack (i, makeEmptyTrack (currentRate.load()));
        }
        else if (loadedPath != wanted.path)
        {
            if (rateKnown)
                startLoad (i, juce::File (wanted.path), false, nullptr);   // otherwise prepareToPlay loads it
        }
        else
        {
            publishLoop (i);
        }
    }

    sendChangeMessage();
}

void WaveRefProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void WaveRefProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
    {
        if (xml->hasTagName (apvts.state.getType()))
        {
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
            applySlotState();
        }
    }
}

juce::AudioProcessorEditor* WaveRefProcessor::createEditor()
{
    return new WaveRefEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new WaveRefProcessor();
}
