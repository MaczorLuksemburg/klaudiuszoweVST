#include "Crossover.h"

namespace dynmap
{
namespace
{
    constexpr double butterworth4Q1 = 0.54119610014619698;
    constexpr double butterworth4Q2 = 1.3065629648763766;
}

//==============================================================================
void MinimumPhaseSplitter::Coeffs::setup (int slopeIndex, double freq, double rate)
{
    slope = slopeIndex;
    onePole.setCutoff (freq, rate);
    svf[0].setup (freq, slope == slope48 ? butterworth4Q1 : juce::MathConstants<double>::sqrt2 * 0.5, rate);
    svf[1].setup (freq, butterworth4Q2, rate);
}

void MinimumPhaseSplitter::split (const Coeffs& c, SplitState& s, float x, float& low, float& high) noexcept
{
    switch (c.slope)
    {
        case slope6:
            low = s.onePole[0].lowpass (x, c.onePole);
            high = x - low;
            break;

        case slope12:
        {
            const float l1 = s.onePole[0].lowpass (x, c.onePole);
            const float h1 = x - l1;
            low = s.onePole[1].lowpass (l1, c.onePole);
            high = -(h1 - s.onePole[2].lowpass (h1, c.onePole));   // odd order: invert so the sum is an allpass
            break;
        }

        case slope24:
        {
            float bp;
            const float lp1 = s.svf[0].tick (x, c.svf[0], bp);
            const float hp1 = x - c.svf[0].k * bp - lp1;
            low = s.svf[1].lowpass (lp1, c.svf[0]);
            high = s.svf[2].highpass (hp1, c.svf[0]);
            break;
        }

        default:
        {
            float bp;
            const float lpA = s.svf[0].tick (x, c.svf[0], bp);
            const float hpA = x - c.svf[0].k * bp - lpA;
            low  = s.svf[3].lowpass (s.svf[2].lowpass (s.svf[1].lowpass (lpA, c.svf[1]), c.svf[0]), c.svf[1]);
            high = s.svf[6].highpass (s.svf[5].highpass (s.svf[4].highpass (hpA, c.svf[1]), c.svf[0]), c.svf[1]);
            break;
        }
    }
}

float MinimumPhaseSplitter::allpass (const Coeffs& c, AllpassState& s, float x) noexcept
{
    switch (c.slope)
    {
        case slope6:  return x;
        case slope12: return 2.0f * s.onePole.lowpass (x, c.onePole) - x;
        case slope24: return s.svf[0].allpass (x, c.svf[0]);
        default:      return s.svf[1].allpass (s.svf[0].allpass (x, c.svf[0]), c.svf[1]);
    }
}

void MinimumPhaseSplitter::prepare (double newSampleRate)
{
    sampleRate = newSampleRate;
    glide = 1.0f - std::exp (-(float) maxChunk / (0.03f * (float) sampleRate));
    hasLayout = false;
    reset();
}

void MinimumPhaseSplitter::reset()
{
    for (auto& slot : splitStates)
        for (auto& s : slot)
            s.reset();

    for (auto& band : allpassStates)
        for (auto& slot : band)
            for (auto& s : slot)
                s.reset();
}

void MinimumPhaseSplitter::setLayout (const BandLayout& newLayout, bool snap)
{
    const bool topologyChanged = ! hasLayout || ! newLayout.sameTopology (layout);

    if (topologyChanged && hasLayout)
    {
        // Filters that weren't part of the old split start from silence.
        for (int p = 1; p < newLayout.numBands; ++p)
        {
            const int slot = newLayout.slots[(size_t) p];
            if (layout.positionOf (slot) < 1)
                for (auto& s : splitStates[(size_t) slot])
                    s.reset();
        }

        for (int q = 0; q < newLayout.numBands; ++q)
        {
            const int bandSlot = newLayout.slots[(size_t) q];
            const int oldPos = layout.positionOf (bandSlot);

            for (int p = q + 2; p < newLayout.numBands; ++p)
            {
                const int crossSlot = newLayout.slots[(size_t) p];
                const int oldCrossPos = layout.positionOf (crossSlot);
                if (oldPos < 0 || oldCrossPos < oldPos + 2)
                    for (auto& s : allpassStates[(size_t) bandSlot][(size_t) crossSlot])
                        s.reset();
            }
        }
    }

    for (int p = 1; p < newLayout.numBands; ++p)
    {
        const int slot = newLayout.slots[(size_t) p];
        const float freq = newLayout.freqs[(size_t) p];
        const bool isNew = layout.positionOf (slot) < 1 || ! hasLayout;

        targetFreq[(size_t) slot] = freq;

        if (snap || isNew || newLayout.slopes[(size_t) p] != coeffs[(size_t) slot].slope)
        {
            currentFreq[(size_t) slot] = freq;
            coeffs[(size_t) slot].setup (newLayout.slopes[(size_t) p], freq, sampleRate);
        }
    }

    layout = newLayout;
    hasLayout = true;
}

void MinimumPhaseSplitter::process (const float* const* input, float* const* const* bands, int numSamples)
{
    const int nb = layout.numBands;

    if (nb <= 1)
    {
        for (int ch = 0; ch < 2; ++ch)
            std::copy (input[ch], input[ch] + numSamples, bands[layout.slots[0]][ch]);
        return;
    }

    for (int start = 0; start < numSamples; start += maxChunk)
    {
        const int length = juce::jmin (maxChunk, numSamples - start);

        for (int p = 1; p < nb; ++p)
        {
            const auto slot = (size_t) layout.slots[(size_t) p];
            auto& current = currentFreq[slot];
            const float target = targetFreq[slot];

            if (current != target)
            {
                const float ratio = target / current;
                current = std::abs (ratio - 1.0f) < 1.0e-4f ? target : current * std::pow (ratio, glide);
                coeffs[slot].setup (coeffs[slot].slope, current, sampleRate);
            }
        }

        for (int ch = 0; ch < 2; ++ch)
        {
            const float* in = input[ch] + start;
            float* rest = bands[layout.slots[(size_t) nb - 1]][ch] + start;
            std::copy (in, in + length, rest);

            // Split upwards: the crossover at position p takes the band below it off the rest.
            for (int p = 1; p < nb; ++p)
            {
                const auto slot = (size_t) layout.slots[(size_t) p];
                const auto& c = coeffs[slot];
                auto& state = splitStates[slot][(size_t) ch];
                float* low = bands[layout.slots[(size_t) p - 1]][ch] + start;

                for (int i = 0; i < length; ++i)
                {
                    float lo, hi;
                    split (c, state, rest[i], lo, hi);
                    low[i] = lo;
                    rest[i] = hi;
                }
            }

            // Each band also gets the allpass of every crossover above its upper edge.
            for (int q = 0; q + 2 < nb; ++q)
            {
                const auto bandSlot = (size_t) layout.slots[(size_t) q];
                float* band = bands[bandSlot][ch] + start;

                for (int p = q + 2; p < nb; ++p)
                {
                    const auto crossSlot = (size_t) layout.slots[(size_t) p];
                    const auto& c = coeffs[crossSlot];
                    auto& state = allpassStates[bandSlot][crossSlot][(size_t) ch];

                    if (c.slope == slope6)
                        continue;

                    for (int i = 0; i < length; ++i)
                        band[i] = allpass (c, state, band[i]);
                }
            }
        }
    }
}

//==============================================================================
LinearPhaseSplitter::LinearPhaseSplitter() : juce::Thread ("DynMap crossover design")
{
    // A fixed partition latency on top of the FIR's own delay is much cheaper than zero-latency
    // convolution, and small next to the ~85 ms the linear-phase FIRs need anyway.
    for (auto& c : convolvers)
        c = std::make_unique<juce::dsp::Convolution> (juce::dsp::Convolution::Latency { convolutionLatency }, queue);
}

LinearPhaseSplitter::~LinearPhaseSplitter()
{
    signalThreadShouldExit();
    wakeUp.signal();
    stopThread (4000);
}

void LinearPhaseSplitter::design (const BandLayout& layout, std::array<juce::AudioBuffer<float>, maxBands>& irs) const
{
    const int n = firLength;
    const int order = juce::roundToInt (std::log2 ((double) n));
    juce::dsp::FFT fft (order);
    const int numBins = n / 2 + 1;
    const int nb = layout.numBands;

    std::vector<float> spectrum ((size_t) n * 2);
    std::vector<double> rest ((size_t) numBins), lowShare ((size_t) numBins);

    // Normalisation of the inverse transform, measured on a flat spectrum.
    std::fill (spectrum.begin(), spectrum.end(), 0.0f);
    for (int b = 0; b < numBins; ++b)
        spectrum[(size_t) b * 2] = 1.0f;
    fft.performRealOnlyInverseTransform (spectrum.data());
    const float scale = 1.0f / spectrum[0];

    std::fill (rest.begin(), rest.end(), 1.0);

    for (int p = 0; p < nb; ++p)
    {
        const int slot = layout.slots[(size_t) p];
        const bool hasUpper = p + 1 < nb;

        std::fill (spectrum.begin(), spectrum.end(), 0.0f);

        for (int b = 0; b < numBins; ++b)
        {
            double share = 1.0;

            if (hasUpper)
            {
                const double f = (double) b * sampleRate / (double) n;
                const double fc = layout.freqs[(size_t) p + 1];
                const int power = 1 << layout.slopes[(size_t) p + 1];   // 1, 2, 4, 8 -> 6..48 dB/oct
                share = 1.0 / (1.0 + std::pow (f / fc, (double) power));
            }

            // Nested shares: band p = rest * low, rest *= high. The sum is exactly 1.
            spectrum[(size_t) b * 2] = (float) (rest[(size_t) b] * share);
            rest[(size_t) b] *= 1.0 - share;
        }

        fft.performRealOnlyInverseTransform (spectrum.data());

        auto& ir = irs[(size_t) slot];
        ir.setSize (1, n);
        auto* out = ir.getWritePointer (0);

        for (int i = 0; i < n; ++i)
        {
            const float value = spectrum[(size_t) ((i + n / 2) % n)] * scale;
            const float window = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) n);
            out[i] = value * window;
        }
    }
}

void LinearPhaseSplitter::prepare (double newSampleRate, int maxBlockSize, const BandLayout& layout)
{
    stopThread (4000);

    sampleRate = newSampleRate;
    maxBlock = maxBlockSize;
    firLength = juce::nextPowerOfTwo ((int) (sampleRate * 0.17));

    std::array<juce::AudioBuffer<float>, maxBands> irs;
    design (layout, irs);

    const juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) maxBlock, 2 };

    for (int slot = 0; slot < maxBands; ++slot)
    {
        auto& conv = *convolvers[(size_t) slot];

        if (irs[(size_t) slot].getNumSamples() > 0)
            conv.loadImpulseResponse (std::move (irs[(size_t) slot]), sampleRate, juce::dsp::Convolution::Stereo::no,
                                      juce::dsp::Convolution::Trim::no, juce::dsp::Convolution::Normalise::no);

        conv.prepare (spec);
    }

    scratch.setSize (2, maxBlock);

    {
        const juce::SpinLock::ScopedLockType l1 (requestLock);
        const juce::SpinLock::ScopedLockType l2 (resultLock);
        requested = current = layout;
        hasRequest = hasResult = false;
    }

    upToDate.store (true);
    startThread (juce::Thread::Priority::low);
}

void LinearPhaseSplitter::reset()
{
    for (auto& c : convolvers)
        c->reset();
}

void LinearPhaseSplitter::setLayout (const BandLayout& layout)
{
    {
        const juce::SpinLock::ScopedTryLockType lock (resultLock);

        if (lock.isLocked() && hasResult)
        {
            for (int p = 0; p < resultLayout.numBands; ++p)
            {
                const int slot = resultLayout.slots[(size_t) p];
                auto& ir = results[(size_t) slot];

                if (ir.getNumSamples() > 0)
                {
                    // A newly used band slot starts from silence rather than fading from an old IR.
                    if (current.positionOf (slot) < 0)
                        convolvers[(size_t) slot]->reset();

                    convolvers[(size_t) slot]->loadImpulseResponse (std::move (ir), sampleRate, juce::dsp::Convolution::Stereo::no,
                                                                    juce::dsp::Convolution::Trim::no, juce::dsp::Convolution::Normalise::no);
                }
            }

            current = resultLayout;
            hasResult = false;
            installedCounter = resultCounter;
        }
    }

    if (! (layout == requested))
    {
        const juce::SpinLock::ScopedTryLockType lock (requestLock);

        if (lock.isLocked())
        {
            requested = layout;
            hasRequest = true;
            ++requestCounter;
            wakeUp.signal();
        }
    }

    upToDate.store (installedCounter == requestCounter && current == layout);
}

void LinearPhaseSplitter::process (const float* const* input, float* const* const* bands, int numSamples)
{
    const juce::dsp::AudioBlock<const float> in (input, 2, (size_t) numSamples);

    for (int p = 0; p < current.numBands; ++p)
    {
        const int slot = current.slots[(size_t) p];
        juce::dsp::AudioBlock<float> out (bands[slot], 2, (size_t) numSamples);
        convolvers[(size_t) slot]->process (juce::dsp::ProcessContextNonReplacing<float> (in, out));
    }
}

void LinearPhaseSplitter::run()
{
    while (! threadShouldExit())
    {
        BandLayout layout;
        uint32_t counter = 0;
        bool work = false;

        {
            const juce::SpinLock::ScopedLockType lock (requestLock);
            if (hasRequest)
            {
                layout = requested;
                counter = requestCounter;
                hasRequest = false;
                work = true;
            }
        }

        if (! work)
        {
            wakeUp.wait (200);
            continue;
        }

        std::array<juce::AudioBuffer<float>, maxBands> irs;
        design (layout, irs);

        const juce::SpinLock::ScopedLockType lock (resultLock);
        for (int slot = 0; slot < maxBands; ++slot)
            results[(size_t) slot] = std::move (irs[(size_t) slot]);
        resultLayout = layout;
        resultCounter = counter;
        hasResult = true;
    }
}
}
