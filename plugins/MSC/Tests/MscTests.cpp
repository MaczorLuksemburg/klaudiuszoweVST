// Offline checks for MSC: DSP behaviour, presets and state, plus an optional
// screenshot of the editor.  Usage: MSC_Tests [--no-gui] [--snapshot <dir>]
#include "../Source/PluginEditor.h"
#include "../Source/PluginProcessor.h"
#include "KlaudTestWindow.h"

#include <iostream>

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

    void setParam (MscProcessor& p, const char* id, float value)
    {
        auto* param = p.apvts.getParameter (id);
        jassert (param != nullptr);
        param->setValueNotifyingHost (param->convertTo0to1 (value));
    }

    void resetAll (MscProcessor& p)
    {
        p.presets.loadPreset (0);   // Init: all defaults, all modules off
    }

    juce::AudioBuffer<float> makeNoise (int numSamples, int seed, bool mono = false)
    {
        juce::Random random (seed);
        juce::AudioBuffer<float> buffer (2, numSamples);

        for (int i = 0; i < numSamples; ++i)
        {
            const float l = (random.nextFloat() * 2.0f - 1.0f) * 0.5f;
            buffer.setSample (0, i, l);
            buffer.setSample (1, i, mono ? l : (random.nextFloat() * 2.0f - 1.0f) * 0.5f);
        }

        return buffer;
    }

    juce::AudioBuffer<float> run (MscProcessor& p, const juce::AudioBuffer<float>& input, int blockSize = 512)
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

    float maxDifference (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b, int skip = 0)
    {
        float diff = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = skip; i < a.getNumSamples(); ++i)
                diff = std::max (diff, std::abs (a.getSample (ch, i) - b.getSample (ch, i)));
        return diff;
    }

    bool allFinite (const juce::AudioBuffer<float>& buffer, float limit = 16.0f)
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

    void testDsp()
    {
        std::cout << "DSP" << std::endl;
        MscProcessor p;
        const auto noise = makeNoise (48000, 1);

        resetAll (p);
        expect (maxDifference (run (p, noise), noise) == 0.0f, "all modules off: output is bit-identical to input");

        // Band split sums back to the input for every shape and slope.
        for (int slope = 0; slope < 3; ++slope)
        {
            for (float shape : { 0.0f, 0.3f, 0.5f, 0.8f, 1.0f })
            {
                resetAll (p);
                setParam (p, msc::ids::inOn, 1.0f);
                setParam (p, msc::ids::inSlope, (float) slope);
                setParam (p, msc::ids::inShape, shape);
                setParam (p, msc::ids::inCutoff, 700.0f);
                const float diff = maxDifference (run (p, noise), noise);
                expect (diff < 1.0e-5f, "input split is transparent (slope " + juce::String (slope)
                                            + ", shape " + juce::String (shape) + "), max diff " + juce::String (diff));
            }
        }

        // Mono bass: low band summed to mono -> below the cutoff L and R agree.
        {
            resetAll (p);
            setParam (p, msc::ids::inOn, 1.0f);
            setParam (p, msc::ids::inShape, 1.0f);
            setParam (p, msc::ids::inCutoff, 2000.0f);
            setParam (p, msc::ids::inSlope, 2.0f);
            setParam (p, msc::ids::inDrySrc, (float) msc::sourceMono);

            juce::AudioBuffer<float> sine (2, 48000);
            for (int i = 0; i < sine.getNumSamples(); ++i)
            {
                const float s = std::sin (juce::MathConstants<float>::twoPi * 60.0f * (float) i / (float) testSampleRate);
                sine.setSample (0, i, s * 0.5f);
                sine.setSample (1, i, -s * 0.5f);   // fully out of phase: mono sum cancels
            }

            const auto out = run (p, sine);
            expect (out.getMagnitude (0, 24000, 24000) < 0.02f, "mono-summed low band cancels an out-of-phase 60 Hz tone");
        }

        // Haas: an impulse on the left comes out 10 ms later.
        {
            resetAll (p);
            setParam (p, msc::ids::hsOn, 1.0f);
            setParam (p, msc::ids::hsLeft, 10.0f);
            juce::AudioBuffer<float> impulse (2, 4800);
            impulse.clear();
            impulse.setSample (0, 100, 1.0f);
            const auto out = run (p, impulse);

            int peak = 0;
            for (int i = 0; i < out.getNumSamples(); ++i)
                if (std::abs (out.getSample (0, i)) > std::abs (out.getSample (0, peak)))
                    peak = i;

            expect (peak == 100 + 480, "Haas left 10 ms delays by 480 samples at 48 kHz (got " + juce::String (peak - 100) + ")");
        }

        // Haas polarity flip.
        {
            resetAll (p);
            setParam (p, msc::ids::hsOn, 1.0f);
            setParam (p, msc::ids::hsInvR, 1.0f);
            const auto in = makeNoise (4800, 2);
            const auto out = run (p, in);
            expect (std::abs (out.getSample (1, 4000) + in.getSample (1, 4000)) < 1.0e-6f, "Haas right polarity flip inverts the signal");
        }

        // Image: side 0 makes a mono output.
        {
            resetAll (p);
            setParam (p, msc::ids::imOn, 1.0f);
            setParam (p, msc::ids::imSide, 0.0f);
            const auto out = run (p, noise);
            float diff = 0.0f;
            for (int i = 2000; i < out.getNumSamples(); ++i)
                diff = std::max (diff, std::abs (out.getSample (0, i) - out.getSample (1, i)));
            expect (diff < 1.0e-6f, "image side 0 % gives identical channels");
        }

        // Chorus widens a mono source.
        {
            resetAll (p);
            setParam (p, msc::ids::chOn, 1.0f);
            const auto out = run (p, makeNoise (48000, 3, true));
            float diff = 0.0f;
            for (int i = 4800; i < out.getNumSamples(); ++i)
                diff = std::max (diff, std::abs (out.getSample (0, i) - out.getSample (1, i)));
            expect (diff > 0.01f && allFinite (out), "chorus creates stereo from a mono source");
        }

        // Chorus Rate: defaults to 1x (the Juno speed), so an untouched knob sounds exactly like before;
        // moving it changes the sweep.
        {
            auto* rate = p.apvts.getParameter (msc::ids::chRate);
            expect (rate != nullptr && std::abs (rate->convertFrom0to1 (rate->getDefaultValue()) - 1.0f) < 1.0e-6f,
                    "chorus rate defaults to 1x (Juno speed)");

            const auto mono = makeNoise (48000 * 4, 6, true);
            auto render = [&] (float rateValue)
            {
                resetAll (p);
                setParam (p, msc::ids::chOn, 1.0f);
                if (rateValue > 0.0f)
                    setParam (p, msc::ids::chRate, rateValue);
                return run (p, mono);
            };

            const auto juno = render (0.0f);   // untouched
            expect (maxDifference (render (1.0f), juno) == 0.0f, "chorus rate 1x is bit-identical to the untouched default");
            expect (maxDifference (render (2.0f), juno) > 0.01f, "chorus rate 2x changes the sweep");
        }

        // Dynamic pan is perfectly mono compatible: L + R equals the input's L + R for any
        // setting (100 % and 1000 %, with and without the input split).
        for (float maxPercent : { 100.0f, 1000.0f })
        {
            for (bool split : { false, true })
            {
                resetAll (p);
                setParam (p, msc::ids::inOn, split ? 1.0f : 0.0f);
                setParam (p, msc::ids::inCutoff, 1500.0f);
                setParam (p, msc::ids::dpOn, 1.0f);
                setParam (p, msc::ids::dpAmount, 1.0f);
                setParam (p, msc::ids::dpMax, maxPercent);
                const auto out = run (p, noise);

                float monoDiff = 0.0f, sideChange = 0.0f;
                for (int i = 0; i < out.getNumSamples(); ++i)
                {
                    const float inSum = noise.getSample (0, i) + noise.getSample (1, i);
                    const float outSum = out.getSample (0, i) + out.getSample (1, i);
                    monoDiff = std::max (monoDiff, std::abs (outSum - inSum));
                    sideChange = std::max (sideChange, std::abs ((out.getSample (0, i) - out.getSample (1, i))
                                                                 - (noise.getSample (0, i) - noise.getSample (1, i))));
                }

                expect (allFinite (out, 64.0f) && monoDiff < 1.0e-5f && sideChange > 0.1f,
                        "dynamic pan (" + juce::String (maxPercent, 0) + " %" + (split ? ", split on" : "")
                            + ") changes the side but leaves the mono sum identical (max diff " + juce::String (monoDiff) + ")");
            }
        }

        {
            resetAll (p);
            setParam (p, msc::ids::dpOn, 1.0f);
            expect (maxDifference (run (p, noise), noise) < 1.0e-6f, "dynamic pan at 0 % is transparent");
        }

        // Mod source: a silent right channel as the modulator leaves the audio untouched.
        {
            auto leftOnly = noise;
            leftOnly.clear (1, 0, leftOnly.getNumSamples());

            resetAll (p);
            setParam (p, msc::ids::dpOn, 1.0f);
            setParam (p, msc::ids::dpAmount, 0.5f);
            setParam (p, msc::ids::dpSource, (float) msc::modRight);
            expect (maxDifference (run (p, leftOnly), leftOnly) < 1.0e-6f, "mod source Right with a silent right channel does nothing");

            setParam (p, msc::ids::dpSource, (float) msc::modLeft);
            expect (maxDifference (run (p, leftOnly), leftOnly) > 0.01f, "mod source Left with the same input pans");
        }

        // The modulator reads the plugin input, not the input module's band: with only the
        // highs processed, a low-passed modulator (the bass) still pans them.
        {
            juce::AudioBuffer<float> mono (2, 48000);
            for (int i = 0; i < mono.getNumSamples(); ++i)
            {
                const float t = (float) i / (float) testSampleRate;
                const float s = 0.4f * std::sin (juce::MathConstants<float>::twoPi * 60.0f * t)
                              + 0.2f * std::sin (juce::MathConstants<float>::twoPi * 5000.0f * t);
                mono.setSample (0, i, s);
                mono.setSample (1, i, s);
            }

            resetAll (p);
            setParam (p, msc::ids::inOn, 1.0f);
            setParam (p, msc::ids::inShape, 1.0f);
            setParam (p, msc::ids::inCutoff, 1000.0f);
            setParam (p, msc::ids::dpOn, 1.0f);
            setParam (p, msc::ids::dpAmount, 0.5f);
            setParam (p, msc::ids::dpShape, 0.0f);
            setParam (p, msc::ids::dpCutoff, 200.0f);
            setParam (p, msc::ids::dpSlope, 1.0f);
            const auto out = run (p, mono);

            float width = 0.0f;
            for (int i = 4800; i < out.getNumSamples(); ++i)
                width = std::max (width, std::abs (out.getSample (0, i) - out.getSample (1, i)));

            expect (width > 0.05f, "bass in the plugin input pans the highs picked by the input module (L-R peak "
                                       + juce::String (width, 3) + ")");
        }

        // The modulator is levelled: pan depth doesn't depend on loudness, and p never passes Amount.
        {
            auto levelledPan = [&] (float gain, float& peak)
            {
                auto in = makeNoise (48000, 7, true);
                in.applyGain (gain);
                resetAll (p);
                setParam (p, msc::ids::dpOn, 1.0f);
                setParam (p, msc::ids::dpMax, 100.0f);
                setParam (p, msc::ids::dpAmount, 1.0f);
                setParam (p, msc::ids::dpThresh, -60.0f);
                const auto out = run (p, in);
                double sumSide = 0.0, sumMid = 0.0;
                peak = 0.0f;
                for (int i = 4800; i < in.getNumSamples(); ++i)
                {
                    const float mid = 0.5f * (out.getSample (0, i) + out.getSample (1, i));
                    const float side = 0.5f * (out.getSample (1, i) - out.getSample (0, i));
                    sumSide += std::abs (side);
                    sumMid += std::abs (mid);
                    if (std::abs (mid) > 1.0e-4f * gain)
                        peak = std::max (peak, std::abs (side / mid));
                }
                return (float) (sumSide / sumMid);
            };
            float quietPeak = 0.0f, loudPeak = 0.0f;
            const float quietPan = levelledPan (0.02f, quietPeak), loudPan = levelledPan (0.5f, loudPeak);
            expect (std::abs (quietPan / loudPan - 1.0f) < 0.05f && quietPan > 0.2f,
                    "dynamic pan moves a -34 dB and a -6 dB signal alike (mean |side/mid| " + juce::String (quietPan, 3) + " vs " + juce::String (loudPan, 3) + ")");
            expect (quietPeak <= 1.001f && loudPeak <= 1.001f,
                    "dynamic pan never passes Amount (peak p " + juce::String (quietPeak, 4) + ", " + juce::String (loudPeak, 4) + ")");

            // A steady tone reads a flat level (the hold spans its cycle), so the side is only the tone
            // times itself (DC and 2f): a rippling level would add 4f, 6f...
            {
                juce::AudioBuffer<float> tone (2, 48000);
                for (int i = 0; i < tone.getNumSamples(); ++i)
                    tone.setSample (0, i, 0.5f * std::sin (juce::MathConstants<float>::twoPi * 40.0f * (float) i / 48000.0f));
                tone.copyFrom (1, 0, tone, 0, 0, tone.getNumSamples());

                resetAll (p);
                setParam (p, msc::ids::dpOn, 1.0f);
                setParam (p, msc::ids::dpMax, 100.0f);
                setParam (p, msc::ids::dpAmount, 1.0f);
                const auto out = run (p, tone);
                auto sideAt = [&] (float hz)   // DFT magnitude over the last 0.5 s (20 whole cycles)
                {
                    double re = 0.0, im = 0.0;
                    for (int i = 24000; i < 48000; ++i)
                    {
                        const double side = 0.5 * (out.getSample (1, i) - out.getSample (0, i));
                        const double phase = juce::MathConstants<double>::twoPi * hz * i / 48000.0;
                        re += side * std::cos (phase);
                        im += side * std::sin (phase);
                    }
                    return std::hypot (re, im);
                };
                const double extra = juce::Decibels::gainToDecibels (std::max (sideAt (160.0f), sideAt (240.0f)) / sideAt (80.0f), -200.0);
                expect (extra < -80.0, "a steady 40 Hz tone gets no level ripple harmonics (4f/6f at " + juce::String (extra, 1) + " dB)");
            }

            // A kick-like hit: the body (20-150 ms after the hit) pans about as wide as the transient.
            {
                juce::Random random (9);
                juce::AudioBuffer<float> kicks (2, 96000);
                kicks.clear();
                for (int start = 0; start < 96000 - 19200; start += 24000)
                    for (int i = 0; i < 19200; ++i)
                    {
                        const float t = (float) i / 48000.0f;
                        const float v = 0.5f * std::sin (juce::MathConstants<float>::twoPi * 60.0f * t) * std::exp (-t / 0.15f)
                                        + (random.nextFloat() * 2.0f - 1.0f) * 1.7f * std::exp (-t / 0.003f);
                        kicks.setSample (0, start + i, 0.5f * v);
                    }
                kicks.copyFrom (1, 0, kicks, 0, 0, kicks.getNumSamples());

                resetAll (p);
                setParam (p, msc::ids::dpOn, 1.0f);
                setParam (p, msc::ids::dpMax, 100.0f);
                setParam (p, msc::ids::dpAmount, 1.0f);
                const auto out = run (p, kicks);
                auto meanPan = [&] (int from, int to)
                {
                    double sum = 0.0;
                    int count = 0;
                    for (int start = 24000; start < 96000 - 19200; start += 24000)
                        for (int i = start + from; i < start + to; ++i)
                        {
                            const float mid = kicks.getSample (0, i);
                            if (std::abs (mid) < 1.0e-3f) continue;
                            sum += std::abs (0.5f * (out.getSample (1, i) - out.getSample (0, i)) / mid);
                            ++count;
                        }
                    return (float) (sum / juce::jmax (1, count));
                };
                const float transient = meanPan (0, 240), body = meanPan (960, 7200);
                expect (body > 0.8f * transient, "a drum's body pans about as wide as its transient (mean p " + juce::String (transient, 3)
                                                      + " -> " + juce::String (body, 3) + ")");
            }

            // Below the floor it fades out.
            auto silentish = makeNoise (48000, 8, true);
            silentish.applyGain (0.0005f);   // about -66 dB
            resetAll (p);
            setParam (p, msc::ids::dpOn, 1.0f);
            setParam (p, msc::ids::dpAmount, 1.0f);
            setParam (p, msc::ids::dpThresh, -30.0f);
            const float below = maxDifference (run (p, silentish), silentish, 4800) / 0.0005f;
            expect (below < 0.2f, "a signal far below the floor barely pans (" + juce::String (below, 4) + ")");

            // The removed clipper and compressor parameters (kept for old projects) change nothing.
            resetAll (p);
            setParam (p, msc::ids::dpOn, 1.0f);
            setParam (p, msc::ids::dpAmount, 0.7f);
            const auto reference = run (p, noise);
            setParam (p, msc::ids::dpClip, (float) msc::clipHard);
            setParam (p, msc::ids::dpComp, 3.0f);
            setParam (p, msc::ids::dpMakeup, 4.0f);
            expect (maxDifference (run (p, noise), reference) == 0.0f, "old mod clip / comp / makeup settings are ignored");
        }

        // Dimension: only the side changes, so the mono sum is the input for any size and mix.
        for (float size : { 0.0f, 50.0f, 100.0f })
        {
            resetAll (p);
            setParam (p, msc::ids::dmOn, 1.0f);
            setParam (p, msc::ids::dmSize, size);
            setParam (p, msc::ids::dmMix, 100.0f);
            const auto out = run (p, noise);

            float monoDiff = 0.0f, sideChange = 0.0f;
            for (int i = 0; i < out.getNumSamples(); ++i)
            {
                monoDiff = std::max (monoDiff, std::abs ((out.getSample (0, i) + out.getSample (1, i))
                                                         - (noise.getSample (0, i) + noise.getSample (1, i))));
                sideChange = std::max (sideChange, std::abs ((out.getSample (0, i) - out.getSample (1, i))
                                                             - (noise.getSample (0, i) - noise.getSample (1, i))));
            }
            expect (monoDiff < 1.0e-5f && sideChange > 0.05f, "dimension (size " + juce::String (size)
                        + " %) widens but leaves the mono sum identical (max diff " + juce::String (monoDiff) + ")");
        }

        // Dimension taps: an impulse comes back in the side at the four measured delays, each with the
        // gain the LFO gives it at that moment (the phase starts at 0 on prepare).
        {
            resetAll (p);
            setParam (p, msc::ids::dmOn, 1.0f);
            setParam (p, msc::ids::dmSize, 50.0f);
            setParam (p, msc::ids::dmMix, 100.0f);

            constexpr int at = 24000;
            juce::AudioBuffer<float> impulse (2, 48000);
            impulse.clear();
            impulse.setSample (0, at, 1.0f);
            impulse.setSample (1, at, 1.0f);
            const auto out = run (p, impulse);

            namespace dm = msc::dsp::dimension;
            const double firstTap = dm::firstDelay (0.5f) * 0.001 * testSampleRate;
            bool tapsOk = true;
            juce::String detail;
            double tapEnergy = 0.0, totalEnergy = 0.0;
            for (int i = at + 1; i < out.getNumSamples(); ++i)
                totalEnergy += std::pow (0.5 * (out.getSample (0, i) - out.getSample (1, i)), 2.0);

            for (size_t k = 0; k < 4; ++k)
            {
                const double delay = firstTap * dm::delayRatio[k];
                const int centre = at + (int) std::lround (delay);
                const double cycles = (double) (centre + 1) * dm::lfoHz / testSampleRate;
                const float expected = dm::tapGains ((float) (juce::MathConstants<double>::twoPi * (cycles - std::floor (cycles))))[k];
                double sum = 0.0;
                for (int i = centre - 4; i <= centre + 4; ++i)
                {
                    const double side = 0.5 * (out.getSample (0, i) - out.getSample (1, i));
                    sum += side;
                    tapEnergy += side * side;
                }
                tapsOk = tapsOk && std::abs (sum - expected) < 0.01;
                detail << juce::String (delay / testSampleRate * 1000.0, 2) << " ms: " << juce::String (sum, 3)
                       << " (" << juce::String (expected, 3) << ")  ";
            }
            expect (tapsOk && tapEnergy > 0.999 * totalEnergy, "dimension taps at the measured delays and gains: " + detail);
        }

        // Dimension level: on noise the added side matches DimensionExpander's render (Size 50 %, 100 % wet:
        // side -3.47 dB re the input, measured in Reaper).
        {
            resetAll (p);
            setParam (p, msc::ids::dmOn, 1.0f);
            setParam (p, msc::ids::dmSize, 50.0f);
            setParam (p, msc::ids::dmMix, 100.0f);
            const auto mono = makeNoise (48000 * 22, 5, true);
            const auto out = run (p, mono);
            double side = 0.0, in = 0.0;
            for (int i = 48000; i < out.getNumSamples(); ++i)
            {
                side += std::pow (0.5 * (out.getSample (0, i) - out.getSample (1, i)), 2.0);
                in += std::pow ((double) mono.getSample (0, i), 2.0);
            }
            const double db = 10.0 * std::log10 (side / in);
            expect (std::abs (db + 3.47) < 0.3, "dimension side level " + juce::String (db, 2) + " dB (DimensionExpander: -3.47 dB)");

            setParam (p, msc::ids::dmMix, 0.0f);
            expect (maxDifference (run (p, noise), noise) < 1.0e-6f, "dimension at 0 % mix is transparent");
        }

        // Every factory preset, odd block sizes, mono-compatible input: finite output.
        for (int i = 0; i < p.presets.getNumFactoryPresets(); ++i)
        {
            p.presets.loadPreset (i);
            const auto out = run (p, noise, 333);
            expect (allFinite (out) && p.presets.getCurrentPresetName() == p.presets.getPresetNames()[i],
                    "preset '" + p.presets.getPresetNames()[i] + "' loads and renders cleanly");
        }

        // Switching modules on mid-stream fades in without a jump.
        {
            resetAll (p);
            juce::AudioBuffer<float> dc (2, 9600);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < dc.getNumSamples(); ++i)
                    dc.setSample (ch, i, 0.5f);

            juce::MidiBuffer midi;
            p.prepareToPlay (testSampleRate, 480);
            float maxStep = 0.0f, last = 0.5f;

            for (int start = 0; start < dc.getNumSamples(); start += 480)
            {
                if (start == 4800)
                {
                    setParam (p, msc::ids::imOn, 1.0f);
                    setParam (p, msc::ids::imMid, 0.0f);
                }

                juce::AudioBuffer<float> block (dc.getArrayOfWritePointers(), 2, start, 480);
                p.processBlock (block, midi);

                for (int i = 0; i < 480; ++i)
                {
                    maxStep = std::max (maxStep, std::abs (block.getSample (0, i) - last));
                    last = block.getSample (0, i);
                }
            }

            expect (maxStep < 0.01f, "turning a module on fades in smoothly (largest step " + juce::String (maxStep, 4) + ")");
        }

        // State round trip.
        {
            p.presets.loadPreset (3);
            juce::MemoryBlock state;
            p.getStateInformation (state);

            MscProcessor other;
            other.setStateInformation (state.getData(), (int) state.getSize());
            bool same = true;
            for (auto* param : p.getParameters())
                same = same && std::abs (param->getValue() - other.getParameters()[param->getParameterIndex()]->getValue()) < 1.0e-6f;

            expect (same && other.presets.getCurrentPresetName() == p.presets.getCurrentPresetName(), "state save/restore round-trips all parameters");
        }
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

        MscProcessor p;
        p.prepareToPlay (testSampleRate, 512);

        {
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            expect (editor != nullptr && editor->getWidth() == MscMainView::baseWidth, "editor opens at the base size");
            saveSnapshot (*editor, folder.getChildFile ("msc-init.png"));
        }

        // Dropdown menus open right under their button (top-left corner on the button's bottom-left), at the
        // base size and with the window scaled up.
        for (const float scale : { 1.0f, 1.25f })
        {
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            editor->setSize (juce::roundToInt (MscMainView::baseWidth * scale), juce::roundToInt (MscMainView::baseHeight * scale));
            editor->addToDesktop (0);
            editor->setTopLeftPosition (klaud::testWindowPosition());
            editor->setVisible (true);

            std::vector<juce::ComboBox*> boxes;
            std::function<void (juce::Component&)> collect = [&] (juce::Component& c)
            {
                if (auto* box = dynamic_cast<juce::ComboBox*> (&c))
                    boxes.push_back (box);
                for (auto* child : c.getChildren())
                    collect (*child);
            };
            collect (*editor);

            int worst = 0;
            juce::String worstName;
            for (auto* box : boxes)
            {
                const int before = juce::Desktop::getInstance().getNumComponents();
                box->showPopup();
                auto& desktop = juce::Desktop::getInstance();
                juce::Component* menu = desktop.getNumComponents() > before ? desktop.getComponent (desktop.getNumComponents() - 1) : nullptr;
                if (menu == nullptr)
                    continue;

                const auto boxArea = box->getScreenBounds();
                const auto menuArea = menu->getScreenBounds();
                // On a short screen JUCE flips a menu that doesn't fit below to just above the button.
                const auto screen = desktop.getDisplays().getDisplayForRect (boxArea)->userArea;
                const bool fitsBelow = boxArea.getBottom() + menuArea.getHeight() <= screen.getBottom();
                const int edge = fitsBelow ? menuArea.getY() - boxArea.getBottom() : menuArea.getBottom() - boxArea.getY();
                const int offset = std::max (std::abs (edge), std::abs (menuArea.getX() - boxArea.getX()));
                if (offset >= worst)
                {
                    worst = offset;
                    worstName = box->getTooltip().isNotEmpty() ? box->getTooltip().substring (0, 30) : box->getText();
                    worstName << " (button " << boxArea.toString() << ", menu " << menuArea.toString() << ")";
                }

                if (scale == 1.0f && box == boxes.front())
                    saveSnapshot (*menu, folder.getChildFile ("msc-menu.png"));

                juce::PopupMenu::dismissAllActiveMenus();
                box->hidePopup();
            }

            expect (! boxes.empty() && worst <= 1, "dropdown menus open under their button at " + juce::String (scale, 2)
                                                       + "x (largest offset " + juce::String (worst) + " px: " + worstName + ")");
            editor->removeFromDesktop();
        }

        // Everything on, with audio flowing so the spectrum displays have content.
        p.presets.loadPreset (1);
        for (const char* id : { msc::ids::dpOn, msc::ids::hsOn, msc::ids::chOn, msc::ids::dmOn })
            setParam (p, id, 1.0f);
        setParam (p, msc::ids::dpAmount, 0.6f);
        setParam (p, msc::ids::dpShape, 0.25f);
        setParam (p, msc::ids::dpCutoff, 600.0f);
        setParam (p, msc::ids::hsRight, 12.0f);

        std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
        auto audio = makeNoise (4096, 4);
        juce::MidiBuffer midi;

        for (int frame = 0; frame < 12; ++frame)
        {
            auto block = audio;
            p.processBlock (block, midi);
            p.inputAnalyzer.process();    // what the displays' timers would do
            p.dynPanAnalyzer.process();
            p.modScope.process();
        }

        saveSnapshot (*editor, folder.getChildFile ("msc-active.png"));
    }
}

int main (int argc, char* argv[])
{
    juce::StringArray args;
    for (int i = 1; i < argc; ++i)
        args.add (argv[i]);

    // The parameter tree uses timers, so a message manager is needed even without a GUI.
    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    testDsp();

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
