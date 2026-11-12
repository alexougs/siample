#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include "../Slice.h"
#include "Theme.h"

namespace siample
{

/** L'enveloppe de volume dessinée en courbe : on attrape les quatre points pour régler
    l'attaque, le decay, le sustain et le release. */
class EnvelopeEditor final : public juce::Component,
                             public juce::SettableTooltipClient
{
public:
    std::function<void()> onChange;

    EnvelopeEditor()
    {
        setTooltip (fr ("Enveloppe de volume. Glisse les points : attaque (montée), decay (descente), "
                        "sustain (niveau tenu), release (extinction). Double-clic : remise à zéro."));
    }

    void setValues (float attack, float decay, float sustain, float release, juce::Colour newColour)
    {
        a = attack; d = decay; s = sustain; r = release;
        colour = newColour;
        repaint();
    }

    float getAttack() const    { return a; }
    float getDecay() const     { return d; }
    float getSustain() const   { return s; }
    float getRelease() const   { return r; }

    void paint (juce::Graphics& g) override
    {
        const float alpha = isEnabled() ? 1.0f : 0.35f;
        const auto points = getPoints();
        const auto area = getCurveArea();

        juce::Path line;
        line.startNewSubPath (area.getX(), area.getBottom());

        for (const auto& p : points)
            line.lineTo (p);

        juce::Path fill (line);
        fill.closeSubPath();

        g.setColour (colour.withAlpha (0.12f * alpha));
        g.fillPath (fill);
        g.setColour (colour.withMultipliedAlpha (alpha));
        g.strokePath (line, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        for (int i = 0; i < 4; ++i)
        {
            const float radius = i == dragging || i == hovered ? 5.5f : 4.0f;
            g.fillEllipse (points[(size_t) i].x - radius, points[(size_t) i].y - radius, radius * 2.0f, radius * 2.0f);
        }

        // Valeurs
        const juce::String labels[] = { "A " + formatTime (a), "D " + formatTime (d),
                                        "S " + juce::String (juce::roundToInt (s * 100.0f)) + " %", "R " + formatTime (r) };
        auto row = getLocalBounds().removeFromBottom (16);
        const int cell = row.getWidth() / 4;

        g.setFont (theme::font (11.0f));

        for (int i = 0; i < 4; ++i)
        {
            g.setColour ((i == dragging ? theme::text : theme::textDim).withMultipliedAlpha (alpha));
            g.drawText (labels[i], row.removeFromLeft (cell),
                        i == 0 ? juce::Justification::centredLeft : i == 3 ? juce::Justification::centredRight
                                                                         : juce::Justification::centred, false);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const int hit = hitTestPoint (e.position);

        if (hit != hovered)
        {
            hovered = hit;
            setMouseCursor (hit >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
            repaint();
        }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        hovered = -1;
        repaint();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragging = isEnabled() ? hitTestPoint (e.position, 24.0f) : -1;
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging < 0)
            return;

        const auto area = getCurveArea();
        const float segment = area.getWidth() * 0.25f;
        const auto points = getPoints();
        const float level = juce::jlimit (0.0f, 1.0f, (area.getBottom() - e.position.y) / area.getHeight());

        // Les durées suivent une échelle douce : plus de précision sur les temps courts
        auto toTime = [segment] (float pixels, float maximum)
        {
            const float t = juce::jlimit (0.0f, 1.0f, pixels / segment);
            return std::round (t * t * maximum * 1000.0f) / 1000.0f;
        };

        if (dragging == 0)
            a = toTime (e.position.x - area.getX(), 2.0f);
        else if (dragging == 1)
        {
            d = toTime (e.position.x - points[0].x, 2.0f);
            s = std::round (level * 100.0f) / 100.0f;
        }
        else if (dragging == 2)
            s = std::round (level * 100.0f) / 100.0f;
        else
            r = toTime (e.position.x - points[2].x, 3.0f);

        repaint();

        if (onChange != nullptr)
            onChange();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        dragging = -1;
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        if (! isEnabled())
            return;

        const SliceParams defaults;
        a = defaults.attack; d = defaults.decay; s = defaults.sustain; r = defaults.release;
        repaint();

        if (onChange != nullptr)
            onChange();
    }

private:
    static juce::String formatTime (float seconds)
    {
        return seconds < 1.0f ? juce::String (juce::roundToInt (seconds * 1000.0f)) + " ms"
                              : juce::String (seconds, 2) + " s";
    }

    juce::Rectangle<float> getCurveArea() const
    {
        return getLocalBounds().toFloat().withTrimmedBottom (20.0f).reduced (6.0f, 6.0f);
    }

    std::array<juce::Point<float>, 4> getPoints() const
    {
        const auto area = getCurveArea();
        const float segment = area.getWidth() * 0.25f;
        auto toPixels = [segment] (float time, float maximum) { return std::sqrt (juce::jlimit (0.0f, 1.0f, time / maximum)) * segment; };

        const float x1 = area.getX() + toPixels (a, 2.0f);
        const float x2 = x1 + toPixels (d, 2.0f);
        const float x4 = area.getRight() - (segment - toPixels (r, 3.0f));
        const float x3 = x4 - toPixels (r, 3.0f);
        const float sustainY = area.getBottom() - s * area.getHeight();

        return { juce::Point<float> (x1, area.getY()), { juce::jmin (x2, x3), sustainY }, { x3, sustainY }, { x4, area.getBottom() } };
    }

    int hitTestPoint (juce::Point<float> position, float reach = 12.0f) const
    {
        const auto points = getPoints();
        int best = -1;
        float bestDistance = reach;

        // en cas d'égalité (points superposés), le dernier l'emporte : on peut toujours tirer vers la droite
        for (int i = 0; i < 4; ++i)
        {
            const float distance = points[(size_t) i].getDistanceFrom (position);

            if (distance <= bestDistance)
            {
                bestDistance = distance;
                best = i;
            }
        }

        return best;
    }

    float a = 0.0f, d = 0.0f, s = 1.0f, r = 0.02f;
    juce::Colour colour { theme::accent };
    int dragging = -1, hovered = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EnvelopeEditor)
};

} // namespace siample
