#pragma once

#include <juce_core/juce_core.h>
#include <array>

namespace dynmap
{
    // One slice (about 5 ms) of what a stage did, for the scrolling history view.
    struct HistoryFrame
    {
        float inDb = -150.0f;        // peak of the stage's input
        float outDb = -150.0f;       // peak of its output
        float gainMinDb = 0.0f;      // most reduction applied in the slice
        float gainMaxDb = 0.0f;      // most boost applied in the slice
        float transientDb = 0.0f;    // transient curve's gain with the biggest magnitude
    };

    // Single producer (audio thread) / single consumer (GUI timer). Frames that don't fit while no
    // editor is reading are dropped.
    class HistoryFifo
    {
    public:
        void push (const HistoryFrame& frame) noexcept
        {
            const auto scope = fifo.write (1);
            if (scope.blockSize1 > 0)
                frames[(size_t) scope.startIndex1] = frame;
        }

        template <typename Callback>
        void pop (Callback&& callback)
        {
            const auto scope = fifo.read (fifo.getNumReady());
            for (int i = 0; i < scope.blockSize1; ++i) callback (frames[(size_t) (scope.startIndex1 + i)]);
            for (int i = 0; i < scope.blockSize2; ++i) callback (frames[(size_t) (scope.startIndex2 + i)]);
        }

        void clear() { fifo.reset(); }

    private:
        static constexpr int capacity = 1024;
        juce::AbstractFifo fifo { capacity };
        std::array<HistoryFrame, capacity> frames {};
    };
}
