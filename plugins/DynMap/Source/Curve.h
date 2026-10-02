#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <atomic>
#include <vector>

// Drawn transfer curves. Each stage has two:
//  - the level curve maps the detected input level (dB) to an output level (dB); the stage
//    applies gain = output - input. Points on the bottom edge mean silence.
//  - the transient curve maps how far the signal sits above (+) or below (-) its own recent
//    average (dB) to a gain (dB). Attacks are on the right, decaying tails on the left.
namespace dynmap
{
    enum class CurveKind { level, transient };

    // Shape of the line from a point to the next one (FL Studio graph editor style).
    enum class Segment { curve, sCurve, hold, stairs, smoothStairs, wave };
    inline constexpr int numSegmentTypes = 6;

    struct CurveRange { float xMin, xMax, yMin, yMax; };

    inline constexpr CurveRange levelRange     { -72.0f, 12.0f, -72.0f, 12.0f };
    inline constexpr CurveRange transientRange { -24.0f, 24.0f, -24.0f, 24.0f };

    inline constexpr CurveRange curveRange (CurveKind kind) { return kind == CurveKind::level ? levelRange : transientRange; }

    // Level curves can also be drawn on linear amplitude axes, like Image-Line Maximus: 0 to 2
    // (+6 dBFS), so 0 dBFS sits in the middle and the slope at the bottom-left corner is the gain
    // for quiet signals. Processing always reads the curve in dB (see gainAt).
    inline constexpr CurveRange linearLevelRange { 0.0f, 2.0f, 0.0f, 2.0f };

    struct CurvePoint
    {
        float x = 0.0f, y = 0.0f;
        Segment segment = Segment::curve;   // shape towards the next point
        float tension = 0.0f;               // -1..1: bend, S-steepness, step or wave count

        bool operator== (const CurvePoint&) const = default;
    };

    class Curve
    {
    public:
        explicit Curve (CurveKind = CurveKind::level, bool linearScale = false);   // neutral: identity (level) or flat 0 dB (transient)

        CurveKind getKind() const { return kind; }
        bool isLinear() const { return linear; }
        CurveRange getRange() const { return linear ? linearLevelRange : curveRange (kind); }

        // The same points moved to the other scale (level curves only).
        Curve withScale (bool linearScale) const;
        const std::vector<CurvePoint>& getPoints() const { return points; }
        int getNumPoints() const { return (int) points.size(); }

        float evaluate (float x) const;   // curve value (y) at x
        float gainAt (float x) const;     // applied gain in dB; level curves give silenceDb on the bottom edge
        bool isNeutral() const;

        // Editing. Endpoints stay on the left/right edges; inner points stay between their neighbours.
        int addPoint (float x, float y);   // returns the new index
        void removePoint (int index);      // endpoints can't be removed
        void movePoint (int index, float x, float y);
        void setSegment (int index, Segment);
        void setTension (int index, float tension);

        static float segmentValue (const CurvePoint& a, const CurvePoint& b, float x);
        static int stepCount (float tension);      // stairs: 2..16
        static int waveCycles (float tension);     // wave: 0..7 extra half cycles

        juce::String toString() const;
        static Curve fromString (CurveKind, const juce::String&);

        static juce::StringArray presetNames (CurveKind);
        static std::vector<int> presetMenuOrder (CurveKind);
        static Curve preset (CurveKind, int index);

        bool operator== (const Curve&) const = default;

        static constexpr float silenceDb = -200.0f;

    private:
        void sanitise();

        CurveKind kind;
        bool linear = false;
        std::vector<CurvePoint> points;
    };

    //==============================================================================
    // A curve baked into a gain table for the audio thread.
    struct CurveTable
    {
        static constexpr int size = 2049;

        std::array<float, size> gainDb {};
        float xMin = 0.0f, stepsPerDb = 1.0f;
        bool neutral = true;

        void bake (const Curve&);

        float lookup (float x) const noexcept
        {
            float pos = (x - xMin) * stepsPerDb;
            pos = pos < 0.0f ? 0.0f : (pos > (float) (size - 1) ? (float) (size - 1) : pos);
            const int i = juce::jmin ((int) pos, size - 2);
            const float frac = pos - (float) i;
            return gainDb[(size_t) i] + frac * (gainDb[(size_t) i + 1] - gainDb[(size_t) i]);
        }
    };

    // Hands a baked table from the message thread to the audio thread without blocking it.
    class CurveSlot
    {
    public:
        void publish (const Curve& curve)
        {
            CurveTable table;
            table.bake (curve);

            const juce::SpinLock::ScopedLockType lock (mutex);
            pending = table;
            version.fetch_add (1);
        }

        // Audio thread: copies the newest table if there is one and the slot isn't being written.
        bool pull (CurveTable& destination, uint32_t& seenVersion)
        {
            const auto current = version.load();
            if (current == seenVersion)
                return false;

            const juce::SpinLock::ScopedTryLockType lock (mutex);
            if (! lock.isLocked())
                return false;

            destination = pending;
            seenVersion = version.load();
            return true;
        }

    private:
        juce::SpinLock mutex;
        CurveTable pending;
        std::atomic<uint32_t> version { 1 };
    };
}
