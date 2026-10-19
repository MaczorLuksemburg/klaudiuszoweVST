// Offline checks for WaveRef: loudness, A/B switching, reference playback and looping, level match,
// the mid/side spectrum, blind test and state, plus an optional screenshot of the editor.
// Usage: WaveRef_Tests [--no-gui] [--snapshot <dir>]
#include "../Source/PluginEditor.h"
#include "../Source/PluginProcessor.h"

#include <iostream>

using namespace waveref;

namespace
{
    constexpr double sr = 48000.0;
    int failures = 0;
    juce::File scratchFolder;   // where the editor keeps its settings during a test run

    void expect (bool condition, const juce::String& what)
    {
        std::cout << (condition ? "  ok    " : "  FAIL  ") << what << std::endl;
        if (! condition)
            ++failures;
    }

    void setParam (WaveRefProcessor& p, const char* id, float value)
    {
        auto* param = p.apvts.getParameter (id);
        jassert (param != nullptr);
        param->setValueNotifyingHost (param->convertTo0to1 (value));
    }

    juce::AudioBuffer<float> noise (int numSamples, float amplitude, int seed, bool mono = false)
    {
        juce::Random random (seed);
        juce::AudioBuffer<float> buffer (2, numSamples);

        for (int i = 0; i < numSamples; ++i)
        {
            const float l = (random.nextFloat() * 2.0f - 1.0f) * amplitude;
            buffer.setSample (0, i, l);
            buffer.setSample (1, i, mono ? l : (random.nextFloat() * 2.0f - 1.0f) * amplitude);
        }

        return buffer;
    }

    juce::AudioBuffer<float> sine (int numSamples, float freq, float amplitude, double rate = sr, int leftOnly = 0)
    {
        juce::AudioBuffer<float> buffer (2, numSamples);

        for (int i = 0; i < numSamples; ++i)
        {
            const float v = amplitude * std::sin (juce::MathConstants<float>::twoPi * freq * (float) i / (float) rate);
            buffer.setSample (0, i, v);
            buffer.setSample (1, i, leftOnly ? 0.0f : v);
        }

        return buffer;
    }

    juce::AudioBuffer<float> constant (int numSamples, float left, float right)
    {
        juce::AudioBuffer<float> buffer (2, numSamples);
        for (int i = 0; i < numSamples; ++i)
        {
            buffer.setSample (0, i, left);
            buffer.setSample (1, i, right);
        }
        return buffer;
    }

    // Runs the processor over `input` in blocks, returns what comes out.
    juce::AudioBuffer<float> run (WaveRefProcessor& p, const juce::AudioBuffer<float>& input, int blockSize = 480)
    {
        juce::AudioBuffer<float> output (input);
        juce::MidiBuffer midi;

        for (int start = 0; start < output.getNumSamples(); start += blockSize)
        {
            const int length = std::min (blockSize, output.getNumSamples() - start);
            juce::AudioBuffer<float> block (output.getArrayOfWritePointers(), 2, start, length);
            p.processBlock (block, midi);
        }

        return output;
    }

    float lufsOf (const juce::AudioBuffer<float>& buffer, int from, int to)
    {
        juce::AudioBuffer<float> part (2, to - from);
        for (int ch = 0; ch < 2; ++ch)
            part.copyFrom (ch, 0, buffer, ch, from, to - from);

        const auto energies = loudness::subBlockEnergies (part, sr);
        return loudness::gatedLoudness (energies.data(), (int) energies.size());
    }

    std::shared_ptr<TrackData> trackOf (juce::AudioBuffer<float> audio, const juce::String& name = "test")
    {
        return std::shared_ptr<TrackData> (makeTrack (std::move (audio), sr, name, {}));
    }

    void prepare (WaveRefProcessor& p)
    {
        p.prepareToPlay (sr, 480);
    }

    void pump (WaveRefProcessor& p, int times = 1)
    {
        for (int i = 0; i < times; ++i)
            p.analyser.process();
    }

    float peakFrequency (const std::vector<float>& curve, float fMax)
    {
        const auto it = std::max_element (curve.begin(), curve.end());
        const float t = (float) std::distance (curve.begin(), it) / (float) (curve.size() - 1);
        return 20.0f * std::pow (fMax / 20.0f, t);
    }

    //==========================================================================
    void testLoudness()
    {
        std::cout << "Loudness" << std::endl;

        // A 997 Hz sine at -20 dBFS in both channels is -20 LUFS.
        const auto tone = sine (48000 * 5, 997.0f, 0.1f);
        const float offline = lufsOf (tone, 0, tone.getNumSamples());
        expect (std::abs (offline - (-20.0f)) < 0.15f, "stereo -20 dBFS sine measures " + juce::String (offline, 2) + " LUFS");

        loudness::LiveMeter meter;
        meter.prepare (sr);
        meter.process (tone.getReadPointer (0), tone.getReadPointer (1), tone.getNumSamples());
        expect (std::abs (meter.getShortLufs() - offline) < 0.1f, "live short-term meter agrees with the offline measure");
        expect (std::abs (meter.getLongLufs() - offline) < 0.1f, "live long-term meter agrees too");

        // Silence around the tone must not pull the gated value down.
        juce::AudioBuffer<float> padded (2, 48000 * 6);
        padded.clear();
        for (int ch = 0; ch < 2; ++ch)
            padded.copyFrom (ch, 0, tone, ch, 0, 48000 * 2);

        const float gated = lufsOf (padded, 0, padded.getNumSamples());
        expect (std::abs (gated - offline) < 0.5f, "silence is gated out (" + juce::String (gated, 2) + " LUFS)");

        expect (std::abs (lufsOf (sine (48000 * 5, 997.0f, 0.2f), 0, 48000 * 5) - (offline + 6.02f)) < 0.1f,
                "doubling the amplitude adds 6 dB");
    }

    void testResampler()
    {
        std::cout << "Resampler" << std::endl;

        const double sourceRate = 44100.0;
        const auto source = sine (44100, 1000.0f, 0.5f, sourceRate);
        const auto out = resampleSinc (source, sourceRate, 48000.0);
        expect (std::abs (out.getNumSamples() - 48000) <= 1, "44.1 kHz becomes 48 kHz long (" + juce::String (out.getNumSamples()) + " samples)");

        float maxError = 0.0f;
        for (int i = 400; i < out.getNumSamples() - 400; ++i)
        {
            const float ideal = 0.5f * std::sin (juce::MathConstants<float>::twoPi * 1000.0f * (float) i / 48000.0f);
            maxError = std::max (maxError, std::abs (out.getSample (0, i) - ideal));
        }
        expect (maxError < 0.002f, "sine survives resampling up (error " + juce::String (maxError, 5) + ")");

        const auto down = resampleSinc (sine (96000, 1000.0f, 0.5f, 96000.0), 96000.0, 48000.0);
        maxError = 0.0f;
        for (int i = 400; i < down.getNumSamples() - 400; ++i)
        {
            const float ideal = 0.5f * std::sin (juce::MathConstants<float>::twoPi * 1000.0f * (float) i / 48000.0f);
            maxError = std::max (maxError, std::abs (down.getSample (0, i) - ideal));
        }
        expect (maxError < 0.002f, "sine survives resampling down (error " + juce::String (maxError, 5) + ")");

        // Above the new Nyquist frequency nothing may fold back in.
        const auto high = resampleSinc (sine (96000, 30000.0f, 0.5f, 96000.0), 96000.0, 48000.0);
        float peak = 0.0f;
        for (int i = 400; i < high.getNumSamples() - 400; ++i)
            peak = std::max (peak, std::abs (high.getSample (0, i)));
        expect (peak < 0.01f, "content above Nyquist is removed when downsampling (peak " + juce::String (peak, 4) + ")");
    }

    void testSwitching()
    {
        std::cout << "A/B" << std::endl;

        WaveRefProcessor p;
        prepare (p);

        // With A selected and no tricks on, the plugin is transparent.
        const auto input = noise (48000, 0.3f, 1);
        const auto through = run (p, input);
        float diff = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < input.getNumSamples(); ++i)
                diff = std::max (diff, std::abs (input.getSample (ch, i) - through.getSample (ch, i)));
        expect (diff == 0.0f, "A passes the mix bit-exact");

        // Switching between two DC levels: a short ramp, no jumps.
        WaveRefProcessor q;
        prepare (q);
        q.installTrack (0, trackOf (constant (48000, -0.5f, -0.5f)));
        setParam (q, ids::matchOn, 0.0f);

        run (q, constant (9600, 0.5f, 0.5f));            // settle on A
        setParam (q, ids::ab, 1.0f);
        const auto out = run (q, constant (9600, 0.5f, 0.5f));

        float maxStep = 0.0f;
        for (int i = 1; i < out.getNumSamples(); ++i)
            maxStep = std::max (maxStep, std::abs (out.getSample (0, i) - out.getSample (0, i - 1)));

        expect (maxStep < 0.01f, "A to B ramps instead of jumping (largest step " + juce::String (maxStep, 4) + ")");
        expect (std::abs (out.getSample (0, out.getNumSamples() - 1) - (-0.5f)) < 1.0e-4f, "B ends up on the reference");

        setParam (q, ids::ab, 0.0f);
        const auto back = run (q, constant (9600, 0.5f, 0.5f));
        expect (std::abs (back.getSample (0, back.getNumSamples() - 1) - 0.5f) < 1.0e-6f, "and A returns the mix exactly");

        // No reference loaded: B is silent rather than passing the mix.
        WaveRefProcessor empty;
        prepare (empty);
        setParam (empty, ids::ab, 1.0f);
        run (empty, constant (4800, 0.5f, 0.5f));
        const auto silent = run (empty, constant (4800, 0.5f, 0.5f));
        expect (std::abs (silent.getSample (0, 4000)) < 1.0e-6f, "B with an empty slot is silent");
    }

    void testPlayback()
    {
        std::cout << "Reference playback" << std::endl;

        WaveRefProcessor p;
        prepare (p);

        auto audio = noise(48000, 0.5f, 7);
        auto track = trackOf (audio);
        p.installTrack (0, track);
        p.setLoop (0, 0.2, 0.6, true);
        setParam (p, ids::matchOn, 0.0f);
        setParam (p, ids::ab, 1.0f);

        const int loopStart = (int) std::lround (0.2 * sr), loopLength = (int) std::lround (0.4 * sr);
        const auto out = run (p, constant (48000, 0.0f, 0.0f));

        int checked = 0, bad = 0;
        for (int i = 2000; i < out.getNumSamples(); ++i)
        {
            const int inLoop = i % loopLength;
            if (inLoop < 100 || inLoop > loopLength - 100)
                continue;   // the join between passes is cross-faded

            ++checked;
            if (std::abs (out.getSample (0, i) - audio.getSample (0, loopStart + inLoop)) > 1.0e-5f
                || std::abs (out.getSample (1, i) - audio.getSample (1, loopStart + inLoop)) > 1.0e-5f)
                ++bad;
        }

        expect (checked > 30000 && bad == 0, "the reference loops its region sample-exactly (" + juce::String (checked) + " samples checked)");

        // The join between passes stays smooth.
        float maxStep = 0.0f;
        const auto sineTrack = trackOf (sine (48000, 440.0f, 0.5f));
        WaveRefProcessor q;
        prepare (q);
        q.installTrack (0, sineTrack);
        q.setLoop (0, 0.1, 0.5, true);
        setParam (q, ids::matchOn, 0.0f);
        setParam (q, ids::ab, 1.0f);
        const auto sinOut = run (q, constant (48000, 0.0f, 0.0f));
        for (int i = 3000; i < sinOut.getNumSamples(); ++i)
            maxStep = std::max (maxStep, std::abs (sinOut.getSample (0, i) - sinOut.getSample (0, i - 1)));
        expect (maxStep < 0.12f, "loop wraps don't click (largest step " + juce::String (maxStep, 3) + ")");

        // Restart on play: the loop jumps back to its start when the host starts.
        expect (p.getPlayheadSlot() == 0 && p.getPlayhead() >= loopStart, "the playhead reports a position inside the loop");
    }

    void testMatch()
    {
        std::cout << "Loudness match" << std::endl;

        WaveRefProcessor p;
        prepare (p);
        p.installTrack (0, trackOf (noise (48000 * 4, 0.6f, 11)));

        const auto mix = noise (48000 * 8, 0.05f, 12);
        const float mixLufs = lufsOf (mix, 0, mix.getNumSamples());

        setParam (p, ids::matchOn, 1.0f);
        setParam (p, ids::ab, 1.0f);
        const auto matched = run (p, mix);
        const float refMatched = lufsOf (matched, 48000 * 5, 48000 * 8);
        expect (std::abs (refMatched - mixLufs) < 0.5f,
                "matched reference sits at the mix loudness (mix " + juce::String (mixLufs, 2) + ", ref " + juce::String (refMatched, 2) + " LUFS)");

        // Trim is applied on top.
        setParam (p, ids::trim, -6.0f);
        const auto trimmed = run (p, mix);
        const float refTrimmed = lufsOf (trimmed, 48000 * 6, 48000 * 8);
        expect (std::abs (refTrimmed - (mixLufs - 6.0f)) < 0.7f, "trim moves it from there (" + juce::String (refTrimmed - mixLufs, 2) + " dB)");

        // Without the match the reference plays at its own level.
        WaveRefProcessor q;
        prepare (q);
        auto loud = noise (48000 * 4, 0.6f, 11);
        q.installTrack (0, trackOf (loud));
        setParam (q, ids::matchOn, 0.0f);
        setParam (q, ids::ab, 1.0f);
        const auto raw = run (q, mix);
        const float expected = lufsOf (loud, 0, loud.getNumSamples());
        expect (std::abs (lufsOf (raw, 48000 * 5, 48000 * 8) - expected) < 0.3f, "match off leaves the reference level alone");

        // The loop's loudness is what counts: a loud half and a quiet half.
        juce::AudioBuffer<float> halves (2, 48000 * 6);
        for (int ch = 0; ch < 2; ++ch)
        {
            auto a = noise (48000 * 3, 0.6f, 20 + ch);
            auto b = noise (48000 * 3, 0.06f, 30 + ch);
            halves.copyFrom (ch, 0, a, 0, 0, 48000 * 3);
            halves.copyFrom (ch, 48000 * 3, b, 0, 0, 48000 * 3);
        }

        WaveRefProcessor r;
        prepare (r);
        r.installTrack (0, trackOf (halves));
        r.setLoop (0, 3.2, 5.8, true);
        setParam (r, ids::ab, 1.0f);
        const auto quiet = run (r, mix);
        expect (std::abs (lufsOf (quiet, 48000 * 5, 48000 * 8) - mixLufs) < 0.7f, "the match uses the loop region, not the whole file");
    }

    void testMonitoring()
    {
        std::cout << "Monitoring" << std::endl;

        WaveRefProcessor p;
        prepare (p);
        setParam (p, ids::mono, 1.0f);
        const auto out = run (p, constant (9600, 0.5f, -0.5f));
        expect (std::abs (out.getSample (0, 9000)) < 1.0e-6f && std::abs (out.getSample (1, 9000)) < 1.0e-6f, "mono fold-down cancels out-of-phase content");

        const auto centre = run (p, constant (9600, 0.4f, 0.4f));
        expect (std::abs (centre.getSample (0, 9000) - 0.4f) < 1.0e-6f, "and keeps centred content");

        // Listening band: 100 Hz is cut hard, 4 kHz passes, with the band at 4 kHz.
        WaveRefProcessor q;
        prepare (q);
        setParam (q, ids::filterOn, 1.0f);
        setParam (q, ids::filterFreq, 4000.0f);
        setParam (q, ids::filterWidth, 1.0f);

        auto level = [&] (float freq)
        {
            const auto o = run (q, sine (48000, freq, 0.5f));
            float peak = 0.0f;
            for (int i = 24000; i < o.getNumSamples(); ++i)
                peak = std::max (peak, std::abs (o.getSample (0, i)));
            return juce::Decibels::gainToDecibels (peak / 0.5f);
        };

        const float inBand = level (4000.0f), low = level (100.0f), high = level (16000.0f);
        expect (inBand > -6.0f, "the band passes its centre (" + juce::String (inBand, 1) + " dB)");
        expect (low < -30.0f && high < -20.0f, "and rejects outside it (" + juce::String (low, 1) + " / " + juce::String (high, 1) + " dB)");
    }

    void testSpectrum()
    {
        std::cout << "Spectrum" << std::endl;

        auto feed = [] (WaveRefProcessor& p, const juce::AudioBuffer<float>& mix)
        {
            p.analyser.setEnabled (true);
            p.analyser.setOrder (13);
            p.analyser.setAveragingSeconds (0.05f);
            p.analyser.reset();

            const int block = 480;
            for (int start = 0; start + block <= mix.getNumSamples(); start += block)
            {
                juce::AudioBuffer<float> b (2, block);
                for (int ch = 0; ch < 2; ++ch)
                    b.copyFrom (ch, 0, mix, ch, start, block);
                juce::MidiBuffer midi;
                p.processBlock (b, midi);

                if ((start / block) % 5 == 0)
                    p.analyser.process();
            }

            p.analyser.process();
        };

        auto curveOf = [] (WaveRefProcessor& p, int channel, float smoothing, float tilt, int points = 600)
        {
            std::vector<float> c ((size_t) points);
            p.analyser.getCurve (channel, points, 20.0f, 20000.0f, (int) smoothing, tilt, c.data());
            return c;
        };

        // A centred tone: all in mid, nothing in side, peak at the right frequency.
        {
            WaveRefProcessor p;
            prepare (p);
            feed (p, sine (48000 * 2, 1000.0f, 0.5f));
            const auto mid = curveOf (p, SpectrumAnalyser::mixMid, 0, 0);
            const auto side = curveOf (p, SpectrumAnalyser::mixSide, 0, 0);

            const float f = peakFrequency (mid, 20000.0f);
            expect (std::abs (f - 1000.0f) < 40.0f, "1 kHz tone peaks at " + juce::String (f, 0) + " Hz in mid");
            expect (*std::max_element (side.begin(), side.end()) < *std::max_element (mid.begin(), mid.end()) - 80.0f,
                    "a centred tone has no side");
        }

        // A tone in the left channel only: mid and side carry it equally.
        {
            WaveRefProcessor p;
            prepare (p);
            feed (p, sine (48000 * 2, 1000.0f, 0.5f, sr, 1));
            const auto midCurve = curveOf (p, SpectrumAnalyser::mixMid, 0, 0);
            const float mid = *std::max_element (midCurve.begin(), midCurve.end());
            const auto sideCurve = curveOf (p, SpectrumAnalyser::mixSide, 0, 0);
            const float side = *std::max_element (sideCurve.begin(), sideCurve.end());
            expect (std::abs (mid - side) < 0.5f, "a hard-left tone reads the same in mid and side (" + juce::String (mid - side, 2) + " dB)");
        }

        // White noise reads its variance, whatever the FFT size.
        {
            WaveRefProcessor p;
            prepare (p);
            const float amp = 0.3f;
            feed (p, noise (48000 * 6, amp, 5, true));
            const float expected = 10.0f * std::log10 (amp * amp / 3.0f);

            auto at = [&] (float freq)
            {
                const auto c = curveOf (p, SpectrumAnalyser::mixMid, 3, 0);
                const float t = std::log (freq / 20.0f) / std::log (1000.0f);
                return c[(size_t) juce::roundToInt (t * 599.0f)];
            };

            expect (std::abs (at (1000.0f) - expected) < 1.5f && std::abs (at (8000.0f) - expected) < 1.5f,
                    "white noise reads " + juce::String (at (1000.0f), 1) + " dB, expected " + juce::String (expected, 1));

            p.analyser.setOrder (11);
            feed (p, noise (48000 * 6, amp, 5, true));
            p.analyser.setOrder (11);
            const float small = at (1000.0f);
            expect (std::abs (small - expected) < 1.5f, "and the same with a 2048 FFT (" + juce::String (small, 1) + " dB)");
        }

        // Mix and reference are separate curves.
        {
            WaveRefProcessor p;
            prepare (p);
            p.installTrack (0, trackOf (sine (48000 * 2, 3000.0f, 0.5f)));
            setParam (p, ids::matchOn, 0.0f);
            feed (p, sine (48000 * 2, 500.0f, 0.5f));

            const float mixPeak = peakFrequency (curveOf (p, SpectrumAnalyser::mixMid, 0, 0), 20000.0f);
            const float refPeak = peakFrequency (curveOf (p, SpectrumAnalyser::refMid, 0, 0), 20000.0f);
            expect (std::abs (mixPeak - 500.0f) < 20.0f && std::abs (refPeak - 3000.0f) < 120.0f,
                    "mix peaks at " + juce::String (mixPeak, 0) + " Hz, reference at " + juce::String (refPeak, 0) + " Hz");
        }
    }

    // Time for the curve to reach half its final power after a noise burst starts.
    double stepTime (int order, float seconds)
    {
        SpectrumAnalyser a;
        a.setEnabled (true);
        a.setSampleRate (sr);
        a.setOrder (order);
        a.setAveragingSeconds (seconds);

        const int frame = 800;
        juce::AudioBuffer<float> silence (2, frame);
        silence.clear();

        for (int i = 0; i < 100; ++i)
        {
            a.push (false, silence.getReadPointer (0), silence.getReadPointer (1), frame);
            a.process();
        }

        const float amp = 0.3f;
        const float final = 10.0f * std::log10 (amp * amp / 3.0f);
        juce::Random random (3);
        std::vector<float> curve (100), noiseFrame ((size_t) frame);

        for (int i = 1; i < 2000; ++i)
        {
            for (auto& v : noiseFrame)
                v = (random.nextFloat() * 2.0f - 1.0f) * amp;

            a.push (false, noiseFrame.data(), noiseFrame.data(), frame);
            a.process();
            a.getCurve (SpectrumAnalyser::mixMid, 100, 20.0f, 20000.0f, 1, 0.0f, curve.data());

            if (curve[50] > final - 3.0f)   // index 50 is about 630 Hz
                return (double) i * frame / sr;
        }

        return 99.0;
    }

    void testSpeed()
    {
        std::cout << "Speed" << std::endl;

        // Up to what the window allows (32768 can't react faster than ~0.37 s), all sizes react alike.
        const double a11 = stepTime (11, 0.8f), a13 = stepTime (13, 0.8f), a15 = stepTime (15, 0.8f);
        expect (std::max ({ a11, a13, a15 }) / std::min ({ a11, a13, a15 }) < 1.25,
                "speed 0.80 s reacts in " + juce::String (a11, 2) + " / " + juce::String (a13, 2) + " / " + juce::String (a15, 2) + " s at 2048 / 8192 / 32768");

        const double b11 = stepTime (11, 0.2f), b13 = stepTime (13, 0.2f);
        expect (std::max (b11, b13) / std::min (b11, b13) < 1.25,
                "speed 0.20 s reacts in " + juce::String (b11, 2) + " / " + juce::String (b13, 2) + " s at 2048 / 8192");

        const double c15 = stepTime (15, 0.05f);
        expect (c15 < 0.5, "and the biggest size still reacts within " + juce::String (c15, 2) + " s at the fastest setting");
    }

    void testSettings()
    {
        std::cout << "Settings and presets" << std::endl;

        const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("WaveRefSettingsTest");
        folder.deleteRecursively();
        ui::UiSettings::setStorageFolder (folder);

        juce::ValueTree treeA ("UI");
        ui::UiSettings a (treeA);
        expect (a.top() == 0.0f && a.fftOrder() == 13 && ! a.diffView(), "a new instance starts from the defaults");

        a.set ("top", 9.0f);
        a.set ("diffView", true);
        a.setColour (2, juce::Colour (0xff112233));
        a.setVisible (1, false);
        a.set ("fftOrder", 15);

        // A second instance starts from what was used last.
        juce::ValueTree treeB ("UI");
        ui::UiSettings b (treeB);
        expect (b.top() == 9.0f && b.diffView() && b.fftOrder() == 15 && b.getColour (2) == juce::Colour (0xff112233) && ! b.isVisible (1),
                "a new instance starts from the last used settings");

        // Saving and loading a user preset.
        const auto saved = a.saveUserPreset ("My look");
        expect (saved == "My look" && ui::UiSettings::getUserPresetNames().contains ("My look"), "a preset can be saved under a name");

        a.loadPreset ("Default");
        expect (a.top() == 0.0f && ! a.diffView() && a.fftOrder() == 13 && a.getColour (2) == ui::UiSettings::getDefaultColour (2) && a.isVisible (1),
                "the Default factory preset puts everything back");

        expect (a.loadPreset ("My look") && a.top() == 9.0f && a.diffView() && a.fftOrder() == 15 && a.getColour (2) == juce::Colour (0xff112233) && ! a.isVisible (1),
                "and the saved preset brings it all back, colours included");

        a.loadPreset ("Default");
        a.setColour (0, juce::Colour (0xff445566));
        a.loadPreset ("EDM (+9 dB top)");
        expect (a.top() == 9.0f && a.getColour (0) == juce::Colour (0xff445566), "factory presets change only what they name");

        expect (a.saveUserPreset ("Default") == "Default (mine)", "a user preset can't take a factory name");
        expect (a.deleteUserPreset ("My look") && ! ui::UiSettings::getUserPresetNames().contains ("My look"), "presets can be deleted");

        ui::UiSettings::setStorageFolder (scratchFolder);
        folder.deleteRecursively();
    }

    // A host transport we control.
    struct TestPlayHead : public juce::AudioPlayHead
    {
        bool playing = true;
        double seconds = 0.0;
        bool beats = false;          // report tempo and beat position too
        double bpm = 120.0, ppq = 0.0;

        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo info;
            info.setIsPlaying (playing);
            info.setTimeInSeconds (seconds);
            info.setTimeInSamples ((int64_t) std::llround (seconds * sr));

            if (beats)
            {
                info.setBpm (bpm);
                info.setPpqPosition (ppq);
            }

            return info;
        }
    };

    // One block at a time, moving the host along with it.
    juce::AudioBuffer<float> runWithHost (WaveRefProcessor& p, TestPlayHead& host, const juce::AudioBuffer<float>& input, int blockSize = 480)
    {
        juce::AudioBuffer<float> output (input);
        juce::MidiBuffer midi;

        for (int start = 0; start < output.getNumSamples(); start += blockSize)
        {
            const int length = std::min (blockSize, output.getNumSamples() - start);
            juce::AudioBuffer<float> block (output.getArrayOfWritePointers(), 2, start, length);
            p.processBlock (block, midi);

            if (host.playing)
            {
                host.seconds += (double) length / sr;
                host.ppq += (double) length / sr * host.bpm / 60.0;
            }
        }

        return output;
    }

    bool matchesTrack (const juce::AudioBuffer<float>& out, const juce::AudioBuffer<float>& audio, int outFrom, int outTo, int64_t trackStart)
    {
        for (int i = outFrom; i < outTo; ++i)
            if (std::abs (out.getSample (0, i) - audio.getSample (0, (int) (trackStart + i))) > 1.0e-5f)
                return false;

        return true;
    }

    void testTransport()
    {
        std::cout << "Reference transport" << std::endl;

        const auto audio = noise (48000 * 4, 0.5f, 21);

        // Stop and play.
        {
            WaveRefProcessor p;
            prepare (p);
            p.installTrack (0, trackOf (audio));
            setParam (p, ids::matchOn, 0.0f);
            setParam (p, ids::ab, 1.0f);

            run (p, constant (4800, 0.0f, 0.0f));
            setParam (p, ids::refPlay, 0.0f);
            run (p, constant (960, 0.0f, 0.0f));
            const auto held = p.getPlayhead();
            const auto silent = run (p, constant (4800, 0.0f, 0.0f));
            expect (std::abs (silent.getSample (0, 4000)) < 1.0e-6f && p.getPlayhead() == held, "stopping silences the reference and holds the playhead");

            setParam (p, ids::refPlay, 1.0f);
            const auto resumed = run (p, constant (4800, 0.0f, 0.0f));
            expect (matchesTrack (resumed, audio, 600, 4800, held) && p.getPlayhead() == held + 4800, "playing resumes from where it stopped");
        }

        // Seek while playing and while stopped.
        {
            WaveRefProcessor p;
            prepare (p);
            p.installTrack (0, trackOf (audio));
            setParam (p, ids::matchOn, 0.0f);
            setParam (p, ids::ab, 1.0f);
            run (p, constant (4800, 0.0f, 0.0f));

            p.seekReference (2.0);
            const auto out = run (p, constant (4800, 0.0f, 0.0f));
            expect (matchesTrack (out, audio, 300, 4800, 96000), "a seek jumps the playing reference (cue style: the new position starts at full level)");

            setParam (p, ids::refPlay, 0.0f);
            run (p, constant (960, 0.0f, 0.0f));
            p.seekReference (3.0);
            const auto quiet = run (p, constant (960, 0.0f, 0.0f));
            expect (std::abs (quiet.getSample (0, 900)) < 1.0e-6f && p.getPlayhead() == 144000, "seeking while stopped moves the playhead and stays silent");
        }

        // Loop rules after a seek: before the loop it plays on into it; after it, it runs to the end and comes back.
        {
            WaveRefProcessor p;
            prepare (p);
            p.installTrack (0, trackOf (audio));
            p.setLoop (0, 1.0, 1.5, true);
            setParam (p, ids::matchOn, 0.0f);
            setParam (p, ids::ab, 1.0f);
            run (p, constant (4800, 0.0f, 0.0f));

            p.seekReference (0.2);
            run (p, constant (48000, 0.0f, 0.0f));
            const double early = p.getPlayheadSeconds();
            expect (std::abs (early - 1.2) < 0.02, "from before the loop it plays on into the loop (at " + juce::String (early, 2) + " s)");

            p.seekReference (3.6);
            run (p, constant (48000 * 3 / 2, 0.0f, 0.0f));
            const double late = p.getPlayheadSeconds();
            expect (std::abs (late - 1.1) < 0.02, "from after the loop it plays to the end, then returns to the loop (at " + juce::String (late, 2) + " s)");
        }
    }

    void testHostSync()
    {
        std::cout << "Host sync" << std::endl;

        const auto audio = noise (48000 * 6, 0.5f, 22);
        WaveRefProcessor p;
        prepare (p);
        p.installTrack (0, trackOf (audio));
        setParam (p, ids::matchOn, 0.0f);
        setParam (p, ids::ab, 1.0f);
        setParam (p, ids::refMode, 1.0f);
        setParam (p, ids::syncOffset, 1.0f);

        TestPlayHead host;
        p.setPlayHead (&host);
        host.seconds = 0.5;

        // Reference time = host time + offset: host at 0.5 s plays the file from 1.5 s.
        const auto out = runWithHost (p, host, constant (4800, 0.0f, 0.0f));
        expect (matchesTrack (out, audio, 300, 4800, 72000), "the reference plays at host time + offset, sample-exact");

        // The host jumps back: the reference follows.
        host.seconds = 0.2;
        const auto jumped = runWithHost (p, host, constant (4800, 0.0f, 0.0f));
        expect (matchesTrack (jumped, audio, 300, 4800, (int64_t) std::llround (1.2 * sr)), "a host jump moves the reference with it");

        // Host stopped: silence, and the playhead sits where the reference would be.
        host.playing = false;
        host.seconds = 3.0;
        runWithHost (p, host, constant (960, 0.0f, 0.0f));
        const auto stopped = runWithHost (p, host, constant (960, 0.0f, 0.0f));
        expect (std::abs (stopped.getSample (0, 900)) < 1.0e-6f && std::abs (p.getPlayheadSeconds() - 4.0) < 1.0e-3,
                "with the host stopped the reference is silent but its playhead follows");

        // Past the end of the file: silent.
        host.playing = true;
        host.seconds = 5.5;
        const auto past = runWithHost (p, host, constant (4800, 0.0f, 0.0f));
        expect (std::abs (past.getSample (0, 4000)) < 1.0e-6f, "beyond the end of the file the reference is silent");

        // Align: put the free-running reference where the host is.
        WaveRefProcessor q;
        prepare (q);
        q.installTrack (0, trackOf (audio));
        TestPlayHead h2;
        h2.playing = false;
        h2.seconds = 0.5;
        q.setPlayHead (&h2);
        setParam (q, ids::refPlay, 0.0f);
        run (q, constant (960, 0.0f, 0.0f));
        q.seekReference (2.0);
        run (q, constant (960, 0.0f, 0.0f));
        expect (q.alignToHost(), "align works when the host reports its time");
        expect (std::abs (q.apvts.getRawParameterValue (ids::syncOffset)->load() - 1.5f) < 0.01f
                    && (int) std::lround (q.apvts.getRawParameterValue (ids::refMode)->load()) == modeHostTime,
                "and it sets offset = reference position - host position, in host mode");
    }

    void testListening()
    {
        std::cout << "Listening modes" << std::endl;

        WaveRefProcessor p;
        prepare (p);
        setParam (p, ids::side, 1.0f);
        const auto side = run (p, constant (9600, 0.5f, -0.5f));
        expect (std::abs (side.getSample (0, 9000) - 0.5f) < 1.0e-6f && std::abs (side.getSample (1, 9000) - 0.5f) < 1.0e-6f, "side listening plays the side signal on both channels");
        const auto centre = run (p, constant (9600, 0.4f, 0.4f));
        expect (std::abs (centre.getSample (0, 9000)) < 1.0e-6f, "and nothing for centred content");

        setParam (p, ids::mono, 1.0f);
        const auto both = run (p, constant (9600, 0.5f, -0.5f));
        expect (std::abs (both.getSample (0, 9000) - 0.5f) < 1.0e-6f, "side wins when mono is on too");

        // Delta = mix - reference at the matched level.
        WaveRefProcessor d;
        prepare (d);
        d.installTrack (0, trackOf (constant (48000, 0.3f, 0.3f)));
        setParam (d, ids::matchOn, 0.0f);
        setParam (d, ids::delta, 1.0f);
        run (d, constant (4800, 0.5f, 0.5f));
        const auto delta = run (d, constant (4800, 0.5f, 0.5f));
        expect (std::abs (delta.getSample (0, 4000) - 0.2f) < 1.0e-4f, "delta plays mix minus reference (" + juce::String (delta.getSample (0, 4000), 3) + ")");
    }

    void testCues()
    {
        std::cout << "Cues" << std::endl;

        WaveRefProcessor p;
        prepare (p);
        p.installTrack (0, trackOf (noise (48000 * 4, 0.5f, 23)));
        p.setCue (0, 1, 1.25);
        p.setCue (0, 3, 3.5);
        p.setCue (0, 3, -1.0);

        juce::MemoryBlock state;
        p.getStateInformation (state);

        WaveRefProcessor q;
        q.setStateInformation (state.getData(), (int) state.getSize());
        const auto info = q.getSlotInfo (0);
        expect (std::abs (info.cues[1] - 1.25) < 1.0e-9 && info.cues[0] < 0.0 && info.cues[3] < 0.0, "cue points are saved with the project, and cleared ones stay cleared");
    }

    // A kick on every beat (louder on the first of each bar), hats between them: something a tempo detector can chew on.
    juce::AudioBuffer<float> kickTrack (double bpm, double firstBeat, double seconds)
    {
        const int length = (int) (seconds * sr);
        juce::AudioBuffer<float> audio (2, length);
        audio.clear();
        juce::Random random (31);
        const double beat = 60.0 / bpm;

        for (int k = 0; firstBeat + k * beat < seconds - 0.3; ++k)
        {
            const int start = (int) ((firstBeat + k * beat) * sr);
            const float level = k % 4 == 0 ? 0.9f : 0.6f;

            for (int i = 0; i < (int) (0.25 * sr) && start + i < length; ++i)
            {
                const double t = (double) i / sr;
                const double phase = juce::MathConstants<double>::twoPi * (50.0 * t + 110.0 * 0.03 * (1.0 - std::exp (-t / 0.03)));
                const float v = level * (float) (std::sin (phase) * std::exp (-t / 0.08));
                audio.addSample (0, start + i, v);
                audio.addSample (1, start + i, v);
            }

            const int hat = start + (int) (0.5 * beat * sr);

            for (int i = 0; i < (int) (0.02 * sr) && hat + i < length; ++i)
            {
                const float v = 0.12f * (random.nextFloat() * 2.0f - 1.0f) * (1.0f - (float) i / (0.02f * (float) sr));
                audio.addSample (0, hat + i, v);
                audio.addSample (1, hat + i, v);
            }
        }

        return audio;
    }

    // A short click on every beat.
    juce::AudioBuffer<float> clickTrack (double bpm, double firstBeat, double seconds)
    {
        const int length = (int) (seconds * sr);
        juce::AudioBuffer<float> audio (2, length);
        audio.clear();

        for (int k = 0; firstBeat + k * 60.0 / bpm < seconds - 0.1; ++k)
        {
            const int start = (int) std::llround ((firstBeat + k * 60.0 / bpm) * sr);

            for (int i = 0; i < 144; ++i)   // 3 ms
            {
                const float window = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / 144.0f);
                const float v = 0.8f * window * std::sin (juce::MathConstants<float>::twoPi * 2000.0f * (float) i / (float) sr);
                audio.setSample (0, start + i, v);
                audio.setSample (1, start + i, v);
            }
        }

        return audio;
    }

    // Times (seconds) where the signal first rises above a level after being quiet for a while.
    std::vector<double> clickTimes (const juce::AudioBuffer<float>& audio, float level = 0.12f)
    {
        std::vector<double> times;
        int last = -1000000;

        for (int i = 0; i < audio.getNumSamples(); ++i)
        {
            if (std::abs (audio.getSample (0, i)) > level && i - last > (int) (0.2 * sr))
                times.push_back ((double) i / sr);

            if (std::abs (audio.getSample (0, i)) > level)
                last = i;
        }

        return times;
    }

    void testTempoDetection()
    {
        std::cout << "Tempo detection" << std::endl;

        struct Case { double bpm, first; };

        for (auto c : { Case { 128.0, 0.37 }, Case { 140.0, 0.12 }, Case { 100.0, 0.5 }, Case { 90.0, 0.2 } })
        {
            const auto track = kickTrack (c.bpm, c.first, 40.0);
            const auto guess = detectTempo (track, sr);
            const double bar = 4.0 * 60.0 / c.bpm;
            const double wanted = std::fmod (c.first, bar);
            double error = std::abs (guess.downbeat - wanted);
            error = std::min (error, bar - error);

            expect (std::abs (guess.bpm - c.bpm) < 0.15 && error < 0.02,
                    juce::String (c.bpm, 0) + " BPM kick track: tempo " + juce::String (guess.bpm, 2) + ", bar start " + juce::String (guess.downbeat, 3)
                        + " s (wanted " + juce::String (wanted, 3) + ")");
        }

        const auto quiet = detectTempo (noise (48000 * 20, 0.0f, 1), sr);
        expect (quiet.bpm == 0.0, "silence has no tempo");
    }

    void testBeatSync()
    {
        std::cout << "Beat sync" << std::endl;

        const auto ref = clickTrack (120.0, 0.2, 24.0);

        // Same tempo: the grids line up exactly. Different tempo: stretched, and still on the host's beats.
        for (double hostBpm : { 120.0, 100.0, 150.0 })
        {
            WaveRefProcessor p;
            prepare (p);
            p.installTrack (0, trackOf (ref));
            p.setGrid (0, 120.0, 0.2);
            setParam (p, ids::matchOn, 0.0f);
            setParam (p, ids::ab, 1.0f);
            setParam (p, ids::refMode, 2.0f);

            TestPlayHead host;
            host.beats = true;
            host.bpm = hostBpm;
            p.setPlayHead (&host);

            const auto started = juce::Time::getMillisecondCounterHiRes();
            const auto out = runWithHost (p, host, constant (48000 * 9, 0.0f, 0.0f));
            const double cpu = (juce::Time::getMillisecondCounterHiRes() - started) / 9000.0;
            std::cout << "    (" << juce::String (cpu * 100.0, 1) << " % of real time on this machine)" << std::endl;
            const auto times = clickTimes (out);
            const double beat = 60.0 / hostBpm;

            // Skip the first second: the stretcher settles.
            int checked = 0;
            double worst = 0.0;
            for (double t : times)
            {
                if (t < 1.0)
                    continue;

                const double nearest = std::round (t / beat) * beat;
                worst = std::max (worst, std::abs (t - nearest));
                ++checked;
            }

            expect (checked >= 5 && worst < 0.008,
                    "reference at 120 BPM under a host at " + juce::String (hostBpm, 0) + " BPM: clicks within " + juce::String (worst * 1000.0, 1)
                        + " ms of the host beats (" + juce::String (checked) + " checked)");
        }

        // The beat offset moves the reference by whole beats.
        {
            WaveRefProcessor p;
            prepare (p);
            p.installTrack (0, trackOf (ref));
            p.setGrid (0, 120.0, 0.2);
            setParam (p, ids::matchOn, 0.0f);
            setParam (p, ids::ab, 1.0f);
            setParam (p, ids::refMode, 2.0f);
            setParam (p, ids::beatOffset, 3.0f);

            TestPlayHead host;
            host.beats = true;
            host.bpm = 120.0;
            p.setPlayHead (&host);
            const auto out = runWithHost (p, host, constant (48000 * 4, 0.0f, 0.0f));
            const auto times = clickTimes (out);
            expect (! times.empty() && std::abs (times.front() - 0.0) < 0.004, "a +3 beat offset: the first click is the reference's 4th beat, right at the host's beat 0");
        }
    }

    void testBlind()
    {
        std::cout << "Blind test" << std::endl;

        WaveRefProcessor p;
        prepare (p);
        p.installTrack (0, trackOf (constant (48000, -0.5f, -0.5f)));
        setParam (p, ids::matchOn, 0.0f);
        setParam (p, ids::blind, 1.0f);

        int heardRef = 0, heardMix = 0;

        for (int trial = 0; trial < 40; ++trial)
        {
            p.rerollBlind();
            setParam (p, ids::ab, 0.0f);   // X
            run (p, constant (4800, 0.5f, 0.5f));
            const auto x = run (p, constant (480, 0.5f, 0.5f));
            const bool xIsRef = x.getSample (0, 400) < 0.0f;
            expect (xIsRef == p.isXTheReference(), "X plays what isXTheReference() says");
            if (trial > 3)
                break;
        }

        // Over many re-rolls both outcomes happen.
        for (int i = 0; i < 200; ++i)
        {
            p.rerollBlind();
            (p.isXTheReference() ? heardRef : heardMix)++;
        }
        expect (heardRef > 60 && heardMix > 60, "X is the reference about half the time (" + juce::String (heardRef) + " / 200)");

        p.resetBlindScore();
        const bool xRef = p.isXTheReference();
        const bool right = p.submitGuess (xRef);
        expect (right && p.getBlindCorrect() == 1 && p.getBlindTotal() == 1, "a right guess scores");
        p.submitGuess (! p.isXTheReference() ? true : false);
        expect (p.getBlindTotal() == 2, "every guess counts");
    }

    // Loads run on a background thread; with no message loop here, wait for them and apply the results.
    void waitForLoads (WaveRefProcessor& p)
    {
        for (int i = 0; i < 500 && p.hasLoadsInFlight(); ++i)
            juce::Thread::sleep (10);

        p.drainFinishedLoads();
    }

    void writeWav (const juce::File& file, const juce::AudioBuffer<float>& audio, double rate)
    {
        file.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (audio.getNumChannels()));

        if (writer != nullptr)
            writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    }

    void testFilesAndState()
    {
        std::cout << "Files and state" << std::endl;

        const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("WaveRefTests");
        folder.createDirectory();
        const auto wavFile = folder.getChildFile ("tone.wav");
        const auto oddRate = folder.getChildFile ("odd.wav");
        writeWav (wavFile, sine (48000 * 3, 1000.0f, 0.3f), sr);
        writeWav (oddRate, sine (44100 * 2, 1000.0f, 0.3f, 44100.0), 44100.0);

        WaveRefProcessor p;
        prepare (p);

        juce::String error;
        expect (p.loadFileBlocking (2, wavFile, &error), "a WAV file loads " + error);
        auto track = p.getTrack (2);
        expect (track != nullptr && std::abs (track->getSeconds() - 3.0) < 0.01 && track->name == "tone", "with its length and name");

        expect (p.loadFileBlocking (3, oddRate, &error) && std::abs (p.getTrack (3)->getSeconds() - 2.0) < 0.01,
                "a 44.1 kHz file is converted to the host rate and keeps its duration");

        juce::String badError;
        expect (! p.loadFileBlocking (4, folder.getChildFile ("missing.wav"), &badError) && badError.isNotEmpty(),
                "a missing file reports an error: " + badError);
        expect (p.getTrack (4) != nullptr && p.getTrack (4)->isEmpty() && p.getSlotInfo (4).path.isNotEmpty(),
                "and leaves the slot empty but remembers the path");

        p.setLoop (2, 0.5, 1.5, true);
        setParam (p, ids::slot, 2.0f);
        setParam (p, ids::trim, -3.0f);

        juce::MemoryBlock state;
        p.getStateInformation (state);

        // A new instance restores settings, paths and loops, then loads the audio in the background.
        {
            WaveRefProcessor q;
            q.setStateInformation (state.getData(), (int) state.getSize());
            q.prepareToPlay (sr, 480);

            waitForLoads (q);

            const auto info = q.getSlotInfo (2);
            expect (info.path == wavFile.getFullPathName() && info.loopOn && std::abs (info.loopStart - 0.5) < 1.0e-6 && std::abs (info.loopEnd - 1.5) < 1.0e-6,
                    "paths and loops come back");
            expect (q.getTrack (2) != nullptr && ! q.getTrack (2)->isEmpty(), "the audio is decoded again");
            expect (std::abs (q.apvts.getRawParameterValue (ids::trim)->load() - (-3.0f)) < 1.0e-4f
                        && std::abs (q.apvts.getRawParameterValue (ids::slot)->load() - 2.0f) < 1.0e-4f,
                    "parameters come back");
        }

        // A different host sample rate re-decodes the slots.
        {
            WaveRefProcessor q;
            q.setStateInformation (state.getData(), (int) state.getSize());
            q.prepareToPlay (44100.0, 480);

            waitForLoads (q);

            const auto t = q.getTrack (2);
            expect (t != nullptr && std::abs (t->sampleRate - 44100.0) < 0.1 && std::abs (t->getSeconds() - 3.0) < 0.01,
                    "at 44.1 kHz the track is converted again");
        }

        p.clearSlot (2);
        expect (p.getTrack (2)->isEmpty() && p.getSlotInfo (2).path.isEmpty(), "clearing a slot empties it");
        folder.deleteRecursively();
    }

    void testDsp()
    {
        testLoudness();
        testResampler();
        testSwitching();
        testPlayback();
        testMatch();
        testMonitoring();
        testSpectrum();
        testSpeed();
        testSettings();
        testTransport();
        testHostSync();
        testListening();
        testCues();
        testTempoDetection();
        testBeatSync();
        testBlind();
        testFilesAndState();
    }

    //==========================================================================
    void saveSnapshot (juce::Component& component, const juce::File& file)
    {
        const auto image = component.createComponentSnapshot (component.getLocalBounds(), true, 1.0f);
        file.deleteFile();
        juce::FileOutputStream stream (file);
        juce::PNGImageFormat png;
        expect (stream.openedOk() && png.writeImageToStream (image, stream), "wrote " + file.getFullPathName());
    }

    // Pink-ish noise with a spectral tilt and some stereo width, so the curves have something to show.
    juce::AudioBuffer<float> musicLike (int numSamples, int seed, float brightness, float width)
    {
        juce::Random random (seed);
        juce::AudioBuffer<float> buffer (2, numSamples);
        float lowL = 0.0f, lowR = 0.0f, shape1 = 0.0f, shape2 = 0.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            const float common = random.nextFloat() * 2.0f - 1.0f;
            const float sideNoise = (random.nextFloat() * 2.0f - 1.0f) * width;
            const float l = common + sideNoise, r = common - sideNoise;

            lowL += 0.04f * (l - lowL);
            lowR += 0.04f * (r - lowR);
            shape1 += 0.3f * (lowL - shape1);
            shape2 += 0.3f * (lowR - shape2);

            const float bass = 0.5f * std::sin (juce::MathConstants<float>::twoPi * 60.0f * (float) i / (float) sr);
            buffer.setSample (0, i, 0.6f * shape1 + brightness * 0.12f * l + bass);
            buffer.setSample (1, i, 0.6f * shape2 + brightness * 0.12f * r + bass);
        }

        return buffer;
    }

    void testGui (const juce::File& folder)
    {
        std::cout << "GUI" << std::endl;
        folder.createDirectory();

        WaveRefProcessor p;
        prepare (p);

        {
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            expect (editor != nullptr && editor->getWidth() == WaveRefMainView::baseWidth, "editor opens at the base size");
            saveSnapshot (*editor, folder.getChildFile ("waveref-empty.png"));
        }

        p.installTrack (0, trackOf (musicLike (48000 * 40, 3, 1.2f, 0.5f), "Reference track"));
        p.installTrack (1, trackOf (musicLike (48000 * 20, 4, 0.5f, 0.1f), "Another mix"));
        p.setLoop (0, 8.0, 24.0, true);
        p.setCue (0, 0, 9.0);
        p.setCue (0, 2, 20.0);
        p.setGrid (0, 124.0, 0.31);
        setParam (p, ids::filterOn, 1.0f);
        setParam (p, ids::filterFreq, 900.0f);
        setParam (p, ids::filterWidth, 1.4f);
        setParam (p, ids::slot, 0.0f);
        setParam (p, ids::ab, 0.0f);

        std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
        auto mix = musicLike (48000 * 4, 9, 0.8f, 0.25f);

        for (int frame = 0; frame < 14; ++frame)
        {
            auto block = mix;
            juce::MidiBuffer midi;
            for (int start = 0; start + 480 <= block.getNumSamples(); start += 480)
            {
                juce::AudioBuffer<float> b (block.getArrayOfWritePointers(), 2, start, 480);
                p.processBlock (b, midi);
            }

            for (int i = 0; i < 6; ++i)
                p.analyser.process();
        }

        // What the editor's timers would do.
        static_cast<WaveRefEditor&> (*editor).refresh();
        saveSnapshot (*editor, folder.getChildFile ("waveref-active.png"));

        // Difference view with a raised top.
        {
            auto& view = static_cast<WaveRefEditor&> (*editor);
            ui::UiSettings settings (p.getUiState());
            settings.set ("diffView", true);
            view.refresh();
            saveSnapshot (*editor, folder.getChildFile ("waveref-diff.png"));
            settings.set ("diffView", false);
            settings.set ("top", 9.0f);
            view.refresh();
            saveSnapshot (*editor, folder.getChildFile ("waveref-top9.png"));
            settings.set ("top", 0.0f);
        }

        setParam (p, ids::blind, 1.0f);
        static_cast<WaveRefEditor&> (*editor).refresh();
        saveSnapshot (*editor, folder.getChildFile ("waveref-blind.png"));
    }
}

int main (int argc, char* argv[])
{
    juce::StringArray args;
    for (int i = 1; i < argc; ++i)
        args.add (argv[i]);

    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    // The editor keeps its settings in the user's application data: use a scratch folder for the whole run.
    const auto scratch = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("WaveRefTestsStorage");
    scratch.deleteRecursively();
    scratchFolder = scratch;
    ui::UiSettings::setStorageFolder (scratch);

    testDsp();

    if (! args.contains ("--no-gui"))
    {
        const int snapshotIndex = args.indexOf ("--snapshot");
        const auto folder = snapshotIndex >= 0 && snapshotIndex + 1 < args.size()
                                ? juce::File (args[snapshotIndex + 1])
                                : juce::File::getCurrentWorkingDirectory();
        testGui (folder);
    }

    ui::UiSettings::setStorageFolder (juce::File());
    scratch.deleteRecursively();

    std::cout << (failures == 0 ? "All tests passed" : juce::String (failures) + " test(s) failed") << std::endl;
    return failures == 0 ? 0 : 1;
}
