#pragma once

#include "Components.h"

namespace dynmap::ui
{
    // Band editing shared by the display and menus.
    namespace bands
    {
        void copyStage (DynMapProcessor&, int fromStage, int toStage);   // parameters and curves
        int split (DynMapProcessor&, float freq);                         // returns the new band's stage, or -1
        int remove (DynMapProcessor&, int bandSlot);                      // returns the stage to select next
        void reset (DynMapProcessor&, int stage);                         // stage parameters and curves to default
    }

    // Saturn-style multiband display: live spectrum, bands coloured low to high, draggable
    // crossovers (slope badge on top), each band's level line with its live gain change.
    //  - double-click or click the "+" to split a band there; double-click a crossover to remove it
    //  - drag a crossover to move it, drag a band's line to set its output level
    //  - right-click a band for solo, mute, bypass, reset and remove
    class BandDisplay : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
    {
    public:
        BandDisplay (DynMapProcessor&, std::function<int()> getSelected, std::function<void (int)> select);
        ~BandDisplay() override;

        void paint (juce::Graphics&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    private:
        struct Hit
        {
            enum Type { none, crossover, slopeBadge, level, plus, band } type = none;
            int position = -1;
        };

        void timerCallback() override;
        juce::Rectangle<float> plotArea() const;
        float xForFreq (float freq) const;
        float freqForX (float x) const;
        float yForDb (float db) const;
        float dbForY (float y) const;
        float bandLeft (int position) const;
        float bandRight (int position) const;
        int positionAt (float x) const;
        Hit hitTest (juce::Point<float>) const;
        juce::Rectangle<float> badgeArea (int position) const;
        juce::Rectangle<float> plusArea() const;

        void showBandMenu (int position);
        void showSlopeMenu (int position);
        juce::RangedAudioParameter* param (int stage, const char* name) const;

        DynMapProcessor& processor;
        std::function<int()> getSelected;
        std::function<void (int)> select;

        BandLayout layout;
        std::array<float, maxBands> shownGain {};   // eased live gain per band slot
        Hit hover, drag;
        float hoverX = -1.0f;
        juce::RangedAudioParameter* dragParam = nullptr;
    };

    // Input / Master selector beside the band display, with its live gain.
    class StageTab : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
    {
    public:
        StageTab (DynMapProcessor&, int stage, std::function<int()> getSelected, std::function<void (int)> select);

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;

    private:
        void timerCallback() override;

        DynMapProcessor& processor;
        const int stage;
        std::function<int()> getSelected;
        std::function<void (int)> select;
        float shownGain = 0.0f;
    };
}
