// Offline checks for FloorMatch: measurements on synthetic dialogue takes, plus a render mode
// for real recordings and an optional screenshot of the editor.
//
//   FloorMatch_Tests [--no-gui] [--snapshot <dir>]
//   FloorMatch_Tests --render <in.wav> <out.wav> [--target dB] [--match %] [--maxred dB]
//                    [--lookahead 0-3] [--fill] [--removed] [--quietest]
//
// --quietest runs the file twice: the first pass finds the quietest steady noise floor, the
// second renders with it as the target (what "Use quietest" does in the plugin).
#include "../Source/PluginEditor.h"
#include "../Source/PluginProcessor.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <iostream>

namespace
{
    using floormatch::dsp::aWeighting;
    namespace ids = floormatch::ids;

    int failures = 0;

    void expect (bool condition, const juce::String& what)
    {
        std::cout << (condition ? "  ok    " : "  FAIL  ") << what << std::endl;
        if (! condition)
            ++failures;
    }

    juce::String db (double v) { return juce::String (v, 2) + " dB"; }

    void setParam (FloorMatchProcessor& p, const char* id, float value)
    {
        auto* param = p.apvts.getParameter (id);
        jassert (param != nullptr);
        param->setValueNotifyingHost (param->convertTo0to1 (value));
    }

    void resetParams (FloorMatchProcessor& p)
    {
        for (auto* param : p.getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param))
                ranged->setValueNotifyingHost (ranged->getDefaultValue());
        p.clearProfile();
    }

    //==============================================================================
    // Signal generation

    struct Rng
    {
        explicit Rng (int seed) : random (seed) {}
        float uniform (float a, float b) { return a + (b - a) * random.nextFloat(); }
        float gaussian()
        {
            const float u1 = std::max (1.0e-7f, random.nextFloat()), u2 = random.nextFloat();
            return std::sqrt (-2.0f * std::log (u1)) * std::cos (juce::MathConstants<float>::twoPi * u2);
        }
        juce::Random random;
    };

    enum NoiseColour { white, pink, brown, roomTone, hiss };

    std::vector<float> makeNoise (int numSamples, NoiseColour colour, int seed, double sampleRate)
    {
        Rng rng (seed);
        std::vector<float> out ((size_t) numSamples);
        float b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0, brownState = 0, lp = 0, hpIn = 0, hpOut = 0;
        const float lpCoeff = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * 7000.0f / (float) sampleRate);
        const float hpCoeff = std::exp (-juce::MathConstants<float>::twoPi * 1500.0f / (float) sampleRate);

        for (auto& s : out)
        {
            const float w = rng.gaussian();

            // Paul Kellet's pink filter.
            b0 = 0.99886f * b0 + w * 0.0555179f;
            b1 = 0.99332f * b1 + w * 0.0750759f;
            b2 = 0.96900f * b2 + w * 0.1538520f;
            b3 = 0.86650f * b3 + w * 0.3104856f;
            b4 = 0.55000f * b4 + w * 0.5329522f;
            b5 = -0.7616f * b5 - w * 0.0168980f;
            const float pinkValue = (b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362f) * 0.11f;
            b6 = w * 0.115926f;

            brownState = 0.997f * brownState + 0.05f * w;

            switch (colour)
            {
                case white: s = w; break;
                case pink:  s = pinkValue; break;
                case brown: s = brownState; break;
                case roomTone:
                    lp += lpCoeff * (pinkValue + 0.6f * brownState - lp);   // rumble + air, rolled off above 7 kHz
                    s = lp;
                    break;
                case hiss:
                    hpOut = hpCoeff * (hpOut + w - hpIn);                   // mostly hiss above 1.5 kHz
                    hpIn = w;
                    s = hpOut + 0.15f * pinkValue;
                    break;
            }
        }

        return out;
    }

    // Welch PSD (mean-square per Hz, one-sided) over a set of sample ranges.
    struct Psd
    {
        std::vector<double> psd;
        double binWidth = 1.0;
        int frames = 0;
    };

    // sqrtHann with order 10 reproduces the engine's own analysis (same leakage), for comparing estimates.
    Psd welch (const float* x, const std::vector<juce::Range<int>>& ranges, double sampleRate, int order = 12, bool sqrtHann = false)
    {
        const int size = 1 << order;
        juce::dsp::FFT fft (order);
        std::vector<float> window ((size_t) size), buffer ((size_t) size * 2);
        double windowPower = 0.0;
        for (int n = 0; n < size; ++n)
        {
            const float hann = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) n / (float) size);
            window[(size_t) n] = sqrtHann ? std::sqrt (hann) : hann;
            windowPower += window[(size_t) n] * window[(size_t) n];
        }

        Psd result;
        result.psd.assign (size / 2 + 1, 0.0);
        result.binWidth = sampleRate / size;

        for (const auto& range : ranges)
        {
            for (int start = range.getStart(); start + size <= range.getEnd(); start += size / 2)
            {
                for (int n = 0; n < size; ++n)
                    buffer[(size_t) n] = x[start + n] * window[(size_t) n];
                std::fill (buffer.begin() + size, buffer.end(), 0.0f);
                fft.performFrequencyOnlyForwardTransform (buffer.data(), true);

                for (int k = 1; k < size / 2; ++k)
                    result.psd[(size_t) k] += 2.0 * (double) buffer[(size_t) k] * buffer[(size_t) k] / (windowPower * sampleRate);
                ++result.frames;
            }
        }

        if (result.frames > 0)
            for (auto& v : result.psd)
                v /= result.frames;

        return result;
    }

    double weightedLevel (const Psd& psd)
    {
        double sum = 0.0;
        for (size_t k = 1; k < psd.psd.size(); ++k)
        {
            const double f = (double) k * psd.binWidth;
            if (f >= 20.0 && f <= 20000.0)
                sum += aWeighting ((float) f) * psd.psd[k] * psd.binWidth;
        }
        return 10.0 * std::log10 (std::max (sum, 1.0e-30)) + 3.0103;
    }

    double octaveLevel (const Psd& psd, double centre)
    {
        double sum = 0.0;
        for (size_t k = 1; k < psd.psd.size(); ++k)
        {
            const double f = (double) k * psd.binWidth;
            if (f >= centre / std::sqrt (2.0) && f < centre * std::sqrt (2.0))
                sum += psd.psd[k] * psd.binWidth;
        }
        return 10.0 * std::log10 (std::max (sum, 1.0e-30)) + 3.0103;
    }

    void scaleToLevel (std::vector<float>& x, double levelDb, double sampleRate)
    {
        const auto psd = welch (x.data(), { { 0, (int) x.size() } }, sampleRate);
        const float gain = (float) std::pow (10.0, (levelDb - weightedLevel (psd)) / 20.0);
        for (auto& s : x)
            s *= gain;
    }

    // Speech-like signal: utterances of syllables (harmonic source through three formant
    // resonators, sometimes with a fricative onset) separated by pauses.
    struct Speech
    {
        std::vector<float> audio;
        std::vector<juce::Range<int>> utterances;
    };

    Speech makeSpeech (int numSamples, int seed, double sampleRate, double levelDb, double leadingPause = 0.6)
    {
        Rng rng (seed);
        Speech result;
        result.audio.assign ((size_t) numSamples, 0.0f);
        const float fs = (float) sampleRate;
        int pos = (int) (leadingPause * sampleRate);

        while (pos < numSamples)
        {
            const int utteranceStart = pos;
            const int utteranceEnd = std::min (numSamples, pos + (int) (rng.uniform (1.0f, 2.6f) * fs));

            while (pos < utteranceEnd)
            {
                const int length = std::min (utteranceEnd - pos, (int) (rng.uniform (0.12f, 0.3f) * fs));
                const float f0a = rng.uniform (95.0f, 210.0f), f0b = f0a * rng.uniform (0.8f, 1.2f);
                const float formants[] { rng.uniform (300, 800), rng.uniform (900, 2300), rng.uniform (2400, 3200) };
                const float formantGain[] { 1.0f, 0.6f, 0.3f };
                const bool fricative = rng.uniform (0, 1) < 0.35f;
                const int fricativeLength = fricative ? std::min (length / 2, (int) (0.06f * fs)) : 0;

                juce::dsp::IIR::Filter<float> resonators[3];
                for (int i = 0; i < 3; ++i)
                    resonators[i].coefficients = juce::dsp::IIR::Coefficients<float>::makeBandPass (sampleRate, formants[i], 6.0f);

                juce::dsp::IIR::Filter<float> fricativeFilter;
                fricativeFilter.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighPass (sampleRate, 3500.0f);

                std::vector<double> phases (64, 0.0);
                for (int n = 0; n < length; ++n)
                {
                    const float t = (float) n / (float) length;
                    const float f0 = f0a + (f0b - f0a) * t;
                    float source = 0.0f;
                    const int harmonics = std::min (60, (int) (4500.0f / f0));
                    for (int h = 1; h <= harmonics; ++h)
                    {
                        phases[(size_t) h] += juce::MathConstants<double>::twoPi * f0 * h / fs;
                        source += (float) std::sin (phases[(size_t) h]) / (float) h;
                    }

                    float voiced = 0.0f;
                    for (int i = 0; i < 3; ++i)
                        voiced += formantGain[i] * resonators[i].processSample (source);

                    const float envelope = std::pow (std::sin (juce::MathConstants<float>::pi * t), 2.0f);
                    float s = voiced * envelope;

                    if (n < fricativeLength)
                    {
                        const float ft = (float) n / (float) fricativeLength;
                        s = s * ft + 0.5f * fricativeFilter.processSample (rng.gaussian()) * std::sin (juce::MathConstants<float>::pi * ft);
                    }

                    result.audio[(size_t) (pos + n)] = s;
                }

                pos += length + (int) (rng.uniform (0.0f, 0.06f) * fs);
            }

            result.utterances.push_back ({ utteranceStart, std::min (pos, numSamples) });
            pos += (int) (rng.uniform (0.35f, 1.0f) * fs);
        }

        // Normalise the speech level measured over the utterances.
        double energy = 0.0;
        int count = 0;
        for (const auto& u : result.utterances)
            for (int i = u.getStart(); i < u.getEnd(); ++i, ++count)
                energy += result.audio[(size_t) i] * result.audio[(size_t) i];

        const float gain = (float) (std::pow (10.0, levelDb / 20.0) / std::sqrt (energy / std::max (1, count)));
        for (auto& s : result.audio)
            s *= gain;

        return result;
    }

    // Pauses between utterances, trimmed so speech tails and early onsets stay out.
    std::vector<juce::Range<int>> pausesOf (const Speech& speech, juce::Range<int> region, double sampleRate,
                                            double afterSpeech = 0.15, double beforeSpeech = 0.06)
    {
        std::vector<juce::Range<int>> pauses;
        int start = region.getStart();

        auto add = [&] (int a, int b)
        {
            a = std::max (a, region.getStart());
            b = std::min (b, region.getEnd());
            if (b - a > (int) (0.1 * sampleRate))
                pauses.push_back ({ a, b });
        };

        for (const auto& u : speech.utterances)
        {
            if (u.getEnd() <= region.getStart() || u.getStart() >= region.getEnd())
                continue;
            add (start == region.getStart() ? start : start + (int) (afterSpeech * sampleRate), u.getStart() - (int) (beforeSpeech * sampleRate));
            start = u.getEnd();
        }
        add (start + (int) (afterSpeech * sampleRate), region.getEnd());
        return pauses;
    }

    //==============================================================================
    // Running the plugin

    juce::AudioBuffer<float> run (FloorMatchProcessor& p, const juce::AudioBuffer<float>& input, double sampleRate,
                                  int blockSize = 512, bool randomBlocks = false)
    {
        const int channels = input.getNumChannels();
        const auto set = channels == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo();
        juce::AudioProcessor::BusesLayout layout;
        layout.inputBuses.add (set);
        layout.outputBuses.add (set);
        p.setBusesLayout (layout);
        p.prepareToPlay (sampleRate, blockSize);

        const int latency = p.getLatencySamples();
        const int total = input.getNumSamples() + latency;
        juce::AudioBuffer<float> io (channels, total);
        io.clear();
        for (int ch = 0; ch < channels; ++ch)
            io.copyFrom (ch, 0, input, ch, 0, input.getNumSamples());

        juce::MidiBuffer midi;
        juce::Random random (7);
        for (int start = 0; start < total;)
        {
            const int length = std::min (total - start, randomBlocks ? 1 + random.nextInt (blockSize) : blockSize);
            juce::AudioBuffer<float> block (io.getArrayOfWritePointers(), channels, start, length);
            p.processBlock (block, midi);
            start += length;
        }

        // Compensate the latency, like a host would.
        juce::AudioBuffer<float> out (channels, input.getNumSamples());
        for (int ch = 0; ch < channels; ++ch)
            out.copyFrom (ch, 0, io, ch, latency, input.getNumSamples());
        return out;
    }

    juce::AudioBuffer<float> toBuffer (const std::vector<float>& x, int channels = 1)
    {
        juce::AudioBuffer<float> b (channels, (int) x.size());
        for (int ch = 0; ch < channels; ++ch)
            b.copyFrom (ch, 0, x.data(), (int) x.size());
        return b;
    }

    // A dialogue track of several takes joined with hard cuts; each take has its own noise.
    struct Take { NoiseColour colour; double noiseDb; };

    struct Track
    {
        std::vector<float> audio, noise;
        Speech speech;
        std::vector<juce::Range<int>> takes;
    };

    Track makeTrack (const std::vector<Take>& takes, double takeSeconds, double sampleRate, int seed, double speechDb = -26.0)
    {
        Track track;
        const int takeLength = (int) (takeSeconds * sampleRate);
        const int total = takeLength * (int) takes.size();
        track.noise.assign ((size_t) total, 0.0f);

        // Speech per take, each starting with a short pause (as dialogue edits usually do).
        track.speech.audio.assign ((size_t) total, 0.0f);
        for (size_t i = 0; i < takes.size(); ++i)
        {
            const int offset = takeLength * (int) i;
            auto noise = makeNoise (takeLength, takes[i].colour, seed + (int) i * 17, sampleRate);
            scaleToLevel (noise, takes[i].noiseDb, sampleRate);
            std::copy (noise.begin(), noise.end(), track.noise.begin() + offset);

            auto speech = makeSpeech (takeLength, seed + 100 + (int) i, sampleRate, speechDb);
            std::copy (speech.audio.begin(), speech.audio.end(), track.speech.audio.begin() + offset);
            for (auto u : speech.utterances)
                track.speech.utterances.push_back (u + offset);

            track.takes.push_back ({ offset, offset + takeLength });
        }

        track.audio.resize ((size_t) total);
        for (int i = 0; i < total; ++i)
            track.audio[(size_t) i] = track.noise[(size_t) i] + track.speech.audio[(size_t) i];

        return track;
    }

    double pauseLevel (const float* x, const Track& track, juce::Range<int> region, double sampleRate)
    {
        return weightedLevel (welch (x, pausesOf (track.speech, region, sampleRate), sampleRate));
    }

    //==============================================================================
    void testEstimator()
    {
        std::cout << "Noise estimator" << std::endl;
        constexpr double fs = 48000.0;

        for (auto colour : { white, pink, roomTone })
        {
            for (int lookahead : { 0, 2 })
            {
                floormatch::dsp::Engine engine;
                engine.prepare (fs, 1, lookahead);

                auto noise = makeNoise ((int) (8 * fs), colour, 3, fs);
                scaleToLevel (noise, -60.0, fs);
                const auto truth = welch (noise.data(), { { 0, (int) noise.size() } }, fs, 10, true);

                // Average the engine's estimate over the last 3 s.
                std::vector<double> estimate ((size_t) engine.getFftSize() / 2 + 1, 0.0);
                int frames = 0, lastCounter = -1;
                auto buffer = noise;
                float* channels[] { buffer.data() };

                for (int start = 0; start + 256 <= (int) buffer.size(); start += 256)
                {
                    float* block[] { channels[0] + start };
                    engine.process (block, 1, 256);
                    if (start > (int) (5 * fs) && engine.isLatestNoiseValid() && engine.getSnapshotCounter() != lastCounter)
                    {
                        lastCounter = engine.getSnapshotCounter();
                        const auto& n = engine.getLatestNoiseEstimate();
                        for (size_t k = 0; k < estimate.size(); ++k)
                            estimate[k] += n[k] * engine.binToPsd();
                        ++frames;
                    }
                }

                // Compare in bands between 100 Hz and 16 kHz.
                double worst = 0.0, mean = 0.0;
                int bandsCompared = 0;
                for (double centre = 125.0; centre <= 16000.0; centre *= 2.0)
                {
                    double est = 0.0, tru = 0.0;
                    const double binWidth = fs / engine.getFftSize();
                    for (size_t k = 1; k < estimate.size(); ++k)
                    {
                        const double f = k * binWidth;
                        if (f >= centre / std::sqrt (2.0) && f < centre * std::sqrt (2.0))
                            est += estimate[k] / frames * binWidth;
                    }
                    for (size_t k = 1; k < truth.psd.size(); ++k)
                    {
                        const double f = k * truth.binWidth;
                        if (f >= centre / std::sqrt (2.0) && f < centre * std::sqrt (2.0))
                            tru += truth.psd[k] * truth.binWidth;
                    }
                    const double error = 10.0 * std::log10 (est / tru);
                    worst = std::max (worst, std::abs (error));
                    mean += error;
                    ++bandsCompared;
                }
                mean /= bandsCompared;

                const char* names[] { "white", "pink", "brown", "room tone", "hiss" };
                expect (std::abs (mean) < 0.5 && worst < 1.0,
                        juce::String ("estimate of steady ") + names[colour] + " noise, lookahead " + juce::String (lookahead)
                            + ": mean error " + db (mean) + ", worst octave " + db (worst));
            }
        }
    }

    void testTransparency()
    {
        std::cout << "Transparency and latency" << std::endl;

        for (double fs : { 44100.0, 48000.0, 96000.0 })
        {
            for (int lookahead : { 0, 1, 3 })
            {
                FloorMatchProcessor p;
                resetParams (p);
                setParam (p, ids::lookahead, (float) lookahead);
                setParam (p, ids::target, -24.0f);   // far above the noise: nothing to do

                auto track = makeTrack ({ { pink, -70.0 }, { roomTone, -60.0 } }, 5.0, fs, 11);
                const auto in = toBuffer (track.audio, 2);
                const auto out = run (p, in, fs, 480, true);

                double error = 0.0, energy = 0.0;
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < in.getNumSamples(); ++i)
                    {
                        const double d = out.getSample (ch, i) - in.getSample (ch, i);
                        error += d * d;
                        energy += (double) in.getSample (ch, i) * in.getSample (ch, i);
                    }

                const double nullDb = 10.0 * std::log10 (std::max (error, 1.0e-30) / energy);
                expect (nullDb < -90.0, "nothing to reduce -> output nulls against the input at "
                                            + juce::String (fs / 1000.0, 1) + " kHz, lookahead " + juce::String (lookahead)
                                            + " (latency " + juce::String (p.getLatencySamples()) + " samples, null " + db (nullDb) + ")");
            }
        }
    }

    void testLevelMatching()
    {
        std::cout << "Level matching across takes" << std::endl;
        constexpr double fs = 48000.0;
        constexpr double targetDb = -64.0, maxReduction = 20.0;

        // Rising and falling cuts, different colours.
        const std::vector<Take> takes { { roomTone, -52.0 }, { pink, -58.0 }, { hiss, -49.0 }, { brown, -61.0 },
                                        { roomTone, -70.0 }, { white, -55.0 } };
        const auto track = makeTrack (takes, 9.0, fs, 21);

        for (int lookahead : { 1, 2, 3, 0 })
        {
            FloorMatchProcessor p;
            resetParams (p);
            setParam (p, ids::lookahead, (float) lookahead);
            setParam (p, ids::target, (float) targetDb);
            setParam (p, ids::maxReduction, (float) maxReduction);
            setParam (p, ids::match, 0.0f);

            const auto out = run (p, toBuffer (track.audio), fs);
            const float* y = out.getReadPointer (0);

            juce::String report;
            double worst = 0.0, worstStart = 0.0;
            for (size_t i = 0; i < takes.size(); ++i)
            {
                const auto region = track.takes[i];
                const double inputDb = pauseLevel (track.audio.data(), track, region, fs);
                const double expectedOut = inputDb <= targetDb ? inputDb : std::max (targetDb, inputDb - maxReduction);

                // Steady state (skip the first 2 s when there's no lookahead) and the first pause after the cut.
                const int settle = lookahead == 0 ? (int) (2.0 * fs) : 0;
                const double outDb = pauseLevel (y, track, { region.getStart() + settle, region.getEnd() }, fs);
                const double startDb = weightedLevel (welch (y, { { region.getStart(), region.getStart() + (int) (0.5 * fs) } }, fs));

                worst = std::max (worst, std::abs (outDb - expectedOut));
                worstStart = std::max (worstStart, std::abs (startDb - expectedOut));
                report << "\n          take " << (int) i + 1 << ": in " << juce::String (inputDb, 1) << ", out "
                       << juce::String (outDb, 1) << " (first 0.5 s " << juce::String (startDb, 1) << "), expected "
                       << juce::String (expectedOut, 1);
            }

            const double tolerance = lookahead == 0 ? 1.5 : 1.0;
            expect (worst < tolerance, "lookahead " + juce::String (lookahead) + ": noise between words lands on the target, worst error "
                                           + db (worst) + report);

            if (lookahead > 0)
                expect (worstStart < 1.5, "lookahead " + juce::String (lookahead) + ": right after each cut the noise is already on target, worst "
                                              + db (worstStart));
        }
    }

    void testSpeechPreservation()
    {
        std::cout << "Dialogue preservation" << std::endl;
        constexpr double fs = 48000.0;

        for (double speechDb : { -26.0, -36.0 })
        {
            const auto track = makeTrack ({ { roomTone, -52.0 }, { pink, -56.0 } }, 10.0, fs, 31, speechDb);

            FloorMatchProcessor p;
            resetParams (p);
            setParam (p, ids::target, -64.0f);
            setParam (p, ids::maxReduction, 20.0f);
            setParam (p, ids::match, 0.0f);
            const auto out = run (p, toBuffer (track.audio), fs);
            const float* y = out.getReadPointer (0);

            // Speech energy = energy in the utterances minus the noise energy measured in the pauses.
            double worst = 0.0;
            juce::String report;
            for (size_t t = 0; t < track.takes.size(); ++t)
            {
                std::vector<juce::Range<int>> speechRanges;
                for (auto u : track.speech.utterances)
                    if (track.takes[t].contains (u.getStart()))
                        speechRanges.push_back (u);

                auto energyIn = [&] (const float* x)
                {
                    double e = 0.0;
                    int n = 0;
                    for (auto r : speechRanges)
                        for (int i = r.getStart(); i < r.getEnd(); ++i, ++n)
                            e += (double) x[i] * x[i];
                    return e / std::max (1, n);
                };

                auto pauseMeanSquare = [&] (const float* x)
                {
                    double e = 0.0;
                    int n = 0;
                    for (auto r : pausesOf (track.speech, track.takes[t], fs))
                        for (int i = r.getStart(); i < r.getEnd(); ++i, ++n)
                            e += (double) x[i] * x[i];
                    return e / std::max (1, n);
                };

                const double speechIn = energyIn (track.speech.audio.data());
                const double speechOut = energyIn (y) - pauseMeanSquare (y);
                const double change = 10.0 * std::log10 (std::max (speechOut, 1.0e-20) / speechIn);
                worst = std::max (worst, std::abs (change));
                report << " take " << (int) t + 1 << ": " << db (change);
            }

            expect (worst < (speechDb > -30.0 ? 0.5 : 1.0), "speech at " + juce::String (speechDb, 0) + " dBFS keeps its level while the noise drops 8-12 dB ("
                                                                 + report.trim() + ")");
        }
    }

    // Engine-level quality: the mix is analysed, and clean speech and clean noise ride along as
    // shadow channels with exactly the same gains, so each component can be measured on its own.
    bool verboseQuality = false;

    struct Quality
    {
        double speechChange = 0, speechSdr = 0, onsetChange = 0, highBandChange = 0;
        double noisePauseDb = 0, noiseSpeechDb = 0, noiseInDb = 0;
    };

    Quality measureQuality (const Track& track, double sampleRate, float targetDb, float maxReductionDb, int lookahead)
    {
        floormatch::dsp::Engine engine;
        engine.prepare (sampleRate, 3, lookahead);
        engine.setAnalysisChannels (1);
        floormatch::dsp::Settings settings;
        settings.targetDb = targetDb;
        settings.maxReductionDb = maxReductionDb;
        settings.match = 0.0f;
        engine.setSettings (settings);

        const int latency = engine.getLatencySamples();
        const int total = (int) track.audio.size() + latency;
        std::vector<float> mix (track.audio), speech (track.speech.audio), noise (track.noise);
        for (auto* v : { &mix, &speech, &noise })
            v->resize ((size_t) total, 0.0f);

        for (int start = 0; start < total; start += 512)
        {
            const int n = std::min (512, total - start);
            float* channels[] { mix.data() + start, speech.data() + start, noise.data() + start };
            engine.process (channels, 3, n);
        }

        auto aligned = [&] (const std::vector<float>& v) { return v.data() + latency; };
        const float* outSpeech = aligned (speech);
        const float* outNoise = aligned (noise);
        const float* inSpeech = track.speech.audio.data();

        Quality q;
        double eIn = 0, eOut = 0, eErr = 0, onsetIn = 0, onsetOut = 0;
        for (auto u : track.speech.utterances)
        {
            for (int i = u.getStart(); i < u.getEnd(); ++i)
            {
                eIn += (double) inSpeech[i] * inSpeech[i];
                eOut += (double) outSpeech[i] * outSpeech[i];
                eErr += (double) (outSpeech[i] - inSpeech[i]) * (outSpeech[i] - inSpeech[i]);
            }
            for (int i = u.getStart(); i < std::min (u.getEnd(), u.getStart() + (int) (0.04 * sampleRate)); ++i)
            {
                onsetIn += (double) inSpeech[i] * inSpeech[i];
                onsetOut += (double) outSpeech[i] * outSpeech[i];
            }
        }
        q.speechChange = 10.0 * std::log10 (eOut / eIn);
        q.speechSdr = 10.0 * std::log10 (eIn / std::max (eErr, 1.0e-30));
        q.onsetChange = 10.0 * std::log10 (std::max (onsetOut, 1.0e-30) / std::max (onsetIn, 1.0e-30));

        const auto psdIn = welch (inSpeech, track.speech.utterances, sampleRate);
        const auto psdOut = welch (outSpeech, track.speech.utterances, sampleRate);
        double hIn = 0, hOut = 0;
        for (size_t k = 1; k < psdIn.psd.size(); ++k)
            if (k * psdIn.binWidth > 3000.0) { hIn += psdIn.psd[k]; hOut += psdOut.psd[k]; }
        q.highBandChange = 10.0 * std::log10 (hOut / hIn);

        std::vector<juce::Range<int>> pauses;
        for (auto take : track.takes)
            for (auto r : pausesOf (track.speech, take, sampleRate))
                pauses.push_back (r);
        if (verboseQuality)
        {
            const auto nP = welch (outNoise, pauses, sampleRate), nS = welch (outNoise, track.speech.utterances, sampleRate);
            std::cout << "          octave: noise under speech - between words / speech - noise under speech" << std::endl;
            for (double c = 125; c <= 8000; c *= 2)
                std::cout << "          " << juce::String (c, 0).paddedLeft (' ', 5) << ": " << juce::String (octaveLevel (nS, c) - octaveLevel (nP, c), 1).paddedLeft (' ', 5)
                          << " / " << juce::String (octaveLevel (psdOut, c) - octaveLevel (nS, c), 1).paddedLeft (' ', 5) << std::endl;
        }
        q.noisePauseDb = weightedLevel (welch (outNoise, pauses, sampleRate));
        q.noiseSpeechDb = weightedLevel (welch (outNoise, track.speech.utterances, sampleRate));
        q.noiseInDb = weightedLevel (welch (track.noise.data(), pauses, sampleRate));
        return q;
    }

    void testQuality()
    {
        std::cout << "Dialogue quality (clean components measured separately)" << std::endl;
        constexpr double fs = 48000.0;

        for (double speechDb : { -26.0, -36.0 })
        {
            const auto track = makeTrack ({ { roomTone, -52.0 }, { pink, -55.0 }, { hiss, -52.0 } }, 10.0, fs, 81, speechDb);
            const auto q = measureQuality (track, fs, -64.0f, 20.0f, 2);

            std::cout << "          speech " << juce::String (speechDb, 0) << " dBFS: level " << db (q.speechChange)
                      << ", SDR " << db (q.speechSdr) << ", first 40 ms " << db (q.onsetChange) << ", above 3 kHz "
                      << db (q.highBandChange) << std::endl
                      << "          noise: in " << juce::String (q.noiseInDb, 1) << ", out between words " << juce::String (q.noisePauseDb, 1)
                      << ", out under speech " << juce::String (q.noiseSpeechDb, 1) << std::endl;

            const bool loud = speechDb > -30.0;
            expect (std::abs (q.speechChange) < (loud ? 0.3 : 0.8) && q.speechSdr > (loud ? 20.0 : 12.0),
                    "speech at " + juce::String (speechDb, 0) + " dBFS passes nearly unchanged (level " + db (q.speechChange)
                        + ", SDR " + db (q.speechSdr) + ")");
            expect (q.onsetChange > (loud ? -1.0 : -2.0) && q.highBandChange > (loud ? -0.5 : -1.5),
                    "word onsets and consonants keep their level (first 40 ms " + db (q.onsetChange) + ", above 3 kHz "
                        + db (q.highBandChange) + ")");
        }
    }

    // Noise events inside a take (close cloth rustle, a passing car) are not background: they must
    // pass untouched, while real cuts, also crossfaded ones, are still followed at once.
    void testEvents()
    {
        std::cout << "Noise events and crossfaded cuts" << std::endl;
        constexpr double fs = 48000.0;

        // Rustle: band-passed noise with a fast random envelope and 400 ms fades, 18 dB over the floor.
        auto track = makeTrack ({ { roomTone, -62.0 } }, 16.0, fs, 91);
        const int eventStart = (int) (6.0 * fs), eventLength = (int) (6.0 * fs), fade = (int) (0.4 * fs);
        {
            Rng rng (92);
            juce::dsp::IIR::Filter<float> high, low;
            high.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighPass (fs, 800.0f);
            low.coefficients = juce::dsp::IIR::Coefficients<float>::makeLowPass (fs, 9000.0f);
            std::vector<float> rustle ((size_t) eventLength);
            float envelope = 1.0f, targetEnvelope = 1.0f;
            for (int i = 0; i < eventLength; ++i)
            {
                if (i % (int) (0.08 * fs) == 0)
                    targetEnvelope = std::pow (10.0f, rng.uniform (-6.0f, 3.0f) / 20.0f);
                envelope += 0.002f * (targetEnvelope - envelope);
                const float ramp = std::min ({ 1.0f, (float) i / fade, (float) (eventLength - i) / fade });
                rustle[(size_t) i] = low.processSample (high.processSample (rng.gaussian())) * envelope * ramp;
            }
            std::vector<float> middle (rustle.begin() + fade, rustle.end() - fade);
            scaleToLevel (middle, -44.0, fs);
            const float gain = middle[0] / std::max (1.0e-9f, rustle[(size_t) fade]);
            for (int i = 0; i < eventLength; ++i)
                track.audio[(size_t) (eventStart + i)] += rustle[(size_t) i] * gain;
        }

        for (int lookahead : { 2, 3, 0 })
        {
            FloorMatchProcessor p;
            resetParams (p);
            setParam (p, ids::lookahead, (float) lookahead);
            setParam (p, ids::target, -70.0f);
            setParam (p, ids::maxReduction, 20.0f);
            setParam (p, ids::match, 0.0f);
            const auto out = run (p, toBuffer (track.audio), fs);

            const std::vector<juce::Range<int>> event { { eventStart + fade, eventStart + eventLength - fade } };
            const double inDb = weightedLevel (welch (track.audio.data(), event, fs));
            const double outDb = weightedLevel (welch (out.getReadPointer (0), event, fs));
            const double before = pauseLevel (out.getReadPointer (0), track, { 0, eventStart }, fs);
            const double after = pauseLevel (out.getReadPointer (0), track, { eventStart + eventLength + (int) fs, track.takes[0].getEnd() }, fs);

            expect (std::abs (outDb - inDb) < 1.5 && std::abs (before + 70.0) < 1.0 && std::abs (after + 70.0) < 1.0,
                    "lookahead " + juce::String (lookahead) + ": a 6 s cloth-rustle-like event passes (" + db (outDb - inDb)
                        + ") while the background around it sits on the target (" + juce::String (before, 1) + " / "
                        + juce::String (after, 1) + ")");
        }

        // A cut with a 100 ms equal-power crossfade from a loud to a quiet take and back.
        {
            const int takeLength = (int) (9.0 * fs), crossfade = (int) (0.1 * fs);
            const std::vector<Take> takes { { pink, -50.0 }, { roomTone, -64.0 }, { hiss, -52.0 } };
            Track faded;
            faded.audio.assign ((size_t) takeLength * takes.size(), 0.0f);
            faded.speech.audio.assign (faded.audio.size(), 0.0f);

            for (size_t i = 0; i < takes.size(); ++i)
            {
                const int offset = takeLength * (int) i;
                auto noise = makeNoise (takeLength + crossfade, takes[i].colour, 93 + (int) i, fs);
                scaleToLevel (noise, takes[i].noiseDb, fs);
                for (int n = 0; n < takeLength + crossfade; ++n)
                {
                    const int at = offset - crossfade / 2 + n;
                    if (at < 0 || at >= (int) faded.audio.size())
                        continue;
                    float gain = 1.0f;
                    if (i > 0 && n < crossfade)                    gain = std::sin (juce::MathConstants<float>::halfPi * (float) n / crossfade);
                    if (i + 1 < takes.size() && n >= takeLength)   gain = std::cos (juce::MathConstants<float>::halfPi * (float) (n - takeLength) / crossfade);
                    faded.audio[(size_t) at] += noise[(size_t) n] * gain;
                }

                auto speech = makeSpeech (takeLength, 193 + (int) i, fs, -26.0);
                for (int n = 0; n < takeLength; ++n)
                    faded.speech.audio[(size_t) (offset + n)] = speech.audio[(size_t) n];
                for (auto u : speech.utterances)
                    faded.speech.utterances.push_back (u + offset);
                faded.takes.push_back ({ offset, offset + takeLength });
            }

            for (size_t n = 0; n < faded.audio.size(); ++n)
                faded.audio[n] += faded.speech.audio[n];

            FloorMatchProcessor p;
            resetParams (p);
            setParam (p, ids::target, -66.0f);
            setParam (p, ids::maxReduction, 20.0f);
            setParam (p, ids::match, 0.0f);
            const auto out = run (p, toBuffer (faded.audio), fs);

            double worst = 0.0;
            juce::String report;
            for (size_t i = 1; i < takes.size(); ++i)
            {
                // Between the end of the crossfade and the first word (at 0.6 s; the gain opens just before it).
                const int start = faded.takes[i].getStart() + (int) (0.1 * fs);
                const double level = weightedLevel (welch (out.getReadPointer (0), { { start, start + (int) (0.3 * fs) } }, fs, 11));
                worst = std::max (worst, std::abs (level + 66.0));
                report << juce::String (level, 1) << " ";
            }

            expect (worst < 1.5, "cuts with 100 ms crossfades are followed at once (0.1-0.4 s after each: " + report.trim() + ")");
        }
    }

    void testColourAndFill()
    {
        std::cout << "Colour match, learn and fill" << std::endl;
        constexpr double fs = 48000.0;

        // Learn the quiet take's room tone, then match a louder take of a different colour to it.
        const auto reference = makeTrack ({ { roomTone, -63.0 } }, 8.0, fs, 41);
        const auto louder = makeTrack ({ { hiss, -50.0 }, { pink, -54.0 } }, 9.0, fs, 42);

        FloorMatchProcessor p;
        resetParams (p);
        p.setLearning (true);
        run (p, toBuffer (reference.audio), fs);
        p.setLearning (false);

        const double referenceDb = pauseLevel (reference.audio.data(), reference, reference.takes[0], fs);
        const double targetDb = p.apvts.getRawParameterValue (ids::target)->load();
        const bool learned = p.hasProfile();
        expect (learned && std::abs (targetDb - referenceDb) < 0.7,
                "learn sets the target to the reference take's noise level (" + juce::String (targetDb, 1) + " vs "
                    + juce::String (referenceDb, 1) + ")");

        setParam (p, ids::maxReduction, 30.0f);
        setParam (p, ids::match, 100.0f);
        setParam (p, ids::fill, 1.0f);
        const auto out = run (p, toBuffer (louder.audio), fs);

        const auto referencePsd = welch (reference.audio.data(), pausesOf (reference.speech, reference.takes[0], fs), fs);

        for (size_t t = 0; t < louder.takes.size(); ++t)
        {
            const auto outPsd = welch (out.getReadPointer (0), pausesOf (louder.speech, louder.takes[t], fs), fs);
            double worst = 0.0;
            juce::String report;
            for (double centre : { 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0 })
            {
                const double d = octaveLevel (outPsd, centre) - octaveLevel (referencePsd, centre);
                worst = std::max (worst, std::abs (d));
                report << juce::String (d, 1) << " ";
            }

            expect (worst < 1.5, "take " + juce::String ((int) t + 1) + " is matched to the reference colour, octave errors 125 Hz-8 kHz: "
                                     + report.trim() + " dB");
        }

        // Fill: a take quieter than the target is brought up to it.
        {
            const auto quiet = makeTrack ({ { pink, -74.0 } }, 8.0, fs, 43);
            FloorMatchProcessor q;
            resetParams (q);
            setParam (q, ids::target, -62.0f);
            setParam (q, ids::match, 0.0f);
            setParam (q, ids::fill, 1.0f);
            const auto filled = run (q, toBuffer (quiet.audio), fs);
            const double level = pauseLevel (filled.getReadPointer (0), quiet, quiet.takes[0], fs);
            expect (std::abs (level + 62.0) < 1.0, "fill lifts a -74 dB take to the -62 dB target (" + db (level) + ")");

            // No fill in digital silence (gaps between takes).
            juce::AudioBuffer<float> gap (1, (int) (4 * fs));
            gap.clear();
            gap.copyFrom (0, 0, quiet.audio.data(), (int) fs);
            const auto gapOut = run (q, gap, fs);
            const float tail = gapOut.getMagnitude (0, (int) (2.5 * fs), (int) (1.5 * fs));
            expect (tail == 0.0f, "no room tone is added over digital silence (peak " + juce::String (tail) + ")");
        }
    }

    void testQuietest()
    {
        std::cout << "Quietest floor" << std::endl;
        constexpr double fs = 48000.0;
        const auto track = makeTrack ({ { pink, -55.0 }, { roomTone, -66.0 }, { hiss, -60.0 } }, 8.0, fs, 51);

        FloorMatchProcessor p;
        resetParams (p);
        run (p, toBuffer (track.audio), fs);

        float level = 0.0f;
        const double truth = pauseLevel (track.audio.data(), track, track.takes[1], fs);
        const bool found = p.getQuietest (level);
        expect (found && std::abs (level - truth) < 1.0,
                "finds the quietest take's floor (" + juce::String (level, 1) + " vs " + juce::String (truth, 1) + ")");

        const bool used = p.useQuietest();
        expect (used && p.hasProfile() && std::abs (p.apvts.getRawParameterValue (ids::target)->load() - level) < 0.05f,
                "use quietest sets the profile and the target");
    }

    void testRobustness()
    {
        std::cout << "Robustness" << std::endl;
        constexpr double fs = 48000.0;
        auto track = makeTrack ({ { roomTone, -50.0 }, { pink, -60.0 } }, 6.0, fs, 61);

        // Digital silence between takes, a loud transient and a near-full-scale burst.
        std::fill (track.audio.begin() + (int) (5.0 * fs), track.audio.begin() + (int) (6.5 * fs), 0.0f);
        track.audio[(size_t) (8.0 * fs)] = 0.99f;
        for (int i = 0; i < (int) (0.2 * fs); ++i)
            track.audio[(size_t) (9.0 * fs) + (size_t) i] = 0.9f * std::sin ((float) i * 0.05f);

        for (int lookahead = 0; lookahead < 4; ++lookahead)
        {
            FloorMatchProcessor p;
            resetParams (p);
            setParam (p, ids::lookahead, (float) lookahead);
            setParam (p, ids::fill, 1.0f);
            setParam (p, ids::target, -58.0f);
            const auto out = run (p, toBuffer (track.audio, 2), fs, 333, true);

            bool finite = true;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < out.getNumSamples(); ++i)
                    finite = finite && std::isfinite (out.getSample (ch, i)) && std::abs (out.getSample (ch, i)) < 2.0f;

            const float silence = out.getMagnitude (0, (int) (5.3 * fs), (int) (0.9 * fs));
            expect (finite && silence < 1.0e-6f, "lookahead " + juce::String (lookahead)
                                                      + ": stereo, random blocks, silence gap and bursts render cleanly (gap peak "
                                                      + juce::String (silence) + ")");
        }

        // Listen to what's removed: mostly noise, little speech.
        {
            FloorMatchProcessor p;
            resetParams (p);
            setParam (p, ids::target, -66.0f);
            setParam (p, ids::maxReduction, 20.0f);
            setParam (p, ids::listen, 1.0f);
            const auto clean = makeTrack ({ { roomTone, -50.0 } }, 8.0, fs, 62);
            const auto removed = run (p, toBuffer (clean.audio), fs);
            const double removedDb = pauseLevel (removed.getReadPointer (0), clean, clean.takes[0], fs);
            const double inDb = pauseLevel (clean.audio.data(), clean, clean.takes[0], fs);
            // Removed = input - output: for 16 dB of reduction that's the input times (1 - 0.16).
            expect (std::abs (removedDb - (inDb + 20.0 * std::log10 (1.0 - std::pow (10.0, -16.0 / 20.0)))) < 1.0,
                    "listen 'Removed' plays the part of the noise being taken out (" + db (removedDb) + ")");
        }

        // State round trip, including the learned profile.
        {
            FloorMatchProcessor p;
            resetParams (p);
            floormatch::dsp::Profile profile;
            for (int i = 0; i < floormatch::dsp::numProfileBands; ++i)
                profile.psd[(size_t) i] = 1.0e-9f / (1.0f + (float) i);
            profile.valid = true;
            p.setProfile (profile, true);
            setParam (p, ids::match, 42.0f);
            setParam (p, ids::lookahead, 3.0f);

            juce::MemoryBlock state;
            p.getStateInformation (state);

            FloorMatchProcessor other;
            other.setStateInformation (state.getData(), (int) state.getSize());

            bool same = other.hasProfile();
            for (auto* param : p.getParameters())
                same = same && std::abs (param->getValue() - other.getParameters()[param->getParameterIndex()]->getValue()) < 1.0e-6f;

            const auto restored = other.getProfile();
            for (int i = 0; i < floormatch::dsp::numProfileBands; ++i)
                same = same && std::abs (std::log10 (restored.psd[(size_t) i] / profile.psd[(size_t) i])) < 1.0e-3f;

            expect (same, "state save/restore round-trips the parameters and the profile");
        }
    }

    //==============================================================================
    // --t <name> <value> pairs on the command line, for tuning sweeps.
    bool parseTuning (const juce::StringArray& args, floormatch::dsp::Engine::Tuning& tuning)
    {
        for (int k = 0; k + 2 < args.size(); ++k)
        {
            if (args[k] != "--t")
                continue;
            const auto name = args[k + 1];
            const float value = args[k + 2].getFloatValue();
            if (name == "ddAlpha")          tuning.ddAlpha = value;
            else if (name == "minPriori")   tuning.minPrioriSnr = value;
            else if (name == "presLow")     tuning.presenceLowDb = value;
            else if (name == "presHigh")    tuning.presenceHighDb = value;
            else if (name == "attack")      tuning.attackStep = value;
            else if (name == "gainFloor")   tuning.speechGainFloor = value;
            else if (name == "release")     tuning.releaseSeconds = value;
            else if (name == "wiener")      tuning.wienerSpeechGain = value > 0.5f;
            else if (name == "smoothBins")  tuning.smoothGainAcrossBins = value > 0.5f;
            else if (name == "spike")       tuning.spikeGain = value;
            else { std::cout << "unknown tuning " << name << std::endl; return false; }
        }
        return true;
    }

    int render (const juce::StringArray& args)
    {
        const int index = args.indexOf ("--render");
        if (index < 0 || index + 2 >= args.size())
        {
            std::cout << "usage: --render <in.wav> <out.wav> [options]" << std::endl;
            return 1;
        }

        const juce::File inFile (juce::File::getCurrentWorkingDirectory().getChildFile (args[index + 1]));
        const juce::File outFile (juce::File::getCurrentWorkingDirectory().getChildFile (args[index + 2]));

        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (inFile));
        if (reader == nullptr)
        {
            std::cout << "can't read " << inFile.getFullPathName() << std::endl;
            return 1;
        }

        const int channels = std::min (2, (int) reader->numChannels);
        juce::AudioBuffer<float> input (channels, (int) reader->lengthInSamples);
        reader->read (&input, 0, input.getNumSamples(), 0, true, channels > 1);
        const double fs = reader->sampleRate;

        auto option = [&] (const char* name, float fallback)
        {
            const int i = args.indexOf (name);
            return i >= 0 && i + 1 < args.size() ? args[i + 1].getFloatValue() : fallback;
        };

        FloorMatchProcessor p;
        resetParams (p);
        setParam (p, ids::target, option ("--target", -60.0f));
        setParam (p, ids::match, option ("--match", 100.0f));
        setParam (p, ids::maxReduction, option ("--maxred", 12.0f));
        setParam (p, ids::lookahead, option ("--lookahead", 2.0f));
        setParam (p, ids::fill, args.contains ("--fill") ? 1.0f : 0.0f);
        setParam (p, ids::listen, args.contains ("--removed") ? 1.0f : 0.0f);

        if (args.contains ("--quietest"))
        {
            run (p, input, fs);
            float level = 0.0f;
            if (p.getQuietest (level) && p.useQuietest())
                std::cout << "quietest floor: " << juce::String (level, 1) << " dB, used as target" << std::endl;
            else
                std::cout << "no steady noise floor found; keeping the target" << std::endl;
        }

        const auto output = run (p, input, fs);

        // Noise floor per second, measured on the input and on the rendered output by the same estimator.
        auto floorTimeline = [&] (const juce::AudioBuffer<float>& audio)
        {
            floormatch::dsp::Engine meter;
            meter.prepare (fs, 1, 2);
            juce::AudioBuffer<float> mono (1, audio.getNumSamples() + meter.getLatencySamples());
            mono.clear();
            for (int ch = 0; ch < audio.getNumChannels(); ++ch)
                mono.addFrom (0, 0, audio, ch, 0, audio.getNumSamples(), 1.0f / (float) audio.getNumChannels());

            std::vector<float> levels;
            const int step = (int) fs;
            for (int start = 0; start + step <= mono.getNumSamples(); start += step)
            {
                float* block[] { mono.getWritePointer (0) + start };
                meter.process (block, 1, step);
                if (start + step > meter.getLatencySamples())
                    levels.push_back (meter.getSnapshot().valid ? meter.getSnapshot().noiseDb : -150.0f);
            }
            return levels;
        };

        const auto before = floorTimeline (input), after = floorTimeline (output);
        std::cout << "time   noise in   noise out (A-weighted dB, estimated between words)" << std::endl;
        for (size_t i = 0; i < std::min (before.size(), after.size()); ++i)
            std::cout << juce::String ((int) i).paddedLeft (' ', 4) << "   " << juce::String (before[i], 1).paddedLeft (' ', 7)
                      << "   " << juce::String (after[i], 1).paddedLeft (' ', 7) << std::endl;

        outFile.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream (outFile.createOutputStream());
        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions {}
                                                       .withSampleRate (fs)
                                                       .withNumChannels (channels)
                                                       .withBitsPerSample (24));
        if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (output, 0, output.getNumSamples()))
        {
            std::cout << "can't write " << outFile.getFullPathName() << std::endl;
            return 1;
        }

        std::cout << "wrote " << outFile.getFullPathName() << std::endl;
        return 0;
    }

    //==============================================================================
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
        constexpr double fs = 48000.0;

        FloorMatchProcessor p;
        resetParams (p);
        {
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            saveSnapshot (*editor, folder.getChildFile ("floormatch-init.png"));
        }

        // A learned profile and audio flowing, so the display and meters have content.
        const auto reference = makeTrack ({ { roomTone, -63.0 } }, 5.0, fs, 71);
        p.setLearning (true);
        run (p, toBuffer (reference.audio, 2), fs);
        p.setLearning (false);
        setParam (p, ids::fill, 1.0f);

        // A louder take of another colour, processed block by block without the silent tail.
        std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
        auto* view = dynamic_cast<FloorMatchEditor*> (editor.get());
        const auto loud = makeTrack ({ { hiss, -50.0 } }, 5.0, fs, 72);
        auto audio = toBuffer (loud.audio, 2);
        juce::MidiBuffer midi;
        p.prepareToPlay (fs, 512);
        for (int start = 0; start + 512 <= audio.getNumSamples(); start += 512)
        {
            juce::AudioBuffer<float> block (audio.getArrayOfWritePointers(), 2, start, 512);
            p.processBlock (block, midi);
            if (view != nullptr && start % 4096 == 0)
                view->refresh();   // what the editor's timer would do
        }

        saveSnapshot (*editor, folder.getChildFile ("floormatch-active.png"));
    }
}

int main (int argc, char* argv[])
{
    juce::StringArray args;
    for (int i = 1; i < argc; ++i)
        args.add (argv[i]);

    // The parameter tree uses timers, so a message manager is needed even without a GUI.
    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    if (args.contains ("--render"))
        return render (args);

    verboseQuality = args.contains ("--verbose");

    // --shadow <in.wav> <out.wav> [--target dB] [--maxred dB] [--lookahead i] [--t <name> <value>]...
    // Channel 0 is analysed; the other channels (e.g. clean speech and clean noise of the same mix)
    // get exactly the same gains. Output is latency-compensated 32-bit float.
    if (args.contains ("--shadow"))
    {
        const int i = args.indexOf ("--shadow");
        auto option = [&] (const char* name, float fallback)
        {
            const int k = args.indexOf (name);
            return k >= 0 && k + 1 < args.size() ? args[k + 1].getFloatValue() : fallback;
        };

        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (juce::File::getCurrentWorkingDirectory().getChildFile (args[i + 1])));
        if (reader == nullptr)
            return 1;

        const double fs = reader->sampleRate;
        const int channels = (int) reader->numChannels;
        floormatch::dsp::Engine engine;
        engine.prepare (fs, channels, (int) option ("--lookahead", 2));
        engine.setAnalysisChannels (1);

        floormatch::dsp::Engine::Tuning tuning;
        if (! parseTuning (args, tuning))
            return 1;
        engine.setTuning (tuning);

        floormatch::dsp::Settings settings;
        settings.targetDb = option ("--target", -62.0f);
        settings.maxReductionDb = option ("--maxred", 15.0f);
        settings.match = 0.0f;
        engine.setSettings (settings);

        const int latency = engine.getLatencySamples();
        const int length = (int) reader->lengthInSamples;
        juce::AudioBuffer<float> audio (channels, length + latency);
        audio.clear();
        reader->read (&audio, 0, length, 0, true, true);

        for (int n = 0; n < audio.getNumSamples(); n += 512)
        {
            const int block = std::min (512, audio.getNumSamples() - n);
            std::vector<float*> pointers;
            for (int ch = 0; ch < channels; ++ch)
                pointers.push_back (audio.getWritePointer (ch) + n);
            engine.process (pointers.data(), channels, block);
        }

        juce::AudioBuffer<float> output (channels, length);
        for (int ch = 0; ch < channels; ++ch)
            output.copyFrom (ch, 0, audio, ch, latency, length);

        const juce::File outFile (juce::File::getCurrentWorkingDirectory().getChildFile (args[i + 2]));
        outFile.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream (outFile.createOutputStream());
        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions {}.withSampleRate (fs).withNumChannels (channels)
                                                       .withBitsPerSample (32).withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
        return writer != nullptr && writer->writeFromAudioSampleBuffer (output, 0, length) ? 0 : 1;
    }

    // --trace <file.wav> <start s> <end s> [lookahead]: the estimator's decisions every 0.1 s.
    if (args.contains ("--trace"))
    {
        const int i = args.indexOf ("--trace");
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (juce::File::getCurrentWorkingDirectory().getChildFile (args[i + 1])));
        if (reader == nullptr)
            return 1;

        const double fs = reader->sampleRate;
        const double from = args[i + 2].getDoubleValue(), to = args[i + 3].getDoubleValue();
        floormatch::dsp::Engine engine;
        engine.prepare (fs, 1, i + 4 < args.size() ? args[i + 4].getIntValue() : 2);
        if (args.contains ("--bin"))
            engine.setProbeBin (args[args.indexOf ("--bin") + 1].getIntValue());
        const int latency = engine.getLatencySamples();
        const int start = std::max (0, (int) (from * fs) - (int) (3.0 * fs));
        const int length = (int) (to * fs) - start + latency;
        juce::AudioBuffer<float> audio (1, length);
        reader->read (&audio, 0, length, start, true, false);

        // The estimator's frame runs 4 frames (the attack lookahead) ahead of the output frame.
        const double offset = (latency - engine.getFftSize() / 2 - 4 * engine.getHopSize()) / fs;
        double nextPrint = from;
        for (int n = 0; n + engine.getHopSize() <= length; n += engine.getHopSize())
        {
            float* block[] { audio.getWritePointer (0) + n };
            engine.process (block, 1, engine.getHopSize());
            const double t = (start + n + engine.getHopSize()) / fs - offset;
            if (t >= nextPrint && t <= to)
            {
                nextPrint += 0.1;
                const auto& d = engine.getDiagnostics();
                std::cout << juce::String (t, 1) << "  noise " << juce::String (engine.getSnapshot().noiseDb, 1) << "  " << d.sides
                          << "  rises " << d.rises << " falls " << d.falls << " of " << d.compared << "  holding " << d.holdingBins
                          << "  | bin " << d.probeBin << ": n " << juce::String (10 * std::log10 (d.probeNoise + 1e-30f), 1)
                          << " past " << juce::String (10 * std::log10 (d.probePast + 1e-30f), 1) << "/" << d.probeCountPast
                          << " future " << juce::String (10 * std::log10 (d.probeFuture + 1e-30f), 1) << "/" << d.probeCountFuture
                          << " hold " << d.probeHold << " track " << juce::String (10 * std::log10 (d.probeTrack + 1e-30f), 1) << std::endl;
            }
        }
        return 0;
    }
    if (args.contains ("--bench"))
    {
        // Real-time factor for 60 s of stereo audio at 48 kHz, per lookahead setting.
        constexpr double fs = 48000.0;
        auto noise = makeNoise ((int) (60 * fs), roomTone, 1, fs);
        scaleToLevel (noise, -50.0, fs);
        for (int lookahead = 0; lookahead < 4; ++lookahead)
        {
            floormatch::dsp::Engine engine;
            engine.prepare (fs, 2, lookahead);
            auto left = noise, right = noise;
            const auto start = juce::Time::getMillisecondCounterHiRes();
            for (int i = 0; i + 512 <= (int) left.size(); i += 512)
            {
                float* channels[] { left.data() + i, right.data() + i };
                engine.process (channels, 2, 512);
            }
            const double ms = juce::Time::getMillisecondCounterHiRes() - start;
            std::cout << "lookahead " << lookahead << ": " << juce::String (ms / 600.0, 2) << " % of one core" << std::endl;
        }
        return 0;
    }
    if (args.contains ("--quality"))
    {
        testQuality();
    testEvents();
        return 0;
    }

    if (args.contains ("--debug"))
    {
        constexpr double fs = 48000.0;
        auto option = [&] (const char* name, float fallback)
        {
            const int i = args.indexOf (name);
            return i >= 0 && i + 1 < args.size() ? args[i + 1].getFloatValue() : fallback;
        };
        const int seed = (int) option ("--seed", 41);
        const auto colour = (NoiseColour) (int) option ("--colour", roomTone);
        const auto track = makeTrack ({ { colour, -63.0 } }, option ("--seconds", 16.0f), fs, seed, option ("--speech", -26.0f));
        const auto truth = welch (track.noise.data(), { { 0, (int) track.noise.size() } }, fs, 10, true);

        floormatch::dsp::Engine engine;
        engine.prepare (fs, 1, (int) option ("--lookahead", 2));
        floormatch::dsp::Engine::Tuning tuning;
        if (! parseTuning (args, tuning))
            return 1;
        engine.setTuning (tuning);
        auto audio = track.audio;
        const int latency = engine.getLatencySamples();
        audio.resize (audio.size() + (size_t) latency, 0.0f);
        std::vector<double> sumSpeech (513, 0.0), sumPause (513, 0.0);
        int nSpeech = 0, nPause = 0;
        for (int start = 0; start + 256 <= (int) audio.size(); start += 256)
        {
            float* block[] { audio.data() + start };
            engine.process (block, 1, 256);
            const int t = start + 256 - latency - 512;   // centre of the latest output frame
            if (t > 0 && t < (int) track.noise.size() && engine.isLatestNoiseValid())
            {
                bool speaking = false;
                for (auto u : track.speech.utterances)
                    speaking = speaking || u.contains (t);
                if (args.contains ("--timeline"))
                {
                    const auto& n = engine.getLatestNoiseEstimate();
                    const int k0 = (int) option ("--bin", 9);
                    double bandTruth = 0.0;
                    for (int k = k0 - 1; k <= k0 + 1; ++k) bandTruth += truth.psd[(size_t) k] / engine.binToPsd();
                    std::cout << juce::String (t / fs, 2) << (speaking ? " S " : "   ") << juce::String (engine.getSnapshot().noiseDb, 1)
                              << "  bin err " << juce::String (10 * std::log10 ((n[(size_t) k0 - 1] + n[(size_t) k0] + n[(size_t) k0 + 1]) / bandTruth), 1)
                              << std::endl;
                }
                auto& sum = speaking ? sumSpeech : sumPause;
                const auto& n = engine.getLatestNoiseEstimate();
                for (size_t k = 0; k < 513; ++k)
                    sum[k] += n[k] * engine.binToPsd();
                (speaking ? nSpeech : nPause)++;
            }
        }
        std::cout << "third-octave error (dB) of the estimate, in speech / in pauses" << std::endl;
        double worstS = 0, worstP = 0, meanS = 0, meanP = 0; int bands = 0;
        for (double fc = 100; fc < 16000; fc *= std::pow (2.0, 1.0 / 3.0))
        {
            double s1 = 0, p1 = 0, t1 = 0;
            for (size_t k = 1; k < 513; ++k)
            {
                const double f = k * fs / 1024;
                if (f >= fc / std::pow (2.0, 1.0 / 6.0) && f < fc * std::pow (2.0, 1.0 / 6.0))
                { s1 += sumSpeech[k] / nSpeech; p1 += sumPause[k] / nPause; t1 += truth.psd[k]; }
            }
            if (t1 <= 0) continue;
            const double es = 10 * std::log10 (s1 / t1), ep = 10 * std::log10 (p1 / t1);
            worstS = std::max (worstS, std::abs (es)); worstP = std::max (worstP, std::abs (ep));
            meanS += es; meanP += ep; ++bands;
            if (args.contains ("--verbose"))
                std::cout << juce::String (fc, 0).paddedLeft (' ', 6) << "  " << juce::String (es, 2) << " / " << juce::String (ep, 2) << std::endl;
        }
        std::cout << "speech: mean " << meanS / bands << " worst " << worstS << "   pauses: mean " << meanP / bands << " worst " << worstP << std::endl;
        return 0;
    }

    testEstimator();
    testTransparency();
    testLevelMatching();
    testSpeechPreservation();
    testQuality();
    testEvents();
    testColourAndFill();
    testQuietest();
    testRobustness();

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
