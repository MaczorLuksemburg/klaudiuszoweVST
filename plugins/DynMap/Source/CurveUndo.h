#pragma once

#include "Engine.h"

namespace dynmap
{
    // Undo/redo for curve edits (not parameters: those belong to the host's automation). Each entry is
    // one finished edit of one curve, stored as curve text; a drag is one entry. Cleared when a preset
    // or a project loads.
    class CurveUndo
    {
    public:
        void record (int stage, CurveKind kind, const juce::String& before, const juce::String& after)
        {
            if (before == after)
                return;

            const juce::ScopedLock lock (mutex);
            undoStack.push_back ({ stage, kind, before, after });
            if (undoStack.size() > maxEntries)
                undoStack.erase (undoStack.begin());
            redoStack.clear();
        }

        // Applies the step to the curves; returns the stage it changed, or -1 when there is nothing to do.
        int undo (CurveBank& curves) { return step (curves, undoStack, redoStack, true); }
        int redo (CurveBank& curves) { return step (curves, redoStack, undoStack, false); }

        bool canUndo() const { const juce::ScopedLock lock (mutex); return ! undoStack.empty(); }
        bool canRedo() const { const juce::ScopedLock lock (mutex); return ! redoStack.empty(); }

        void clear()
        {
            const juce::ScopedLock lock (mutex);
            undoStack.clear();
            redoStack.clear();
        }

    private:
        struct Entry
        {
            int stage;
            CurveKind kind;
            juce::String before, after;
        };

        int step (CurveBank& curves, std::vector<Entry>& from, std::vector<Entry>& to, bool backwards)
        {
            const juce::ScopedLock lock (mutex);
            if (from.empty())
                return -1;

            const auto entry = from.back();
            from.pop_back();
            curves.set (entry.stage, entry.kind, Curve::fromString (entry.kind, backwards ? entry.before : entry.after));
            to.push_back (entry);
            return entry.stage;
        }

        static constexpr size_t maxEntries = 200;
        juce::CriticalSection mutex;
        std::vector<Entry> undoStack, redoStack;
    };
}
