#include "Align.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <deque>
#include <limits>
#include <memory>
#include <numeric>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace lavalign
{
namespace
{
    constexpr double pi = 3.14159265358979323846;
    using cfloat = std::complex<float>;

    int orderFor (double size) { return std::max (6, (int) std::lround (std::log2 (size))); }

    std::vector<float> periodicHann (int n)
    {
        std::vector<float> w ((size_t) n);
        for (int i = 0; i < n; ++i)
            w[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * pi * i / n));
        return w;
    }

    // Symmetric smoothing weights for +-k frames (a Hann bump, like numpy's hanning(2k+3)[1:-1]), summing to 1.
    std::vector<float> bumpWeights (int k)
    {
        std::vector<float> w ((size_t) (2 * k + 1));
        double sum = 0;
        for (int i = 0; i < 2 * k + 1; ++i)
        {
            w[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * pi * (i + 1) / (2 * k + 2)));
            sum += w[(size_t) i];
        }
        for (auto& v : w) v = (float) (v / sum);
        return w;
    }

    // Real FFT of size n = 2^order with an interleaved complex buffer of n floats * 2.
    struct RealFFT
    {
        explicit RealFFT (int order) : fft (order), n (1 << order), buf ((size_t) (2 << order)) {}

        void forward()  { fft.performRealOnlyForwardTransform (buf.data(), true); }
        void inverse()  { fft.performRealOnlyInverseTransform (buf.data()); }  // includes 1/n
        cfloat* bins()  { return reinterpret_cast<cfloat*> (buf.data()); }
        float*  data()  { return buf.data(); }
        void clear()    { std::fill (buf.begin(), buf.end(), 0.0f); }

        juce::dsp::FFT fft;
        int n;
        std::vector<float> buf;
    };

    // Windowed copy of x[start .. start + win) (zero outside 0..numSamples) into the FFT buffer, zero padded.
    void loadFrame (RealFFT& f, const float* x, int64_t numSamples, int64_t start, const std::vector<float>& window)
    {
        f.clear();
        const int win = (int) window.size();
        const int64_t a = std::max<int64_t> (0, -start), b = std::min<int64_t> (win, numSamples - start);
        for (int64_t i = a; i < b; ++i)
            f.buf[(size_t) i] = x[start + i] * window[(size_t) i];
    }

    // Like loadFrame, reading x[n - shift] (an integer pre-shift of the lav).
    void loadFrameShifted (RealFFT& f, const float* x, int64_t numSamples, int64_t start, int64_t shift,
                           const std::vector<float>& window)
    {
        f.clear();
        const int win = (int) window.size();
        const int64_t a = std::max<int64_t> (0, shift - start), b = std::min<int64_t> (win, numSamples + shift - start);
        for (int64_t i = a; i < b; ++i)
            f.buf[(size_t) i] = x[start + i - shift] * window[(size_t) i];
    }

    // Solves (W + lambda D2'D2) z = W y (Whittaker smoother) with a banded Cholesky factorisation.
    std::vector<double> whittaker (const std::vector<double>& y, const std::vector<double>& w, double lambda)
    {
        const size_t n = y.size();
        std::vector<double> z (n);

        if (n < 4)
        {
            double sw = 0, swy = 0;
            for (size_t i = 0; i < n; ++i) { sw += w[i]; swy += w[i] * y[i]; }
            std::fill (z.begin(), z.end(), sw > 0 ? swy / sw : 0.0);
            return z;
        }

        std::vector<double> a0 (n, 6.0), a1 (n, -4.0), a2 (n, 1.0);
        a0[0] = a0[n - 1] = 1.0; a0[1] = a0[n - 2] = 5.0;
        a1[0] = a1[n - 2] = -2.0;
        for (size_t i = 0; i < n; ++i) { a0[i] = w[i] + lambda * a0[i]; a1[i] *= lambda; a2[i] *= lambda; }

        std::vector<double> l0 (n), l1 (n, 0.0), l2 (n, 0.0);
        for (size_t i = 0; i < n; ++i)
        {
            double d = a0[i];
            if (i >= 1) d -= l1[i - 1] * l1[i - 1];
            if (i >= 2) d -= l2[i - 2] * l2[i - 2];
            l0[i] = std::sqrt (std::max (d, 1e-300));
            if (i + 1 < n) l1[i] = (a1[i] - (i >= 1 ? l2[i - 1] * l1[i - 1] : 0.0)) / l0[i];
            if (i + 2 < n) l2[i] = a2[i] / l0[i];
        }

        std::vector<double> u (n);
        for (size_t i = 0; i < n; ++i)
        {
            double v = w[i] * y[i];
            if (i >= 1) v -= l1[i - 1] * u[i - 1];
            if (i >= 2) v -= l2[i - 2] * u[i - 2];
            u[i] = v / l0[i];
        }
        for (size_t k = n; k-- > 0;)
        {
            double v = u[k];
            if (k + 1 < n) v -= l1[k] * z[k + 1];
            if (k + 2 < n) v -= l2[k] * z[k + 2];
            z[k] = v / l0[k];
        }
        return z;
    }

    // Frame-wise generalised cross-correlation between boom and the (pre-shifted) lav.
    // Each frame's cross-spectrum is averaged over +-contextFrames neighbours, weighted per bin by
    // coherence (gamma^2 / (1 - gamma^2), the maximum-likelihood GCC weighting), so bins where the lav and
    // the boom don't hear the same thing (clothing noise, room reverb, other sources) count little.
    class FrameCorrelator
    {
    public:
        FrameCorrelator (const float* boomIn, const float* lavIn, int64_t n, double fs, int64_t lavShift,
                         const Settings& s, int maxLagSamples)
            : boom (boomIn), lav (lavIn), numSamples (n), shift (lavShift)
        {
            win = 1 << orderFor (0.043 * fs);
            hop = win / 4;
            nfftOrder = orderFor (win) + 1;
            nfft = 1 << nfftOrder;
            fwdB = std::make_unique<RealFFT> (nfftOrder);
            fwdL = std::make_unique<RealFFT> (nfftOrder);
            inv  = std::make_unique<RealFFT> (nfftOrder + 1);  // 2x zero padded: half-sample lag grid
            window = periodicHann (win);

            const double binHz = fs / nfft;
            k0 = std::max (1, (int) std::ceil (s.minFreq / binHz));
            k1 = std::min (nfft / 2, (int) std::floor (std::min (s.maxFreq, 0.45 * fs) / binHz) + 1);
            numBand = std::max (1, k1 - k0);

            context = std::max (1, (int) std::lround (s.contextMs * 1e-3 * fs / hop));
            ctxWeights = bumpWeights (context);

            maxLag = maxLagSamples;
            numLags = 4 * maxLag + 1;  // half-sample grid
            numFrames = (int) (numSamples / hop) + 1;
            ring.resize ((size_t) (2 * context + 1));
            for (auto& r : ring) r.assign ((size_t) numBand, {});
            ringB.resize (ring.size()); for (auto& r : ringB) r.assign ((size_t) numBand, 0.0f);
            ringL.resize (ring.size()); for (auto& r : ringL) r.assign ((size_t) numBand, 0.0f);
        }

        int frames() const   { return numFrames; }
        int hopSize() const  { return hop; }
        int lags() const     { return numLags; }
        float lagOf (int i) const { return (float) (i - 2 * maxLag) * 0.5f; }  // samples

        // Computes frame k's correlation over the lag grid (normalised: ~1 for a clean delay) and its evidence 0..1.
        // Frames must be requested in increasing order (k, k+1, ...), starting anywhere.
        void correlate (int k, std::vector<float>& r, float& evidence)
        {
            // make sure frames k - context .. k + context are in the ring
            const int need = k + context;
            if (nextRaw > need || nextRaw < k - context)
                nextRaw = k - context;
            for (; nextRaw <= need; ++nextRaw)
                computeRaw (nextRaw);

            std::vector<cfloat>& acc = sumS;
            acc.assign ((size_t) numBand, {});
            sumB.assign ((size_t) numBand, 0.0f);
            sumL.assign ((size_t) numBand, 0.0f);
            for (int j = -context; j <= context; ++j)
            {
                const int f = k + j;
                if (f < 0 || f >= numFrames) continue;
                const float wj = ctxWeights[(size_t) (j + context)];
                const size_t slot = (size_t) (f % (int) ring.size());
                const auto& rs = ring[slot]; const auto& rb = ringB[slot]; const auto& rl = ringL[slot];
                for (int b = 0; b < numBand; ++b)
                {
                    acc[(size_t) b] += wj * rs[(size_t) b];
                    sumB[(size_t) b] += wj * rb[(size_t) b];
                    sumL[(size_t) b] += wj * rl[(size_t) b];
                }
            }

            inv->clear();
            auto* g = inv->bins();
            double mass = 0;
            for (int b = 0; b < numBand; ++b)
            {
                const cfloat s = acc[(size_t) b];
                const float mag2 = std::norm (s);
                const float pp = sumB[(size_t) b] * sumL[(size_t) b];
                if (mag2 <= 0.0f || pp <= 0.0f) continue;
                const float coh = std::min (mag2 / pp, 0.999999f);
                const float psi = std::min (coh / (1.0f - coh + 1e-6f), 30.0f);
                mass += psi;
                g[k0 + b] = s * (psi / std::sqrt (mag2));
            }
            inv->inverse();

            const float scale = mass > 0 ? (float) (inv->n * 0.5 / mass) : 0.0f;
            r.resize ((size_t) numLags);
            const int nUp = inv->n;
            for (int i = 0; i < numLags; ++i)
            {
                const int lagIdx = i - 2 * maxLag;
                r[(size_t) i] = inv->buf[(size_t) ((lagIdx + nUp) % nUp)] * scale;
            }
            evidence = (float) std::min (mass / (0.02 * numBand), 1.0);
        }

    private:
        void computeRaw (int f)
        {
            const size_t slot = (size_t) (((f % (int) ring.size()) + (int) ring.size()) % (int) ring.size());
            auto& rs = ring[slot]; auto& rb = ringB[slot]; auto& rl = ringL[slot];
            if (f < 0 || f >= numFrames)
            {
                std::fill (rs.begin(), rs.end(), cfloat {});
                std::fill (rb.begin(), rb.end(), 0.0f);
                std::fill (rl.begin(), rl.end(), 0.0f);
                return;
            }
            const int64_t start = (int64_t) f * hop - win / 2;
            loadFrame (*fwdB, boom, numSamples, start, window);
            loadFrameShifted (*fwdL, lav, numSamples, start, shift, window);
            fwdB->forward(); fwdL->forward();
            const cfloat* B = fwdB->bins(); const cfloat* L = fwdL->bins();
            for (int b = 0; b < numBand; ++b)
            {
                const cfloat x = B[k0 + b], y = L[k0 + b];
                rs[(size_t) b] = x * std::conj (y);
                rb[(size_t) b] = std::norm (x);
                rl[(size_t) b] = std::norm (y);
            }
        }

        const float* boom; const float* lav;
        int64_t numSamples, shift;
        int win = 0, hop = 0, nfft = 0, nfftOrder = 0, k0 = 0, k1 = 0, numBand = 0, context = 0;
        int maxLag = 0, numLags = 0, numFrames = 0, nextRaw = std::numeric_limits<int>::min();
        std::vector<float> window, ctxWeights;
        std::unique_ptr<RealFFT> fwdB, fwdL, inv;
        std::vector<std::vector<cfloat>> ring;
        std::vector<std::vector<float>> ringB, ringL;
        std::vector<cfloat> sumS;
        std::vector<float> sumB, sumL;
    };

    // Constant offset over the whole file (wireless latency, files synced by timecode). Each segment gets its
    // own PHAT-weighted correlation and their magnitudes are summed: when the delay moves during the take the
    // segments' peaks still land close together, whereas one correlation of the whole file would smear them.
    struct Coarse { double offset = 0; float polarity = 1; float confidence = 0; };

    Coarse findCoarseOffset (const float* boom, const float* lav, int64_t n, double fs, const Settings& s,
                             const Progress& progress)
    {
        const double rangeSamples = std::max (1.0, s.maxOffsetMs * 1e-3 * fs);
        const int segOrder = orderFor (std::max (4.0 * rangeSamples, 0.17 * fs) * 1.4142);
        const int seg = 1 << segOrder, hop = seg / 2;
        RealFFT fb (segOrder + 1), fl (segOrder + 1), inv (segOrder + 1);
        const auto window = periodicHann (seg);
        const double binHz = fs / (2 * seg);
        const int k0 = std::max (1, (int) std::ceil (std::max (s.minFreq, 150.0) / binHz));
        const int k1 = std::min (seg, (int) (std::min (std::min (s.maxFreq, 6000.0), 0.45 * fs) / binHz));
        const int range = (int) std::ceil (rangeSamples);
        std::vector<double> acc ((size_t) (2 * range + 1), 0.0), signedAcc ((size_t) (2 * range + 1), 0.0);
        const int nInv = inv.n;

        const int64_t numSeg = std::max<int64_t> (1, (n - seg) / hop + 2);
        int used = 0;
        for (int64_t i = 0; i < numSeg; ++i)
        {
            const int64_t start = i * hop - seg / 2;
            loadFrame (fb, boom, n, start, window); loadFrame (fl, lav, n, start, window);
            fb.forward(); fl.forward();
            const cfloat* B = fb.bins(); const cfloat* L = fl.bins();
            inv.clear();
            auto* g = inv.bins();
            double mass = 0;
            for (int k = k0; k < k1; ++k)
            {
                const cfloat x = B[k] * std::conj (L[k]);
                const float m = std::abs (x);
                if (m <= 1e-30f) continue;
                const float w = std::pow (m, -0.8f);  // PHAT-beta
                g[k] = x * w;
                mass += m * w;
            }
            if (mass <= 0) continue;
            inv.inverse();
            const double scale = nInv * 0.5 / mass;
            for (int lag = -range; lag <= range; ++lag)
            {
                const double v = inv.buf[(size_t) ((lag + nInv) % nInv)] * scale;
                acc[(size_t) (lag + range)] += std::abs (v);
                signedAcc[(size_t) (lag + range)] += v;
            }
            ++used;
            if (progress && (i & 15) == 0 && ! progress ((float) i / (float) numSeg))
                return {};
        }
        if (used == 0) return {};

        // smooth over +-0.1 ms, prefer small offsets a little
        const int sm = std::max (1, (int) (1e-4 * fs));
        int best = 0; double bestVal = -1;
        for (int lag = -range; lag <= range; ++lag)
        {
            double v = 0; int cnt = 0;
            for (int j = std::max (-range, lag - sm); j <= std::min (range, lag + sm); ++j) { v += acc[(size_t) (j + range)]; ++cnt; }
            v = v / cnt * (1.0 - 0.05 * std::abs (lag) / (double) range);
            if (v > bestVal) { bestVal = v; best = lag; }
        }
        // the exact peak inside the smoothing window
        int peak = best;
        for (int j = std::max (-range, best - sm); j <= std::min (range, best + sm); ++j)
            if (acc[(size_t) (j + range)] > acc[(size_t) (peak + range)]) peak = j;

        Coarse c;
        c.offset = peak;
        c.polarity = signedAcc[(size_t) (peak + range)] < 0 ? -1.0f : 1.0f;
        c.confidence = (float) (acc[(size_t) (peak + range)] / used);
        return c;
    }

    struct SincTable
    {
        static constexpr int half = 32;      // taps -31 .. 32
        static constexpr int taps = 2 * half;
        static constexpr int phases = 1024;
        std::vector<float> t;                // (phases + 1) x taps

        SincTable()
        {
            const double beta = 9.0;
            auto i0 = [] (double x)
            {
                double sum = 1, term = 1, y = x * x / 4;
                for (int k = 1; k < 40; ++k) { term *= y / ((double) k * k); sum += term; }
                return sum;
            };
            const double norm = i0 (beta);
            t.resize ((size_t) ((phases + 1) * taps));
            for (int p = 0; p <= phases; ++p)
            {
                const double frac = (double) p / phases;
                for (int j = 0; j < taps; ++j)
                {
                    const double x = (j - half + 1) - frac;  // tap offset relative to the read position
                    double v;
                    if (std::abs (x) < 1e-12) v = 1.0;
                    else if (p == 0 || p == phases) v = 0.0;  // integer positions: a pure impulse
                    else v = std::sin (pi * x) / (pi * x);
                    const double u = x / half;
                    const double win = std::abs (u) >= 1.0 ? 0.0 : i0 (beta * std::sqrt (1.0 - u * u)) / norm;
                    t[(size_t) (p * taps + j)] = (float) (v * win);
                }
            }
        }

        // x at fractional position pos (x has numSamples samples, zero outside)
        float read (const float* x, int64_t numSamples, double pos) const
        {
            const double fl = std::floor (pos);
            const int64_t i0 = (int64_t) fl;
            const double fracPhase = (pos - fl) * phases;
            const int p = std::min ((int) fracPhase, phases - 1);
            const float a = (float) (fracPhase - p);
            const float* r0 = &t[(size_t) (p * taps)];
            const float* r1 = r0 + taps;
            const int64_t first = i0 - half + 1;
            float acc = 0.0f;
            if (first >= 0 && first + taps <= numSamples)
            {
                const float* xs = x + first;
                for (int j = 0; j < taps; ++j)
                    acc += xs[j] * (r0[j] + a * (r1[j] - r0[j]));
            }
            else
            {
                for (int j = 0; j < taps; ++j)
                {
                    const int64_t idx = first + j;
                    if (idx >= 0 && idx < numSamples)
                        acc += x[idx] * (r0[j] + a * (r1[j] - r0[j]));
                }
            }
            return acc;
        }
    };

    const SincTable& sincTable()
    {
        static const SincTable table;
        return table;
    }

    float median (std::vector<float> v)
    {
        if (v.empty()) return 0.0f;
        auto mid = v.begin() + (std::ptrdiff_t) (v.size() / 2);
        std::nth_element (v.begin(), mid, v.end());
        return *mid;
    }
}

//==============================================================================
double DelayCurve::delayAtSample (double pos) const
{
    if (delay.empty()) return 0.0;
    const double f = (pos - firstFrameCentre) / hop;
    // segment containing frame round(f): interpolate only inside it
    const int nf = (int) delay.size();
    const int k = std::clamp ((int) std::floor (f), 0, nf - 1);
    int segStart = 0, segEnd = nf;
    for (int s : steps)
    {
        if (s <= k) segStart = s;
        else { segEnd = s; break; }
    }
    if (f <= segStart) return delay[(size_t) segStart];
    if (f >= segEnd - 1) return delay[(size_t) (segEnd - 1)];
    const int a = (int) std::floor (f);
    const double t = f - a;
    return delay[(size_t) a] + t * (delay[(size_t) a + 1] - delay[(size_t) a]);
}

float DelayCurve::medianDelayMs() const
{
    std::vector<float> v;
    for (size_t i = 0; i < delay.size(); ++i)
        if (weight[i] > 0.05f) v.push_back (delay[i]);
    if (v.empty()) v = delay;
    return (float) (median (v) * 1000.0 / sampleRate);
}

float DelayCurve::minDelayMs() const
{
    float m = std::numeric_limits<float>::max();
    for (size_t i = 0; i < delay.size(); ++i) if (weight[i] > 0.05f) m = std::min (m, delay[i]);
    return m == std::numeric_limits<float>::max() ? medianDelayMs() : (float) (m * 1000.0 / sampleRate);
}

float DelayCurve::maxDelayMs() const
{
    float m = std::numeric_limits<float>::lowest();
    for (size_t i = 0; i < delay.size(); ++i) if (weight[i] > 0.05f) m = std::max (m, delay[i]);
    return m == std::numeric_limits<float>::lowest() ? medianDelayMs() : (float) (m * 1000.0 / sampleRate);
}

//==============================================================================
DelayCurve analyse (const float* boom, const float* lav, int64_t n, double fs, const Settings& s,
                    const Progress& progress)
{
    DelayCurve curve;
    curve.sampleRate = fs;

    auto sub = [&progress] (float a, float b) -> Progress
    {
        if (! progress) return {};
        return [&progress, a, b] (float p) { return progress (a + (b - a) * p); };
    };

    auto tStart = std::chrono::steady_clock::now();
    auto lap = [&tStart] (const char* what)
    {
        if (std::getenv ("LAVALIGN_TIMING") == nullptr) return;
        const auto now = std::chrono::steady_clock::now();
        std::fprintf (stderr, "%s %.3f s\n", what, std::chrono::duration<double> (now - tStart).count());
        tStart = now;
    };
    // 1. constant offset + polarity
    const Coarse coarse = findCoarseOffset (boom, lav, n, fs, s, sub (0.0f, 0.15f));
    if (progress && ! progress (0.15f)) return curve;
    curve.coarseOffset = coarse.offset;
    curve.coarseConfidence = coarse.confidence;
    curve.polarity = s.detectPolarity ? coarse.polarity : 1.0f;
    const auto shift = (int64_t) coarse.offset;

    const int maxLag = (int) std::ceil (s.trackRangeMs * 1e-3 * fs);
    const int numThreads = std::max (1, s.threads > 0 ? s.threads : (int) std::thread::hardware_concurrency());
    // one correlator per thread (each keeps its own ring of neighbouring frames)
    std::vector<std::unique_ptr<FrameCorrelator>> correlators;
    for (int i = 0; i < numThreads; ++i)
        correlators.push_back (std::make_unique<FrameCorrelator> (boom, lav, n, fs, shift, s, maxLag));
    const auto& corr = *correlators.front();
    const int T = corr.frames();
    const int hop = corr.hopSize();
    curve.hop = hop;
    curve.firstFrameCentre = 0.0;
    const int numLags = corr.lags();

    // Runs fn (correlator, firstFrame, endFrame) over [begin, end) split between the threads.
    auto parallelFrames = [&] (int begin, int end, const std::function<void (FrameCorrelator&, int, int)>& fn)
    {
        const int count = end - begin;
        const int used = std::max (1, std::min (numThreads, count / 64));
        if (used == 1) { fn (*correlators[0], begin, end); return; }
        std::vector<std::thread> pool;
        for (int i = 0; i < used; ++i)
        {
            const int a = begin + (int) ((int64_t) count * i / used), e = begin + (int) ((int64_t) count * (i + 1) / used);
            pool.emplace_back ([&fn, &correlators, i, a, e] { fn (*correlators[(size_t) i], a, e); });
        }
        for (auto& th : pool) th.join();
    };
    const int blockFrames = 2048;  // frames analysed in parallel between progress / cancel checks

    lap ("coarse+setup");
    // Polarity: evidence-weighted vote of each frame's strongest correlation peak (every third frame). More
    // reliable than the whole-file correlation, which smears when the delay moves.
    if (s.detectPolarity)
    {
        std::vector<double> votes ((size_t) numThreads, 0.0);
        const int numPicks = (T + 2) / 3;
        for (int p0 = 0; p0 < numPicks; p0 += blockFrames)
        {
            const int p1 = std::min (numPicks, p0 + blockFrames);
            std::atomic<int> slot { 0 };
            parallelFrames (p0, p1, [&] (FrameCorrelator& c, int a, int e)
            {
                const int me = slot++;
                std::vector<float> r;
                double v = 0;
                for (int i = a; i < e; ++i)
                {
                    const int t = i * 3;
                    if (t >= T) break;
                    float evid = 0;
                    c.correlate (t, r, evid);
                    size_t k = 0;
                    for (size_t j = 1; j < r.size(); ++j) if (std::abs (r[j]) > std::abs (r[k])) k = j;
                    v += (double) evid * r[k];
                }
                votes[(size_t) me] += v;
            });
            if (progress && ! progress (0.15f + 0.1f * (float) p1 / (float) numPicks))
                return curve;
        }
        const double vote = std::accumulate (votes.begin(), votes.end(), 0.0);
        if (vote != 0) curve.polarity = vote < 0 ? -1.0f : 1.0f;
    }

    lap ("polarity");
    // 2. Viterbi over a coarse lag grid (cells of ~0.08 ms): maximise evidence-weighted correlation minus a
    //    cost per change of delay, or a fixed cost for a jump (cut / other talker). The frames' scores are
    //    computed in parallel, a block at a time; the Viterbi recursion itself runs in order.
    const int halfPerCell = std::max (1, (int) std::lround (0.08e-3 * fs * 2.0));  // half-samples per cell
    const int cells = (numLags + halfPerCell - 1) / halfPerCell;
    const double frameMs = hop * 1000.0 / fs;
    const float alphaPerCell = (float) (3.84 * (halfPerCell * 0.5 / fs * 1000.0) * (frameMs / 10.667));
    const float jumpCost = s.allowSteps ? 4.0f : 1e9f;

    std::vector<uint16_t> back ((size_t) T * (size_t) cells);
    std::vector<uint8_t> jumped ((size_t) T * (size_t) cells, 0);
    std::vector<float> D ((size_t) cells, 0.0f), best ((size_t) cells);
    std::vector<int> argL ((size_t) cells), argR ((size_t) cells);
    std::vector<float> left ((size_t) cells), right ((size_t) cells);
    std::vector<float> ev ((size_t) T, 0.0f);
    std::vector<float> blockScores ((size_t) blockFrames * (size_t) cells);
    const float pol = curve.polarity;

    for (int b0 = 0; b0 < T; b0 += blockFrames)
    {
        const int b1 = std::min (T, b0 + blockFrames);
        parallelFrames (b0, b1, [&] (FrameCorrelator& c, int a, int e)
        {
            std::vector<float> r;
            for (int t = a; t < e; ++t)
            {
                float evid = 0;
                c.correlate (t, r, evid);
                ev[(size_t) t] = evid;
                float* sc = &blockScores[(size_t) (t - b0) * (size_t) cells];
                for (int cc = 0; cc < cells; ++cc)
                {
                    float m = -std::numeric_limits<float>::max();
                    for (int j = cc * halfPerCell; j < std::min (numLags, (cc + 1) * halfPerCell); ++j)
                        m = std::max (m, pol * r[(size_t) j]);
                    sc[cc] = m * evid;
                }
            }
        });

        for (int t = b0; t < b1; ++t)
        {
            const float* score = &blockScores[(size_t) (t - b0) * (size_t) cells];
            uint16_t* bk = &back[(size_t) t * (size_t) cells];
            uint8_t* jp = &jumped[(size_t) t * (size_t) cells];
            if (t == 0)
            {
                for (int c = 0; c < cells; ++c) { D[(size_t) c] = score[c]; bk[c] = (uint16_t) c; }
                continue;
            }
            // distance transform: best(c) = max_c' D(c') - alpha |c - c'|
            for (int c = 0; c < cells; ++c)
            {
                if (c == 0 || D[(size_t) c] >= left[(size_t) c - 1] - alphaPerCell) { left[(size_t) c] = D[(size_t) c]; argL[(size_t) c] = c; }
                else { left[(size_t) c] = left[(size_t) c - 1] - alphaPerCell; argL[(size_t) c] = argL[(size_t) c - 1]; }
            }
            for (int c = cells - 1; c >= 0; --c)
            {
                if (c == cells - 1 || D[(size_t) c] >= right[(size_t) c + 1] - alphaPerCell) { right[(size_t) c] = D[(size_t) c]; argR[(size_t) c] = c; }
                else { right[(size_t) c] = right[(size_t) c + 1] - alphaPerCell; argR[(size_t) c] = argR[(size_t) c + 1]; }
            }
            const int cm = (int) (std::max_element (D.begin(), D.end()) - D.begin());
            const float jv = D[(size_t) cm] - jumpCost;
            for (int c = 0; c < cells; ++c)
            {
                float v; int a;
                if (left[(size_t) c] >= right[(size_t) c]) { v = left[(size_t) c]; a = argL[(size_t) c]; }
                else { v = right[(size_t) c]; a = argR[(size_t) c]; }
                if (jv > v && cm != a) { v = jv; a = cm; jp[c] = 1; }
                best[(size_t) c] = v;
                bk[c] = (uint16_t) a;
            }
            // keep D bounded (only differences matter)
            float mx = -std::numeric_limits<float>::max();
            for (int c = 0; c < cells; ++c) { D[(size_t) c] = best[(size_t) c] + score[c]; mx = std::max (mx, D[(size_t) c]); }
            for (auto& v : D) v -= mx;
        }
        if (progress && ! progress (0.25f + 0.3f * (float) b1 / (float) T))
            return curve;
    }
    blockScores.clear(); blockScores.shrink_to_fit();

    std::vector<int> path ((size_t) T);
    std::vector<int> steps;
    path[(size_t) T - 1] = (int) (std::max_element (D.begin(), D.end()) - D.begin());
    for (int t = T - 1; t > 0; --t)
    {
        const size_t idx = (size_t) t * (size_t) cells + (size_t) path[(size_t) t];
        if (jumped[idx]) steps.push_back (t);
        path[(size_t) t - 1] = back[idx];
    }
    std::reverse (steps.begin(), steps.end());
    back.clear(); back.shrink_to_fit(); jumped.clear(); jumped.shrink_to_fit();

    // A step is rendered as a 10 ms crossfade between two shifted copies of the lav, which can be heard as a
    // flam inside a word. The Viterbi puts it where the evidence switches; move it to the quietest moment of the
    // lav within +-150 ms (an edited dialogue track had 4 of 22 steps mid-speech, up to 8.5 ms apart).
    if (! steps.empty())
    {
        const int reach = std::max (1, (int) std::lround (0.15 * fs / hop));
        auto energyBefore = [&] (int t)  // the crossfade sits between frames t-1 and t
        {
            const int64_t c = (int64_t) t * hop;
            double e = 0;
            for (int64_t i = std::max<int64_t> (0, c - hop); i < std::min<int64_t> (n, c); ++i) e += (double) lav[i] * lav[i];
            return e;
        };
        for (size_t i = 0; i < steps.size(); ++i)
        {
            const int lo = std::max (i > 0 ? steps[i - 1] + 1 : 1, steps[i] - reach);
            const int hi = std::min (i + 1 < steps.size() ? steps[i + 1] - 1 : T - 1, steps[i] + reach);
            int best = steps[i];
            const double here = energyBefore (best);
            double bestE = here;
            for (int t = lo; t <= hi; ++t)
                if (const double e = energyBefore (t); e < bestE) { bestE = e; best = t; }
            if (bestE > here * 0.25) best = steps[i];  // not clearly quieter (6 dB): keep the step where it was
            // frames that change sides follow their new segment's path
            if (best > steps[i]) for (int t = steps[i]; t < best; ++t) path[(size_t) t] = path[(size_t) steps[i] - 1];
            else                 for (int t = best; t < steps[i]; ++t) path[(size_t) t] = path[(size_t) steps[i]];
            steps[i] = best;
        }
    }

    lap ("viterbi");
    // 3. refine: sub-sample peak near the path in every frame
    std::vector<double> meas ((size_t) T), wts ((size_t) T);
    const int radius = (int) (0.3e-3 * fs * 2.0) + halfPerCell;
    for (int b0 = 0; b0 < T; b0 += blockFrames * 2)
    {
        const int b1 = std::min (T, b0 + blockFrames * 2);
        parallelFrames (b0, b1, [&] (FrameCorrelator& c, int a, int e)
        {
            std::vector<float> r;
            for (int t = a; t < e; ++t)
            {
                float evid = 0;
                c.correlate (t, r, evid);
                const int centre = path[(size_t) t] * halfPerCell + halfPerCell / 2;
                const int lo = std::max (1, centre - radius), hi = std::min (numLags - 2, centre + radius);
                int k = lo;
                for (int j = lo; j <= hi; ++j) if (pol * r[(size_t) j] > pol * r[(size_t) k]) k = j;
                const double y0 = pol * r[(size_t) k - 1], y1 = pol * r[(size_t) k], y2 = pol * r[(size_t) k + 1];
                const double den = y0 - 2 * y1 + y2;
                const double dk = den < 0 ? std::clamp (0.5 * (y0 - y2) / den, -0.5, 0.5) : 0.0;
                meas[(size_t) t] = c.lagOf (k) + dk * 0.5;
                wts[(size_t) t] = std::max (y1, 0.0) * std::max (y1, 0.0) * evid;
            }
        });
        if (progress && ! progress (0.55f + 0.3f * (float) b1 / (float) T))
            return curve;
    }

    lap ("refine");
    // 4. smooth each segment (Whittaker); outside the first/last confident frame the delay holds
    const double framesPerSmooth = s.smoothingMs * 1e-3 * fs / hop;
    const double lambda = std::min (1e10, std::pow (framesPerSmooth, 4.0) / 16.0);
    std::vector<int> bounds { 0 };
    bounds.insert (bounds.end(), steps.begin(), steps.end());
    bounds.push_back (T);
    curve.delay.assign ((size_t) T, 0.0f);
    double totalWeight = 0;
    for (size_t b = 0; b + 1 < bounds.size(); ++b)
    {
        const int a = bounds[b], e = bounds[b + 1];
        std::vector<double> y (meas.begin() + a, meas.begin() + e), w (wts.begin() + a, wts.begin() + e);
        double sw = 0;
        for (auto& v : w) { sw += v; v += 1e-6; }
        totalWeight += sw;
        std::vector<double> z;
        if (sw < 1e-6)
        {
            std::vector<float> yf (y.begin(), y.end());
            z.assign (y.size(), median (yf));
        }
        else
            z = whittaker (y, w, lambda);

        int firstConf = -1, lastConf = -1;
        for (int i = 0; i < (int) w.size(); ++i)
            if (w[(size_t) i] > 0.05) { if (firstConf < 0) firstConf = i; lastConf = i; }
        for (int i = 0; i < (int) z.size(); ++i)
        {
            double v = z[(size_t) i];
            if (firstConf >= 0 && i < firstConf) v = z[(size_t) firstConf];
            if (lastConf >= 0 && i > lastConf) v = z[(size_t) lastConf];
            curve.delay[(size_t) (a + i)] = (float) (v + coarse.offset);
        }
    }

    curve.measured.resize ((size_t) T);
    curve.weight.resize ((size_t) T);
    for (int t = 0; t < T; ++t)
    {
        curve.measured[(size_t) t] = (float) (meas[(size_t) t] + coarse.offset);
        curve.weight[(size_t) t] = (float) wts[(size_t) t];
    }
    curve.steps = steps;
    // found: enough clearly correlated frames (~0.27 s). Unrelated signals only reach weights near 0.1 by chance;
    // booth silence (lav at -90 dBFS) reached 12-38 frames, every real speech file of the Zenodo set >= 73.
    int confident = 0;
    for (auto w : wts) if (w >= 0.2) ++confident;
    curve.confidentFrames = confident;
    curve.found = totalWeight > 0.5 && confident >= 25;
    if (! curve.found)
    {
        std::fill (curve.delay.begin(), curve.delay.end(), 0.0f);
        curve.polarity = 1.0f;
        curve.steps.clear();
    }
    if (progress) progress (1.0f);
    return curve;
}

//==============================================================================
void renderAligned (const float* lav, float* out, int64_t n, const DelayCurve& curve)
{
    const auto& table = sincTable();
    const float pol = curve.polarity;

    if (curve.delay.empty() || ! curve.found)
    {
        for (int64_t i = 0; i < n; ++i) out[i] = lav[i];
        return;
    }

    // Segment boundaries in samples; across a step both delays are read and crossfaded over 10 ms.
    const int64_t xfade = std::max<int64_t> (16, (int64_t) (0.010 * curve.sampleRate));
    std::vector<int> bounds { 0 };
    bounds.insert (bounds.end(), curve.steps.begin(), curve.steps.end());
    bounds.push_back ((int) curve.delay.size());
    const double hop = curve.hop, c0 = curve.firstFrameCentre;

    auto segDelay = [&] (size_t seg, double pos)
    {
        const int a = bounds[seg], e = bounds[seg + 1];
        const double f = (pos - c0) / hop;
        if (f <= a) return (double) curve.delay[(size_t) a];
        if (f >= e - 1) return (double) curve.delay[(size_t) e - 1];
        const int k = (int) std::floor (f);
        const double t = f - k;
        return curve.delay[(size_t) k] + t * (curve.delay[(size_t) k + 1] - curve.delay[(size_t) k]);
    };

    size_t seg = 0;
    for (int64_t i = 0; i < n; ++i)
    {
        const double pos = (double) i;
        while (seg + 2 < bounds.size() && pos >= c0 + (bounds[seg + 1] - 0.5) * hop + xfade / 2.0)
            ++seg;
        double y;
        // within a crossfade around the boundary to the next segment?
        const double bNext = seg + 2 < bounds.size() ? c0 + (bounds[seg + 1] - 0.5) * hop : 1e300;
        const double bPrev = seg > 0 ? c0 + (bounds[seg] - 0.5) * hop : -1e300;
        if (pos > bNext - xfade / 2.0)
        {
            const double g = (pos - (bNext - xfade / 2.0)) / (double) xfade;
            const double ya = table.read (lav, n, pos - segDelay (seg, pos));
            const double yb = table.read (lav, n, pos - segDelay (seg + 1, pos));
            y = ya + g * (yb - ya);
        }
        else if (pos < bPrev + xfade / 2.0)
        {
            const double g = (pos - (bPrev - xfade / 2.0)) / (double) xfade;
            const double ya = table.read (lav, n, pos - segDelay (seg - 1, pos));
            const double yb = table.read (lav, n, pos - segDelay (seg, pos));
            y = ya + g * (yb - ya);
        }
        else
            y = table.read (lav, n, pos - segDelay (seg, pos));
        out[i] = (float) (pol * y);
    }
}

//==============================================================================
void correctPhase (const float* boom, float* lav, int64_t n, double fs, float strength, const Progress& progress,
                   const std::vector<int64_t>& segmentStarts)
{
    if (strength <= 0.0f || n <= 0) return;

    const int winOrder = orderFor (0.043 * fs);
    const int win = 1 << winOrder, hop = win / 2;
    const int nfftOrder = winOrder + 1, nfft = 1 << nfftOrder, nb = nfft / 2 + 1;
    const int irHalf = std::min ((int) (0.008 * fs), (nfft - win) / 2 - 1);  // correction filters stay within +-8 ms
    const auto window = periodicHann (win);  // hop win/2: sums to exactly 1
    const double binHz = fs / nfft;

    const int blockFrames = std::max (1, (int) std::lround (0.25 * fs / hop));
    const int J = std::max (1, (int) std::lround (1.5 / (blockFrames * hop / fs)));  // +-1.5 s of blocks
    const auto blockWeights = bumpWeights (J);

    const int numFrames = (int) ((n + win) / hop) + 1;          // frame f starts at f * hop - win
    const int numBlocks = (numFrames + blockFrames - 1) / blockFrames;

    // segment of each block (by its centre): statistics are only shared within a segment
    std::vector<int> blockSegment ((size_t) numBlocks, 0);
    for (int j = 0; j < numBlocks; ++j)
    {
        const auto centre = (int64_t) ((j + 0.5) * blockFrames * hop) - win / 2;
        blockSegment[(size_t) j] = (int) (std::upper_bound (segmentStarts.begin(), segmentStarts.end(), centre) - segmentStarts.begin());
    }

    // frequency smoothing width per bin: 1/6 octave, at least 60 Hz wide
    std::vector<int> lo ((size_t) nb), hi ((size_t) nb);
    for (int k = 0; k < nb; ++k)
    {
        const double f = k * binHz;
        const double halfW = std::max (f * (std::pow (2.0, 1.0 / 12.0) - 1.0), 30.0) / binHz;
        lo[(size_t) k] = std::clamp ((int) (k - halfW), 0, nb - 1);
        hi[(size_t) k] = std::clamp ((int) (k + halfW) + 1, 1, nb);
    }
    // the correction fades in above 60 Hz and out towards Nyquist (nothing reliable to measure there)
    std::vector<float> bandGain ((size_t) nb);
    for (int k = 0; k < nb; ++k)
    {
        const double f = k * binHz;
        double g = std::clamp ((f - 40.0) / 40.0, 0.0, 1.0);
        g *= std::clamp ((0.46 * fs - f) / (0.06 * fs), 0.0, 1.0);
        bandGain[(size_t) k] = (float) g;
    }

    struct BlockStats { std::vector<cfloat> s; std::vector<float> pb, pl; };
    std::deque<BlockStats> stats;    // blocks [statsFirst, statsFirst + size)
    int statsFirst = 0;
    std::deque<std::vector<cfloat>> filters;  // control filters for blocks [filtFirst, ...)
    int filtFirst = 0;

    RealFFT fb (nfftOrder), fl (nfftOrder), work (nfftOrder);
    std::vector<float> out ((size_t) n, 0.0f);
    std::vector<float> taper ((size_t) nfft, 0.0f);
    for (int i = 0; i <= irHalf; ++i)
    {
        const float v = (float) (0.5 + 0.5 * std::cos (pi * i / (irHalf + 1)));
        taper[(size_t) i] = v;
        if (i > 0) taper[(size_t) (nfft - i)] = v;
    }

    std::vector<std::complex<double>> cs ((size_t) nb + 1);
    std::vector<double> cb ((size_t) nb + 1), cl ((size_t) nb + 1);

    auto makeFilter = [&] (int j) -> std::vector<cfloat>
    {
        // smoothed stats over blocks j-J .. j+J (those that exist)
        std::vector<std::complex<double>> S ((size_t) nb);
        std::vector<double> Pb ((size_t) nb, 0.0), Pl ((size_t) nb, 0.0);
        for (int i = -J; i <= J; ++i)
        {
            const int b = j + i;
            if (b < statsFirst || b >= statsFirst + (int) stats.size()) continue;
            if (blockSegment[(size_t) b] != blockSegment[(size_t) j]) continue;
            const auto& st = stats[(size_t) (b - statsFirst)];
            const double w = blockWeights[(size_t) (i + J)];
            for (int k = 0; k < nb; ++k)
            {
                S[(size_t) k] += w * std::complex<double> (st.s[(size_t) k]);
                Pb[(size_t) k] += w * st.pb[(size_t) k];
                Pl[(size_t) k] += w * st.pl[(size_t) k];
            }
        }
        cs[0] = 0; cb[0] = 0; cl[0] = 0;
        for (int k = 0; k < nb; ++k)
        {
            cs[(size_t) k + 1] = cs[(size_t) k] + S[(size_t) k];
            cb[(size_t) k + 1] = cb[(size_t) k] + Pb[(size_t) k];
            cl[(size_t) k + 1] = cl[(size_t) k] + Pl[(size_t) k];
        }
        work.clear();
        auto* H = work.bins();
        for (int k = 0; k < nb; ++k)
        {
            const auto ss = cs[(size_t) hi[(size_t) k]] - cs[(size_t) lo[(size_t) k]];
            const double bb = cb[(size_t) hi[(size_t) k]] - cb[(size_t) lo[(size_t) k]];
            const double ll = cl[(size_t) hi[(size_t) k]] - cl[(size_t) lo[(size_t) k]];
            const double m2 = std::norm (ss);
            double g = 0, ph = 0;
            if (m2 > 0 && bb * ll > 0)
            {
                const double coh = m2 / (bb * ll);
                g = std::clamp ((coh - 0.1) / 0.4, 0.0, 1.0) * strength * bandGain[(size_t) k];
                ph = std::arg (ss);
            }
            H[k] = cfloat ((float) std::cos (g * ph), (float) std::sin (g * ph));
        }
        // keep the filter short: impulse response tapered to +-irHalf samples
        work.inverse();
        for (int i = 0; i < nfft; ++i) work.buf[(size_t) i] *= taper[(size_t) i];
        std::fill (work.buf.begin() + nfft, work.buf.end(), 0.0f);
        work.forward();
        return std::vector<cfloat> (work.bins(), work.bins() + nb);
    };

    int nextStatsFrame = 0, nextFilterFrame = 0;
    std::vector<cfloat> Ht ((size_t) nb);

    auto filterFrame = [&] (int f)
    {
        // control filters are centred on their blocks; interpolate between the two around this frame
        const double pos = (f + 0.5) / blockFrames - 0.5;
        const int below = (int) std::floor (pos);
        const int j0 = std::clamp (below, 0, numBlocks - 1);
        const int j1 = std::clamp (below + 1, 0, numBlocks - 1);
        const float a = (float) std::clamp (pos - below, 0.0, 1.0);
        const auto& h0 = filters[(size_t) (j0 - filtFirst)];
        const auto& h1 = filters[(size_t) (j1 - filtFirst)];
        for (int k = 0; k < nb; ++k) Ht[(size_t) k] = h0[(size_t) k] + a * (h1[(size_t) k] - h0[(size_t) k]);

        const int64_t start = (int64_t) f * hop - win;
        loadFrame (fl, lav, n, start, window);
        fl.forward();
        auto* L = fl.bins();
        for (int k = 0; k < nb; ++k) L[k] *= Ht[(size_t) k];
        fl.inverse();
        // linear convolution of the windowed frame with a +-irHalf filter: samples [start - irHalf, start + win + irHalf)
        for (int i = -irHalf; i < win + irHalf; ++i)
        {
            const int64_t o = start + i;
            if (o < 0 || o >= n) continue;
            out[(size_t) o] += fl.buf[(size_t) ((i + nfft) % nfft)];
        }
    };

    for (int b = 0; b < numBlocks; ++b)
    {
        // block statistics
        BlockStats st { std::vector<cfloat> ((size_t) nb), std::vector<float> ((size_t) nb, 0.0f), std::vector<float> ((size_t) nb, 0.0f) };
        for (int f = b * blockFrames; f < std::min (numFrames, (b + 1) * blockFrames); ++f, ++nextStatsFrame)
        {
            const int64_t start = (int64_t) f * hop - win;
            loadFrame (fb, boom, n, start, window); loadFrame (work, lav, n, start, window);
            fb.forward(); work.forward();
            const cfloat* B = fb.bins(); const cfloat* L = work.bins();
            for (int k = 0; k < nb; ++k)
            {
                st.s[(size_t) k] += B[k] * std::conj (L[k]);
                st.pb[(size_t) k] += std::norm (B[k]);
                st.pl[(size_t) k] += std::norm (L[k]);
            }
        }
        stats.push_back (std::move (st));

        // control filter for block b - J now has all its neighbours
        const int ready = b - J;
        if (ready >= 0)
        {
            if (filters.empty()) filtFirst = ready;
            filters.push_back (makeFilter (ready));
        }
        // frames whose two control filters exist
        while (nextFilterFrame < numFrames)
        {
            const double pos = (nextFilterFrame + 0.5) / blockFrames - 0.5;
            const int j1 = std::min ((int) std::floor (pos) + 1, numBlocks - 1);
            if (filters.empty() || j1 >= filtFirst + (int) filters.size()) break;
            filterFrame (nextFilterFrame++);
        }
        // drop what is no longer needed
        const int needFilt = std::max (0, (int) std::floor ((nextFilterFrame + 0.5) / blockFrames - 0.5));
        while (! filters.empty() && filtFirst < needFilt) { filters.pop_front(); ++filtFirst; }
        const int needStats = (filtFirst + (int) filters.size()) - J;
        while (! stats.empty() && statsFirst < needStats) { stats.pop_front(); ++statsFirst; }

        if (progress && (b & 7) == 0 && ! progress ((float) b / (float) numBlocks))
            return;
    }
    // flush: remaining control filters use the blocks that exist
    for (int j = filtFirst + (int) filters.size(); j < numBlocks; ++j)
    {
        if (filters.empty()) filtFirst = j;
        filters.push_back (makeFilter (j));
    }
    while (nextFilterFrame < numFrames)
        filterFrame (nextFilterFrame++);

    std::copy (out.begin(), out.end(), lav);
    if (progress) progress (1.0f);
}

//==============================================================================
Quality measureQuality (const float* boom, const float* lav, int64_t n, double fs)
{
    Quality q;
    const int order = fs > 30000 ? 11 : 10;
    const int win = 1 << order, hop = win / 2, nb = win / 2 + 1;
    RealFFT fb (order), fl (order);
    const auto window = periodicHann (win);
    const double binHz = fs / win;

    const double fmin = 150.0, fmax = std::min (8000.0, 0.45 * fs);
    std::vector<double> edges;
    for (double e = fmin; e <= fmax * 1.0001; e *= std::pow (2.0, 1.0 / 3.0)) edges.push_back (e);
    const int numBands = (int) edges.size() - 1;
    if (numBands < 1) return q;
    std::vector<int> bandOf ((size_t) nb, -1);
    for (int k = 0; k < nb; ++k)
        for (int b = 0; b < numBands; ++b)
            if (k * binHz >= edges[(size_t) b] && k * binHz < edges[(size_t) b + 1]) { bandOf[(size_t) k] = b; break; }

    const int K = 4;
    const auto w = bumpWeights (K);
    const int numFrames = (int) ((n - win) / hop) + 1;
    if (numFrames < 1) return q;
    std::vector<std::vector<cfloat>> ring ((size_t) (2 * K + 1), std::vector<cfloat> ((size_t) nb));
    std::vector<double> re ((size_t) numBands, 0.0), mag ((size_t) numBands, 0.0);
    std::vector<cfloat> acc ((size_t) nb);

    auto raw = [&] (int f) -> std::vector<cfloat>&
    {
        return ring[(size_t) (((f % (2 * K + 1)) + (2 * K + 1)) % (2 * K + 1))];
    };
    auto compute = [&] (int f)
    {
        auto& dst = raw (f);
        if (f < 0 || f >= numFrames) { std::fill (dst.begin(), dst.end(), cfloat {}); return; }
        loadFrame (fb, boom, n, (int64_t) f * hop, window); loadFrame (fl, lav, n, (int64_t) f * hop, window);
        fb.forward(); fl.forward();
        for (int k = 0; k < nb; ++k) dst[(size_t) k] = fb.bins()[k] * std::conj (fl.bins()[k]);
    };
    for (int f = -K; f < K; ++f) compute (f);
    for (int t = 0; t < numFrames; ++t)
    {
        compute (t + K);
        std::fill (acc.begin(), acc.end(), cfloat {});
        for (int j = -K; j <= K; ++j)
        {
            const auto& src = raw (t + j);
            const float wj = w[(size_t) (j + K)];
            for (int k = 0; k < nb; ++k) acc[(size_t) k] += wj * src[(size_t) k];
        }
        for (int k = 0; k < nb; ++k)
        {
            const int b = bandOf[(size_t) k];
            if (b < 0) continue;
            re[(size_t) b] += acc[(size_t) k].real();
            mag[(size_t) b] += std::abs (acc[(size_t) k]);
        }
    }
    double sum = 0;
    for (int b = 0; b < numBands; ++b)
    {
        q.bandCentres.push_back ((float) std::sqrt (edges[(size_t) b] * edges[(size_t) b + 1]));
        const float v = mag[(size_t) b] > 0 ? (float) (re[(size_t) b] / mag[(size_t) b]) : 0.0f;
        q.bands.push_back (v);
        sum += v;
    }
    q.mean = (float) (sum / numBands);
    return q;
}

//==============================================================================
bool alignLav (const float* boom, const float* lav, int64_t n, double fs, const Settings& s, LavResult& result,
               const Progress& progress, bool measure)
{
    auto sub = [&progress] (float a, float b) -> Progress
    {
        if (! progress) return {};
        return [&progress, a, b] (float p) { return progress (a + (b - a) * p); };
    };
    bool cancelled = false;
    auto guard = [&cancelled] (const Progress& p) -> Progress
    {
        if (! p) return {};
        return [p, &cancelled] (float v) { if (! p (v)) { cancelled = true; return false; } return true; };
    };

    if (measure) result.before = measureQuality (boom, lav, n, fs);
    result.curve = analyse (boom, lav, n, fs, s, guard (sub (0.0f, 0.6f)));
    if (cancelled) return false;

    result.aligned.assign ((size_t) n, 0.0f);
    renderAligned (lav, result.aligned.data(), n, result.curve);
    if (progress && ! progress (0.7f)) return false;

    if (result.curve.found && s.phaseCorrection > 0.0f)
    {
        correctPhase (boom, result.aligned.data(), n, fs, s.phaseCorrection, guard (sub (0.7f, 0.97f)),
                      s.phaseBySegment ? result.curve.stepSamples() : std::vector<int64_t> {});
        if (cancelled) return false;
    }
    if (measure) result.after = measureQuality (boom, result.aligned.data(), n, fs);
    if (progress) progress (1.0f);
    return true;
}
}

namespace lavalign
{
std::vector<int64_t> DelayCurve::stepSamples() const
{
    std::vector<int64_t> v;
    for (int s : steps) v.push_back ((int64_t) std::llround (firstFrameCentre + (s - 0.5) * hop));
    return v;
}
}
