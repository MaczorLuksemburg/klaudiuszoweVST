#pragma once

#include "Components.h"

namespace dynmap::ui
{
    // Draws and edits one stage's level or transient curve.
    //  - drag points; double-click empty space to add one, double-click a point to delete it
    //  - drag the small handle in the middle of a segment to bend it (or set steps/waves)
    //  - right-click a segment for its shape, right-click empty space for curve presets
    //  - a live dot shows where the signal sits on the curve
    class CurveEditor : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
    {
    public:
        CurveEditor (DynMapProcessor&, CurveKind);

        void setStage (int stage, juce::Colour accent);

        void paint (juce::Graphics&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

        static juce::String segmentName (Segment);

    private:
        struct Hit { enum Type { none, point, handle } type = none; int index = -1; };

        void timerCallback() override;
        juce::Rectangle<float> plotArea() const;
        juce::Point<float> toScreen (float x, float y) const;
        juce::Point<float> fromScreen (juce::Point<float>) const;
        Hit hitTest (juce::Point<float>) const;
        juce::Point<float> handlePosition (int segment) const;
        void commit();
        void showSegmentMenu (int segment);
        void showCurveMenu();

        DynMapProcessor& processor;
        const CurveKind kind;
        const CurveRange range;
        int stage = inputStage;
        juce::Colour accent;

        Curve curve;
        int seenChanges = -1;
        Hit hover, drag;
        float dragStartTension = 0.0f;
        juce::Point<float> dragStart;

        // Live position trail.
        static constexpr int trailLength = 40;
        std::array<float, trailLength> trail {};
        int trailHead = 0;
        bool live = false;
        float liveX = 0.0f, gainDb = 0.0f;

        static Curve clipboard[2];
        static bool clipboardFull[2];
    };
}
