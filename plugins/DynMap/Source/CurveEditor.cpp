#include "CurveEditor.h"

namespace dynmap::ui
{
Curve CurveEditor::clipboard[2] { Curve (CurveKind::level), Curve (CurveKind::transient) };
bool CurveEditor::clipboardFull[2] { false, false };

namespace
{
    constexpr float pointRadius = 4.5f, handleRadius = 3.0f;
    constexpr float gridStep = 6.0f;                 // grid lines every 6 dB (labels every 12)
    constexpr float snapStep = gridStep * 0.5f;      // Shift snaps points to half the grid

    // Mirrors the curve's gain: compression becomes expansion, a boost becomes a cut.
    Curve invertedGains (const Curve& curve)
    {
        const auto r = curve.getRange();
        juce::StringArray parts;

        for (const auto& p : curve.getPoints())
        {
            float y = -p.y;
            if (curve.getKind() == CurveKind::level)
                y = curve.isLinear() ? (p.y <= 1.0e-6f ? r.yMax : juce::jlimit (r.yMin, r.yMax, p.x * p.x / p.y))
                                     : juce::jlimit (r.yMin, r.yMax, 2.0f * p.x - p.y);
            parts.add (juce::String (p.x) + "," + juce::String (y) + "," + juce::String ((int) p.segment) + "," + juce::String (-p.tension));
        }

        return Curve::fromString (curve.getKind(), (curve.isLinear() ? "lin;" : "") + parts.joinIntoString (";"));
    }

    // dB marks on the linear axes, and which of them get a label.
    constexpr float linearMarks[] { -24.0f, -18.0f, -12.0f, -9.0f, -6.0f, -3.0f, 0.0f, 3.0f, 6.0f };
    bool isLabelledMark (float db) { return db != -24.0f && db != -18.0f && db != -9.0f; }
}

juce::String CurveEditor::segmentName (Segment segment)
{
    switch (segment)
    {
        case Segment::curve:        return "Curve";
        case Segment::sCurve:       return "S-curve";
        case Segment::hold:         return "Hold";
        case Segment::stairs:       return "Stairs";
        case Segment::smoothStairs: return "Smooth stairs";
        case Segment::wave:         return "Wave";
    }
    return {};
}

CurveEditor::CurveEditor (DynMapProcessor& p, CurveKind k)
    : processor (p), kind (k), range (curveRange (k)), accent (palette().accent), curve (k)
{
    setTooltip (kind == CurveKind::level
                    ? "Level map: detected input level (across) to output level (up). Drag points (hold Shift to snap to 3 dB), "
                      "double-click to add or delete, drag the small handles to bend a segment, right-click for shapes and presets. "
                      "Points on the bottom edge mean silence; the dotted lines mark 0 dBFS (the graph goes on to +12 dB)."
                    : "Transient map: how far the signal jumps above its recent level (right, attacks) or falls below it (left, tails) "
                      "to a gain (up = boost). Drag points (hold Shift to snap to 3 dB), double-click to add or delete, "
                      "right-click for shapes and presets.");
    setRepaintsOnMouseActivity (false);
    setWantsKeyboardFocus (true);   // so Ctrl+Z reaches the editor after a click here
    startTimerHz (30);
}

void CurveEditor::setStage (int newStage, juce::Colour newAccent)
{
    stage = newStage;
    accent = newAccent;
    load (processor.engine.curves.get (stage, kind));
    seenChanges = processor.engine.curves.getChangeCount();
    trail.fill (0.0f);
    live = false;
    repaint();
}

juce::Rectangle<float> CurveEditor::plotArea() const
{
    return getLocalBounds().toFloat().withTrimmedLeft (28.0f).withTrimmedBottom (16.0f).withTrimmedTop (6.0f).withTrimmedRight (8.0f);
}

juce::Point<float> CurveEditor::toScreen (float x, float y) const
{
    const auto a = plotArea();
    return { a.getX() + (x - range.xMin) / (range.xMax - range.xMin) * a.getWidth(),
             a.getBottom() - (y - range.yMin) / (range.yMax - range.yMin) * a.getHeight() };
}

juce::Point<float> CurveEditor::fromScreen (juce::Point<float> p) const
{
    const auto a = plotArea();
    return { juce::jlimit (range.xMin, range.xMax, range.xMin + (p.x - a.getX()) / a.getWidth() * (range.xMax - range.xMin)),
             juce::jlimit (range.yMin, range.yMax, range.yMin + (a.getBottom() - p.y) / a.getHeight() * (range.yMax - range.yMin)) };
}

juce::Point<float> CurveEditor::handlePosition (int segment) const
{
    const auto& points = curve.getPoints();
    const auto& a = points[(size_t) segment];
    const auto& b = points[(size_t) segment + 1];
    const float x = 0.5f * (a.x + b.x);
    return toScreen (x, Curve::segmentValue (a, b, x));
}

// A segment within 3 degrees of flat on screen: its bend handle can't do anything visible there.
bool CurveEditor::isNearlyFlat (int segment) const
{
    const auto& points = curve.getPoints();
    const auto a = toScreen (points[(size_t) segment].x, points[(size_t) segment].y);
    const auto b = toScreen (points[(size_t) segment + 1].x, points[(size_t) segment + 1].y);
    return std::abs (b.y - a.y) <= 0.0524f * std::abs (b.x - a.x);   // tan (3 degrees)
}

CurveEditor::Hit CurveEditor::hitTest (juce::Point<float> position) const
{
    const auto& points = curve.getPoints();
    Hit best;
    float bestDistance = 9.0f;

    for (int i = 0; i < (int) points.size(); ++i)
    {
        const float d = toScreen (points[(size_t) i].x, points[(size_t) i].y).getDistanceFrom (position);
        if (d < bestDistance)
        {
            bestDistance = d;
            best = { Hit::point, i };
        }
    }

    if (best.type != Hit::none)
        return best;

    bestDistance = 7.0f;

    for (int i = 0; i + 1 < (int) points.size(); ++i)
    {
        const float d = handlePosition (i).getDistanceFrom (position);
        if (d < bestDistance)
        {
            bestDistance = d;
            best = { Hit::handle, i };
        }
    }

    return best;
}

void CurveEditor::load (const Curve& c)
{
    curve = c;
    range = curve.getRange();
}

void CurveEditor::setLinear (bool linear)
{
    if (kind != CurveKind::level || linear == curve.isLinear())
        return;

    load (curve.withScale (linear));
    commit();
}

float CurveEditor::curveXForDb (float db) const
{
    return curve.isLinear() ? juce::jlimit (range.xMin, range.xMax, std::pow (10.0f, db / 20.0f)) : juce::jlimit (range.xMin, range.xMax, db);
}

float CurveEditor::snap (float v) const
{
    if (! curve.isLinear())
        return std::round (v / snapStep) * snapStep;

    // Linear axes snap to the same 3 dB steps (and to 0 below -60 dB).
    if (v < 0.001f)
        return 0.0f;
    return std::pow (10.0f, std::round (20.0f * std::log10 (v) / snapStep) * snapStep / 20.0f);
}

juce::String CurveEditor::levelText (float v) const
{
    if (! curve.isLinear())
        return juce::String (v, 1);
    return v <= 1.0e-6f ? juce::String ("-inf") : juce::String (20.0f * std::log10 (v), 1);
}

void CurveEditor::commit()
{
    const auto before = processor.engine.curves.get (stage, kind).toString();
    processor.engine.curves.set (stage, kind, curve);

    if (drag.type == Hit::none)   // a drag is recorded once, when it ends
        processor.curveUndo.record (stage, kind, before, curve.toString());

    seenChanges = processor.engine.curves.getChangeCount();
    repaint();
}

void CurveEditor::timerCallback()
{
    const int changes = processor.engine.curves.getChangeCount();

    if (changes != seenChanges && drag.type == Hit::none)
    {
        load (processor.engine.curves.get (stage, kind));
        seenChanges = changes;
    }

    const auto& meter = processor.engine.getStage (stage).meter;
    const bool wasLive = live;
    live = meter.active.load();

    if (live)
    {
        liveX = kind == CurveKind::level ? meter.levelDb.load() : meter.transientDb.load();
        gainDb = meter.gainDb.load();
        trail[(size_t) trailHead] = liveX;
        trailHead = (trailHead + 1) % trailLength;
    }

    if (live || wasLive)
        repaint();
}

void CurveEditor::paint (juce::Graphics& g)
{
    const auto& pal = palette();
    const auto a = plotArea();

    g.setColour (pal.background.withAlpha (0.6f));
    g.fillRoundedRectangle (a.expanded (2.0f), 4.0f);

    // Grid: every 6 dB on dB axes; dB marks at their amplitude on linear axes.
    const float step = gridStep;
    g.setFont (klaud::font (9.5f));

    if (curve.isLinear())
    {
        for (float db : linearMarks)
        {
            const float v = std::pow (10.0f, db / 20.0f);
            const auto pos = toScreen (v, v);
            const bool labelled = isLabelledMark (db);
            g.setColour (juce::Colours::white.withAlpha (labelled ? 0.07f : 0.035f));
            g.drawVerticalLine ((int) pos.x, a.getY(), a.getBottom());
            g.drawHorizontalLine ((int) pos.y, a.getX(), a.getRight());

            if (labelled)
            {
                const auto text = (db > 0.0f ? "+" : "") + juce::String ((int) db);
                g.setColour (pal.textDim.withAlpha (0.8f));
                g.drawText (text, (int) pos.x - 14, (int) a.getBottom() + 2, 28, 12, juce::Justification::centred, false);
                g.drawText (text, 0, (int) pos.y - 6, 24, 12, juce::Justification::centredRight, false);
            }
        }

        g.setColour (pal.textDim.withAlpha (0.8f));
        g.drawText ("-inf", (int) a.getX() - 2, (int) a.getBottom() + 2, 24, 12, juce::Justification::centredLeft, false);
    }

    for (float v = range.xMin; ! curve.isLinear() && v <= range.xMax + 0.01f; v += step)
    {
        const bool major = std::fmod (std::abs (v), 12.0f) < 0.01f;
        const float x = toScreen (v, range.yMin).x;
        g.setColour (juce::Colours::white.withAlpha (major ? 0.07f : 0.03f));
        g.drawVerticalLine ((int) x, a.getY(), a.getBottom());

        if (major)
        {
            g.setColour (pal.textDim.withAlpha (0.8f));
            g.drawText (juce::String ((int) v), (int) x - 14, (int) a.getBottom() + 2, 28, 12, juce::Justification::centred, false);
        }
    }

    for (float v = range.yMin; ! curve.isLinear() && v <= range.yMax + 0.01f; v += step)
    {
        const bool major = std::fmod (std::abs (v), 12.0f) < 0.01f;
        const float y = toScreen (range.xMin, v).y;
        g.setColour (juce::Colours::white.withAlpha (major ? 0.07f : 0.03f));
        g.drawHorizontalLine ((int) y, a.getX(), a.getRight());

        if (major)
        {
            g.setColour (pal.textDim.withAlpha (0.8f));
            g.drawText (juce::String ((int) v), 0, (int) y - 6, 24, 12, juce::Justification::centredRight, false);
        }
    }

    // Reference: the identity line (level) or 0 dB (transient).
    {
        juce::Path reference;
        if (kind == CurveKind::level)
        {
            reference.startNewSubPath (toScreen (range.xMin, range.yMin));
            reference.lineTo (toScreen (range.xMax, range.yMax));
        }
        else
        {
            reference.startNewSubPath (toScreen (range.xMin, 0.0f));
            reference.lineTo (toScreen (range.xMax, 0.0f));
        }

        juce::Path dashed;
        const float dashes[] { 4.0f, 4.0f };
        juce::PathStrokeType (1.0f).createDashedStroke (dashed, reference, dashes, 2);
        g.setColour (juce::Colours::white.withAlpha (0.18f));
        g.fillPath (dashed);
    }

    // 0 dBFS on both axes: the graph goes on to +12 dB (room for pre gain), so full scale is marked.
    if (kind == CurveKind::level)
    {
        g.setColour (colours::master.withAlpha (0.45f));
        const float fullScale = curve.isLinear() ? 1.0f : 0.0f;

        for (const auto& [from, to] : { std::pair { toScreen (fullScale, range.yMin), toScreen (fullScale, range.yMax) },
                                        std::pair { toScreen (range.xMin, fullScale), toScreen (range.xMax, fullScale) } })
        {
            juce::Path line, dotted;
            line.startNewSubPath (from);
            line.lineTo (to);
            const float dots[] { 1.5f, 3.0f };
            juce::PathStrokeType (1.2f).createDashedStroke (dotted, line, dots, 2);
            g.fillPath (dotted);
        }

        const auto corner = toScreen (fullScale, fullScale);
        g.setFont (klaud::font (9.5f, true));
        g.setColour (colours::master.withAlpha (0.75f));
        g.drawText ("0 dBFS", (int) a.getX() + 34, (int) corner.y - 13, 50, 12, juce::Justification::centredLeft, false);
    }

    // Curve, with the area between it and the reference tinted.
    juce::Path line, fill;
    {
        bool first = true;
        for (float px = a.getX(); px <= a.getRight() + 0.5f; px += 1.0f)
        {
            const float x = fromScreen ({ px, a.getY() }).x;
            const auto p = toScreen (x, curve.evaluate (x));

            if (first) { line.startNewSubPath (p); first = false; }
            else       line.lineTo (p);
        }

        fill = line;
        if (kind == CurveKind::level)
        {
            fill.lineTo (toScreen (range.xMax, range.yMax));
            fill.lineTo (toScreen (range.xMin, range.yMin));
        }
        else
        {
            fill.lineTo (toScreen (range.xMax, 0.0f));
            fill.lineTo (toScreen (range.xMin, 0.0f));
        }
        fill.closeSubPath();
    }

    g.saveState();
    g.reduceClipRegion (a.toNearestInt());
    g.setColour (accent.withAlpha (0.10f));
    g.fillPath (fill);
    g.setColour (accent);
    g.strokePath (line, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.restoreState();

    // Live dot and its trail.
    if (live)
    {
        for (int i = 0; i < trailLength; ++i)
        {
            const int index = (trailHead + i) % trailLength;
            if (kind == CurveKind::level && trail[(size_t) index] <= levelRange.xMin + 0.01f)
                continue;

            const float x = kind == CurveKind::level ? curveXForDb (trail[(size_t) index])
                                                     : juce::jlimit (range.xMin, range.xMax, trail[(size_t) index]);

            const auto p = toScreen (x, curve.evaluate (x));
            const float age = (float) i / (float) trailLength;
            g.setColour (juce::Colours::white.withAlpha (0.25f * age * age));
            g.fillEllipse (juce::Rectangle<float> (3.0f, 3.0f).withCentre (p));
        }

        const float x = kind == CurveKind::level ? curveXForDb (liveX) : juce::jlimit (range.xMin, range.xMax, liveX);
        if (! (kind == CurveKind::level && liveX < levelRange.xMin - 20.0f))
        {
            const auto p = toScreen (x, curve.evaluate (x));
            g.setColour (accent.withAlpha (0.25f));
            g.fillEllipse (juce::Rectangle<float> (14.0f, 14.0f).withCentre (p));
            g.setColour (juce::Colours::white);
            g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre (p));
        }
    }

    // Points and bend handles.
    const auto& points = curve.getPoints();

    for (int i = 0; i + 1 < (int) points.size(); ++i)
    {
        const auto h = handlePosition (i);
        const bool hot = (hover.type == Hit::handle && hover.index == i) || (drag.type == Hit::handle && drag.index == i);
        g.setColour (accent.withAlpha (hot ? 1.0f : 0.45f));
        g.drawEllipse (juce::Rectangle<float> (handleRadius * 2.0f, handleRadius * 2.0f).withCentre (h), hot ? 1.6f : 1.0f);
    }

    for (int i = 0; i < (int) points.size(); ++i)
    {
        const auto p = toScreen (points[(size_t) i].x, points[(size_t) i].y);
        const bool hot = (hover.type == Hit::point && hover.index == i) || (drag.type == Hit::point && drag.index == i);
        const float r = hot ? pointRadius + 1.5f : pointRadius;
        g.setColour (pal.background);
        g.fillEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (p));
        g.setColour (accent);
        g.drawEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (p), 1.8f);
    }

    // Labels and readout.
    g.setFont (klaud::font (9.5f, true));
    g.setColour (pal.textDim.withAlpha (0.7f));

    if (kind == CurveKind::level)
    {
        g.drawText ("OUT", (int) a.getX() + 6, (int) a.getY() + 4, 40, 12, juce::Justification::centredLeft, false);
        const bool sidechain = processor.apvts.getRawParameterValue (stageParamId (stage, ids::scSource))->load() > 0.5f;
        g.drawText (sidechain ? "SIDECHAIN" : "IN", (int) a.getRight() - 86, (int) a.getBottom() - 16, 80, 12, juce::Justification::centredRight, false);
    }
    else
    {
        g.drawText ("TAILS", (int) a.getX() + 6, (int) a.getBottom() - 16, 60, 12, juce::Justification::centredLeft, false);
        g.drawText ("ATTACKS", (int) a.getRight() - 66, (int) a.getBottom() - 16, 60, 12, juce::Justification::centredRight, false);
        g.drawText ("BOOST", (int) a.getX() + 6, (int) a.getY() + 4, 60, 12, juce::Justification::centredLeft, false);
    }

    if (drag.type == Hit::point)
    {
        const auto& p = points[(size_t) drag.index];
        const auto text = kind == CurveKind::level ? levelText (p.x) + " > " + levelText (p.y) + " dB"
                                                   : juce::String (p.x, 1) + " dB: " + formatDb (p.y);
        g.setFont (klaud::font (11.0f, true));
        g.setColour (pal.text);
        g.drawText (text, a.reduced (6.0f, 4.0f).toNearestInt(), juce::Justification::topRight, false);
    }
    else if (drag.type == Hit::handle)
    {
        const auto& p = points[(size_t) drag.index];
        juce::String text = segmentName (p.segment);
        if (p.segment == Segment::stairs || p.segment == Segment::smoothStairs)
            text << ": " << Curve::stepCount (p.tension) << " steps";
        else if (p.segment == Segment::wave)
            text << ": " << (2 * Curve::waveCycles (p.tension) + 1) << " half waves";
        else
            text << ": " << juce::roundToInt (p.tension * 100.0f) << " %";

        g.setFont (klaud::font (11.0f, true));
        g.setColour (pal.text);
        g.drawText (text, a.reduced (6.0f, 4.0f).toNearestInt(), juce::Justification::topRight, false);
    }
    else if (live && kind == CurveKind::level)
    {
        g.setFont (klaud::font (11.0f));
        g.setColour (pal.textDim);
        g.drawText ("gain " + formatDb (gainDb), a.reduced (6.0f, 4.0f).toNearestInt(), juce::Justification::topRight, false);
    }
}

void CurveEditor::mouseMove (const juce::MouseEvent& e)
{
    const auto h = hitTest (e.position);
    if (h.type != hover.type || h.index != hover.index)
    {
        hover = h;
        setMouseCursor (h.type == Hit::none ? juce::MouseCursor::CrosshairCursor
                                            : h.type == Hit::handle ? juce::MouseCursor::UpDownResizeCursor
                                                                    : juce::MouseCursor::DraggingHandCursor);
        repaint();
    }
}

void CurveEditor::mouseExit (const juce::MouseEvent&)
{
    hover = {};
    repaint();
}

void CurveEditor::mouseDown (const juce::MouseEvent& e)
{
    const auto h = hitTest (e.position);
    dragBefore = processor.engine.curves.get (stage, kind).toString();

    if (e.mods.isPopupMenu())
    {
        if (h.type == Hit::handle)
            showSegmentMenu (h.index);
        else if (h.type == Hit::point)
            showSegmentMenu (juce::jmin (h.index, curve.getNumPoints() - 2));
        else
            showCurveMenu();
        return;
    }

    if (h.type == Hit::point && e.mods.isAltDown())
    {
        curve.removePoint (h.index);
        commit();
        return;
    }

    drag = h;
    dragStart = e.position;

    if (h.type == Hit::handle)
        dragStartTension = curve.getPoints()[(size_t) h.index].tension;
}

void CurveEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (drag.type == Hit::point)
    {
        auto p = fromScreen (e.position);

        if (e.mods.isShiftDown())
            p = { snap (p.x), snap (p.y) };

        curve.movePoint (drag.index, p.x, p.y);
        commit();
    }
    else if (drag.type == Hit::handle)
    {
        const float delta = (dragStart.y - e.position.y) / 120.0f;
        const auto& points = curve.getPoints();
        const auto segment = points[(size_t) drag.index].segment;

        // Bends follow the mouse (up = bulge up); counts (stairs, waves) grow when dragging up.
        const bool isCount = segment == Segment::stairs || segment == Segment::smoothStairs || segment == Segment::wave;
        curve.setTension (drag.index, dragStartTension + (isCount ? delta * 0.5f : delta));
        commit();
    }
}

void CurveEditor::mouseUp (const juce::MouseEvent&)
{
    if (drag.type != Hit::none)
        processor.curveUndo.record (stage, kind, dragBefore, processor.engine.curves.get (stage, kind).toString());

    drag = {};
    repaint();
}

void CurveEditor::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto h = hitTest (e.position);

    if (h.type == Hit::point)
        curve.removePoint (h.index);
    else if (h.type == Hit::handle && ! isNearlyFlat (h.index))
        curve.setTension (h.index, 0.0f);
    else    // empty space, or the handle of a (nearly) flat segment: add a point there
    {
        const auto p = fromScreen (e.position);
        curve.addPoint (p.x, p.y);
    }

    drag = {};
    commit();
}

void CurveEditor::showSegmentMenu (int segment)
{
    if (! juce::isPositiveAndBelow (segment, curve.getNumPoints() - 1))
        return;

    juce::PopupMenu menu;
    const auto current = curve.getPoints()[(size_t) segment].segment;

    for (int i = 0; i < numSegmentTypes; ++i)
        menu.addItem (i + 1, segmentName ((Segment) i), true, (int) current == i);

    menu.addSeparator();
    menu.addItem (100, "Reset bend");
    menu.addItem (101, "Delete point", segment + 1 < curve.getNumPoints() - 1);

    juce::Component::SafePointer<CurveEditor> safeThis (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [safeThis, segment] (int result)
    {
        if (safeThis == nullptr || result == 0)
            return;

        auto& self = *safeThis;
        if (result <= numSegmentTypes)
            self.curve.setSegment (segment, (Segment) (result - 1));
        else if (result == 100)
            self.curve.setTension (segment, 0.0f);
        else if (result == 101)
            self.curve.removePoint (segment + 1);

        self.commit();
    });
}

void CurveEditor::showCurveMenu()
{
    juce::PopupMenu menu, shapes;
    const auto names = Curve::presetNames (kind);

    for (int i : Curve::presetMenuOrder (kind))
        menu.addItem (i + 1, names[i]);

    for (int i = 0; i < numSegmentTypes; ++i)
        shapes.addItem (200 + i, segmentName ((Segment) i));

    const int k = kind == CurveKind::level ? 0 : 1;
    menu.addSeparator();
    menu.addSubMenu ("Set every segment to", shapes);
    menu.addItem (300, "Invert gains");
    if (kind == CurveKind::level)
        menu.addItem (303, "Linear scale", true, curve.isLinear());
    menu.addItem (301, "Copy curve");
    menu.addItem (302, "Paste curve", clipboardFull[k]);

    juce::Component::SafePointer<CurveEditor> safeThis (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [safeThis, k] (int result)
    {
        if (safeThis == nullptr || result == 0)
            return;

        auto& self = *safeThis;

        if (result >= 1 && result < 200)
            self.load (Curve::preset (self.kind, result - 1));
        else if (result >= 200 && result < 200 + numSegmentTypes)
            for (int i = 0; i + 1 < self.curve.getNumPoints(); ++i)
                self.curve.setSegment (i, (Segment) (result - 200));
        else if (result == 300)
            self.load (invertedGains (self.curve));
        else if (result == 303)
        {
            self.setLinear (! self.curve.isLinear());
            return;
        }
        else if (result == 301)
        {
            clipboard[k] = self.curve;
            clipboardFull[k] = true;
            return;
        }
        else if (result == 302)
            self.load (clipboard[k]);

        self.commit();
    });
}
}
