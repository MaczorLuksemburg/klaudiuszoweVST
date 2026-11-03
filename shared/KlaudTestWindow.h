#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace klaud
{
    // Where test programs put a window that must really be on screen (menu tests): near the top-left
    // of the rightmost display, so it never flashes up on the main monitor while the user works.
    inline juce::Point<int> testWindowPosition()
    {
        const juce::Displays::Display* rightmost = nullptr;

        for (const auto& display : juce::Desktop::getInstance().getDisplays().displays)
            if (rightmost == nullptr || display.totalArea.getX() > rightmost->totalArea.getX())
                rightmost = &display;

        return rightmost != nullptr ? rightmost->userArea.getTopLeft() + juce::Point<int> (40, 40)
                                    : juce::Point<int> (60, 60);
    }
}
