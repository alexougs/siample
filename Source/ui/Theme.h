#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

namespace siample
{

/** Texte français avec accents : les sources sont en UTF-8. */
inline juce::String fr (const char* utf8)   { return juce::String::fromUTF8 (utf8); }

/** Nom d'une note MIDI tel que le DAW l'affiche : FL Studio appelle C5 le do central, les autres C3. */
inline juce::String noteName (int midiNote)
{
    static const int octaveOfMiddleC = juce::PluginHostType().isFruityLoops() ? 5 : 3;
    return juce::MidiMessage::getMidiNoteName (midiNote, true, true, octaveOfMiddleC);
}

namespace theme
{
    const juce::Colour background  { 0xff14161a };
    const juce::Colour panel       { 0xff1d2026 };
    const juce::Colour panelLight  { 0xff2a2e37 };
    const juce::Colour outline     { 0xff383d48 };
    const juce::Colour text        { 0xffe9e6df };
    const juce::Colour textDim     { 0xff8d929c };
    const juce::Colour accent      { 0xffff8a3c };   // orange : sélection, lecture
    const juce::Colour region      { 0xff4cc9b0 };   // turquoise : zone de travail
    const juce::Colour wave        { 0xffd8d3c6 };
    const juce::Colour error       { 0xffff6b6b };

    inline juce::Font font (float height, bool bold = false)
    {
        return juce::Font (juce::FontOptions (height, bold ? juce::Font::bold : juce::Font::plain));
    }
}

/** Une couleur stable par tranche, pour repérer ses répétitions sur la piste. */
inline juce::Colour sliceColour (int sliceIndex)
{
    return juce::Colour::fromHSV (std::fmod (0.07f + (float) sliceIndex * 0.137f, 1.0f), 0.6f, 0.92f, 1.0f);
}

/** Petite vignette qui suit la souris quand on glisse une tranche vers la piste. */
inline juce::ScaledImage makeSliceDragImage (int sliceIndex)
{
    juce::Image image (juce::Image::ARGB, 88, 88, true);
    juce::Graphics g (image);
    const juce::Rectangle<float> r (2.0f, 2.0f, 84.0f, 84.0f);

    g.setColour (theme::panelLight.withAlpha (0.92f));
    g.fillRoundedRectangle (r, 12.0f);
    g.setColour (theme::accent);
    g.drawRoundedRectangle (r, 12.0f, 3.0f);
    g.setColour (theme::text);
    g.setFont (theme::font (38.0f, true));
    g.drawText (juce::String (sliceIndex + 1), r, juce::Justification::centred, false);

    return juce::ScaledImage (image, 2.0);
}

class SiampleLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    SiampleLookAndFeel()
    {
        using namespace theme;

        setColour (juce::ResizableWindow::backgroundColourId, background);
        setColour (juce::Label::textColourId, text);

        setColour (juce::TextButton::buttonColourId, panelLight);
        setColour (juce::TextButton::buttonOnColourId, accent);
        setColour (juce::TextButton::textColourOffId, text);
        setColour (juce::TextButton::textColourOnId, background);

        setColour (juce::ComboBox::backgroundColourId, panelLight);
        setColour (juce::ComboBox::outlineColourId, outline);
        setColour (juce::ComboBox::textColourId, text);
        setColour (juce::ComboBox::arrowColourId, textDim);

        setColour (juce::PopupMenu::backgroundColourId, panelLight);
        setColour (juce::PopupMenu::textColourId, text);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, accent);
        setColour (juce::PopupMenu::highlightedTextColourId, background);

        setColour (juce::Slider::thumbColourId, accent);
        setColour (juce::Slider::trackColourId, accent);
        setColour (juce::Slider::backgroundColourId, panelLight);
        setColour (juce::Slider::rotarySliderFillColourId, accent);
        setColour (juce::Slider::rotarySliderOutlineColourId, panelLight);
        setColour (juce::Slider::textBoxTextColourId, text);
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxHighlightColourId, accent.withAlpha (0.4f));

        setColour (juce::TextEditor::backgroundColourId, panelLight);
        setColour (juce::TextEditor::textColourId, text);
        setColour (juce::TextEditor::highlightColourId, accent.withAlpha (0.4f));
        setColour (juce::TextEditor::outlineColourId, outline);
        setColour (juce::TextEditor::focusedOutlineColourId, accent);

        setColour (juce::ScrollBar::thumbColourId, outline.brighter (0.3f));
        setColour (juce::ScrollBar::trackColourId, juce::Colours::transparentBlack);

        setColour (juce::TooltipWindow::backgroundColourId, panelLight);
        setColour (juce::TooltipWindow::textColourId, text);
        setColour (juce::TooltipWindow::outlineColourId, outline);
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float position,
                           float startAngle, float endAngle, juce::Slider& slider) override
    {
        const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (4.0f);
        const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const auto centre = bounds.getCentre();
        const float thickness = juce::jmax (3.0f, radius * 0.16f);
        const float arcRadius = radius - thickness * 0.5f;
        const float angle = startAngle + position * (endAngle - startAngle);
        const float alpha = slider.isEnabled() ? 1.0f : 0.35f;

        juce::Path track;
        track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
        g.setColour (theme::panelLight.withMultipliedAlpha (alpha));
        g.strokePath (track, juce::PathStrokeType (thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // L'arc de valeur part du centre pour les réglages symétriques (pan, pitch…), sinon du début
        const bool bipolar = slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0;
        const float from = bipolar ? startAngle + (float) slider.valueToProportionOfLength (0.0) * (endAngle - startAngle)
                                   : startAngle;

        if (std::abs (angle - from) > 0.01f)
        {
            juce::Path value;
            value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                                 juce::jmin (from, angle), juce::jmax (from, angle), true);
            g.setColour (theme::accent.withMultipliedAlpha (alpha));
            g.strokePath (value, juce::PathStrokeType (thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        const float knobRadius = arcRadius - thickness * 1.1f;
        g.setColour (theme::panelLight.brighter (0.08f).withMultipliedAlpha (alpha));
        g.fillEllipse (centre.x - knobRadius, centre.y - knobRadius, knobRadius * 2.0f, knobRadius * 2.0f);

        const juce::Point<float> tip (centre.x + std::sin (angle) * knobRadius * 0.82f,
                                      centre.y - std::cos (angle) * knobRadius * 0.82f);
        const juce::Point<float> base (centre.x + std::sin (angle) * knobRadius * 0.35f,
                                       centre.y - std::cos (angle) * knobRadius * 0.35f);
        g.setColour (theme::text.withMultipliedAlpha (alpha));
        g.drawLine ({ base, tip }, 2.2f);
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour,
                               bool highlighted, bool down) override
    {
        auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
        auto colour = backgroundColour;

        if (! button.isEnabled())
            colour = colour.withMultipliedAlpha (0.4f);
        else if (down)
            colour = colour.brighter (0.2f);
        else if (highlighted)
            colour = colour.brighter (0.1f);

        const float corner = 5.0f;
        juce::Path shape;
        shape.addRoundedRectangle (bounds.getX(), bounds.getY(), bounds.getWidth(), bounds.getHeight(), corner, corner,
                                   ! button.isConnectedOnLeft(), ! button.isConnectedOnRight(),
                                   ! button.isConnectedOnLeft(), ! button.isConnectedOnRight());
        g.setColour (colour);
        g.fillPath (shape);

        if (! button.getToggleState())
        {
            g.setColour (theme::outline.withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.4f));
            g.strokePath (shape, juce::PathStrokeType (1.0f));
        }
    }

    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override
    {
        return theme::font (juce::jmin (14.0f, (float) buttonHeight * 0.55f));
    }

    juce::Font getComboBoxFont (juce::ComboBox&) override       { return theme::font (14.0f); }
    juce::Font getPopupMenuFont() override                      { return theme::font (14.0f); }
    juce::Font getLabelFont (juce::Label& label) override       { return label.getFont(); }
};

} // namespace siample
