// LavAlign offline tests and command-line tools.
//
//   LavAlign_Tests                      synthetic alignment checks, WAV I/O, sessions (CI)
//   LavAlign_Tests --align <boom.wav[:ch]> <lav.wav[:ch]> <out.wav> [--phase 0..1] [--curve out.csv] [--range ms]
//                                       aligns one lav to a boom and prints what it found
//   LavAlign_Tests --process <out folder> <files or folders...> [--split] [--phase 0..1]
//                                       does what the app's Export does (roles guessed from names)
//   LavAlign_Tests --bench              speed on a 5 minute synthetic take
//   LavAlign_Tests --snapshot <folder>  renders the app window to PNG (needs the GUI build)

#include "../Source/Align.h"
#include "../Source/Session.h"
#include "../Source/WavFile.h"
#include "../Source/Platform.h"

#if LAVALIGN_TESTS_WITH_GUI
 #include "../Source/MainComponent.h"
#endif

#include <juce_dsp/juce_dsp.h>

#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <random>
#include <thread>
#include <atomic>

using namespace lavalign;

namespace
{
    constexpr double pi = 3.14159265358979323846;
    int failures = 0;

    void check (bool ok, const juce::String& what)
    {
        std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
        if (! ok) ++failures;
    }

    struct Biquad
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        static Biquad make (const juce::dsp::IIR::Coefficients<float>& c)
        {
            Biquad q; auto* k = c.coefficients.begin();
            if (c.coefficients.size() == 3) { q.b0 = k[0]; q.b1 = k[1]; q.a1 = k[2]; return q; }  // first order
            q.b0 = k[0]; q.b1 = k[1]; q.b2 = k[2]; q.a1 = k[3]; q.a2 = k[4];
            return q;
        }
        float process (float x)
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return (float) y;
        }
        void run (std::vector<float>& v) { for (auto& s : v) s = process (s); }
    };
    using Coefs = juce::dsp::IIR::Coefficients<float>;

    // Speech-like test signal: voiced syllables (pulse train through three moving formants), fricatives, pauses.
    std::vector<float> speechLike (double fs, double seconds, unsigned seed)
    {
        std::mt19937 rng (seed);
        std::uniform_real_distribution<double> U (0.0, 1.0);
        std::normal_distribution<double> N (0.0, 1.0);
        const auto n = (size_t) (seconds * fs);
        std::vector<float> out (n, 0.0f);
        size_t pos = (size_t) (0.3 * fs);
        double phase = 0;
        while (pos < n)
        {
            const double r = U (rng);
            if (r < 0.12)  // pause
            {
                pos += (size_t) ((0.15 + U (rng) * (U (rng) < 0.3 ? 1.2 : 0.3)) * fs);
                continue;
            }
            const size_t len = (size_t) ((0.12 + 0.25 * U (rng)) * fs);
            const size_t end = std::min (n, pos + len);
            std::vector<float> seg (end - pos, 0.0f);
            if (r < 0.27)  // fricative
            {
                for (auto& s : seg) s = (float) N (rng) * 0.15f;
                auto hp = Biquad::make (*Coefs::makeHighPass (fs, 2500 + 2500 * U (rng), 0.7));
                hp.run (seg);
            }
            else  // voiced
            {
                const double f0 = 100 + 110 * U (rng), glide = (U (rng) - 0.5) * 40;
                for (size_t i = 0; i < seg.size(); ++i)
                {
                    const double t = (double) i / fs;
                    phase += (f0 + glide * t / (len / fs) + 3 * std::sin (2 * pi * 5 * t)) / fs;
                    if (phase >= 1.0) { phase -= 1.0; seg[i] = 1.0f; }
                    seg[i] += (float) (N (rng) * 0.01);
                }
                const double f1 = 300 + 500 * U (rng), f2 = 900 + 1300 * U (rng), f3 = 2400 + 700 * U (rng);
                std::vector<float> sum (seg.size(), 0.0f);
                for (auto [f, g] : { std::pair { f1, 1.0 }, std::pair { f2, 0.6 }, std::pair { f3, 0.35 } })
                {
                    auto bp = Biquad::make (*Coefs::makeBandPass (fs, f, 8.0));
                    auto copy = seg;
                    bp.run (copy);
                    for (size_t i = 0; i < sum.size(); ++i) sum[i] += (float) g * copy[i];
                }
                seg = sum;
            }
            for (size_t i = 0; i < seg.size(); ++i)
            {
                const double w = std::sin (pi * (double) i / (double) seg.size());
                out[pos + i] += (float) (seg[i] * w * (0.5 + 0.5 * U (rng) * 0 + 0.5));
            }
            pos = end;
        }
        float peak = 0;
        for (auto v : out) peak = std::max (peak, std::abs (v));
        for (auto& v : out) v *= 0.5f / std::max (peak, 1e-9f);
        return out;
    }

    // out[n] = x[n - d(n)] (d in samples) using the engine's own renderer with a dense curve.
    std::vector<float> delayBy (const std::vector<float>& x, double fs, const std::function<double (double)>& dSamples)
    {
        DelayCurve c;
        c.sampleRate = fs; c.hop = 32; c.firstFrameCentre = 0; c.found = true;
        const size_t frames = x.size() / 32 + 2;
        for (size_t k = 0; k < frames; ++k) { c.delay.push_back ((float) dSamples (k * 32.0)); c.weight.push_back (1); c.measured.push_back (0); }
        std::vector<float> y (x.size());
        renderAligned (x.data(), y.data(), (int64_t) x.size(), c);
        return y;
    }

    std::vector<float> fftConvolve (const std::vector<float>& x, const std::vector<float>& h)
    {
        const size_t n = x.size() + h.size() - 1;
        int order = 1; while ((size_t) (1 << order) < n) ++order;
        juce::dsp::FFT fft (order);
        const size_t N = (size_t) 1 << order;
        std::vector<std::complex<float>> a (N), b (N), A (N), B (N), c (N);
        for (size_t i = 0; i < x.size(); ++i) a[i] = x[i];
        for (size_t i = 0; i < h.size(); ++i) b[i] = h[i];
        fft.perform (a.data(), A.data(), false); fft.perform (b.data(), B.data(), false);
        for (size_t i = 0; i < N; ++i) a[i] = A[i] * B[i];
        fft.perform (a.data(), c.data(), true);
        std::vector<float> y (x.size());
        for (size_t i = 0; i < x.size(); ++i) y[i] = c[i].real();
        return y;
    }

    double rms (const std::vector<float>& v)
    {
        double s = 0; for (auto x : v) s += (double) x * x;
        return std::sqrt (s / std::max<size_t> (1, v.size()));
    }

    struct Pair { std::vector<float> boom, lav; std::function<double (double)> truth; };

    // Boom: delayed source + floor reflection + diffuse reverb + 80 Hz high pass + noise.
    // Lav: chest resonance, duller top, 120 Hz high pass, noise, clothing rustle, optional polarity flip.
    Pair makePair (const std::vector<float>& src, double fs, std::function<double (double)> delayMs, bool invert, unsigned seed,
                   double reverbSeconds = 0.4, double noiseDb = -45)
    {
        std::mt19937 rng (seed);
        std::normal_distribution<double> N (0.0, 1.0);
        std::uniform_real_distribution<double> U (0.0, 1.0);
        Pair p;
        p.truth = [delayMs, fs] (double sample) { return delayMs (sample / fs) * 1e-3 * fs; };
        auto direct = delayBy (src, fs, p.truth);
        auto refl = delayBy (src, fs, [&] (double s) { return p.truth (s) + 2.2e-3 * fs; });
        auto lp = Biquad::make (*Coefs::makeLowPass (fs, 5000)); lp.run (refl);
        p.boom.resize (src.size());
        for (size_t i = 0; i < src.size(); ++i) p.boom[i] = direct[i] + 0.45f * refl[i];
        if (reverbSeconds > 0)
        {
            std::vector<float> ir ((size_t) (reverbSeconds * fs));
            for (size_t i = (size_t) (0.004 * fs); i < ir.size(); ++i)
                ir[i] = (float) (N (rng) * std::exp (-6.9 * (double) i / fs / reverbSeconds));
            auto lp2 = Biquad::make (*Coefs::makeFirstOrderLowPass (fs, 3000)); lp2.run (ir);
            auto wet = fftConvolve (direct, ir);
            const double g = rms (direct) / std::max (1e-12, rms (wet)) * std::pow (10.0, -3.0 / 20.0);
            for (size_t i = 0; i < wet.size(); ++i) p.boom[i] += (float) (wet[i] * g);
        }
        auto hp = Biquad::make (*Coefs::makeHighPass (fs, 80)); hp.run (p.boom);
        const double br = rms (p.boom);
        for (auto& v : p.boom) v += (float) (N (rng) * br * std::pow (10.0, noiseDb / 20.0) * 3);

        p.lav = src;
        auto bp = Biquad::make (*Coefs::makeBandPass (fs, 800, 1.2));
        auto chest = src; bp.run (chest);
        for (size_t i = 0; i < src.size(); ++i) p.lav[i] = src[i] + 0.8f * chest[i];
        auto dull = p.lav; auto lp3 = Biquad::make (*Coefs::makeLowPass (fs, 7000)); lp3.run (dull);
        for (size_t i = 0; i < src.size(); ++i) p.lav[i] = 0.5f * (p.lav[i] + dull[i]);
        auto hp2 = Biquad::make (*Coefs::makeHighPass (fs, 120)); hp2.run (p.lav);
        const double lr = rms (p.lav);
        for (auto& v : p.lav) v += (float) (N (rng) * lr * std::pow (10.0, -50.0 / 20.0) * 3);
        // rustle bursts
        const int bursts = (int) (src.size() / fs / 4);
        for (int k = 0; k < bursts; ++k)
        {
            const size_t s0 = (size_t) (U (rng) * (double) (src.size() - (size_t) fs));
            const size_t len = (size_t) ((0.1 + 0.4 * U (rng)) * fs);
            auto bpn = Biquad::make (*Coefs::makeBandPass (fs, 1200, 0.5));
            const double amp = (0.3 + 0.7 * U (rng)) * lr * 3;
            for (size_t i = 0; i < len && s0 + i < p.lav.size(); ++i)
                p.lav[s0 + i] += (float) (bpn.process ((float) N (rng)) * amp * std::sin (pi * (double) i / (double) len));
        }
        if (invert) for (auto& v : p.lav) v = -v;
        return p;
    }

    //==============================================================================
    void testRenderExact()
    {
        std::cout << "Renderer" << std::endl;
        const double fs = 48000;
        auto x = speechLike (fs, 2.0, 7);
        for (int k = 0; k < 4; ++k) { auto lp = Biquad::make (*Coefs::makeLowPass (fs, 16000)); lp.run (x); }  // band-limited
        auto y = delayBy (x, fs, [] (double) { return 37.0; });
        bool exact = true;
        for (size_t i = 37; i < x.size(); ++i) if (y[i] != x[i - 37]) { exact = false; break; }
        check (exact, "integer delay is an exact shift");
        auto y2 = delayBy (x, fs, [] (double) { return -12.0; });
        exact = true;
        for (size_t i = 0; i + 12 < x.size(); ++i) if (y2[i] != x[i + 12]) { exact = false; break; }
        check (exact, "negative integer delay is an exact advance");
        // half-sample delay twice = one sample
        auto h = delayBy (delayBy (x, fs, [] (double) { return 0.5; }), fs, [] (double) { return 0.5; });
        double err = 0, ref = 0;
        for (size_t i = 100; i + 100 < x.size(); ++i) { err += std::pow (h[i] - x[i - 1], 2); ref += std::pow (x[i - 1], 2); }
        const double db = 10 * std::log10 (err / ref);
        check (db < -60, "two half-sample delays = one sample (error " + juce::String (db, 1) + " dB)");
    }

    struct CaseResult { double rmsUs = 0, p95Us = 0; float pol = 1; Quality before, after; DelayCurve curve; };

    CaseResult runCase (const Pair& p, double fs, Settings s = {})
    {
        LavResult r;
        alignLav (p.boom.data(), p.lav.data(), (int64_t) p.lav.size(), fs, s, r);
        CaseResult c;
        c.pol = r.curve.polarity; c.before = r.before; c.after = r.after; c.curve = r.curve;
        std::vector<double> errs;
        for (size_t k = 0; k < r.curve.delay.size(); ++k)
        {
            if (r.curve.weight[k] < 0.05f) continue;
            const double t = r.curve.firstFrameCentre + (double) k * r.curve.hop;
            errs.push_back (std::abs (r.curve.delay[k] - p.truth (t)) / fs * 1e6);
        }
        if (! errs.empty())
        {
            double s2 = 0; for (auto e : errs) s2 += e * e;
            c.rmsUs = std::sqrt (s2 / (double) errs.size());
            std::sort (errs.begin(), errs.end());
            c.p95Us = errs[(size_t) (0.95 * (double) (errs.size() - 1))];
        }
        return c;
    }

    void testAlignment()
    {
        std::cout << "Alignment on synthetic boom/lav pairs (48 kHz, 30 s)" << std::endl;
        const double fs = 48000;
        const auto src = speechLike (fs, 30.0, 1);

        struct Case { const char* name; std::function<double (double)> ms; bool invert; double maxRms; };
        const std::vector<Case> cases {
            { "static 1.6 ms",            [] (double) { return 1.6; }, false, 40 },
            { "moving 0.6..3 ms",         [] (double t) { return 1.8 + 1.2 * std::sin (2 * pi * t / 7) + 0.4 * std::sin (2 * pi * t / 2.3 + 1); }, true, 50 },
            { "walking away 1..5 ms",     [] (double t) { return 1.0 + 4.0 * (0.5 - 0.5 * std::cos (2 * pi * t / 10)); }, false, 50 },
            // the cut falls mid-syllable: steps move to the nearest quiet moment (up to 150 ms), so a few frames
            // around it keep the old delay. p95 stays ~25 us.
            { "cut: 1.2 -> 3.4 ms",       [] (double t) { return (t < 15 ? 1.2 : 3.4) + 0.02 * t; }, false, 200 },
            { "wireless lav late 2.9 ms", [] (double t) { return -2.9 + 0.8 * std::sin (2 * pi * t / 9); }, false, 50 },
            { "sync offset 31 ms",        [] (double t) { return 31.0 + 0.5 * std::sin (2 * pi * t / 6); }, false, 50 },
        };
        unsigned seed = 10;
        for (auto& c : cases)
        {
            const auto pair = makePair (src, fs, c.ms, c.invert, seed++);
            const auto r = runCase (pair, fs);
            // reference: the lav moved by the true delay (time alignment only)
            auto ideal = delayBy (pair.lav, fs, pair.truth);
            if (c.invert) for (auto& v : ideal) v = -v;
            const auto idealQ = measureQuality (pair.boom.data(), ideal.data(), (int64_t) ideal.size(), fs);
            juce::String line;
            line << c.name << ": error rms " << juce::String (r.rmsUs, 1) << " us, p95 " << juce::String (r.p95Us, 1)
                 << " us, sum quality " << juce::String (r.before.mean, 2) << " -> " << juce::String (r.after.mean, 2)
                 << " (true delay alone " << juce::String (idealQ.mean, 2) << ")";
            check (r.rmsUs < c.maxRms, line);
            check (r.pol == (c.invert ? -1.0f : 1.0f), juce::String (c.name) + ": polarity " + (r.pol < 0 ? "inverted" : "normal"));
            check (r.after.mean > idealQ.mean + 0.05f, juce::String (c.name) + ": sums with the boom better than the true delay alone (phase correction helps)");
        }

        // a lav that hears something else entirely: left alone
        {
            std::mt19937 rng (5); std::normal_distribution<double> N (0, 0.05);
            const auto other = speechLike (fs, 30.0, 99);
            auto pair = makePair (src, fs, [] (double) { return 2.0; }, false, 77);
            for (size_t i = 0; i < pair.lav.size(); ++i) pair.lav[i] = other[i] + (float) N (rng) * 0.01f;
            LavResult r;
            alignLav (pair.boom.data(), pair.lav.data(), (int64_t) pair.lav.size(), fs, {}, r);
            bool same = true;
            for (size_t i = 0; i < pair.lav.size(); ++i) if (r.aligned[i] != pair.lav[i]) { same = false; break; }
            check (! r.curve.found && same, "unrelated lav: nothing found, left unchanged (" + juce::String (r.curve.confidentFrames) + " confident frames)");
        }
    }

    void testPhaseCorrection()
    {
        std::cout << "Spectral phase correction" << std::endl;
        const double fs = 48000;
        const auto src = speechLike (fs, 20.0, 3);
        // boom = source through two allpass sections (phase only) + noise: time alignment alone can't fix it
        auto boom = src;
        auto ap1 = Biquad::make (*Coefs::makeAllPass (fs, 700, 0.8));
        auto ap2 = Biquad::make (*Coefs::makeAllPass (fs, 3500, 1.5));
        ap1.run (boom); ap2.run (boom);
        std::mt19937 rng (1); std::normal_distribution<double> N (0, 1);
        for (auto& v : boom) v += (float) (N (rng) * 0.002);
        auto lav = src;
        const auto before = measureQuality (boom.data(), lav.data(), (int64_t) lav.size(), fs);
        correctPhase (boom.data(), lav.data(), (int64_t) lav.size(), fs, 1.0f);
        const auto after = measureQuality (boom.data(), lav.data(), (int64_t) lav.size(), fs);
        check (after.mean > 0.95f && after.mean > before.mean + 0.1f,
               "allpass difference corrected: quality " + juce::String (before.mean, 2) + " -> " + juce::String (after.mean, 2));

        // magnitude kept: long-term level per octave within 0.5 dB
        auto octaveLevels = [fs] (const std::vector<float>& x)
        {
            std::vector<double> lv;
            for (double f = 125; f <= 8000; f *= 2)
            {
                auto bp = Biquad::make (*Coefs::makeBandPass (fs, f, 1.4));
                auto y = x; bp.run (y);
                lv.push_back (20 * std::log10 (rms (y) + 1e-12));
            }
            return lv;
        };
        const auto a = octaveLevels (src), b = octaveLevels (lav);
        double worst = 0;
        for (size_t i = 0; i < a.size(); ++i) worst = std::max (worst, std::abs (a[i] - b[i]));
        check (worst < 0.5, "phase correction keeps the lav's tone (max octave change " + juce::String (worst, 2) + " dB)");

        // already in phase: nothing changes much
        auto same = src;
        correctPhase (src.data(), same.data(), (int64_t) same.size(), fs, 1.0f);
        double err = 0;
        for (size_t i = 0; i < src.size(); ++i) err += std::pow (same[i] - src[i], 2);
        const double db = 10 * std::log10 (err / (std::pow (rms (src), 2) * (double) src.size()) + 1e-30);
        check (db < -40, "identical signals pass almost unchanged (" + juce::String (db, 1) + " dB)");
    }

    //==============================================================================
    juce::File tempDir()
    {
        auto d = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("LavAlignTests");
        d.createDirectory();
        return d;
    }

    juce::MemoryBlock dataBytes (const AudioFileInfo& info)
    {
        juce::FileInputStream in (info.file);
        in.setPosition (info.dataOffset);
        juce::MemoryBlock b ((size_t) info.dataSize);
        in.read (b.getData(), (int) info.dataSize);
        return b;
    }

    juce::MemoryBlock chunkBytes (const AudioFileInfo& info, const juce::String& id)
    {
        for (auto& c : info.otherChunks)
            if (c.id == id)
            {
                juce::FileInputStream in (info.file);
                in.setPosition (c.offset);
                juce::MemoryBlock b ((size_t) c.size);
                in.read (b.getData(), (int) c.size);
                return b;
            }
        return {};
    }

    void testWavIo()
    {
        std::cout << "Broadcast WAV files" << std::endl;
        const auto dir = tempDir();
        const double fs = 48000;
        const int64_t n = 48000;
        std::vector<std::vector<float>> ch (4, std::vector<float> ((size_t) n));
        std::mt19937 rng (3); std::uniform_real_distribution<float> U (-0.9f, 0.9f);
        for (auto& c : ch) for (auto& v : c) v = U (rng);
        const juce::String ixml = "<?xml version=\"1.0\"?><BWFXML><SCENE>12A</SCENE><TAKE>3</TAKE><TRACK_LIST><TRACK_COUNT>4</TRACK_COUNT>"
                                  "<TRACK><CHANNEL_INDEX>1</CHANNEL_INDEX><INTERLEAVE_INDEX>1</INTERLEAVE_INDEX><NAME>MixL</NAME></TRACK>"
                                  "<TRACK><CHANNEL_INDEX>2</CHANNEL_INDEX><INTERLEAVE_INDEX>2</INTERLEAVE_INDEX><NAME>Boom</NAME></TRACK>"
                                  "<TRACK><CHANNEL_INDEX>3</CHANNEL_INDEX><INTERLEAVE_INDEX>3</INTERLEAVE_INDEX><NAME>Lav Anna</NAME></TRACK>"
                                  "<TRACK><CHANNEL_INDEX>4</CHANNEL_INDEX><INTERLEAVE_INDEX>4</INTERLEAVE_INDEX><NAME>Lav Tom</NAME></TRACK>"
                                  "</TRACK_LIST></BWFXML>";

        for (auto [bits, fl] : { std::pair { 24, false }, std::pair { 16, false }, std::pair { 32, true } })
        {
            const auto tag = juce::String (bits) + (fl ? " float" : " bit");
            const auto src = dir.getChildFile ("poly" + juce::String (bits) + ".wav");
            writeSimpleWav (src, { ch[0].data(), ch[1].data(), ch[2].data(), ch[3].data() }, n, fs, bits, fl, (int64_t) 1728000000, ixml);
            AudioFileInfo info;
            auto err = readInfo (src, info);
            check (err.isEmpty() && info.numChannels == 4 && info.numFrames == n && info.bitsPerSample == bits && info.isFloat == fl,
                   tag + ": header read back");
            check (info.timeReference && *info.timeReference == 1728000000, tag + ": bext timecode " + info.timecodeString());
            check (info.trackNames.size() == 4 && info.trackNames[1] == "Boom" && info.trackNames[3] == "Lav Tom" && info.scene == "12A" && info.take == "3",
                   tag + ": iXML track names, scene and take");

            std::vector<float> repl ((size_t) n);
            for (auto& v : repl) v = U (rng) * 0.5f;
            const auto dst = dir.getChildFile ("poly" + juce::String (bits) + "_out.wav");
            err = writeReplaced (info, dst, { { 2, repl.data() } });
            AudioFileInfo out;
            readInfo (dst, out);
            check (err.isEmpty() && out.numChannels == 4 && out.numFrames == n, tag + ": replaced file written");
            const auto a = dataBytes (info), b = dataBytes (out);
            const int bytes = bits / 8;
            bool untouched = a.getSize() == b.getSize();
            for (int64_t i = 0; untouched && i < n; ++i)
                for (int c : { 0, 1, 3 })
                    if (std::memcmp (static_cast<const char*> (a.getData()) + (i * 4 + c) * bytes,
                                     static_cast<const char*> (b.getData()) + (i * 4 + c) * bytes, (size_t) bytes) != 0)
                    { untouched = false; break; }
            check (untouched, tag + ": other channels byte for byte");
            std::vector<std::vector<float>> back;
            readChannels (out, { 2 }, back);
            double worst = 0;
            for (int64_t i = 0; i < n; ++i) worst = std::max (worst, (double) std::abs (back[0][(size_t) i] - repl[(size_t) i]));
            check (worst <= std::ldexp (1.0, -(bits - 1)) * 0.51 + 1e-9, tag + ": replaced channel within half an LSB");
            check (chunkBytes (info, "iXML") == chunkBytes (out, "iXML") && chunkBytes (info, "bext") == chunkBytes (out, "bext"),
                   tag + ": bext and iXML copied unchanged");

            const auto mono = dir.getChildFile ("mono" + juce::String (bits) + ".wav");
            err = writeMono (info, mono, repl.data());
            AudioFileInfo m;
            readInfo (mono, m);
            check (err.isEmpty() && m.numChannels == 1 && m.timeReference && *m.timeReference == 1728000000 && chunkBytes (m, "iXML").getSize() == 0
                   && m.bitsPerSample == bits && m.isFloat == fl,
                   tag + ": mono split keeps timecode and format, drops the poly iXML");
            check (writeReplaced (info, src, {}).isNotEmpty() && src.existsAsFile(), tag + ": never overwrites the source");
        }

        // RF64 written by hand: ds64 + data size 0xFFFFFFFF
        {
            juce::MemoryOutputStream o;
            const int64_t frames = 1000;
            o.write ("RF64", 4); o.writeInt (-1); o.write ("WAVE", 4);
            o.write ("ds64", 4); o.writeInt (28); o.writeInt64 (0); o.writeInt64 (frames * 2); o.writeInt64 (frames); o.writeInt (0);
            o.write ("fmt ", 4); o.writeInt (16); o.writeShort (1); o.writeShort (1); o.writeInt (48000); o.writeInt (96000); o.writeShort (2); o.writeShort (16);
            o.write ("data", 4); o.writeInt (-1);
            for (int i = 0; i < frames; ++i) o.writeShort ((short) (i * 7));
            const auto f = dir.getChildFile ("rf64.wav");
            f.replaceWithData (o.getData(), o.getDataSize());
            AudioFileInfo info;
            const auto err = readInfo (f, info);
            std::vector<std::vector<float>> d;
            readChannels (info, { 0 }, d);
            check (err.isEmpty() && info.isRf64 && info.numFrames == frames && std::abs (d[0][10] - 70 / 32768.0f) < 1e-7f, "RF64 file read");
        }

        // Zoom-style names in the bext description, numbered by track slot
        {
            AudioFileInfo info;
            info.numChannels = 3;
            const auto f = dir.getChildFile ("zoom.wav");
            writeSimpleWav (f, { ch[0].data(), ch[1].data(), ch[2].data() }, 100, fs, 24, false, (int64_t) 0);
            // patch the description
            juce::MemoryBlock all; f.loadFileAsData (all);
            const char* desc = "zSPEED=25.000ND\r\nzTAKE=032\r\nzSCENE=260424\r\nzTRK1=TrL\r\nzTRK3=Tr1\r\nzTRK6=Tr4\r\n";
            auto* p = static_cast<char*> (all.getData());
            for (size_t i = 0; i + 4 < all.getSize(); ++i)
                if (std::memcmp (p + i, "bext", 4) == 0) { std::memcpy (p + i + 8, desc, std::strlen (desc)); break; }
            f.replaceWithData (all.getData(), all.getSize());
            readInfo (f, info);
            check (info.trackNames.size() == 3 && info.trackNames[0] == "TrL" && info.trackNames[1] == "Tr1" && info.trackNames[2] == "Tr4"
                   && info.take == "032" && info.scene == "260424",
                   "Zoom bext track names (by slot) and scene / take");
        }
    }

    void testSession()
    {
        std::cout << "Takes and roles" << std::endl;
        const auto dir = tempDir().getChildFile ("session");
        dir.deleteRecursively(); dir.createDirectory();
        const double fs = 48000;
        const auto src = speechLike (fs, 8.0, 21);
        auto pair = makePair (src, fs, [] (double) { return 2.0; }, true, 3);
        auto pair2 = makePair (src, fs, [] (double) { return 2.0; }, false, 4);
        pair2.lav = delayBy (pair2.lav, fs, [fs] (double) { return 1e-3 * fs; });  // Tom's lav 1 ms later: 1 ms to the boom
        // a poly take: MixL, Boom, Lav Anna, Lav Tom
        const juce::String ixml = "<BWFXML><TRACK_LIST>"
                                  "<TRACK><INTERLEAVE_INDEX>1</INTERLEAVE_INDEX><NAME>MixL</NAME></TRACK>"
                                  "<TRACK><INTERLEAVE_INDEX>2</INTERLEAVE_INDEX><NAME>Boom</NAME></TRACK>"
                                  "<TRACK><INTERLEAVE_INDEX>3</INTERLEAVE_INDEX><NAME>Lav Anna</NAME></TRACK>"
                                  "<TRACK><INTERLEAVE_INDEX>4</INTERLEAVE_INDEX><NAME>Lav Tom</NAME></TRACK></TRACK_LIST></BWFXML>";
        std::vector<float> mix (src.size());
        for (size_t i = 0; i < mix.size(); ++i) mix[i] = 0.5f * (pair.boom[i] + pair.lav[i]);
        writeSimpleWav (dir.getChildFile ("T001.wav"), { mix.data(), pair.boom.data(), pair.lav.data(), pair2.lav.data() },
                        (int64_t) src.size(), fs, 24, false, (int64_t) 48000 * 3600, ixml);
        // the same moment as mono files (AAF media), starting half a second apart
        const int64_t off = 24000;
        std::vector<float> boomPart (pair.boom.begin() + off, pair.boom.end());
        writeSimpleWav (dir.getChildFile ("A1-0001_clip.wav"), { boomPart.data() }, (int64_t) boomPart.size(), fs, 24, false, (int64_t) 48000 * 7200 + off);
        writeSimpleWav (dir.getChildFile ("A2-0001_clip.wav"), { pair.lav.data() }, (int64_t) pair.lav.size(), fs, 24, false, (int64_t) 48000 * 7200);

        Session s;
        auto msgs = s.addFiles ({ dir });
        check (s.numTakes() == 2 && msgs.isEmpty(), "two takes: one poly file, two mono files grouped by timecode");
        const Take* poly = nullptr; const Take* mono = nullptr;
        int polyIdx = -1, monoIdx = -1;
        for (int i = 0; i < s.numTakes(); ++i)
            if (s.take (i).files.size() == 1) { poly = &s.take (i); polyIdx = i; } else { mono = &s.take (i); monoIdx = i; }
        if (poly == nullptr || mono == nullptr) { check (false, "takes found"); return; }
        check (poly->channels[0].role == Role::off && poly->channels[1].role == Role::boom
               && poly->channels[2].role == Role::lav && poly->channels[3].role == Role::lav,
               "roles guessed from track names (mix off, boom, two lavs)");
        check (mono->files[1].start == off || mono->files[0].start == off, "mono files placed by timecode");

        // the roles of mono AAF files follow their track prefix
        int a1 = -1, a2 = -1;
        for (size_t i = 0; i < mono->channels.size(); ++i)
            (mono->channels[i].name.startsWith ("A1") ? a1 : a2) = (int) i;
        s.setRole (monoIdx, a2, Role::lav);
        s.setRole (monoIdx, a1, Role::boom);
        check (s.take (monoIdx).boomChannel() == a1, "boom set by hand");

        std::vector<LavOutcome> out;
        auto err = processTake (s.take (polyIdx), {}, out, {});
        check (err.isEmpty() && out.size() == 2, "poly take processed");
        if (out.size() == 2)
        {
            check (std::abs (out[0].result.curve.medianDelayMs() - 2.0f) < 0.05f && out[0].result.curve.polarity < 0,
                   "Lav Anna: 2 ms, inverted (" + juce::String (out[0].result.curve.medianDelayMs(), 3) + " ms)");
            check (std::abs (out[1].result.curve.medianDelayMs() - 1.0f) < 0.05f, "Lav Tom: 1 ms (" + juce::String (out[1].result.curve.medianDelayMs(), 3) + " ms)");
        }
        juce::String e;
        const auto outDir = dir.getChildFile ("out");
        auto files = exportTake (s.take (polyIdx), out, { outDir, false, {} }, e);
        check (e.isEmpty() && files.size() == 1 && files[0].getFileName() == "T001.wav", "poly export keeps the file name");
        files = exportTake (s.take (polyIdx), out, { outDir, true, "_al" }, e);
        check (e.isEmpty() && files.size() == 2 && files[0].getFileName() == "T001_Lav Anna_al.wav", "split export: one mono file per lav");

        out.clear();
        err = processTake (s.take (monoIdx), {}, out, {});
        check (err.isEmpty() && out.size() == 1 && std::abs (out[0].result.curve.medianDelayMs() - 2.0f) < 0.05f,
               "mono files with different starts aligned through timecode (" + (out.empty() ? juce::String() : juce::String (out[0].result.curve.medianDelayMs(), 3)) + " ms)");
        files = exportTake (s.take (monoIdx), out, {}, e);
        check (e.isEmpty() && files.size() == 1 && files[0].getParentDirectory().getFileName() == "Aligned", "default output: Aligned folder next to the source");
        // re-adding the folder skips the Aligned results
        Session s2;
        s2.addFiles ({ dir });
        check (s2.numTakes() == 2, "results in Aligned folders are not picked up again");
    }

    //==============================================================================
    bool parseSpec (const juce::String& spec, juce::File& file, int& channel)
    {
        channel = 0;
        auto path = spec;
        const int colon = spec.lastIndexOfChar (':');
        if (colon > 1 && spec.substring (colon + 1).containsOnly ("0123456789"))
        {
            channel = spec.substring (colon + 1).getIntValue() - 1;
            path = spec.substring (0, colon);
        }
        file = juce::File::getCurrentWorkingDirectory().getChildFile (path);
        return file.existsAsFile();
    }

    int runAlign (const juce::StringArray& args)
    {
        juce::File bf, lf; int bc, lc;
        if (args.size() < 4 || ! parseSpec (args[1], bf, bc) || ! parseSpec (args[2], lf, lc))
        { std::cout << "usage: --align <boom.wav[:ch]> <lav.wav[:ch]> <out.wav> [--phase x] [--curve f.csv] [--range ms] [--smooth ms]" << std::endl; return 2; }
        Settings s;
        const int pi_ = args.indexOf ("--phase"); if (pi_ > 0) s.phaseCorrection = (float) args[pi_ + 1].getDoubleValue();
        const int ri = args.indexOf ("--range"); if (ri > 0) s.maxOffsetMs = args[ri + 1].getDoubleValue();
        const int si = args.indexOf ("--smooth"); if (si > 0) s.smoothingMs = args[si + 1].getDoubleValue();
        if (args.contains ("--noseg")) s.phaseBySegment = false;
        AudioFileInfo bi, li;
        auto err = readInfo (bf, bi); if (err.isEmpty()) err = readInfo (lf, li);
        if (err.isNotEmpty()) { std::cout << err << std::endl; return 1; }
        std::vector<std::vector<float>> b, l;
        readChannels (bi, { bc }, b); readChannels (li, { lc }, l);
        // line up by timecode when both have it
        const int64_t off = (bi.timeReference && li.timeReference) ? *li.timeReference - *bi.timeReference : 0;
        std::vector<float> view (l[0].size(), 0.0f);
        for (size_t i = 0; i < view.size(); ++i)
        {
            const int64_t k = (int64_t) i + off;
            if (k >= 0 && k < (int64_t) b[0].size()) view[i] = b[0][(size_t) k];
        }
        const auto t0 = std::chrono::steady_clock::now();
        LavResult r;
        alignLav (view.data(), l[0].data(), (int64_t) view.size(), li.sampleRate, s, r);
        const double secs = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        const auto& c = r.curve;
        std::cout << "found " << (c.found ? "yes" : "no") << ", polarity " << (c.polarity < 0 ? "inverted" : "normal")
                  << ", offset " << c.coarseOffset / li.sampleRate * 1000 << " ms (confidence " << c.coarseConfidence << ")"
                  << ", delay median " << c.medianDelayMs() << " ms, range " << c.minDelayMs() << " .. " << c.maxDelayMs() << " ms"
                  << ", steps " << c.steps.size() << ", quality " << r.before.mean << " -> " << r.after.mean
                  << ", " << secs << " s" << std::endl;
        juce::String bands;
        for (size_t i = 0; i < r.after.bands.size(); ++i)
            bands << juce::String ((int) r.after.bandCentres[i]) << ":" << juce::String (r.before.bands[i], 2) << ">" << juce::String (r.after.bands[i], 2) << " ";
        std::cout << bands << std::endl;
        const int ci = args.indexOf ("--curve");
        if (ci > 0)
        {
            juce::MemoryOutputStream csv;  // (a juce::String grows by copying: far too slow for long takes)
            csv << "t,delay_ms,measured_ms,weight\n";
            for (size_t k = 0; k < c.delay.size(); ++k)
                csv << juce::String ((c.firstFrameCentre + (double) k * c.hop) / li.sampleRate, 4) << "," << juce::String (c.delay[k] / li.sampleRate * 1000, 4)
                    << "," << juce::String (c.measured[k] / li.sampleRate * 1000, 4) << "," << juce::String (c.weight[k], 4) << "\n";
            juce::File::getCurrentWorkingDirectory().getChildFile (args[ci + 1]).replaceWithData (csv.getData(), csv.getDataSize());
        }
        const auto out = juce::File::getCurrentWorkingDirectory().getChildFile (args[3]);
        err = writeSimpleWav (out, { view.data(), r.aligned.data() }, (int64_t) view.size(), li.sampleRate, 32, true);
        if (err.isNotEmpty()) { std::cout << err << std::endl; return 1; }
        return 0;
    }

    int runProcess (const juce::StringArray& args)
    {
        if (args.size() < 3) { std::cout << "usage: --process <out folder> <files...> [--split] [--phase x]" << std::endl; return 2; }
        Settings s;
        ExportOptions opt;
        opt.folder = juce::File::getCurrentWorkingDirectory().getChildFile (args[1]);
        juce::Array<juce::File> in;
        juce::StringArray roles;
        for (int i = 2; i < args.size(); ++i)
        {
            if (args[i] == "--split") { opt.splitPoly = true; continue; }
            if (args[i] == "--phase") { s.phaseCorrection = (float) args[++i].getDoubleValue(); continue; }
            if (args[i] == "--role") { roles.add (args[++i]); continue; }  // key=boom|lav|off, e.g. a3=boom
            in.add (juce::File::getCurrentWorkingDirectory().getChildFile (args[i]));
        }
        Session session;
        for (auto& m : session.addFiles (in)) std::cout << m << std::endl;
        for (auto& r : roles)
        {
            const auto v = r.fromFirstOccurrenceOf ("=", false, false);
            session.setRoleForKey (r.upToFirstOccurrenceOf ("=", false, false).toLowerCase(), v == "boom" ? Role::boom : v == "lav" ? Role::lav : Role::off);
        }
        for (int t = 0; t < session.numTakes(); ++t)
        {
            const auto& take = session.take (t);
            juce::String roles;
            for (auto& ch : take.channels) roles << ch.name << "=" << (ch.role == Role::boom ? "boom" : ch.role == Role::lav ? "lav" : "off") << " ";
            std::cout << take.name << ": " << roles << std::endl;
            std::vector<LavOutcome> out;
            auto err = processTake (take, s, out, {});
            if (err.isNotEmpty()) { std::cout << "  " << err << std::endl; continue; }
            for (auto& o : out)
                std::cout << "  " << take.channels[(size_t) o.channelIndex].name << ": " << o.result.curve.medianDelayMs() << " ms ("
                          << o.result.curve.minDelayMs() << " .. " << o.result.curve.maxDelayMs() << "), "
                          << (o.result.curve.polarity < 0 ? "inverted, " : "") << "quality " << o.result.before.mean << " -> " << o.result.after.mean << std::endl;
            juce::String e;
            auto files = exportTake (take, out, opt, e);
            if (e.isNotEmpty()) std::cout << "  " << e << std::endl;
            for (auto& f : files) std::cout << "  -> " << f.getFullPathName() << std::endl;
        }
        return 0;
    }

    // Parameter tuning: synthetic cases (delay error) and real boom/lav pairs listed in a file, one
    // "boom.wav[:ch]|lav.wav[:ch]" per line (sum quality with time alignment only, i.e. no phase correction).
    int runTune (const juce::StringArray& args)
    {
        Settings s;
        s.phaseCorrection = 0.0f;
        juce::File list;
        double maxSeconds = 240;
        bool synth = true;
        for (int i = 1; i < args.size(); ++i)
        {
            const auto key = args[i].upToFirstOccurrenceOf ("=", false, false);
            const double v = args[i].fromFirstOccurrenceOf ("=", false, false).getDoubleValue();
            if (key == "smooth") s.smoothingMs = v;
            else if (key == "context") s.contextMs = v;
            else if (key == "segphase") s.phaseBySegment = v > 0;
            else if (key == "phase") s.phaseCorrection = (float) v;
            else if (key == "range") s.maxOffsetMs = v;
            else if (key == "minfreq") s.minFreq = v;
            else if (key == "maxfreq") s.maxFreq = v;
            else if (key == "steps") s.allowSteps = v > 0;
            else if (key == "seconds") maxSeconds = v;
            else if (key == "nosynth") synth = false;
            else if (key == "real") list = juce::File::getCurrentWorkingDirectory().getChildFile (args[i].fromFirstOccurrenceOf ("=", false, false));
        }
        double sumRms = 0, sumQ = 0; int nSynth = 0;
        if (synth)
        {
            const double fs = 48000;
            const auto src = speechLike (fs, 30.0, 1);
            const std::vector<std::function<double (double)>> cases {
                [] (double) { return 1.6; },
                [] (double t) { return 1.8 + 1.2 * std::sin (2 * pi * t / 7) + 0.4 * std::sin (2 * pi * t / 2.3 + 1); },
                [] (double t) { return 1.0 + 4.0 * (0.5 - 0.5 * std::cos (2 * pi * t / 10)); },
                [] (double t) { return (t < 15 ? 1.2 : 3.4) + 0.02 * t; },
                [] (double t) { return -2.9 + 0.8 * std::sin (2 * pi * t / 9); },
                [] (double t) { return 1.5 + 1.2 * std::sin (2 * pi * t / 3.0); },  // fast head movement (2.5 ms/s)
            };
            unsigned seed = 10;
            for (auto& c : cases)
            {
                for (double noise : { -45.0, -25.0 })
                {
                    const auto pair = makePair (src, fs, c, false, seed++, 0.5, noise);
                    const auto r = runCase (pair, fs, s);
                    sumRms += r.rmsUs; sumQ += r.after.mean; ++nSynth;
                    std::cout << "synthetic " << nSynth << (r.pol < 0 ? " INVERTED" : "") << ": rms " << juce::String (r.rmsUs, 1) << " us, p95 " << juce::String (r.p95Us, 1)
                              << " us, quality " << juce::String (r.after.mean, 3) << std::endl;
                }
            }
            std::cout << "SYNTH mean rms " << juce::String (sumRms / nSynth, 1) << " us, mean quality " << juce::String (sumQ / nSynth, 3) << std::endl;
        }
        if (list.existsAsFile())
        {
            juce::StringArray lines;
            list.readLines (lines);
            double q = 0; int n = 0;
            for (auto& line : lines)
            {
                if (line.trim().isEmpty() || line.startsWith ("#")) continue;
                juce::File bf, lf; int bc, lc;
                if (! parseSpec (line.upToFirstOccurrenceOf ("|", false, false).trim(), bf, bc)
                    || ! parseSpec (line.fromFirstOccurrenceOf ("|", false, false).trim(), lf, lc))
                { std::cout << "missing: " << line << std::endl; continue; }
                AudioFileInfo bi, li;
                readInfo (bf, bi); readInfo (lf, li);
                std::vector<std::vector<float>> b, l;
                readChannels (bi, { bc }, b); readChannels (li, { lc }, l);
                const auto len = (size_t) std::min ({ b[0].size(), l[0].size(), (size_t) (maxSeconds * li.sampleRate) });
                b[0].resize (len); l[0].resize (len);
                LavResult r;
                alignLav (b[0].data(), l[0].data(), (int64_t) len, li.sampleRate, s, r);
                q += r.after.mean; ++n;
                std::cout << "real " << lf.getFileName() << ":" << (lc + 1) << ": " << juce::String (r.curve.medianDelayMs(), 3) << " ms"
                          << (r.curve.polarity < 0 ? " inv" : "") << " steps " << (int) r.curve.steps.size() << ", quality "
                          << juce::String (r.before.mean, 3) << " -> " << juce::String (r.after.mean, 3) << std::endl;
            }
            if (n > 0) std::cout << "REAL mean quality " << juce::String (q / n, 4) << " over " << n << std::endl;
        }
        return 0;
    }

    // Batch evaluation of many short pairs ("boom.wav[:ch]|lav.wav[:ch]" per line), several pairs at a time.
    // One CSV row per pair: delay found, polarity, and the sum quality untreated, with one fixed offset (the
    // median delay), with the tracked delay, and with tracking + phase correction.
    int runBatchEval (const juce::StringArray& args)
    {
        if (args.size() < 3) { std::cout << "usage: --batch-eval <pairs.txt> <out.csv> [smooth=ms] [context=ms]" << std::endl; return 2; }
        juce::StringArray lines;
        juce::File::getCurrentWorkingDirectory().getChildFile (args[1]).readLines (lines);
        lines.removeEmptyStrings();
        Settings s;
        s.threads = 1;
        for (int i = 3; i < args.size(); ++i)
        {
            const auto key = args[i].upToFirstOccurrenceOf ("=", false, false);
            const double v = args[i].fromFirstOccurrenceOf ("=", false, false).getDoubleValue();
            if (key == "smooth") s.smoothingMs = v;
            else if (key == "context") s.contextMs = v;
            else if (key == "segphase") s.phaseBySegment = v > 0;
        }
        std::vector<juce::String> rows ((size_t) lines.size());
        std::atomic<int> next { 0 }, done { 0 };
        auto worker = [&]
        {
            for (int i; (i = next++) < lines.size();)
            {
                const auto& line = lines[i];
                juce::File bf, lf; int bc, lc;
                if (line.startsWith ("#") || ! parseSpec (line.upToFirstOccurrenceOf ("|", false, false).trim(), bf, bc)
                    || ! parseSpec (line.fromFirstOccurrenceOf ("|", false, false).trim(), lf, lc))
                { ++done; continue; }
                AudioFileInfo bi, li;
                readInfo (bf, bi); readInfo (lf, li);
                std::vector<std::vector<float>> b, l;
                if (readChannels (bi, { bc }, b).isNotEmpty() || readChannels (li, { lc }, l).isNotEmpty()) { ++done; continue; }
                const auto n = (int64_t) std::min (b[0].size(), l[0].size());
                const double fs = li.sampleRate;
                const auto before = measureQuality (b[0].data(), l[0].data(), n, fs);
                const auto curve = analyse (b[0].data(), l[0].data(), n, fs, s);
                std::vector<float> y ((size_t) n);
                // one fixed offset: the median delay, same polarity
                DelayCurve fixed = curve;
                std::fill (fixed.delay.begin(), fixed.delay.end(), (float) (curve.medianDelayMs() * fs / 1000.0));
                fixed.steps.clear();
                renderAligned (l[0].data(), y.data(), n, fixed);
                const auto qFixed = measureQuality (b[0].data(), y.data(), n, fs);
                renderAligned (l[0].data(), y.data(), n, curve);
                const auto qTrack = measureQuality (b[0].data(), y.data(), n, fs);
                if (curve.found) correctPhase (b[0].data(), y.data(), n, fs, 1.0f, {}, s.phaseBySegment ? curve.stepSamples() : std::vector<int64_t> {});
                const auto qPhase = measureQuality (b[0].data(), y.data(), n, fs);
                double lavRms = 0;
                for (int64_t k = 0; k < n; ++k) lavRms += (double) l[0][(size_t) k] * l[0][(size_t) k];
                lavRms = 10 * std::log10 (lavRms / std::max<int64_t> (1, n) + 1e-30);
                juce::String row;
                row << lf.getParentDirectory().getFileName() << "," << lf.getFileNameWithoutExtension() << ","
                    << juce::String ((double) n / fs, 2) << "," << juce::String (lavRms, 1) << "," << (curve.found ? 1 : 0) << ","
                    << (curve.polarity < 0 ? -1 : 1) << "," << juce::String (curve.medianDelayMs(), 4) << ","
                    << juce::String (curve.minDelayMs(), 4) << "," << juce::String (curve.maxDelayMs(), 4) << ","
                    << (int) curve.steps.size() << "," << curve.confidentFrames << ","
                    << juce::String (before.mean, 4) << "," << juce::String (qFixed.mean, 4) << ","
                    << juce::String (qTrack.mean, 4) << "," << juce::String (qPhase.mean, 4);
                rows[(size_t) i] = row;
                const int d = ++done;
                if (d % 250 == 0) std::cout << d << " / " << lines.size() << std::endl;
            }
        };
        std::vector<std::thread> pool;
        const int nt = std::max (1, (int) std::thread::hardware_concurrency());
        for (int i = 0; i < nt; ++i) pool.emplace_back (worker);
        for (auto& t : pool) t.join();

        juce::MemoryOutputStream csv;
        csv << "talker,file,seconds,lav_db,found,polarity,delay_ms,min_ms,max_ms,steps,confident,q_before,q_fixed,q_track,q_phase\n";
        for (auto& r : rows) if (r.isNotEmpty()) csv << r << "\n";
        juce::File::getCurrentWorkingDirectory().getChildFile (args[2]).replaceWithData (csv.getData(), csv.getDataSize());
        std::cout << "wrote " << args[2] << std::endl;
        return 0;
    }

    int runBench()
    {
        const double fs = 48000;
        const auto src = speechLike (fs, 300.0, 5);
        const auto pair = makePair (src, fs, [] (double t) { return 2.0 + std::sin (t / 5); }, false, 1, 0.0);
        auto now = [] { return std::chrono::steady_clock::now(); };
        auto secsSince = [&] (auto t) { return std::chrono::duration<double> (now() - t).count(); };
        const auto n = (int64_t) pair.lav.size();
        auto t0 = now();
        Settings s;
        auto curve = analyse (pair.boom.data(), pair.lav.data(), n, fs, s);
        const double ta = secsSince (t0);
        t0 = now();
        std::vector<float> out ((size_t) n);
        renderAligned (pair.lav.data(), out.data(), n, curve);
        const double tr = secsSince (t0);
        t0 = now();
        correctPhase (pair.boom.data(), out.data(), n, fs, 1.0f);
        const double tp = secsSince (t0);
        t0 = now();
        measureQuality (pair.boom.data(), out.data(), n, fs);
        const double tq = secsSince (t0);
        const double secs = ta + tr + tp;
        std::cout << "5 min take, one lav: " << secs << " s (" << 300.0 / secs << "x real time): analyse " << ta << " s, render " << tr
                  << " s, phase " << tp << " s (quality measurement " << tq << " s)" << std::endl;
        return 0;
    }
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    lavalign::requestFullSpeed();
    juce::StringArray args;
    for (int i = 1; i < argc; ++i) args.add (juce::String::fromUTF8 (argv[i]));

    if (args[0] == "--align")   return runAlign (args);
    if (args[0] == "--process") return runProcess (args);
    if (args[0] == "--bench")   return runBench();
    if (args[0] == "--tune")    return runTune (args);
    if (args[0] == "--batch-eval") return runBatchEval (args);
#if LAVALIGN_TESTS_WITH_GUI
    if (args[0] == "--snapshot") return lavalign::renderSnapshots (juce::File::getCurrentWorkingDirectory().getChildFile (args[1]));
#endif

    const auto only = args.indexOf ("--only") >= 0 ? args[args.indexOf ("--only") + 1] : juce::String();
    if (only.isEmpty() || only == "render")  testRenderExact();
    if (only.isEmpty() || only == "wav")     testWavIo();
    if (only.isEmpty() || only == "session") testSession();
    if (only.isEmpty() || only == "phase")   testPhaseCorrection();
    if (only.isEmpty() || only == "align")   testAlignment();
    std::cout << (failures == 0 ? "All tests passed" : juce::String (failures) + " test(s) failed") << std::endl;
    return failures == 0 ? 0 : 1;
}
