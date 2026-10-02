#include "Curve.h"

namespace dynmap
{
namespace
{
    constexpr float minPointGap = 0.05f;   // dB between neighbouring points

    float smoothstep (float t)
    {
        t = juce::jlimit (0.0f, 1.0f, t);
        return t * t * (3.0f - 2.0f * t);
    }

    Curve makeCurve (CurveKind kind, std::initializer_list<CurvePoint> points)
    {
        juce::StringArray parts;
        for (const auto& p : points)
            parts.add (juce::String (p.x) + "," + juce::String (p.y) + "," + juce::String ((int) p.segment) + "," + juce::String (p.tension));

        return Curve::fromString (kind, parts.joinIntoString (";"));
    }

    constexpr auto curveSeg  = Segment::curve;
    constexpr auto holdSeg   = Segment::hold;
    constexpr auto stairsSeg = Segment::stairs;
}

Curve::Curve (CurveKind k) : kind (k)
{
    const auto r = curveRange (kind);

    if (kind == CurveKind::level)
        points = { { r.xMin, r.yMin }, { r.xMax, r.yMax } };
    else
        points = { { r.xMin, 0.0f }, { r.xMax, 0.0f } };
}

int Curve::stepCount (float tension)
{
    return 2 + juce::roundToInt ((juce::jlimit (-1.0f, 1.0f, tension) + 1.0f) * 7.0f);
}

int Curve::waveCycles (float tension)
{
    return juce::roundToInt ((juce::jlimit (-1.0f, 1.0f, tension) + 1.0f) * 3.5f);
}

float Curve::segmentValue (const CurvePoint& a, const CurvePoint& b, float x)
{
    const float width = b.x - a.x;
    const float t = width > 0.0f ? juce::jlimit (0.0f, 1.0f, (x - a.x) / width) : 1.0f;
    const float tension = juce::jlimit (-1.0f, 1.0f, a.tension);
    float s = t;

    switch (a.segment)
    {
        case Segment::curve:
        {
            // Positive tension bends the line upwards whichever way it goes. Exponential shape,
            // so the slope stays finite at both ends.
            const float k = 8.0f * (b.y >= a.y ? -tension : tension);
            s = std::abs (k) < 1.0e-3f ? t : std::expm1 (k * t) / std::expm1 (k);
            break;
        }

        case Segment::sCurve:
        {
            const float e = std::exp2 (3.0f * tension);
            s = t < 0.5f ? 0.5f * std::pow (2.0f * t, e) : 1.0f - 0.5f * std::pow (2.0f - 2.0f * t, e);
            break;
        }

        case Segment::hold:
            s = t < 1.0f ? 0.0f : 1.0f;
            break;

        case Segment::stairs:
        {
            const int n = stepCount (tension);
            s = t >= 1.0f ? 1.0f : std::floor (t * (float) n) / (float) (n - 1);
            break;
        }

        case Segment::smoothStairs:
        {
            const int n = stepCount (tension);
            const float width01 = 0.5f / (float) n;
            float level = 0.0f;

            for (int j = 1; j < n; ++j)
                level += smoothstep ((t - (float) j / (float) n) / width01 + 0.5f);

            s = level / (float) (n - 1);
            break;
        }

        case Segment::wave:
        {
            const int m = waveCycles (tension);
            s = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * (float) (2 * m + 1) * t);
            break;
        }
    }

    return a.y + (b.y - a.y) * juce::jlimit (0.0f, 1.0f, s);
}

float Curve::evaluate (float x) const
{
    if (x <= points.front().x) return points.front().y;
    if (x >= points.back().x)  return points.back().y;

    const auto next = std::upper_bound (points.begin(), points.end(), x,
                                        [] (float value, const CurvePoint& p) { return value < p.x; });
    const auto& b = *next;
    const auto& a = *(next - 1);
    return segmentValue (a, b, x);
}

float Curve::gainAt (float x) const
{
    const float y = evaluate (x);

    if (kind == CurveKind::transient)
        return y;

    // The bottom edge means silence, except at the bottom-left corner itself (where an identity
    // line starts): there the curve counts as silent only if it stays on the floor.
    const bool onFloor = x <= levelRange.xMin + 0.001f ? evaluate (levelRange.xMin + 0.01f) <= levelRange.yMin + 0.001f
                                                        : y <= levelRange.yMin + 0.001f;
    return onFloor ? silenceDb : y - x;
}

bool Curve::isNeutral() const
{
    for (size_t i = 0; i < points.size(); ++i)
    {
        const auto& p = points[i];
        const float expected = kind == CurveKind::level ? p.x : 0.0f;

        if (std::abs (p.y - expected) > 1.0e-4f)
            return false;

        // Straight segments between neutral points are neutral. Other shapes between points
        // with equal y (transient curves) are flat anyway.
        if (i + 1 < points.size() && kind == CurveKind::level
            && ! (p.segment == Segment::curve && std::abs (p.tension) < 1.0e-4f))
            return false;
    }

    return true;
}

int Curve::addPoint (float x, float y)
{
    const auto r = curveRange (kind);
    x = juce::jlimit (r.xMin + minPointGap, r.xMax - minPointGap, x);
    y = juce::jlimit (r.yMin, r.yMax, y);

    const auto it = std::upper_bound (points.begin(), points.end(), x,
                                      [] (float value, const CurvePoint& p) { return value < p.x; });
    const int index = (int) std::distance (points.begin(), it);

    // The new point continues the shape of the segment it splits.
    const auto& previous = points[(size_t) index - 1];
    points.insert (it, { x, y, previous.segment, previous.tension });
    sanitise();
    return index;
}

void Curve::removePoint (int index)
{
    if (index <= 0 || index >= (int) points.size() - 1)
        return;

    points.erase (points.begin() + index);
}

void Curve::movePoint (int index, float x, float y)
{
    if (! juce::isPositiveAndBelow (index, (int) points.size()))
        return;

    const auto r = curveRange (kind);
    auto& p = points[(size_t) index];
    p.y = juce::jlimit (r.yMin, r.yMax, y);

    if (index == 0)
        p.x = r.xMin;
    else if (index == (int) points.size() - 1)
        p.x = r.xMax;
    else
        p.x = juce::jlimit (points[(size_t) index - 1].x + minPointGap, points[(size_t) index + 1].x - minPointGap, x);
}

void Curve::setSegment (int index, Segment segment)
{
    if (juce::isPositiveAndBelow (index, (int) points.size() - 1))
    {
        points[(size_t) index].segment = segment;
        points[(size_t) index].tension = 0.0f;
    }
}

void Curve::setTension (int index, float tension)
{
    if (juce::isPositiveAndBelow (index, (int) points.size() - 1))
        points[(size_t) index].tension = juce::jlimit (-1.0f, 1.0f, tension);
}

void Curve::sanitise()
{
    const auto r = curveRange (kind);

    for (auto& p : points)
    {
        p.x = juce::jlimit (r.xMin, r.xMax, std::isfinite (p.x) ? p.x : 0.0f);
        p.y = juce::jlimit (r.yMin, r.yMax, std::isfinite (p.y) ? p.y : 0.0f);
        p.tension = juce::jlimit (-1.0f, 1.0f, std::isfinite (p.tension) ? p.tension : 0.0f);
    }

    std::stable_sort (points.begin(), points.end(), [] (const CurvePoint& a, const CurvePoint& b) { return a.x < b.x; });

    if (points.size() < 2)
    {
        *this = Curve (kind);
        return;
    }

    points.front().x = r.xMin;
    points.back().x = r.xMax;

    // Drop points squeezed on top of each other.
    for (size_t i = 1; i + 1 < points.size();)
    {
        if (points[i].x - points[i - 1].x < minPointGap || points.back().x - points[i].x < minPointGap)
            points.erase (points.begin() + (std::ptrdiff_t) i);
        else
            ++i;
    }
}

juce::String Curve::toString() const
{
    juce::StringArray parts;

    for (const auto& p : points)
        parts.add (juce::String (p.x, 3) + "," + juce::String (p.y, 3) + ","
                   + juce::String ((int) p.segment) + "," + juce::String (p.tension, 3));

    return parts.joinIntoString (";");
}

Curve Curve::fromString (CurveKind kind, const juce::String& text)
{
    Curve curve (kind);
    std::vector<CurvePoint> parsed;

    for (const auto& part : juce::StringArray::fromTokens (text, ";", ""))
    {
        const auto values = juce::StringArray::fromTokens (part, ",", "");
        if (values.size() < 2)
            continue;

        CurvePoint p;
        p.x = values[0].getFloatValue();
        p.y = values[1].getFloatValue();

        if (values.size() > 2)
            p.segment = (Segment) juce::jlimit (0, numSegmentTypes - 1, values[2].getIntValue());
        if (values.size() > 3)
            p.tension = values[3].getFloatValue();

        parsed.push_back (p);
    }

    if (parsed.size() >= 2)
    {
        curve.points = std::move (parsed);
        curve.sanitise();
    }

    return curve;
}

juce::StringArray Curve::presetNames (CurveKind kind)
{
    if (kind == CurveKind::level)
        return { "Neutral", "Compress 2:1", "Compress 4:1", "Limit", "Upward 2:1", "OTT", "Smash",
                 "Expand 1:2", "Gate", "Invert", "Stairs", "Soft Clip (waveshaper)", "Fold (waveshaper)", "Extreme OTT", "Duck (sidechain)" };

    return { "Neutral", "Punch", "Snap", "Soften", "Tighten", "Bloom", "Flip", "Punch Hard" };
}

std::vector<int> Curve::presetMenuOrder (CurveKind kind)
{
    // Indices are stored in factory presets, so new shapes are appended and only sorted for the menu.
    if (kind == CurveKind::level)
        return { 0, 1, 2, 3, 4, 5, 13, 6, 7, 8, 14, 9, 10, 11, 12 };

    return { 0, 1, 7, 2, 3, 4, 5, 6 };
}

Curve Curve::preset (CurveKind kind, int index)
{
    if (kind == CurveKind::level)
    {
        switch (index)
        {
            case 1:  return makeCurve (kind, { { -72, -72 }, { -24, -24 }, { 12, -6 } });
            case 2:  return makeCurve (kind, { { -72, -72 }, { -24, -24 }, { 12, -15 } });
            case 3:  return makeCurve (kind, { { -72, -72 }, { -6, -6 }, { 12, -6 } });
            case 4:  return makeCurve (kind, { { -72, -51 }, { -30, -30 }, { 12, 12 } });
            // OTT (Ableton's original: 4.17:1 up below -41, 66:1 down above -33) with ~8 dB of its band
            // makeup baked in, so it sounds like OTT on any stage. Extreme OTT (13): 8:1 up below -38,
            // brick wall above -32, +10 dB makeup.
            case 5:  return makeCurve (kind, { { -72, -40.43f }, { -41, -33 }, { -33, -25 }, { 12, -24.32f } });
            case 6:  return makeCurve (kind, { { -72, -40 }, { -48, -16, curveSeg, 0.3f }, { 12, -8 } });
            case 7:  return makeCurve (kind, { { -72, -72, holdSeg }, { -56, -72 }, { -40, -40 }, { 12, 12 } });
            case 8:  return makeCurve (kind, { { -72, -72, holdSeg }, { -45, -45 }, { 12, 12 } });
            case 9:  return makeCurve (kind, { { -72, -72 }, { -30, -30 }, { 12, -54 } });
            case 10: return makeCurve (kind, { { -72, -72, stairsSeg, -0.286f }, { 12, 12 } });
            case 11: return makeCurve (kind, { { -72, -72 }, { -18, -18, curveSeg, 0.55f }, { 12, -1 } });
            case 12: return makeCurve (kind, { { -72, -72 }, { -12, -12, Segment::wave, -0.43f }, { 12, -12 } });
            // Duck: meant for a sidechain; up to 18 dB down once the trigger passes about -40 dB.
            case 14: return makeCurve (kind, { { -72, -72 }, { -40, -40 }, { -10, -28 }, { 12, -6 } });
            case 13: return makeCurve (kind, { { -72, -32.25f }, { -38, -28 }, { -32, -22 }, { 12, -22 } });
            default: break;
        }

        return Curve (kind);
    }

    switch (index)
    {
        case 1:  return makeCurve (kind, { { -24, 0 }, { 0, 0 }, { 12, 8 }, { 24, 10 } });
        case 2:  return makeCurve (kind, { { -24, 0 }, { 0, 0 }, { 6, 12 }, { 24, 12 } });
        case 3:  return makeCurve (kind, { { -24, 0 }, { 0, 0 }, { 12, -8 }, { 24, -12 } });
        case 4:  return makeCurve (kind, { { -24, -18 }, { -12, -8 }, { 0, 0 }, { 24, 0 } });
        case 5:  return makeCurve (kind, { { -24, 12 }, { -12, 6 }, { 0, 0 }, { 24, 0 } });
        case 6:  return makeCurve (kind, { { -24, 10 }, { 0, 0 }, { 24, -10 } });
        case 7:  return makeCurve (kind, { { -24, 0 }, { 0, -6 }, { 12, 8 }, { 24, 10 } });
        default: break;
    }

    return Curve (kind);
}

//==============================================================================
void CurveTable::bake (const Curve& curve)
{
    const auto r = curveRange (curve.getKind());
    xMin = r.xMin;
    stepsPerDb = (float) (size - 1) / (r.xMax - r.xMin);
    neutral = curve.isNeutral();

    for (int i = 0; i < size; ++i)
        gainDb[(size_t) i] = neutral ? 0.0f : curve.gainAt (r.xMin + (float) i / stepsPerDb);
}
}
