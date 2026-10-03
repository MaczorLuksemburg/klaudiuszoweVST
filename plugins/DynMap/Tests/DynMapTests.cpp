// Offline checks for DynMap: DSP behaviour, presets and state, a CPU bench and an optional
// screenshot of the editor.
// Usage: DynMap_Tests [--no-gui] [--snapshot <dir>] [--bench] [--only <group>]
//        DynMap_Tests --kit <file.wav> | --compare <render.wav> [--preset <name>] [--out <file.wav>]  (see Measure.h)
#include "../Source/PluginEditor.h"
#include "../Source/PluginProcessor.h"
#include "Measure.h"

#include <iostream>

using namespace dynmap;

namespace
{
    constexpr double testSampleRate = 48000.0;
    int failures = 0;

    void expect (bool condition, const juce::String& what)
    {
        std::cout << (condition ? "  ok    " : "  FAIL  ") << what << std::endl;
        if (! condition)
            ++failures;
    }

    juce::String db (float value) { return juce::String (value, 2) + " dB"; }

    void setParam (DynMapProcessor& p, const juce::String& id, float value)
    {
        auto* param = p.apvts.getParameter (id);
        jassert (param != nullptr);
        param->setValueNotifyingHost (param->convertTo0to1 (value));
    }

    void setStage (DynMapProcessor& p, int stage, const char* name, float value)
    {
        setParam (p, stageParamId (stage, name), value);
    }

    void addBand (DynMapProcessor& p, int slot, float hz, int slope = slope24)
    {
        setStage (p, bandStage (slot), ids::bandOn, 1.0f);
        setStage (p, bandStage (slot), ids::freq, hz);
        setStage (p, bandStage (slot), ids::slope, (float) slope);
    }

    void resetAll (DynMapProcessor& p)
    {
        p.presets.resetToDefaults();
    }

    void setCurve (DynMapProcessor& p, int stage, CurveKind kind, int presetIndex)
    {
        p.engine.curves.set (stage, kind, Curve::preset (kind, presetIndex));
    }

    void loadPresetNamed (DynMapProcessor& p, const juce::String& name)
    {
        const int index = p.presets.getPresetNames().indexOf (name);
        jassert (index >= 0);
        p.presets.loadPreset (index);
    }

    juce::AudioBuffer<float> makeNoise (int numSamples, int seed, float amplitude = 0.5f)
    {
        juce::Random random (seed);
        juce::AudioBuffer<float> buffer (2, numSamples);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < numSamples; ++i)
                buffer.setSample (ch, i, (random.nextFloat() * 2.0f - 1.0f) * amplitude);

        return buffer;
    }

    juce::AudioBuffer<float> makeSine (int numSamples, float freq, float peakDb)
    {
        juce::AudioBuffer<float> buffer (2, numSamples);
        const float amp = juce::Decibels::decibelsToGain (peakDb);

        for (int i = 0; i < numSamples; ++i)
        {
            const float v = amp * std::sin (juce::MathConstants<float>::twoPi * freq * (float) i / (float) testSampleRate);
            buffer.setSample (0, i, v);
            buffer.setSample (1, i, v);
        }

        return buffer;
    }

    juce::AudioBuffer<float> makeImpulse (int numSamples, int position = 100)
    {
        juce::AudioBuffer<float> buffer (2, numSamples);
        buffer.clear();
        buffer.setSample (0, position, 1.0f);
        buffer.setSample (1, position, 1.0f);
        return buffer;
    }

    // Decaying noise bursts: 1 ms attack, ~80 ms decay, every 250 ms.
    juce::AudioBuffer<float> makeDrums (int numSamples, int seed = 7)
    {
        juce::Random random (seed);
        juce::AudioBuffer<float> buffer (2, numSamples);
        const int period = (int) (0.25 * testSampleRate);

        for (int i = 0; i < numSamples; ++i)
        {
            const float t = (float) (i % period) / (float) testSampleRate;
            const float env = juce::jmin (1.0f, t / 0.001f) * std::exp (-t / 0.08f);
            const float v = 0.5f * env * (random.nextFloat() * 2.0f - 1.0f);
            buffer.setSample (0, i, v);
            buffer.setSample (1, i, v);
        }

        return buffer;
    }

    juce::AudioBuffer<float> run (DynMapProcessor& p, const juce::AudioBuffer<float>& input, int blockSize = 512)
    {
        juce::AudioBuffer<float> output (input);
        juce::MidiBuffer midi;
        p.prepareToPlay (testSampleRate, blockSize);

        for (int start = 0; start < output.getNumSamples(); start += blockSize)
        {
            const int length = std::min (blockSize, output.getNumSamples() - start);
            juce::AudioBuffer<float> block (output.getArrayOfWritePointers(), 2, start, length);
            p.processBlock (block, midi);
        }

        return output;
    }

    // Same, but through the engine with a sidechain.
    juce::AudioBuffer<float> runWithSidechain (DynMapProcessor& p, const juce::AudioBuffer<float>& input,
                                               const juce::AudioBuffer<float>& sidechain, int blockSize = 512)
    {
        juce::AudioBuffer<float> output (input);
        p.prepareToPlay (testSampleRate, blockSize);

        for (int start = 0; start < output.getNumSamples(); start += blockSize)
        {
            const int length = std::min (blockSize, output.getNumSamples() - start);
            p.engine.process (output.getWritePointer (0, start), output.getWritePointer (1, start),
                              sidechain.getReadPointer (0, start), sidechain.getReadPointer (1, start), length);
        }

        return output;
    }

    float peakDb (const juce::AudioBuffer<float>& b, int from, int to, int ch = 0)
    {
        float peak = 0.0f;
        for (int i = from; i < to; ++i)
            peak = std::max (peak, std::abs (b.getSample (ch, i)));
        return juce::Decibels::gainToDecibels (peak, -200.0f);
    }

    float rmsDb (const juce::AudioBuffer<float>& b, int from, int to, int ch = 0)
    {
        double sum = 0.0;
        for (int i = from; i < to; ++i)
            sum += (double) b.getSample (ch, i) * b.getSample (ch, i);
        return juce::Decibels::gainToDecibels ((float) std::sqrt (sum / juce::jmax (1, to - from)), -200.0f);
    }

    // Largest difference between output and input shifted by the latency.
    float maxShiftedDifference (const juce::AudioBuffer<float>& out, const juce::AudioBuffer<float>& in, int latency)
    {
        float diff = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = latency; i < out.getNumSamples(); ++i)
                diff = std::max (diff, std::abs (out.getSample (ch, i) - in.getSample (ch, i - latency)));
        return diff;
    }

    int peakPosition (const juce::AudioBuffer<float>& b)
    {
        int best = 0;
        for (int i = 0; i < b.getNumSamples(); ++i)
            if (std::abs (b.getSample (0, i)) > std::abs (b.getSample (0, best)))
                best = i;
        return best;
    }

    // Magnitude response (min/max in dB between 20 Hz and 20 kHz) of an impulse response.
    std::pair<float, float> magnitudeRange (const juce::AudioBuffer<float>& response, int start)
    {
        constexpr int order = 15, size = 1 << order;
        juce::dsp::FFT fft (order);
        std::vector<float> data ((size_t) size * 2, 0.0f);

        for (int i = 0; i < size && start + i < response.getNumSamples(); ++i)
            data[(size_t) i] = response.getSample (0, start + i);

        fft.performFrequencyOnlyForwardTransform (data.data());

        float lo = 1000.0f, hi = -1000.0f;
        for (int bin = 1; bin < size / 2; ++bin)
        {
            const double f = bin * testSampleRate / size;
            if (f < 20.0 || f > 20000.0)
                continue;
            const float mag = juce::Decibels::gainToDecibels (data[(size_t) bin], -200.0f);
            lo = std::min (lo, mag);
            hi = std::max (hi, mag);
        }
        return { lo, hi };
    }

    bool allFinite (const juce::AudioBuffer<float>& buffer, float limit = 64.0f)
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                const float v = buffer.getSample (ch, i);
                if (! std::isfinite (v) || std::abs (v) > limit)
                    return false;
            }
        return true;
    }

    //==============================================================================
    void testCurves()
    {
        std::cout << "Curves" << std::endl;

        Curve identity (CurveKind::level);
        expect (identity.isNeutral() && identity.gainAt (-30.0f) == 0.0f && identity.gainAt (-72.0f) == 0.0f,
                "neutral level curve has 0 dB gain everywhere, including the bottom-left corner");

        const auto comp = Curve::preset (CurveKind::level, 2);   // 4:1 above -24
        expect (std::abs (comp.gainAt (-6.0f) - (-13.5f)) < 0.01f, "4:1 curve: -6 dB in gives -13.5 dB gain");

        const auto gate = Curve::preset (CurveKind::level, 8);
        expect (gate.gainAt (-60.0f) == Curve::silenceDb && gate.gainAt (-80.0f) == Curve::silenceDb
                    && std::abs (gate.gainAt (-30.0f)) < 0.01f,
                "gate curve: silence on the floor (also below the graph), unity above the threshold");

        const auto stairs = Curve::preset (CurveKind::level, 10);
        expect (stairs.evaluate (-71.0f) == stairs.evaluate (-66.0f) && stairs.evaluate (0.0f) > stairs.evaluate (-30.0f),
                "stairs hold flat steps");

        const auto text = comp.toString();
        expect (Curve::fromString (CurveKind::level, text).toString() == text, "curve text round-trips");

        auto edited = Curve (CurveKind::level);
        const int index = edited.addPoint (-20.0f, -30.0f);
        edited.movePoint (index, 50.0f, 0.0f);   // clamped between neighbours
        expect (edited.getNumPoints() == 3 && edited.getPoints()[1].x < 12.0f && ! edited.isNeutral(), "points stay inside their neighbours");

        // Linear (Maximus-style) axes: the slope at the corner is the gain for quiet signals.
        const auto maximus = Curve::preset (CurveKind::level, 15);
        expect (maximus.isLinear() && std::abs (maximus.gainAt (-60.0f) - 10.6f) < 0.6f && std::abs (maximus.gainAt (0.0f)) < 0.01f,
                "linear Maximus-default curve: " + juce::String (maximus.gainAt (-60.0f), 2) + " dB at -60, "
                    + juce::String (maximus.gainAt (0.0f), 2) + " dB at 0 dBFS");
        expect (Curve (CurveKind::level).withScale (true).isNeutral() && Curve (CurveKind::level, true).withScale (false).isNeutral()
                    && Curve::fromString (CurveKind::level, maximus.toString()) == maximus,
                "identity stays neutral across scales; linear curves round-trip as text");

        const Curve flat (CurveKind::transient);
        expect (flat.getNumPoints() == 3 && flat.getPoints()[1].x == 0.0f && flat.isNeutral(),
                "a new transient curve is neutral with a point at 0, so attacks and tails edit separately");

        CurveTable table;
        table.bake (comp);
        expect (std::abs (table.lookup (-6.0f) - comp.gainAt (-6.0f)) < 0.02f && std::abs (table.lookup (-100.0f)) < 0.01f,
                "baked table matches the curve and holds its end values outside the graph");
    }

    void testTransparency()
    {
        std::cout << "Transparency and latency" << std::endl;
        auto owner = std::make_unique<DynMapProcessor>();
        auto& p = *owner;
        const auto noise = makeNoise (48000, 1);

        for (int quality = 0; quality < 4; ++quality)
        {
            resetAll (p);
            setParam (p, ids::quality, (float) quality);
            const auto out = run (p, noise);
            const int latency = p.getLatencySamples();
            const float diff = maxShiftedDifference (out, noise, latency);
            expect (diff == 0.0f, "init is bit-transparent at oversampling " + juce::String (quality)
                                      + " (latency " + juce::String (latency) + ")");
        }

        // Reported latency = where an impulse comes out, whatever adds delay.
        struct Case { const char* name; int quality; int lookahead; bool linear; int bands; };
        const Case cases[] { { "oversampling 4x", 2, 0, false, 1 },
                             { "input lookahead 5 ms", 1, 4, false, 1 },
                             { "band lookahead 2 ms", 1, 3, false, 3 },
                             { "linear-phase bands", 1, 0, true, 4 } };

        for (const auto& c : cases)
        {
            resetAll (p);
            setParam (p, ids::quality, (float) c.quality);
            setParam (p, ids::phase, c.linear ? (float) phaseLinear : (float) phaseMinimum);
            if (c.bands == 1)
                setStage (p, inputStage, ids::lookahead, (float) c.lookahead);
            for (int b = 1; b < c.bands; ++b)
                addBand (p, b, 100.0f * std::pow (4.0f, (float) b));
            if (c.bands > 1)
                setStage (p, bandStage (1), ids::lookahead, (float) c.lookahead);

            const auto out = run (p, makeImpulse (48000));
            const int measured = peakPosition (out) - 100;
            expect (measured == p.getLatencySamples(), juce::String ("latency matches (") + c.name + "): reported "
                                                         + juce::String (p.getLatencySamples()) + ", measured " + juce::String (measured));
        }
    }

    void testCrossovers()
    {
        std::cout << "Crossovers" << std::endl;
        auto owner = std::make_unique<DynMapProcessor>();
        auto& p = *owner;

        for (int slope = 0; slope < 4; ++slope)
        {
            for (int numBands : { 2, 5, 12 })
            {
                resetAll (p);
                setParam (p, ids::quality, 0.0f);
                for (int b = 1; b < numBands; ++b)
                    addBand (p, b, 40.0f * std::pow (400.0f, (float) (b - 1) / (float) juce::jmax (1, numBands - 2)), slope);

                const auto out = run (p, makeImpulse (1 << 16));
                const auto [lo, hi] = magnitudeRange (out, 100);
                expect (hi - lo < 0.02f, "minimum phase, " + juce::String (numBands) + " bands, slope " + juce::String (6 << slope)
                                             + " dB/oct: flat within " + juce::String (hi - lo, 4) + " dB");
            }
        }

        // Mixed slopes, slots enabled out of frequency order.
        {
            resetAll (p);
            setParam (p, ids::quality, 0.0f);
            addBand (p, 7, 300.0f, slope48);
            addBand (p, 2, 3000.0f, slope12);
            addBand (p, 11, 80.0f, slope6);
            const auto out = run (p, makeImpulse (1 << 16));
            const auto [lo, hi] = magnitudeRange (out, 100);
            expect (hi - lo < 0.02f, "mixed slopes, unordered slots: flat within " + juce::String (hi - lo, 4) + " dB");
            expect (p.engine.getLayout().slots[1] == 11 && p.engine.getLayout().slots[3] == 2, "bands sort by crossover frequency");
        }

        // Linear phase: the bands sum back to a clean delayed impulse.
        for (int slope : { slope6, slope48 })
        {
            resetAll (p);
            setParam (p, ids::quality, 0.0f);
            setParam (p, ids::phase, (float) phaseLinear);
            for (int b = 1; b < 6; ++b)
                addBand (p, b, 60.0f * std::pow (3.5f, (float) b), slope);

            const auto out = run (p, makeImpulse (48000));
            const int latency = p.getLatencySamples();
            float residual = 0.0f;
            for (int i = 0; i < out.getNumSamples(); ++i)
                if (i != 100 + latency)
                    residual = std::max (residual, std::abs (out.getSample (0, i)));

            expect (std::abs (out.getSample (0, 100 + latency) - 1.0f) < 1.0e-4f && residual < 1.0e-4f,
                    "linear phase (" + juce::String (6 << slope) + " dB/oct) sums to a delayed impulse, residual " + juce::String (residual, 6));
        }

        // Changing the layout in linear phase: the redesign arrives in the background and stays flat.
        {
            resetAll (p);
            setParam (p, ids::quality, 0.0f);
            setParam (p, ids::phase, (float) phaseLinear);
            addBand (p, 1, 500.0f);
            auto buffer = makeImpulse (4096);
            juce::MidiBuffer midi;
            p.prepareToPlay (testSampleRate, 512);
            addBand (p, 2, 4000.0f);

            for (int round = 0; round < 200 && (round < 2 || p.engine.isCrossoverDesignPending()); ++round)
            {
                juce::AudioBuffer<float> silence (2, 512);
                silence.clear();
                p.processBlock (silence, midi);
                juce::Thread::sleep (5);
            }

            // JUCE's convolution swaps the new filter in on its own background thread, then cross-fades
            // over 50 ms: give both real time before measuring.
            for (int round = 0; round < 40; ++round)
            {
                juce::AudioBuffer<float> silence (2, 512);
                silence.clear();
                p.processBlock (silence, midi);
                juce::Thread::sleep (10);
            }

            juce::AudioBuffer<float> response (2, 1 << 16);
            response.clear();
            response.setSample (0, 0, 1.0f);
            response.setSample (1, 0, 1.0f);
            for (int start = 0; start < response.getNumSamples(); start += 512)
            {
                juce::AudioBuffer<float> block (response.getArrayOfWritePointers(), 2, start, 512);
                p.processBlock (block, midi);
            }

            const auto [lo, hi] = magnitudeRange (response, 0);
            expect (! p.engine.isCrossoverDesignPending() && hi - lo < 0.02f,
                    "linear phase: new band designed in the background, flat within " + juce::String (hi - lo, 4) + " dB");
        }
    }

    void testCurvesInAction()
    {
        std::cout << "Dynamics" << std::endl;
        auto owner = std::make_unique<DynMapProcessor>();
        auto& p = *owner;
        const int n = 48000;
        const int tail = n / 2;

        // Static compression.
        resetAll (p);
        setParam (p, ids::quality, 0.0f);
        setCurve (p, inputStage, CurveKind::level, 2);   // 4:1 above -24
        {
            const float out = peakDb (run (p, makeSine (n, 1000.0f, -6.0f)), tail, n);
            expect (std::abs (out - (-19.5f)) < 0.5f, "4:1 above -24: -6 dB sine comes out at " + db (out) + " (expected -19.5)");
        }

        // Amount 0 = off; negative amount = inverted curve (expansion).
        setParam (p, ids::amount, 0.0f);
        {
            const auto in = makeSine (n, 1000.0f, -6.0f);
            const auto out = run (p, in);
            expect (maxShiftedDifference (out, in, p.getLatencySamples()) < 1.0e-6f, "amount 0 % leaves the signal untouched");
        }
        setParam (p, ids::amount, -100.0f);
        {
            const float out = peakDb (run (p, makeSine (n, 1000.0f, -6.0f)), tail, n);
            expect (std::abs (out - 7.5f) < 0.5f, "amount -100 % inverts 4:1 into expansion: " + db (out) + " (expected +7.5)");
        }
        setParam (p, ids::amount, 100.0f);

        // Upward compression.
        setCurve (p, inputStage, CurveKind::level, 4);   // 2:1 upward below -30
        {
            const float out = peakDb (run (p, makeSine (n, 1000.0f, -40.0f)), tail, n);
            expect (std::abs (out - (-35.0f)) < 0.5f, "upward 2:1 below -30: -40 dB sine comes out at " + db (out) + " (expected -35)");
        }

        // Gate: silence under the threshold, unity above.
        setCurve (p, inputStage, CurveKind::level, 8);
        {
            const float quiet = rmsDb (run (p, makeSine (n, 1000.0f, -60.0f)), tail, n);
            const float loud = peakDb (run (p, makeSine (n, 1000.0f, -20.0f)), tail, n);
            expect (quiet < -140.0f && std::abs (loud - (-20.0f)) < 0.1f,
                    "gate: -60 dB sine -> " + db (quiet) + ", -20 dB sine -> " + db (loud));
        }

        // Pre gain moves the signal along the curve, post gain is plain makeup.
        setCurve (p, inputStage, CurveKind::level, 2);
        setStage (p, inputStage, ids::pre, 6.0f);
        setStage (p, inputStage, ids::post, 3.0f);
        {
            const float out = peakDb (run (p, makeSine (n, 1000.0f, -6.0f)), tail, n);
            expect (std::abs (out - (-18.0f + 3.0f)) < 0.5f, "pre +6 / post +3 around 4:1: " + db (out) + " (expected -15)");
        }
        setStage (p, inputStage, ids::pre, 0.0f);
        setStage (p, inputStage, ids::post, 0.0f);

        // Max cut limits how far a curve can pull down.
        setStage (p, inputStage, ids::maxCut, 6.0f);
        {
            const float out = peakDb (run (p, makeSine (n, 1000.0f, -6.0f)), tail, n);
            expect (std::abs (out - (-12.0f)) < 0.5f, "max cut 6 dB caps the 4:1 reduction: " + db (out));
        }
        setStage (p, inputStage, ids::maxCut, 96.0f);

        // Attack lag creates a transient on a level step (the Maximus trick).
        resetAll (p);
        setParam (p, ids::quality, 0.0f);
        setCurve (p, inputStage, CurveKind::level, 2);
        setStage (p, inputStage, ids::attack, 20.0f);
        {
            auto step = makeSine (n, 500.0f, -40.0f);
            step.applyGain (n / 2, n / 2, juce::Decibels::decibelsToGain (34.0f));   // -40 -> -6 dB
            const auto out = run (p, step);
            const int at = n / 2 + p.getLatencySamples();
            const float onset = peakDb (out, at, at + 240), settled = peakDb (out, at + 9600, at + 14400);
            expect (onset - settled > 6.0f, "slow attack on a 4:1 curve forces a transient: onset " + db (onset)
                                                + ", settled " + db (settled));

            setStage (p, inputStage, ids::lookahead, 4.0f);
            setStage (p, inputStage, ids::attack, 1.0f);
            const auto caught = run (p, step);
            const int at2 = n / 2 + p.getLatencySamples();
            const float onset2 = peakDb (caught, at2 - 240, at2 + 240), settled2 = peakDb (caught, at2 + 9600, at2 + 14400);
            expect (onset2 - settled2 < 1.5f, "fast attack + lookahead catches the step: onset " + db (onset2) + ", settled " + db (settled2));
        }

        // Accelerating release (Maximus's REL curves): the envelope falls A * (t / release)^p dB after the
        // built-in 10 ms peak window. 4:1 above -24, -6 -> -30 dB drop, release 100 ms: 13.5 dB to recover.
        resetAll (p);
        setParam (p, ids::quality, 0.0f);
        setCurve (p, inputStage, CurveKind::level, 2);
        setStage (p, inputStage, ids::attack, 0.1f);
        setStage (p, inputStage, ids::release, 100.0f);
        {
            auto drop = makeSine (n, 1000.0f, -6.0f);
            drop.applyGain (n / 2, n / 2, juce::Decibels::decibelsToGain (-24.0f));
            auto reductionAt = [&] (const juce::AudioBuffer<float>& out, double ms)
            {
                const int at = n / 2 + p.getLatencySamples() + (int) (ms * 0.001 * testSampleRate);
                return -30.0f - peakDb (out, at - 48, at + 48);
            };

            setStage (p, inputStage, ids::relLaw, (float) ids::accelRelease (1));   // straight in dB, 35.5 dB per release time
            const auto straight = run (p, drop);
            const float early = reductionAt (straight, 30.0), late = reductionAt (straight, 80.0);
            expect (std::abs (early - 8.2f) < 1.5f && late < 0.5f, "release Accel 1 is straight in dB: reduction "
                                                                       + db (early) + " at 30 ms (expected 8.2), " + db (late) + " at 80 ms");

            setStage (p, inputStage, ids::relLaw, (float) ids::accelRelease (8));   // slow start: 27 * (t / release)^3.1 dB
            const auto slow = run (p, drop);
            const float held = reductionAt (slow, 60.0), done = reductionAt (slow, 130.0);
            expect (std::abs (held - 11.1f) < 1.5f && done < 0.5f, "release Accel 8 starts slowly: reduction "
                                                                       + db (held) + " at 60 ms (expected 11.1), " + db (done) + " at 130 ms");

            // Second release: quick first release, then the gain settles over REL 2.
            setStage (p, inputStage, ids::relLaw, (float) ids::relLawClassic);
            setStage (p, inputStage, ids::release, 10.0f);
            const float quick = reductionAt (run (p, drop), 60.0);
            setStage (p, inputStage, ids::release2, 300.0f);
            const float settling = reductionAt (run (p, drop), 60.0);
            expect (quick < 0.5f && settling > 2.0f && settling < 10.0f, "REL 2 keeps the gain settling after a quick release: "
                                                                             + db (settling) + " at 60 ms (" + db (quick) + " without)");
            setStage (p, inputStage, ids::release2, 0.0f);
        }

        // Eased attack: about half of the needed reduction lands at once, however slow the attack.
        resetAll (p);
        setParam (p, ids::quality, 0.0f);
        setCurve (p, inputStage, CurveKind::level, 2);   // 4:1 above -24: -6 dB needs 13.5 dB
        setStage (p, inputStage, ids::attack, 100.0f);
        {
            auto step = makeSine (n, 500.0f, -40.0f);
            step.applyGain (n / 2, n / 2, juce::Decibels::decibelsToGain (34.0f));
            auto overshoot = [&] (const juce::AudioBuffer<float>& out)
            {
                const int at = n / 2 + p.getLatencySamples();
                return peakDb (out, at + 96, at + 480) - peakDb (out, at + 19200, at + 23000);
            };

            const float classic = overshoot (run (p, step));
            setStage (p, inputStage, ids::attLaw, 2.0f);
            const float eased = overshoot (run (p, step));
            expect (classic > 11.0f && eased > 4.5f && eased < 9.0f, "eased attack lets about half through: overshoot "
                                                                         + db (eased) + " (classic " + db (classic) + ", needed 13.5)");
        }

        // Auto release: a short burst recovers fast, a long loud passage slowly.
        resetAll (p);
        setParam (p, ids::quality, 0.0f);
        setCurve (p, inputStage, CurveKind::level, 2);
        setStage (p, inputStage, ids::attack, 0.1f);
        setStage (p, inputStage, ids::release, 50.0f);
        setStage (p, inputStage, ids::relLaw, (float) ids::relLawAuto);
        {
            auto burst = [&] (double seconds)
            {
                auto b = makeSine (n, 1000.0f, -30.0f);
                const int from = n / 4, to = from + (int) (seconds * testSampleRate);
                b.applyGain (from, to - from, juce::Decibels::decibelsToGain (24.0f));
                const auto out = run (p, b);
                const int at = to + p.getLatencySamples() + (int) (0.15 * testSampleRate);
                return -30.0f - peakDb (out, at - 48, at + 48);   // reduction left 150 ms after the burst
            };

            const float shortBurst = burst (0.02), longBurst = burst (0.4);
            expect (shortBurst < 0.5f && longBurst > 2.0f, "auto release: " + db (shortBurst) + " left 150 ms after a 20 ms burst, "
                                                               + db (longBurst) + " after a 400 ms one");
        }
    }

    // Pink-ish noise (three one-pole-filtered layers) at a given RMS, for level experiments.
    juce::AudioBuffer<float> makePink (int numSamples, float rmsTargetDb, int seed = 3)
    {
        juce::Random random (seed);
        juce::AudioBuffer<float> b (2, numSamples);
        float s1 = 0, s2 = 0, s3 = 0;
        for (int i = 0; i < numSamples; ++i)
        {
            const float w = random.nextFloat() * 2.0f - 1.0f;
            s1 = 0.997f * s1 + 0.03f * w; s2 = 0.96f * s2 + 0.1f * w; s3 = 0.6f * s3 + 0.3f * w;
            const float v = s1 + s2 + s3 + 0.1f * w;
            b.setSample (0, i, v); b.setSample (1, i, v);
        }
        b.applyGain (juce::Decibels::decibelsToGain (rmsTargetDb - rmsDb (b, 0, numSamples)));
        return b;
    }

    // Per drum hit: peak of the first 3 ms against the RMS of the body (15-60 ms), and the RMS of
    // the tail (100-240 ms) against the body. Averaged in dB over the hits.
    struct DrumStats { float attack, tail; };

    DrumStats drumStats (const juce::AudioBuffer<float>& b, int latency)
    {
        const int period = (int) (0.25 * testSampleRate);
        auto ms = [] (double t) { return (int) (t * 0.001 * testSampleRate); };
        float attack = 0.0f, tail = 0.0f;
        int hits = 0;

        for (int start = period * 2 + latency; start + period <= b.getNumSamples(); start += period)
        {
            const float body = rmsDb (b, start + ms (15), start + ms (60));
            attack += peakDb (b, start, start + ms (3)) - body;
            tail += rmsDb (b, start + ms (100), start + ms (240)) - body;
            ++hits;
        }

        return { attack / (float) hits, tail / (float) hits };
    }

    void testTransients()
    {
        std::cout << "Transient curves" << std::endl;
        auto owner = std::make_unique<DynMapProcessor>();
        auto& p = *owner;
        const auto drums = makeDrums (48000 * 3);

        resetAll (p);
        setParam (p, ids::quality, 0.0f);
        const auto dryOut = run (p, drums);
        const auto dry = drumStats (dryOut, p.getLatencySamples());

        auto withCurve = [&] (int presetIndex)
        {
            resetAll (p);
            setParam (p, ids::quality, 0.0f);
            setCurve (p, inputStage, CurveKind::transient, presetIndex);
            const auto out = run (p, drums);
            return drumStats (out, p.getLatencySamples());
        };

        const auto punch = withCurve (1), soften = withCurve (3), tighten = withCurve (4), bloom = withCurve (5);

        expect (punch.attack > dry.attack + 3.0f, "Punch lifts the attack over the body: " + db (dry.attack) + " -> " + db (punch.attack));
        expect (soften.attack < dry.attack - 3.0f, "Soften lowers the attack: " + db (dry.attack) + " -> " + db (soften.attack));
        expect (tighten.tail < dry.tail - 3.0f, "Tighten cuts the tails: " + db (dry.tail) + " -> " + db (tighten.tail));
        expect (bloom.tail > dry.tail + 3.0f, "Bloom lifts the tails: " + db (dry.tail) + " -> " + db (bloom.tail));

        // OTT presets (fitted to the real OTT) bring any input to about one level and lift the
        // quiet tails.
        for (const char* name : { "OTT Style", "Extreme OTT" })
        {
            loadPresetNamed (p, name);
            const auto quiet = run (p, makePink (48000 * 3, -30.0f));
            const auto loud = run (p, makePink (48000 * 3, -8.0f));
            const float quietOut = rmsDb (quiet, 48000, 48000 * 3), loudOut = rmsDb (loud, 48000, 48000 * 3);
            const auto hits = drumStats (run (p, drums), p.getLatencySamples());

            expect (std::abs (quietOut - loudOut) < 3.0f && hits.tail > dry.tail + 2.0f,
                    juce::String (name) + ": -30 and -8 dB RMS in -> " + db (quietOut) + " / " + db (loudOut)
                        + " out, drum tails " + db (dry.tail) + " -> " + db (hits.tail));
        }

        loadPresetNamed (p, "Multiband Drum Punch");
        {
            const auto out = run (p, drums);
            const auto stats = drumStats (out, p.getLatencySamples());
            expect (stats.attack > dry.attack + 2.0f && stats.tail > dry.tail,
                    "Multiband Drum Punch: attack " + db (dry.attack) + " -> " + db (stats.attack) + ", tail " + db (dry.tail) + " -> " + db (stats.tail));
        }
    }

    void testShaperAndOutput()
    {
        std::cout << "Waveshaper, saturation and output" << std::endl;
        auto owner = std::make_unique<DynMapProcessor>();
        auto& p = *owner;

        // Waveshaper mode applies the drawn curve to each sample.
        resetAll (p);
        setParam (p, ids::quality, 0.0f);
        setStage (p, inputStage, ids::mode, (float) modeWaveshaper);
        const auto softClip = Curve::preset (CurveKind::level, 11);
        p.engine.curves.set (inputStage, CurveKind::level, softClip);
        {
            juce::AudioBuffer<float> ramp (2, 20000);
            for (int i = 0; i < ramp.getNumSamples(); ++i)
            {
                const float v = std::pow (10.0f, -3.0f + 3.5f * (float) i / (float) ramp.getNumSamples()) * (i % 2 == 0 ? 1.0f : -1.0f);
                ramp.setSample (0, i, v);
                ramp.setSample (1, i, v);
            }

            const auto out = run (p, ramp);
            const int latency = p.getLatencySamples();
            float worst = 0.0f;
            int worstAt = 0;
            for (int i = 2000; i + latency < out.getNumSamples(); ++i)
            {
                const float before = worst;
                const float x = ramp.getSample (0, i);
                const float expected = std::copysign (juce::Decibels::decibelsToGain (softClip.evaluate (juce::Decibels::gainToDecibels (std::abs (x)))), x);
                worst = std::max (worst, std::abs (juce::Decibels::gainToDecibels (std::abs (out.getSample (0, i + latency)))
                                                   - juce::Decibels::gainToDecibels (std::abs (expected))));
                if (worst > before) worstAt = i;
            }
            expect (worst < 0.1f, "waveshaper output follows the curve within " + juce::String (worst, 3) + " dB (worst at input "
                                      + db (juce::Decibels::gainToDecibels (std::abs (ramp.getSample (0, worstAt)))) + ")");
        }

        // Saturation types stay bounded and finite at any drive.
        for (int type = satTape; type <= satCrush; ++type)
        {
            resetAll (p);
            setStage (p, inputStage, ids::satType, (float) type);
            setStage (p, inputStage, ids::drive, 36.0f);
            const auto out = run (p, makeNoise (24000, 3, 1.0f));
            expect (allFinite (out, 4.0f), "saturation type " + juce::String (type) + " at full drive stays finite and bounded");
        }

        // Limiter holds the ceiling (sample and true peak) on a hot signal.
        resetAll (p);
        setParam (p, ids::limiter, 1.0f);
        setParam (p, ids::ceiling, -1.0f);
        setParam (p, ids::outGain, 12.0f);
        {
            const auto out = run (p, makeNoise (48000, 5, 0.8f));
            dsp::TruePeakDetector tp;
            float truePeak = 0.0f;
            for (int i = 0; i < out.getNumSamples(); ++i)
                truePeak = std::max (truePeak, tp.process (out.getSample (0, i)));

            const float samplePeak = peakDb (out, 0, out.getNumSamples());
            const float truePeakDb = juce::Decibels::gainToDecibels (truePeak);
            expect (samplePeak <= -1.0f + 1.0e-4f && truePeakDb < -0.8f,
                    "limiter at -1 dBTP with +12 dB drive: sample peak " + db (samplePeak) + ", true peak " + db (truePeakDb)
                        + ", deepest gain " + db (p.engine.meters.limiterGainDb.load()));
        }

        // Clipper + limiter at 4x keeps a loud master under the ceiling.
        setParam (p, ids::clip, (float) clipSoft);
        setParam (p, ids::quality, 2.0f);
        {
            const auto out = run (p, makeDrums (48000));
            expect (peakDb (out, 0, out.getNumSamples()) <= -1.0f + 1.0e-4f, "soft clip + limiter stays under the ceiling");
        }

        // Delta: only the difference comes out, so a neutral setup is silent, with bands too.
        resetAll (p);
        setParam (p, ids::delta, 1.0f);
        expect (peakDb (run (p, makeNoise (24000, 6)), 0, 24000) < -200.0f + 1.0f, "delta of a neutral setup is silent");
        for (int b = 1; b < 5; ++b)
            addBand (p, b, 60.0f * std::pow (4.0f, (float) b), b % 2 == 0 ? slope48 : slope12);
        {
            const auto out = run (p, makeNoise (24000, 6));
            const float residual = peakDb (out, 0, out.getNumSamples());
            expect (residual < -100.0f, "delta of a neutral 5-band setup is silent (" + db (residual) + ")");
        }

        // Global mix blends inside the stages: at 0 % heavy band compression disappears completely,
        // and the result is the same as a neutral multiband setup (no phase cancellation against
        // an unfiltered dry signal).
        {
            const auto in = makeDrums (48000);

            resetAll (p);
            for (int b = 1; b < 4; ++b)
                addBand (p, b, 100.0f * std::pow (5.0f, (float) b));
            const auto neutral = run (p, in);

            for (int b = 0; b < 4; ++b)
                setCurve (p, bandStage (b), CurveKind::level, 6);   // Smash
            setParam (p, ids::globalMix, 0.0f);
            const auto dryMix = run (p, in);

            float diff = 0.0f;
            for (int i = 0; i < in.getNumSamples(); ++i)
                diff = std::max (diff, std::abs (dryMix.getSample (0, i) - neutral.getSample (0, i)));
            expect (diff < 1.0e-6f, "global mix 0 % equals the neutral multiband signal (max diff " + juce::String (diff, 8) + ")");

            // Mix 50 % with the limiter on stays under the ceiling (the dry part is limited too).
            setParam (p, ids::globalMix, 50.0f);
            setParam (p, ids::limiter, 1.0f);
            setParam (p, ids::outGain, 12.0f);
            const auto half = run (p, in);
            expect (peakDb (half, 0, half.getNumSamples()) <= -0.5f + 1.0e-4f, "global mix 50 % + limiter stays under the ceiling");
        }

        // Auto gain matches loudness after heavy compression.
        resetAll (p);
        setCurve (p, inputStage, CurveKind::level, 6);   // Smash
        setParam (p, ids::autoGain, 1.0f);
        {
            const auto in = makeDrums (48000 * 8);
            const auto out = run (p, in);
            const int from = 48000 * 5, to = 48000 * 8;
            const float diff = rmsDb (out, from, to) - rmsDb (in, from, to);
            expect (std::abs (diff) < 2.0f, "auto gain brings the level back within 2 dB (" + db (diff) + ")");
        }

        // Low cut at the input: 12 dB/oct, leaves the mids alone.
        resetAll (p);
        setParam (p, ids::lowCut, 100.0f);
        {
            const float lows = peakDb (run (p, makeSine (48000, 25.0f, -6.0f)), 24000, 48000);
            const float mids = peakDb (run (p, makeSine (48000, 2000.0f, -6.0f)), 24000, 48000);
            expect (lows < -26.0f && std::abs (mids + 6.0f) < 0.05f, "low cut 100 Hz: 25 Hz comes out at " + db (lows)
                                                                          + ", 2 kHz at " + db (mids));
        }
        setParam (p, ids::lowCut, lowCutOffHz);

        // Width on the master stage (not only bands): 0 % folds the output to mono.
        setStage (p, masterStage, ids::width, 0.0f);
        {
            auto in = makeNoise (24000, 11);
            const auto out = run (p, in);
            float side = 0.0f;
            for (int i = 12000; i < 24000; ++i)
                side = juce::jmax (side, std::abs (out.getSample (0, i) - out.getSample (1, i)));
            expect (side < 1.0e-5f, "master width 0 % leaves no side signal (" + juce::String (side) + ")");
        }
    }

    void testBandsAndStereo()
    {
        std::cout << "Bands, sidechain and stereo" << std::endl;
        auto owner = std::make_unique<DynMapProcessor>();
        auto& p = *owner;
        const int n = 48000;

        // Mute / solo.
        resetAll (p);
        setParam (p, ids::quality, 0.0f);
        addBand (p, 1, 200.0f, slope48);
        addBand (p, 2, 5000.0f, slope48);
        setStage (p, bandStage (1), ids::mute, 1.0f);
        const float muted = rmsDb (run (p, makeSine (n, 1000.0f, -6.0f)), n / 2, n);
        setStage (p, bandStage (1), ids::mute, 0.0f);
        setStage (p, bandStage (0), ids::solo, 1.0f);
        const float soloedOther = rmsDb (run (p, makeSine (n, 1000.0f, -6.0f)), n / 2, n);
        expect (muted < -60.0f && soloedOther < -60.0f, "muting the 1 kHz band (" + db (muted) + ") or soloing another ("
                                                           + db (soloedOther) + ") removes a 1 kHz tone");

        // A band's curve only touches its own frequencies.
        resetAll (p);
        setParam (p, ids::quality, 0.0f);
        addBand (p, 1, 500.0f, slope48);
        setCurve (p, bandStage (1), CurveKind::level, 3);   // limit at -6 in the upper band
        const float low = peakDb (run (p, makeSine (n, 100.0f, -1.0f)), n / 2, n);
        const float high = peakDb (run (p, makeSine (n, 3000.0f, -1.0f)), n / 2, n);
        expect (std::abs (low - (-1.0f)) < 0.2f && std::abs (high - (-6.0f)) < 0.5f,
                "upper-band limiter: 100 Hz stays at " + db (low) + ", 3 kHz limited to " + db (high));

        // Sidechain ducking on the input stage.
        resetAll (p);
        setParam (p, ids::quality, 0.0f);
        setStage (p, inputStage, ids::scSource, (float) scExternal);
        p.engine.curves.set (inputStage, CurveKind::level, Curve::fromString (CurveKind::level, "-72,-72;-30,-30;0,-24;12,-12"));
        {
            const auto main = makeSine (n, 300.0f, -20.0f);
            auto kick = makeSine (n, 60.0f, -3.0f);
            kick.applyGain (0, n / 2, 0.0f);                    // sidechain silent in the first half
            const auto out = runWithSidechain (p, main, kick);
            const float before = peakDb (out, n / 4, n / 2 - 2000), during = peakDb (out, n * 3 / 4, n);
            expect (std::abs (before - (-20.0f)) < 0.2f && during < -35.0f,
                    "sidechain ducks the input: " + db (before) + " before, " + db (during) + " while the sidechain plays");
        }

        // Multiband sidechain: a 60 Hz kick on the sidechain ducks only the bass band ("same band"),
        // while a 5 kHz hat only ducks the bass when the band follows the whole sidechain.
        {
            const auto duck = Curve::fromString (CurveKind::level, "-72,-72;-40,-40;-10,-34;12,-12");   // -24 dB above -10

            auto twoTones = makeSine (n, 60.0f, -12.0f);
            const auto highTone = makeSine (n, 3000.0f, -12.0f);
            for (int ch = 0; ch < 2; ++ch)
                twoTones.addFrom (ch, 0, highTone, ch, 0, n);

            auto measure = [&] (const juce::AudioBuffer<float>& out, float freq)
            {
                // Level of one tone in the second half, via a simple correlation.
                double re = 0.0, im = 0.0;
                for (int i = n / 2; i < n; ++i)
                {
                    const double ph = juce::MathConstants<double>::twoPi * freq * i / testSampleRate;
                    re += out.getSample (0, i) * std::cos (ph);
                    im += out.getSample (0, i) * std::sin (ph);
                }
                return juce::Decibels::gainToDecibels ((float) (2.0 * std::sqrt (re * re + im * im) / (n / 2)), -200.0f);
            };

            for (int source : { scExternal, scExternalFull })
            {
                for (float trigger : { 60.0f, 5000.0f })
                {
                    resetAll (p);
                    setParam (p, ids::quality, 0.0f);
                    addBand (p, 1, 300.0f, slope48);
                    setStage (p, bandStage (0), ids::scSource, (float) source);
                    p.engine.curves.set (bandStage (0), CurveKind::level, duck);

                    const auto out = runWithSidechain (p, twoTones, makeSine (n, trigger, -3.0f));
                    const float bass = measure (out, 60.0f), top = measure (out, 3000.0f);
                    const bool shouldDuck = source == scExternalFull || trigger < 300.0f;

                    expect ((shouldDuck ? bass < -30.0f : std::abs (bass + 12.0f) < 0.5f) && std::abs (top + 12.0f) < 0.5f,
                            juce::String (source == scExternal ? "same-band" : "full-range") + " sidechain, "
                                + juce::String ((int) trigger) + " Hz trigger: bass " + db (bass) + ", top " + db (top));
                }
            }

            // Sidechain gain moves the trigger along the curve: -24 dB ducks much less.
            setParam (p, ids::scGain, -24.0f);
            const auto quieter = runWithSidechain (p, twoTones, makeSine (n, 60.0f, -3.0f));
            expect (measure (quieter, 60.0f) > -25.0f, "sidechain gain -24 dB ducks the bass less (" + db (measure (quieter, 60.0f)) + ")");

            // Listen plays the sidechain itself.
            setParam (p, ids::scGain, 0.0f);
            setParam (p, ids::scListen, 1.0f);
            const auto trigger = makeSine (n, 60.0f, -3.0f);
            const auto heard = runWithSidechain (p, twoTones, trigger);
            float diff = 0.0f;
            for (int i = 0; i < n; ++i)
                diff = std::max (diff, std::abs (heard.getSample (0, i) - trigger.getSample (0, i)));
            expect (diff < 1.0e-6f, "sidechain listen outputs the sidechain");
        }

        // The sidechain bus is on by default and reaches the engine through processBlock (inputs 3/4).
        {
            resetAll (p);
            setParam (p, ids::quality, 0.0f);
            setStage (p, inputStage, ids::scSource, (float) scExternal);
            p.engine.curves.set (inputStage, CurveKind::level, Curve::preset (CurveKind::level, 14));   // Duck

            const auto main = makeSine (n, 300.0f, -20.0f);
            const auto kick = makeSine (n, 60.0f, -3.0f);
            juce::AudioBuffer<float> buffer (4, n);
            for (int ch = 0; ch < 2; ++ch)
            {
                buffer.copyFrom (ch, 0, main, ch, 0, n);
                buffer.copyFrom (ch + 2, 0, kick, ch, 0, n);
            }

            p.prepareToPlay (testSampleRate, 512);
            juce::MidiBuffer midi;
            for (int start = 0; start < n; start += 512)
            {
                juce::AudioBuffer<float> block (buffer.getArrayOfWritePointers(), 4, start, juce::jmin (512, n - start));
                p.processBlock (block, midi);
            }

            const bool busOn = p.getBusCount (true) > 1 && p.getBus (true, 1)->isEnabled();
            expect (busOn && peakDb (buffer, n / 2, n) < -32.0f,
                    "sidechain bus on by default; a kick on inputs 3/4 ducks the input stage to " + db (peakDb (buffer, n / 2, n)));
        }

        // Mid/side processing with neutral curves is transparent.
        resetAll (p);
        setStage (p, inputStage, ids::stereo, (float) stereoMS);
        setCurve (p, inputStage, CurveKind::transient, 0);
        {
            const auto in = makeNoise (n, 8);
            const auto msOut = run (p, in);
            const float diff = maxShiftedDifference (msOut, in, p.getLatencySamples());
            expect (diff < 1.0e-6f, "mid/side mode is transparent with neutral curves (max diff " + juce::String (diff, 8) + ")");
        }

        // Width 0 in a band makes that band mono.
        resetAll (p);
        setParam (p, ids::quality, 0.0f);
        setStage (p, bandStage (0), ids::width, 0.0f);
        {
            const auto out = run (p, makeNoise (n, 9));
            float diff = 0.0f;
            for (int i = n / 2; i < n; ++i)
                diff = std::max (diff, std::abs (out.getSample (0, i) - out.getSample (1, i)));
            expect (diff < 1.0e-6f, "band width 0 % is mono");
        }
    }

    void testStateAndPresets()
    {
        std::cout << "State and presets" << std::endl << std::flush;
        auto owner = std::make_unique<DynMapProcessor>();
        auto& p = *owner;

        // Curve undo/redo, and a preset load clearing the history.
        {
            auto& curves = p.engine.curves;
            const auto original = curves.get (masterStage, CurveKind::level).toString();
            const auto edited = Curve::preset (CurveKind::level, 3);
            curves.set (masterStage, CurveKind::level, edited);
            p.curveUndo.record (masterStage, CurveKind::level, original, edited.toString());

            const int undone = p.curveUndo.undo (curves);
            const bool backToOriginal = curves.get (masterStage, CurveKind::level).toString() == original;
            p.curveUndo.redo (curves);
            const bool redone = curves.get (masterStage, CurveKind::level).toString() == edited.toString();
            loadPresetNamed (p, "Init");
            expect (undone == masterStage && backToOriginal && redone && ! p.curveUndo.canUndo() && ! p.curveUndo.canRedo(),
                    "curve undo restores, redo reapplies, loading a preset clears the history");
        }

        loadPresetNamed (p, "Loud Master");   // bands + curves
        juce::MemoryBlock state;
        p.getStateInformation (state);

        auto otherOwner = std::make_unique<DynMapProcessor>();
        auto& other = *otherOwner;
        other.setStateInformation (state.getData(), (int) state.getSize());

        bool same = true;
        for (auto* param : p.getParameters())
            same = same && std::abs (param->getValue() - other.getParameters()[param->getParameterIndex()]->getValue()) < 1.0e-6f;

        bool curvesSame = true;
        for (int stage = 0; stage < numStages; ++stage)
            for (auto kind : { CurveKind::level, CurveKind::transient })
                curvesSame = curvesSame && p.engine.curves.get (stage, kind).toString() == other.engine.curves.get (stage, kind).toString();

        expect (same && curvesSame && other.presets.getCurrentPresetName() == "Loud Master", "state save/restore round-trips parameters and curves");

        const auto names = p.presets.getPresetNames();
        for (int i = 0; i < p.presets.getNumFactoryPresets(); ++i)
        {
            p.presets.loadPreset (i);
            const auto out = run (p, makeDrums (48000));
            expect (allFinite (out, 16.0f), "preset \"" + names[i] + "\" runs clean (peak " + db (peakDb (out, 0, out.getNumSamples())) + ")");
        }

        // Random settings never blow up.
        juce::Random random (42);
        bool stable = true;
        for (int round = 0; round < 12; ++round)
        {
            for (auto* param : p.getParameters())
                if (! param->getName (64).contains ("Delta"))
                    param->setValueNotifyingHost (random.nextFloat());

            for (int stage = 0; stage < numStages; ++stage)
            {
                p.engine.curves.set (stage, CurveKind::level, Curve::preset (CurveKind::level, random.nextInt (13)));
                p.engine.curves.set (stage, CurveKind::transient, Curve::preset (CurveKind::transient, random.nextInt (7)));
            }

            stable = stable && allFinite (run (p, makeNoise (12000, round), 256), 64.0f);
        }
        expect (stable, "random parameters and curves stay finite");

        // Block size doesn't change the result.
        loadPresetNamed (p, "OTT Style");
        const auto noise = makeNoise (24000, 11);
        const auto a = run (p, noise, 64);
        const auto b = run (p, noise, 1024);
        float diff = 0.0f;
        for (int i = 0; i < noise.getNumSamples(); ++i)
            diff = std::max (diff, std::abs (a.getSample (0, i) - b.getSample (0, i)));
        expect (diff < 1.0e-5f, "block size doesn't change the output (max diff " + juce::String (diff, 8) + ")");
    }

    void bench()
    {
        std::cout << "CPU bench" << std::endl;
        auto owner = std::make_unique<DynMapProcessor>();
        auto& p = *owner;

        auto measure = [&p] (const juce::String& name)
        {
            const int seconds = 10;
            auto audio = makeDrums (48000 * seconds);
            juce::MidiBuffer midi;
            p.prepareToPlay (testSampleRate, 512);
            const auto start = juce::Time::getMillisecondCounterHiRes();

            for (int s = 0; s < audio.getNumSamples(); s += 512)
            {
                juce::AudioBuffer<float> block (audio.getArrayOfWritePointers(), 2, s, 512);
                p.processBlock (block, midi);
            }

            const double ms = juce::Time::getMillisecondCounterHiRes() - start;
            std::cout << "  " << name << ": " << juce::String (ms / (seconds * 10.0), 2) << " % of one core" << std::endl;
        };

        p.presets.loadPreset (0);
        measure ("Init");
        loadPresetNamed (p, "OTT Style");
        measure ("OTT Style (3 bands)");
        loadPresetNamed (p, "Multiband Drum Punch");
        measure ("Multiband Drum Punch (4 bands, limiter)");
        loadPresetNamed (p, "Loud Master");
        measure ("Loud Master (4 bands, 4x)");

        p.presets.resetToDefaults();
        for (int b = 1; b < maxBands; ++b)
            addBand (p, b, 30.0f * std::pow (1.8f, (float) b), slope48);
        for (int stage = 0; stage < numStages; ++stage)
        {
            setCurve (p, stage, CurveKind::level, 5);
            setCurve (p, stage, CurveKind::transient, 1);
            setStage (p, stage, ids::satType, (float) satTape);
        }
        setParam (p, ids::limiter, 1.0f);
        setParam (p, ids::clip, (float) clipSoft);
        measure ("12 bands, all stages active + saturation, 2x");
        setParam (p, ids::phase, (float) phaseLinear);
        measure ("same in linear phase");
    }

    void saveSnapshot (juce::Component& component, const juce::File& file)
    {
        const auto image = component.createComponentSnapshot (component.getLocalBounds(), true, 1.0f);
        file.deleteFile();
        juce::FileOutputStream stream (file);
        juce::PNGImageFormat png;
        expect (stream.openedOk() && png.writeImageToStream (image, stream), "wrote " + file.getFullPathName());
    }

    void testGui (const juce::File& folder)
    {
        std::cout << "GUI" << std::endl;
        folder.createDirectory();

        auto owner = std::make_unique<DynMapProcessor>();
        auto& p = *owner;
        p.prepareToPlay (testSampleRate, 512);

        {
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            expect (editor != nullptr, "editor opens");
            saveSnapshot (*editor, folder.getChildFile ("dynmap-init.png"));
        }

        p.presets.loadPreset (p.presets.getNumFactoryPresets() - 1);   // Multiband Mangle: 5 bands
        std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
        auto audio = makeDrums (4096);
        juce::MidiBuffer midi;

        for (int frame = 0; frame < 12; ++frame)
        {
            auto block = audio;
            p.processBlock (block, midi);
            p.engine.preSpectrum.process();
            p.engine.postSpectrum.process();
        }

        saveSnapshot (*editor, folder.getChildFile ("dynmap-active.png"));
        editor.reset();

        // Detector with every control shown.
        p.apvts.state.setProperty ("detectorAdvanced", true, nullptr);
        editor.reset (p.createEditor());
        saveSnapshot (*editor, folder.getChildFile ("dynmap-advanced.png"));
        editor.reset();

        // Linear (Maximus-style) level map on the master stage.
        loadPresetNamed (p, "Maximus Default");
        p.apvts.state.setProperty ("detectorAdvanced", false, nullptr);
        p.apvts.state.setProperty ("selectedStage", masterStage, nullptr);
        editor.reset (p.createEditor());
        saveSnapshot (*editor, folder.getChildFile ("dynmap-linear.png"));
        editor.reset();

        // History view of the master stage after a few seconds of drums.
        p.apvts.state.setProperty ("historyView", true, nullptr);
        editor.reset (p.createEditor());
        {
            dynmap::ui::HistoryView* view = nullptr;
            std::function<void (juce::Component&)> find = [&] (juce::Component& c)
            {
                if (auto* h = dynamic_cast<dynmap::ui::HistoryView*> (&c)) view = h;
                for (auto* child : c.getChildren()) find (*child);
            };
            find (*editor);
            expect (view != nullptr && view->isVisible(), "history view is shown when switched on");

            auto drums = makeDrums (48000 * 4);
            drums.applyGain (2.0f);
            juce::MidiBuffer noMidi;
            for (int start = 0; start + 512 <= drums.getNumSamples(); start += 512)
            {
                juce::AudioBuffer<float> block (drums.getArrayOfWritePointers(), 2, start, 512);
                p.processBlock (block, noMidi);
                if (view != nullptr && start % (512 * 32) == 0) view->pull();
            }
            if (view != nullptr) view->pull();
        }
        saveSnapshot (*editor, folder.getChildFile ("dynmap-history.png"));
        p.apvts.state.setProperty ("historyView", false, nullptr);
    }
}

// Writes the measurement kit.
int makeKit (const juce::File& file)
{
    const bool ok = measure::writeWav (measure::makeKit (48000.0), 48000.0, file);
    std::cout << (ok ? "Wrote " : "Could not write ") << file.getFullPathName() << std::endl;
    return ok ? 0 : 1;
}

// Measures a render of the kit made with another plugin and compares it with DynMap.
int compare (const juce::StringArray& args)
{
    const auto value = [&args] (const char* flag) { const int i = args.indexOf (flag); return i >= 0 && i + 1 < args.size() ? args[i + 1] : juce::String(); };

    juce::AudioBuffer<float> other;
    double sr = 48000.0;
    if (! measure::readAudio (juce::File (value ("--compare")), other, sr))
    {
        std::cout << "Could not read " << value ("--compare") << std::endl;
        return 1;
    }

    auto owner = std::make_unique<DynMapProcessor>();
    auto& p = *owner;
    const auto presetName = value ("--preset").isNotEmpty() ? value ("--preset") : juce::String ("Init");
    const int presetIndex = p.presets.getPresetNames().indexOf (presetName);

    if (presetIndex < 0)
    {
        std::cout << "No preset called \"" << presetName << "\". Presets: " << p.presets.getPresetNames().joinIntoString (", ") << std::endl;
        return 1;
    }

    p.presets.loadPreset (presetIndex);

    // Overrides for calibration: --set <param id>=<value>, --curve <stage>:<l|t>:<curve text>.
    for (int i = 0; i + 1 < args.size(); ++i)
    {
        if (args[i] == "--set")
        {
            const auto id = args[i + 1].upToFirstOccurrenceOf ("=", false, false);
            if (auto* param = p.apvts.getParameter (id))
                param->setValueNotifyingHost (param->convertTo0to1 (args[i + 1].fromFirstOccurrenceOf ("=", false, false).getFloatValue()));
            else
                std::cout << "Unknown parameter " << id << std::endl;
        }
        else if (args[i] == "--curve")
        {
            const auto parts = juce::StringArray::fromTokens (args[i + 1], ":", "");
            const auto kind = parts[1] == "t" ? CurveKind::transient : CurveKind::level;
            p.engine.curves.set (parts[0].getIntValue(), kind, Curve::fromString (kind, args[i + 1].fromLastOccurrenceOf (":", false, false)));
        }
    }

    auto ours = measure::makeKit (sr);
    juce::MidiBuffer midi;
    p.prepareToPlay (sr, 512);
    for (int start = 0; start < ours.getNumSamples(); start += 512)
    {
        juce::AudioBuffer<float> block (ours.getArrayOfWritePointers(), 2, start, juce::jmin (512, ours.getNumSamples() - start));
        p.processBlock (block, midi);
    }

    if (value ("--out").isNotEmpty())
        measure::writeWav (ours, sr, juce::File (value ("--out")));

    std::cout << "Render: " << value ("--compare") << " (" << sr << " Hz)   DynMap preset: " << presetName << std::endl;
    measure::printComparison (measure::analyse (other, sr), "other", measure::analyse (ours, sr), "DynMap");
    return 0;
}

int main (int argc, char* argv[])
{
    juce::StringArray args;
    for (int i = 1; i < argc; ++i)
        args.add (argv[i]);

    // The parameter tree uses timers, so a message manager is needed even without a GUI.
    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    if (args.contains ("--kit"))
        return makeKit (juce::File (args[args.indexOf ("--kit") + 1]));

    if (args.contains ("--compare"))
        return compare (args);

    if (args.contains ("--bench"))
    {
        bench();
        return 0;
    }

    // --only <group> runs a single group (curves, transparency, crossovers, dynamics, transients,
    // output, bands, state).
    const int onlyIndex = args.indexOf ("--only");
    const auto only = onlyIndex >= 0 && onlyIndex + 1 < args.size() ? args[onlyIndex + 1] : juce::String();
    auto wants = [&only] (const char* name) { return only.isEmpty() || only == name; };

    if (wants ("curves"))       testCurves();
    if (wants ("transparency")) testTransparency();
    if (wants ("crossovers"))   testCrossovers();
    if (wants ("dynamics"))     testCurvesInAction();
    if (wants ("transients"))   testTransients();
    if (wants ("output"))       testShaperAndOutput();
    if (wants ("bands"))        testBandsAndStereo();
    if (wants ("state"))        testStateAndPresets();

    if (! args.contains ("--no-gui"))
    {
        const int snapshotIndex = args.indexOf ("--snapshot");
        const auto folder = snapshotIndex >= 0 && snapshotIndex + 1 < args.size()
                                ? juce::File (args[snapshotIndex + 1])
                                : juce::File::getCurrentWorkingDirectory();
        testGui (folder);
    }

    std::cout << (failures == 0 ? "All tests passed" : juce::String (failures) + " test(s) failed") << std::endl;
    return failures == 0 ? 0 : 1;
}
