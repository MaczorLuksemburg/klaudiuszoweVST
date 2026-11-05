#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

// Lav-to-boom alignment engine (no GUI, no file I/O).
//
// The boom is the reference and is never changed. For each lav the engine finds how much later the
// boom hears the same sound, over time (the actor and the boom move, wireless lavs add latency), and
// whether the lav's polarity is flipped. Rendering then delays (or advances) the lav by that amount
// with a windowed-sinc fractional delay, so boom + lav sum without comb filtering. An optional
// spectral phase correction afterwards turns the lav's remaining phase difference to the boom
// (different mics, chest vs. air, recorder filters) towards zero, slowly varying and smooth over
// frequency, without changing its magnitude response.
//
// Delay convention: delay > 0 means the lav is early (it is closer to the mouth), so the output is
// out[n] = polarity * lav[n - delay(n)].
namespace lavalign
{
    struct Settings
    {
        double maxOffsetMs     = 50.0;  // coarse search range for a constant offset (wireless latency, sync)
        double trackRangeMs    = 12.0;  // tracking range around that offset
        double smoothingMs     = 80.0;  // how quickly the delay may follow movement
        double contextMs       = 70.0;  // averaging of the cross-spectrum around each analysis frame
        double minFreq         = 120.0; // analysis band
        double maxFreq         = 8000.0;
        bool   detectPolarity  = true;
        bool   allowSteps      = true;  // delay may jump at cuts / speaker changes (crossfaded)
        float  phaseCorrection = 1.0f;  // 0..1, spectral phase correction strength
        int    threads         = 0;     // analysis threads, 0 = one per CPU core
    };

    // Delay over time, one value per analysis frame (frame k is centred on sample k * hop + firstFrameCentre).
    struct DelayCurve
    {
        double sampleRate = 48000.0;
        int    hop = 512;
        double firstFrameCentre = 0.0;

        std::vector<float> delay;       // smoothed delay in samples (includes the coarse offset)
        std::vector<float> measured;    // raw per-frame measurement (samples)
        std::vector<float> weight;      // confidence of each measurement (0 = none)
        std::vector<int>   steps;       // frames where a new segment starts (delay jumps, crossfaded)

        float  polarity = 1.0f;         // -1: lav is inverted relative to the boom
        double coarseOffset = 0.0;      // samples
        float  coarseConfidence = 0.0f;
        bool   found = false;           // false: no common signal, the lav is left as it is
        int    confidentFrames = 0;     // frames with a clear correlation peak

        double delayAtSample (double samplePos) const;  // linear interpolation between frames
        float  medianDelayMs() const;
        float  minDelayMs() const;
        float  maxDelayMs() const;
    };

    using Progress = std::function<bool (float)>;  // return false to cancel

    // Finds the delay curve. 'boom' and 'lav' are the same length and start at the same time.
    DelayCurve analyse (const float* boom, const float* lav, int64_t numSamples, double sampleRate,
                        const Settings& settings, const Progress& progress = {});

    // out[n] = polarity * lav[n - delay(n)], windowed sinc. 'out' must not alias 'lav'.
    void renderAligned (const float* lav, float* out, int64_t numSamples, const DelayCurve& curve);

    // Turns the remaining phase difference between 'lav' (already time-aligned) and 'boom' towards zero.
    // In place on 'lav'. strength 0..1.
    void correctPhase (const float* boom, float* lav, int64_t numSamples, double sampleRate, float strength,
                       const Progress& progress = {});

    // How well two signals sum: per 1/3-octave band from 150 Hz to 8 kHz, the power-weighted mean cosine of the
    // phase difference between them over short frames (1 = in phase, 0 = unrelated, -1 = cancelling).
    struct Quality
    {
        std::vector<float> bandCentres;
        std::vector<float> bands;
        float mean = 0.0f;
    };
    Quality measureQuality (const float* boom, const float* lav, int64_t numSamples, double sampleRate);

    // Everything for one lav: analyse, render, phase-correct. Returns false if cancelled.
    struct LavResult
    {
        DelayCurve curve;
        std::vector<float> aligned;
        Quality before, after;
    };
    bool alignLav (const float* boom, const float* lav, int64_t numSamples, double sampleRate,
                   const Settings& settings, LavResult& result, const Progress& progress = {},
                   bool measure = true);
}
