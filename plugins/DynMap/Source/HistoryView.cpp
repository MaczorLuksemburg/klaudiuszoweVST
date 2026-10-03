#include "HistoryView.h"
#include "Components.h"

namespace dynmap::ui
{
namespace
{
    constexpr float levelTop = 6.0f, levelBottom = -60.0f;   // dB range of the level lane
    constexpr float gainRange = 24.0f;                       // +-dB of the gain lane
}

HistoryView::HistoryView (DynMapProcessor& p, std::function<int()> selected, std::function<void (int)> onSelect)
    : processor (p), getSelected (std::move (selected)), select (std::move (onSelect))
{
    setTooltip ("History of the selected stage over the last 5 seconds: input level (filled) and output level (line) on "
                "top; below, the gain it applied (down = reduction, up = boost) and, as a thin line, the transient "
                "curve's part of it. Click a chip to watch another stage.");
    startTimerHz (30);
}

void HistoryView::visibilityChanged()
{
    // Frames gathered while hidden are stale: start from now.
    if (isVisible())
        for (int s = 0; s < numStages; ++s)
        {
            processor.engine.getStage (s).history.clear();
            rings[(size_t) s].count = 0;
        }
}

void HistoryView::timerCallback()
{
    pull();

    if (isVisible())
        repaint();
}

void HistoryView::pull()
{
    for (int s = 0; s < numStages; ++s)
    {
        auto& ring = rings[(size_t) s];
        processor.engine.getStage (s).history.pop ([&ring] (const HistoryFrame& f)
        {
            ring.frames[(size_t) ring.next] = f;
            ring.next = (ring.next + 1) % capacity;
            ring.count = juce::jmin (capacity, ring.count + 1);
        });
    }
}

juce::Rectangle<float> HistoryView::plotArea() const
{
    return getLocalBounds().toFloat().withTrimmedBottom (16.0f).withTrimmedRight (26.0f);
}

std::vector<std::pair<int, juce::Rectangle<float>>> HistoryView::chips() const
{
    std::vector<std::pair<int, juce::Rectangle<float>>> result;
    const auto a = plotArea();
    float x = a.getX() + 8.0f;
    const auto& layout = processor.engine.getLayout();

    auto add = [&] (int stage, float width)
    {
        result.push_back ({ stage, { x, a.getY() + 6.0f, width, 18.0f } });
        x += width + 4.0f;
    };

    add (inputStage, 30.0f);
    for (int position = 0; position < layout.numBands; ++position)
        add (bandStage (layout.slots[(size_t) position]), 20.0f);
    add (masterStage, 30.0f);
    return result;
}

void HistoryView::mouseDown (const juce::MouseEvent& e)
{
    for (const auto& [stage, bounds] : chips())
        if (bounds.contains (e.position))
        {
            select (stage);
            repaint();
            return;
        }
}

void HistoryView::paint (juce::Graphics& g)
{
    const auto& pal = palette();
    const auto a = plotArea();
    const int stage = getSelected();
    const auto colour = stageColour (processor, stage);
    const auto& ring = rings[(size_t) stage];

    g.setColour (pal.background.withAlpha (0.7f));
    g.fillRoundedRectangle (a, 5.0f);

    auto levels = a.withTrimmedTop (30.0f);
    auto gains = levels.removeFromBottom (levels.getHeight() * 0.36f);
    levels.removeFromBottom (6.0f);

    auto yLevel = [&] (float db) { return levels.getY() + (levelTop - juce::jlimit (levelBottom, levelTop, db)) / (levelTop - levelBottom) * levels.getHeight(); };
    auto yGain = [&] (float db) { return gains.getCentreY() - juce::jlimit (-gainRange, gainRange, db) / gainRange * gains.getHeight() * 0.5f; };

    // Grids and scales.
    g.setFont (klaud::font (9.5f));
    for (float db : { 0.0f, -12.0f, -24.0f, -36.0f, -48.0f })
    {
        const float y = yLevel (db);
        g.setColour (juce::Colours::white.withAlpha (db == 0.0f ? 0.09f : 0.035f));
        g.drawHorizontalLine ((int) y, a.getX(), a.getRight());
        g.setColour (pal.textDim.withAlpha (0.7f));
        g.drawText (juce::String ((int) db), (int) a.getRight() + 2, (int) y - 6, 24, 12, juce::Justification::centredLeft, false);
    }

    for (float db : { gainRange, 12.0f, 0.0f, -12.0f, -gainRange })
    {
        const float y = yGain (db);
        g.setColour (juce::Colours::white.withAlpha (db == 0.0f ? 0.12f : 0.03f));
        g.drawHorizontalLine ((int) y, a.getX(), a.getRight());
        if (std::abs (db) == 12.0f || db == 0.0f)
        {
            g.setColour (pal.textDim.withAlpha (0.7f));
            g.drawText ((db > 0 ? "+" : "") + juce::String ((int) db), (int) a.getRight() + 2, (int) y - 6, 24, 12,
                        juce::Justification::centredLeft, false);
        }
    }

    const float frameWidth = a.getWidth() / (float) shownFrames;
    for (int s = 1; s <= 4; ++s)
    {
        const float x = a.getRight() - (float) s * 200.0f * frameWidth;   // 200 frames = 1 s
        g.setColour (juce::Colours::white.withAlpha (0.04f));
        g.drawVerticalLine ((int) x, levels.getY(), gains.getBottom());
        g.setColour (pal.textDim.withAlpha (0.8f));
        g.drawText ("-" + juce::String (s) + " s", (int) x - 16, (int) a.getBottom() + 2, 32, 12, juce::Justification::centred, false);
    }

    // Traces, newest at the right.
    const int n = juce::jmin (ring.count, shownFrames);

    if (n > 1)
    {
        juce::Path input, output, transient, gainBand;
        auto xAt = [&] (int age) { return a.getRight() - ((float) age + 0.5f) * frameWidth; };

        input.startNewSubPath (xAt (n - 1), levels.getBottom());
        for (int age = n - 1; age >= 0; --age)
        {
            const auto& f = ring.back (age);
            const float x = xAt (age);
            input.lineTo (x, yLevel (f.inDb));
            if (age == n - 1) output.startNewSubPath (x, yLevel (f.outDb)); else output.lineTo (x, yLevel (f.outDb));
            if (age == n - 1) transient.startNewSubPath (x, yGain (f.transientDb)); else transient.lineTo (x, yGain (f.transientDb));
        }
        input.lineTo (xAt (0), levels.getBottom());
        input.closeSubPath();

        // Gain: the range applied within each frame, as a band from its top to its bottom.
        for (int age = n - 1; age >= 0; --age)
        {
            const auto& f = ring.back (age);
            const float top = yGain (juce::jmax (0.0f, f.gainMaxDb)), bottom = yGain (juce::jmin (0.0f, f.gainMinDb));
            if (bottom - top > 0.5f)
                gainBand.addRectangle (xAt (age) - frameWidth * 0.5f, top, frameWidth + 0.3f, bottom - top);
        }

        g.saveState();
        g.reduceClipRegion (a.toNearestInt());
        g.setColour (colour.withAlpha (0.16f));
        g.fillPath (input);
        g.setColour (colour.withAlpha (0.95f));
        g.strokePath (output, juce::PathStrokeType (1.3f));
        g.setColour (colour.withAlpha (0.55f));
        g.fillPath (gainBand);
        g.setColour (juce::Colours::white.withAlpha (0.65f));
        g.strokePath (transient, juce::PathStrokeType (1.0f));
        g.restoreState();
    }
    else
    {
        g.setColour (pal.textDim);
        g.setFont (klaud::font (12.0f));
        g.drawText ("waiting for audio", a.toNearestInt(), juce::Justification::centred, false);
    }

    // Lane captions.
    g.setFont (klaud::font (9.5f));
    g.setColour (pal.textDim);
    g.drawText ("LEVEL  in / out", levels.toNearestInt().reduced (8, 2), juce::Justification::bottomLeft, false);
    g.drawText ("GAIN", gains.toNearestInt().reduced (8, 2), juce::Justification::topLeft, false);

    // Stage chips.
    g.setFont (klaud::font (10.5f, true));
    for (const auto& [chipStage, bounds] : chips())
    {
        const bool on = chipStage == stage;
        const auto c = stageColour (processor, chipStage);
        g.setColour (on ? c : c.withAlpha (0.18f));
        g.fillRoundedRectangle (bounds, 4.0f);
        g.setColour (on ? pal.background : c);
        const auto label = chipStage == inputStage ? juce::String ("IN") : chipStage == masterStage ? juce::String ("MST")
                                                                            : stageLabel (processor, chipStage).fromLastOccurrenceOf (" ", false, false);
        g.drawText (label, bounds, juce::Justification::centred, false);
    }
}
}
