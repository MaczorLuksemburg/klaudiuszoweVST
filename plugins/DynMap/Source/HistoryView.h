#pragma once

#include "PluginProcessor.h"

namespace dynmap::ui
{
    // Scrolling history of one stage (the selected one): its input (filled) and output (line) levels on
    // top, the gain it applied below (reduction down, boost up) with the transient curve's share as a
    // thin line. Chips at the top left pick the stage. Shares the band display's place in the editor.
    class HistoryView : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
    {
    public:
        HistoryView (DynMapProcessor&, std::function<int()> getSelected, std::function<void (int)> select);

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void visibilityChanged() override;
        void pull();   // takes the stages' new frames (the timer does this 30 times a second)

    private:
        void timerCallback() override;
        juce::Rectangle<float> plotArea() const;
        std::vector<std::pair<int, juce::Rectangle<float>>> chips() const;   // stage, bounds

        static constexpr int capacity = 1200;     // ~6 s of 5 ms frames
        static constexpr int shownFrames = 1000;  // ~5 s on screen

        struct Ring
        {
            std::vector<HistoryFrame> frames = std::vector<HistoryFrame> ((size_t) capacity);
            int next = 0, count = 0;
            const HistoryFrame& back (int age) const { return frames[(size_t) ((next - 1 - age + 2 * capacity) % capacity)]; }
        };

        DynMapProcessor& processor;
        std::function<int()> getSelected;
        std::function<void (int)> select;
        std::array<Ring, numStages> rings;
    };
}
