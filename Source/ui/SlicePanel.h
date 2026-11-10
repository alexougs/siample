#pragma once

#include "../PluginProcessor.h"
#include "Theme.h"

namespace siample
{

/** Un bouton rotatif avec son nom au-dessus et sa valeur en dessous. */
class Knob final : public juce::Component
{
public:
    Knob (const juce::String& name, double minimum, double maximum, double step, double defaultValue,
          const juce::String& suffix, const juce::String& tooltip)
    {
        label.setText (name, juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setFont (theme::font (12.5f));
        label.setColour (juce::Label::textColourId, theme::textDim);
        addAndMakeVisible (label);

        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 17);
        slider.setRange (minimum, maximum, step);
        slider.setDoubleClickReturnValue (true, defaultValue);
        slider.setTextValueSuffix (suffix);
        slider.setValue (defaultValue, juce::dontSendNotification);
        slider.setTooltip (tooltip);
        addAndMakeVisible (slider);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        label.setBounds (area.removeFromTop (16));
        slider.setBounds (area);
    }

    juce::Slider slider;
    juce::Label label;
};

/** Réglages de la tranche sélectionnée. */
class SlicePanel final : public juce::Component
{
public:
    std::function<void()> onEdit;

    explicit SlicePanel (SiampleProcessor& p) : proc (p)
    {
        pan.slider.textFromValueFunction = [] (double v)
        {
            const int amount = juce::roundToInt (std::abs (v));
            return amount == 0 ? juce::String ("Centre") : (v < 0.0 ? "G " : "D ") + juce::String (amount);
        };
        pan.slider.updateText();

        filter.slider.textFromValueFunction = [] (double v)
        {
            const int amount = juce::roundToInt (std::abs (v));
            return amount == 0 ? juce::String ("Aucun") : (v < 0.0 ? "LP " : "HP ") + juce::String (amount);
        };
        filter.slider.updateText();

        for (auto* k : { &attack, &decay, &release })
            k->slider.setSkewFactorFromMidPoint (150.0);

        for (auto* k : getKnobs())
        {
            k->slider.onValueChange = [this] { pushToProcessor(); };
            addAndMakeVisible (*k);
        }

        oneShotButton.setButtonText ("One-shot");
        oneShotButton.setTooltip (fr ("La tranche se joue jusqu'au bout, même si tu relâches la touche"));
        gateButton.setButtonText ("Gate");
        gateButton.setTooltip (fr ("La tranche s'arrête quand tu relâches la touche (avec le temps de Release)"));

        loopButton.setButtonText ("Boucle");
        loopButton.setTooltip (fr ("La tranche se répète tant que tu tiens la touche, puis s'arrête au relâchement"));

        for (auto* b : { &oneShotButton, &gateButton, &loopButton })
        {
            b->setClickingTogglesState (true);
            b->setRadioGroupId (7102);
            b->onClick = [this] { pushToProcessor(); };
            addAndMakeVisible (*b);
        }

        oneShotButton.setConnectedEdges (juce::Button::ConnectedOnRight);
        gateButton.setConnectedEdges (juce::Button::ConnectedOnLeft | juce::Button::ConnectedOnRight);
        loopButton.setConnectedEdges (juce::Button::ConnectedOnLeft);
        oneShotButton.setToggleState (true, juce::dontSendNotification);

        reverseButton.setButtonText (fr ("Inversé"));
        reverseButton.setTooltip (fr ("Joue la tranche à l'envers"));
        reverseButton.setClickingTogglesState (true);
        reverseButton.onClick = [this] { pushToProcessor(); };
        addAndMakeVisible (reverseButton);

        chokeLabel.setText ("Choke", juce::dontSendNotification);
        chokeLabel.setFont (theme::font (13.0f));
        chokeLabel.setColour (juce::Label::textColourId, theme::textDim);
        chokeLabel.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (chokeLabel);

        chokeBox.addItem ("Aucun", 1);
        for (int i = 1; i <= numChokeGroups; ++i)
            chokeBox.addItem ("Groupe " + juce::String (i), i + 1);
        chokeBox.setSelectedId (1, juce::dontSendNotification);
        chokeBox.setTooltip (fr ("Les tranches d'un même groupe se coupent entre elles (ex. charley ouvert / fermé)"));
        chokeBox.onChange = [this] { pushToProcessor(); };
        addAndMakeVisible (chokeBox);

        applyAllButton.setButtonText (fr ("Appliquer à toutes"));
        applyAllButton.setTooltip (fr ("Copie ces réglages sur toutes les tranches"));
        applyAllButton.onClick = [this]
        {
            proc.setAllSliceParams (readControls());
            if (onEdit != nullptr) onEdit();
        };
        addAndMakeVisible (applyAllButton);

        resetButton.setButtonText (fr ("Réinitialiser"));
        resetButton.setTooltip (fr ("Remet cette tranche à ses réglages d'origine"));
        resetButton.onClick = [this]
        {
            proc.setSliceParams (proc.getSelectedSlice(), SliceParams());
            refresh();
            if (onEdit != nullptr) onEdit();
        };
        addAndMakeVisible (resetButton);

        refresh();
    }

    /** Met les contrôles à jour d'après la tranche sélectionnée. */
    void refresh()
    {
        const bool enabled = proc.hasSample() && proc.getNumSlices() > 0;
        updating = true;

        if (enabled)
        {
            const int index = proc.getSelectedSlice();
            const auto& p = proc.getSlice (index).params;

            gain.slider.setValue (p.gainDb, juce::dontSendNotification);
            pan.slider.setValue (p.pan * 100.0, juce::dontSendNotification);
            pitch.slider.setValue (p.pitch, juce::dontSendNotification);
            fine.slider.setValue (p.fine, juce::dontSendNotification);
            attack.slider.setValue (p.attack * 1000.0, juce::dontSendNotification);
            decay.slider.setValue (p.decay * 1000.0, juce::dontSendNotification);
            sustain.slider.setValue (p.sustain * 100.0, juce::dontSendNotification);
            release.slider.setValue (p.release * 1000.0, juce::dontSendNotification);
            filter.slider.setValue (p.filter * 100.0, juce::dontSendNotification);
            oneShotButton.setToggleState (p.oneShot && ! p.loop, juce::dontSendNotification);
            gateButton.setToggleState (! p.oneShot && ! p.loop, juce::dontSendNotification);
            loopButton.setToggleState (p.loop, juce::dontSendNotification);
            reverseButton.setToggleState (p.reverse, juce::dontSendNotification);
            chokeBox.setSelectedId (p.choke + 1, juce::dontSendNotification);

            const double seconds = (double) (proc.getSliceEnd (index) - proc.getSlice (index).start) / proc.getSourceSampleRate();
            title = "Tranche " + juce::String (index + 1);
            subtitle = "Note " + noteName (baseNote + index)
                     + fr ("  ·  ") + juce::String (seconds, seconds < 10.0 ? 2 : 1) + " s";
        }
        else
        {
            title = "Tranche";
            subtitle = {};
        }

        updating = false;

        for (auto* k : getKnobs())
            k->setEnabled (enabled);

        for (auto* c : std::initializer_list<juce::Component*> { &oneShotButton, &gateButton, &loopButton, &reverseButton, &chokeBox, &applyAllButton, &resetButton })
            c->setEnabled (enabled);

        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (theme::panel);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);

        auto header = getLocalBounds().reduced (14, 0).removeFromTop (headerHeight);
        const auto titleFont = theme::font (17.0f, true);
        g.setColour (theme::text);
        g.setFont (titleFont);
        g.drawText (title, header, juce::Justification::centredLeft, false);

        header.removeFromLeft (juce::roundToInt (juce::GlyphArrangement::getStringWidth (titleFont, title)) + 14);
        header.setRight (applyAllButton.getX() - 8);
        g.setColour (theme::textDim);
        g.setFont (theme::font (13.0f));
        g.drawText (subtitle, header, juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12, 0);

        auto header = area.removeFromTop (headerHeight).withSizeKeepingCentre (area.getWidth(), 26);
        resetButton.setBounds (header.removeFromRight (104));
        header.removeFromRight (6);
        applyAllButton.setBounds (header.removeFromRight (140));

        area.removeFromBottom (12);
        auto options = area.removeFromBottom (28);
        oneShotButton.setBounds (options.removeFromLeft (80));
        gateButton.setBounds (options.removeFromLeft (56));
        loopButton.setBounds (options.removeFromLeft (68));
        options.removeFromLeft (12);
        reverseButton.setBounds (options.removeFromLeft (76));
        chokeBox.setBounds (options.removeFromRight (104));
        options.removeFromRight (6);
        chokeLabel.setBounds (options.removeFromRight (50));

        area.removeFromBottom (8);
        const int rowHeight = area.getHeight() / 2;
        const auto knobs = getKnobs();

        // 5 colonnes : la première ligne en a 5, la seconde 4
        for (int row = 0; row < 2; ++row)
        {
            auto line = area.removeFromTop (rowHeight);
            const int width = line.getWidth() / 5;

            for (int column = 0; column < 5; ++column)
            {
                const int index = row * 5 + column;
                auto cell = line.removeFromLeft (width).reduced (2, 2);

                if (index < (int) knobs.size())
                    knobs[(size_t) index]->setBounds (cell);
            }
        }
    }

private:
    std::array<Knob*, 9> getKnobs()
    {
        return { &gain, &pan, &pitch, &fine, &filter, &attack, &decay, &sustain, &release };
    }

    SliceParams readControls() const
    {
        SliceParams p;
        p.gainDb  = (float) gain.slider.getValue();
        p.pan     = (float) (pan.slider.getValue() / 100.0);
        p.pitch   = juce::roundToInt (pitch.slider.getValue());
        p.fine    = (float) fine.slider.getValue();
        p.attack  = (float) (attack.slider.getValue() / 1000.0);
        p.decay   = (float) (decay.slider.getValue() / 1000.0);
        p.sustain = (float) (sustain.slider.getValue() / 100.0);
        p.release = (float) (release.slider.getValue() / 1000.0);
        p.filter  = (float) (filter.slider.getValue() / 100.0);
        p.oneShot = oneShotButton.getToggleState();
        p.loop    = loopButton.getToggleState();
        p.reverse = reverseButton.getToggleState();
        p.choke   = juce::jmax (0, chokeBox.getSelectedId() - 1);
        return p;
    }

    void pushToProcessor()
    {
        if (updating || ! proc.hasSample())
            return;

        proc.setSliceParams (proc.getSelectedSlice(), readControls());
    }

    static constexpr int headerHeight = 44;

    SiampleProcessor& proc;
    bool updating = false;
    juce::String title, subtitle;

    Knob gain    { "Volume",  -24.0, 12.0, 0.1, 0.0, " dB", fr ("Volume de la tranche") };
    Knob pan     { "Pan",    -100.0, 100.0, 1.0, 0.0, "",   fr ("Position gauche / droite") };
    Knob pitch   { "Pitch",   -24.0, 24.0, 1.0, 0.0, " st", fr ("Hauteur en demi-tons. Change aussi la durée, sauf quand Sync est activé.") };
    Knob fine    { "Fin",    -100.0, 100.0, 1.0, 0.0, " ct", fr ("Accordage fin, en centièmes de demi-ton") };
    Knob filter  { "Filtre", -100.0, 100.0, 1.0, 0.0, "",   fr ("Vers la gauche : coupe les aigus (son étouffé). Vers la droite : coupe les graves (son fin).") };
    Knob attack  { "Attack",    0.0, 2000.0, 1.0, 0.0, " ms", fr ("Temps de montée du son") };
    Knob decay   { "Decay",     0.0, 2000.0, 1.0, 0.0, " ms", fr ("Temps de descente vers le niveau Sustain") };
    Knob sustain { "Sustain",   0.0, 100.0, 1.0, 100.0, " %", fr ("Niveau tenu après le Decay") };
    Knob release { "Release",   0.0, 3000.0, 1.0, 20.0, " ms", fr ("Temps d'extinction après le relâchement (mode Gate)") };

    juce::TextButton oneShotButton, gateButton, loopButton, reverseButton, applyAllButton, resetButton;
    juce::Label chokeLabel;
    juce::ComboBox chokeBox;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SlicePanel)
};

} // namespace siample
