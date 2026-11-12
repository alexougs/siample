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

/** Capitales, accents compris (la fonction standard les oublie selon le système). */
inline juce::String upper (const juce::String& text)
{
    return text.toUpperCase().replace (fr ("é"), fr ("É")).replace (fr ("è"), fr ("È"))
                             .replace (fr ("ê"), fr ("Ê")).replace (fr ("à"), fr ("À"));
}

namespace theme
{
    const juce::Colour background  { 0xff121419 };
    const juce::Colour panel       { 0xff1a1d24 };   // les cartes
    const juce::Colour panelLight  { 0xff232730 };   // boutons, pads, blocs
    const juce::Colour well        { 0xff14161b };   // fond de la forme d'onde et du montage
    const juce::Colour outline     { 0xff343945 };
    const juce::Colour cardOutline { 0xff252933 };
    const juce::Colour text        { 0xffeceae4 };
    const juce::Colour textDim     { 0xffa2a8b3 };
    const juce::Colour accent      { 0xffff8a3c };   // orange : sélection, lecture
    const juce::Colour onAccent    { 0xff1a1206 };   // texte posé sur l'orange
    const juce::Colour region      { 0xff4cc9b0 };   // turquoise : zone de travail
    const juce::Colour wave        { 0xffcfcabd };
    const juce::Colour error       { 0xffff6b6b };

    /** Les polices embarquées dans le plugin (Inter et Space Grotesk, licence OFL). */
    juce::Typeface::Ptr regularTypeface();
    juce::Typeface::Ptr boldTypeface();
    juce::Typeface::Ptr headingTypeface();

    inline juce::Font font (float height, bool bold = false)
    {
        if (auto face = bold ? boldTypeface() : regularTypeface())
            return juce::Font (juce::FontOptions (face).withHeight (height * 1.12f));

        return juce::Font (juce::FontOptions (height, bold ? juce::Font::bold : juce::Font::plain));
    }

    /** Police des titres et des gros chiffres. */
    inline juce::Font heading (float height)
    {
        if (auto face = headingTypeface())
            return juce::Font (juce::FontOptions (face).withHeight (height * 1.12f));

        return font (height, true);
    }

    /** Fond d'une carte : un panneau arrondi avec un fin contour. */
    inline void drawCard (juce::Graphics& g, juce::Rectangle<int> bounds)
    {
        const auto r = bounds.toFloat().reduced (0.5f);
        g.setColour (panel);
        g.fillRoundedRectangle (r, 10.0f);
        g.setColour (cardOutline);
        g.drawRoundedRectangle (r, 10.0f, 1.0f);
    }

    /** Petit titre de section, en capitales espacées. */
    inline void drawSectionLabel (juce::Graphics& g, const juce::String& label, juce::Rectangle<int> area,
                                  juce::Justification justification = juce::Justification::centredLeft)
    {
        g.setColour (textDim);
        g.setFont (heading (11.5f).withExtraKerningFactor (0.08f));
        g.drawText (upper (label), area, justification, false);
    }
}

/** Une couleur stable par tranche : la même sur la forme d'onde, les pads et le montage. */
inline juce::Colour sliceColour (int sliceIndex)
{
    static const juce::uint32 palette[] = { 0xffff8a3c, 0xff4cc9b0, 0xff6ea8ff, 0xfff2c94c,
                                            0xffc58bff, 0xffff7a90, 0xff8bd450, 0xff5bd1f0 };
    return juce::Colour (palette[((sliceIndex % 8) + 8) % 8]);
}

inline juce::String deckName (int deckIndex)
{
    return juce::String::charToString ((juce::juce_wchar) ('A' + deckIndex));
}

/** Petite vignette qui suit la souris quand on glisse une tranche vers le montage. */
inline juce::ScaledImage makeSliceDragImage (int sliceIndex)
{
    juce::Image image (juce::Image::ARGB, 88, 88, true);
    juce::Graphics g (image);
    const juce::Rectangle<float> r (2.0f, 2.0f, 84.0f, 84.0f);

    g.setColour (theme::panelLight.withAlpha (0.92f));
    g.fillRoundedRectangle (r, 14.0f);
    g.setColour (sliceColour (sliceIndex));
    g.drawRoundedRectangle (r, 14.0f, 3.0f);
    g.setColour (theme::text);
    g.setFont (theme::heading (36.0f));
    g.drawText (juce::String (sliceIndex + 1), r, juce::Justification::centred, false);

    return juce::ScaledImage (image, 2.0);
}

/** Relie des boutons en un sélecteur à plusieurs positions. */
inline void makeSegment (std::initializer_list<juce::TextButton*> buttons, int radioGroup)
{
    int i = 0;
    const int last = (int) buttons.size() - 1;

    for (auto* b : buttons)
    {
        b->setClickingTogglesState (true);
        b->setRadioGroupId (radioGroup);
        b->setConnectedEdges ((i > 0 ? juce::Button::ConnectedOnLeft : 0) | (i < last ? juce::Button::ConnectedOnRight : 0));
        ++i;
    }
}

class SiampleLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    SiampleLookAndFeel()
    {
        using namespace theme;

        if (auto face = regularTypeface())
            setDefaultSansSerifTypeface (face);

        setColour (juce::ResizableWindow::backgroundColourId, background);
        setColour (juce::Label::textColourId, text);

        setColour (juce::TextButton::buttonColourId, panelLight);
        setColour (juce::TextButton::buttonOnColourId, accent);
        setColour (juce::TextButton::textColourOffId, text);
        setColour (juce::TextButton::textColourOnId, onAccent);

        setColour (juce::ComboBox::backgroundColourId, panelLight);
        setColour (juce::ComboBox::outlineColourId, outline);
        setColour (juce::ComboBox::textColourId, text);
        setColour (juce::ComboBox::arrowColourId, textDim);

        setColour (juce::PopupMenu::backgroundColourId, panelLight);
        setColour (juce::PopupMenu::textColourId, text);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, accent);
        setColour (juce::PopupMenu::highlightedTextColourId, onAccent);

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
            g.setColour (slider.findColour (juce::Slider::rotarySliderFillColourId).withMultipliedAlpha (alpha));
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

        const float corner = 7.0f;
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

    juce::Font getTextButtonFont (juce::TextButton& button, int buttonHeight) override
    {
        return theme::font (juce::jmin (13.0f, (float) buttonHeight * 0.5f), button.getToggleState());
    }

    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos, float, float,
                           juce::Slider::SliderStyle style, juce::Slider& slider) override
    {
        if (style != juce::Slider::LinearHorizontal)
        {
            LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, 0.0f, 0.0f, style, slider);
            return;
        }

        const float alpha = slider.isEnabled() ? 1.0f : 0.35f;
        const float centre = (float) y + (float) height * 0.5f;
        const juce::Rectangle<float> track ((float) x, centre - 2.0f, (float) width, 4.0f);

        g.setColour (theme::outline.withMultipliedAlpha (alpha));
        g.fillRoundedRectangle (track, 2.0f);
        g.setColour (theme::accent.withMultipliedAlpha (alpha));
        g.fillRoundedRectangle (track.withRight (sliderPos), 2.0f);
        g.setColour (theme::text.withMultipliedAlpha (alpha));
        g.fillEllipse (sliderPos - 7.0f, centre - 7.0f, 14.0f, 14.0f);
    }

    void drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box) override
    {
        const juce::Rectangle<float> bounds (0.5f, 0.5f, (float) width - 1.0f, (float) height - 1.0f);
        const float alpha = box.isEnabled() ? 1.0f : 0.4f;

        g.setColour (theme::panelLight.withMultipliedAlpha (alpha));
        g.fillRoundedRectangle (bounds, 7.0f);
        g.setColour (theme::outline.withMultipliedAlpha (alpha));
        g.drawRoundedRectangle (bounds, 7.0f, 1.0f);

        juce::Path arrow;
        const float cx = (float) width - 14.0f, cy = (float) height * 0.5f;
        arrow.startNewSubPath (cx - 4.0f, cy - 2.0f);
        arrow.lineTo (cx, cy + 2.5f);
        arrow.lineTo (cx + 4.0f, cy - 2.0f);
        g.setColour (theme::textDim.withMultipliedAlpha (alpha));
        g.strokePath (arrow, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds (8, 0, box.getWidth() - 30, box.getHeight());
        label.setFont (getComboBoxFont (box));
    }

    juce::Font getComboBoxFont (juce::ComboBox&) override       { return theme::font (13.0f); }
    juce::Font getPopupMenuFont() override                      { return theme::font (13.5f); }
    juce::Font getLabelFont (juce::Label& label) override       { return label.getFont(); }
};

} // namespace siample
