#pragma once

#include "Components.h"

namespace dynmap::ui
{
    // Draws and edits one stage's level or transient curve.
    //  - drag points; double-click empty space to add one, double-click a point to delete it
    //    (an end point, which can't go, moves back to where the neutral curve has it)
    //  - drag the small handle in the middle of a segment to bend it (or set steps/waves)
    //  - right-click a segment for its shape, right-click empty space for curve presets
    //  - a live dot shows where the signal sits on the curve
    //  - points snap to 3 dB steps while Shift is held or grid snapping is on (shared by both maps)
    class CurveEditor : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
    {
    public:
        CurveEditor (DynMapProcessor&, CurveKind);

        void setStage (int stage, juce::Colour accent);

        // Level map only: linear amplitude axes like Maximus (the curve is converted).
        bool isLinear() const { return curve.isLinear(); }
        void setLinear (bool linear);

        void paint (juce::Graphics&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

        static juce::String segmentName (Segment);

        // Grid snapping (a UI setting saved with the project, shared by the level and transient maps).
        static bool isSnapOn (DynMapProcessor&);
        static void setSnapOn (DynMapProcessor&, bool);

    private:
        struct Hit { enum Type { none, point, handle } type = none; int index = -1; };

        void timerCallback() override;
        juce::Rectangle<float> plotArea() const;
        juce::Point<float> toScreen (float x, float y) const;
        juce::Point<float> fromScreen (juce::Point<float>) const;
        Hit hitTest (juce::Point<float>) const;
        juce::Point<float> handlePosition (int segment) const;
        bool isNearlyFlat (int segment) const;
        void commit();
        void load (const Curve&);
        float curveXForDb (float db) const;
        float snap (float v) const;
        bool snapping (const juce::ModifierKeys&) const;   // Shift always snaps; otherwise the grid button decides
        bool showsSnapGrid() const;
        void addSnapItem (juce::PopupMenu&) const;
        juce::String levelText (float v) const;
        void showSegmentMenu (int segment);
        void showCurveMenu();

        DynMapProcessor& processor;
        const CurveKind kind;
        CurveRange range;
        int stage = inputStage;
        juce::Colour accent;

        Curve curve;
        int seenChanges = -1;
        Hit hover, drag;
        float dragStartTension = 0.0f;
        juce::Point<float> dragStart;

        // Live position trail.
        juce::String dragBefore;   // the curve when the current drag started (for undo)

        static constexpr int trailLength = 40;
        std::array<float, trailLength> trail {};
        int trailHead = 0;
        bool live = false;
        float liveX = 0.0f, gainDb = 0.0f;
        bool snapShown = false;

        static Curve clipboard[2];
        static bool clipboardFull[2];
    };

    // The grid snapping switch in a map's header: a small grid icon, lit while snapping is on.
    class GridSnapButton : public juce::Button, private juce::Timer
    {
    public:
        explicit GridSnapButton (DynMapProcessor&);
        void paintButton (juce::Graphics&, bool highlighted, bool down) override;
        void clicked() override;

    private:
        void timerCallback() override;
        DynMapProcessor& processor;
    };
}
