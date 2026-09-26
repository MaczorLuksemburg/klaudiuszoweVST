// Offline checks for MSC: DSP behaviour, presets and state, plus an optional
// screenshot of the editor.  Usage: MSC_Tests [--no-gui] [--snapshot <dir>]
#include "../Source/PluginEditor.h"
#include "../Source/PluginProcessor.h"

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

        // Dynamic pan: with any mod clip the balance law only turns a side down, never up.
        for (int clipMode : { msc::clipHard, msc::clipSoft, msc::clipExtreme })
        {
            resetAll (p);
            setParam (p, msc::ids::dpOn, 1.0f);
            setParam (p, msc::ids::dpAmount, 1.0f);
            setParam (p, msc::ids::dpMax, 1000.0f);
            setParam (p, msc::ids::dpComp, 3.0f);
            setParam (p, msc::ids::dpClip, (float) clipMode);
            const auto out = run (p, noise);

            bool neverLouder = allFinite (out);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < out.getNumSamples(); ++i)
                    neverLouder = neverLouder && std::abs (out.getSample (ch, i)) <= std::abs (noise.getSample (ch, i)) + 1.0e-6f;

            expect (neverLouder, "dynamic pan at 1000 % + 8:1 comp, mod clip " + juce::String (clipMode) + ": no sample gets louder");
        }

        {
            resetAll (p);
            setParam (p, msc::ids::dpOn, 1.0f);
            setParam (p, msc::ids::dpClip, (float) msc::clipOff);
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

        // Comp lifts quiet modulators: a -40 dB signal pans much harder with 8:1.
        {
            juce::AudioBuffer<float> quiet (noise);
            quiet.applyGain (0.02f);

            auto panAmount = [&] (float compIndex, float makeupIndex)
            {
                resetAll (p);
                setParam (p, msc::ids::dpOn, 1.0f);
                setParam (p, msc::ids::dpAmount, 0.5f);
                setParam (p, msc::ids::dpComp, compIndex);
                setParam (p, msc::ids::dpMakeup, makeupIndex);
                return maxDifference (run (p, quiet), quiet, 4800);
            };

            const float without = panAmount (0.0f, 4.0f), with = panAmount (3.0f, 4.0f);
            expect (with > without * 10.0f, "8:1 mod comp at 100 % makeup makes a quiet signal pan harder (" + juce::String (without, 5)
                                                + " -> " + juce::String (with, 5) + ")");

            float previous = 0.0f;
            bool increasing = true;
            for (int makeupIndex = 0; makeupIndex <= 4; ++makeupIndex)
            {
                const float amount = panAmount (3.0f, (float) makeupIndex);
                increasing = increasing && amount > previous;
                previous = amount;
            }
            expect (increasing, "more makeup (0 -> 100 %) pans a quiet signal progressively harder");
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

        // Everything on, with audio flowing so the spectrum displays have content.
        p.presets.loadPreset (1);
        for (const char* id : { msc::ids::dpOn, msc::ids::hsOn, msc::ids::chOn })
            setParam (p, id, 1.0f);
        setParam (p, msc::ids::dpAmount, 0.6f);
        setParam (p, msc::ids::dpComp, 2.0f);
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
