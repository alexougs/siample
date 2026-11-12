#pragma once

#include "../PluginProcessor.h"
#include "Theme.h"
#include "DragExportButton.h"
#include "EnvelopeEditor.h"

namespace siample
{

/** Un bouton rotatif avec son nom au-dessus et sa valeur en dessous. */
class Knob final : public juce::Component
{
public:
    Knob (const juce::String& name, double minimum, double maximum, double step, double defaultValue,
          const juce::String& suffix, const juce::String& tooltip)
    {
        label.setText (upper (name), juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setFont (theme::font (10.5f));
        label.setColour (juce::Label::textColourId, theme::textDim);
        label.setMinimumHorizontalScale (0.8f);
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
        label.setBounds (area.removeFromTop (15));
        slider.setBounds (area);
    }

    juce::Slider slider;
    juce::Label label;
};

/** Réglages de la tranche sélectionnée du sample affiché. */
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

        for (auto* k : getKnobs())
        {
            k->slider.onValueChange = [this] { pushToProcessor(); };
            addAndMakeVisible (*k);
        }

        envelope.onChange = [this] { pushToProcessor(); };
        addAndMakeVisible (envelope);

        oneShotButton.setButtonText ("One-shot");
        oneShotButton.setTooltip (fr ("La tranche se joue jusqu'au bout, même si tu relâches la touche"));
        gateButton.setButtonText ("Gate");
        gateButton.setTooltip (fr ("La tranche s'arrête quand tu relâches la touche (avec le temps de release)"));
        loopButton.setButtonText ("Boucle");
        loopButton.setTooltip (fr ("La tranche se répète tant que tu tiens la touche, puis s'arrête au relâchement"));

        makeSegment ({ &oneShotButton, &gateButton, &loopButton }, 7102);

        for (auto* b : { &oneShotButton, &gateButton, &loopButton })
        {
            b->onClick = [this] { pushToProcessor(); updateEnablement(); };
            addAndMakeVisible (*b);
        }

        oneShotButton.setToggleState (true, juce::dontSendNotification);

        reverseButton.setButtonText (fr ("Inversé"));
        reverseButton.setTooltip (fr ("Joue la tranche à l'envers"));
        reverseButton.setClickingTogglesState (true);
        reverseButton.onClick = [this] { pushToProcessor(); updateEnablement(); };
        addAndMakeVisible (reverseButton);

        for (auto* l : { &fadeLabel, &chokeLabel })
        {
            l->setFont (theme::font (12.0f));
            l->setColour (juce::Label::textColourId, theme::textDim);
            l->setJustificationType (juce::Justification::centredRight);
            addAndMakeVisible (*l);
        }

        fadeLabel.setText ("Fondu", juce::dontSendNotification);
        chokeLabel.setText ("Choke", juce::dontSendNotification);

        fadeSlider.setSliderStyle (juce::Slider::LinearHorizontal);
        fadeSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        fadeSlider.setRange (0.0, 250.0, 1.0);
        fadeSlider.setSkewFactorFromMidPoint (60.0);
        fadeSlider.setDoubleClickReturnValue (true, 0.0);
        fadeSlider.setTextValueSuffix (" ms");
        fadeSlider.setPopupDisplayEnabled (true, true, nullptr);
        fadeSlider.setTooltip (fr ("Mode Boucle : fondu enchaîné au point de bouclage, pour les nappes et les notes tenues. "
                                   "Il se nourrit du son qui précède la tranche."));
        fadeSlider.onValueChange = [this] { pushToProcessor(); };
        addAndMakeVisible (fadeSlider);

        chokeBox.addItem ("Aucun", 1);
        for (int i = 1; i <= numChokeGroups; ++i)
            chokeBox.addItem ("Groupe " + juce::String (i), i + 1);
        chokeBox.setSelectedId (1, juce::dontSendNotification);
        chokeBox.setTooltip (fr ("Les tranches d'un même groupe se coupent entre elles (ex. charley ouvert / fermé)"));
        chokeBox.onChange = [this] { pushToProcessor(); };
        addAndMakeVisible (chokeBox);

        applyAllButton.setButtonText (fr ("Appliquer à toutes"));
        applyAllButton.setTooltip (fr ("Copie ces réglages sur toutes les tranches du sample"));
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

        audioDrag.createFile = [this] { return proc.createSliceAudioFile (proc.getSelectedSlice()); };
        addAndMakeVisible (audioDrag);

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
            const auto colour = sliceColour (index);

            gain.slider.setValue (p.gainDb, juce::dontSendNotification);
            pan.slider.setValue (p.pan * 100.0, juce::dontSendNotification);
            pitch.slider.setValue (p.pitch, juce::dontSendNotification);
            fine.slider.setValue (p.fine, juce::dontSendNotification);
            filter.slider.setValue (p.filter * 100.0, juce::dontSendNotification);
            resonance.slider.setValue (p.resonance * 100.0, juce::dontSendNotification);
            drive.slider.setValue (p.drive * 100.0, juce::dontSendNotification);
            envelope.setValues (p.attack, p.decay, p.sustain, p.release, colour);
            fadeSlider.setValue (p.loopFade, juce::dontSendNotification);
            (p.loop ? loopButton : p.oneShot ? oneShotButton : gateButton).setToggleState (true, juce::dontSendNotification);
            reverseButton.setToggleState (p.reverse, juce::dontSendNotification);
            chokeBox.setSelectedId (p.choke + 1, juce::dontSendNotification);

            for (auto* k : getKnobs())
                k->slider.setColour (juce::Slider::rotarySliderFillColourId, colour);

            title = "Tranche " + juce::String (index + 1);
            subtitle = "Note " + noteName (baseNote + index) + fr ("  ·  ") + juce::String (proc.getSliceSeconds (index), 2) + " s";
            titleColour = colour;
        }
        else
        {
            title = "Tranche";
            subtitle = fr ("Charge un sample pour commencer");
            titleColour = theme::outline;
        }

        updating = false;

        for (auto* k : getKnobs())
            k->setEnabled (enabled);

        for (auto* c : std::initializer_list<juce::Component*> { &envelope, &oneShotButton, &gateButton, &loopButton, &reverseButton,
                                                                 &chokeBox, &applyAllButton, &resetButton, &audioDrag })
            c->setEnabled (enabled);

        updateEnablement();
        repaint();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (14, 10);

        auto header = area.removeFromTop (30);
        resetButton.setBounds (header.removeFromRight (100));
        header.removeFromRight (6);
        applyAllButton.setBounds (header.removeFromRight (132));
        header.removeFromRight (6);
        audioDrag.setBounds (header.removeFromRight (124));
        titleArea = header.withTrimmedRight (8);

        area.removeFromTop (6);
        auto options = area.removeFromBottom (30);
        area.removeFromBottom (8);

        // Potards à gauche, enveloppe à droite
        auto envelopeArea = area.removeFromRight (juce::jlimit (170, 260, area.getWidth() * 34 / 100));
        area.removeFromRight (14);
        separatorX = area.getRight() + 7;
        sectionArea = area.removeFromTop (16);
        envelopeLabelArea = envelopeArea.removeFromTop (16);
        envelope.setBounds (envelopeArea);

        const auto knobs = getKnobs();
        const int cell = area.getWidth() / (int) knobs.size();
        int x = area.getX();

        for (auto* k : knobs)
        {
            k->setBounds (juce::Rectangle<int> (x, area.getY(), cell, juce::jmin (area.getHeight(), 92)).reduced (1, 0));
            x += cell;
        }

        oneShotButton.setBounds (options.removeFromLeft (76));
        gateButton.setBounds (options.removeFromLeft (54));
        loopButton.setBounds (options.removeFromLeft (64));
        options.removeFromLeft (10);
        reverseButton.setBounds (options.removeFromLeft (72));

        chokeBox.setBounds (options.removeFromRight (104));
        chokeLabel.setBounds (options.removeFromRight (50));
        options.removeFromLeft (4);
        fadeLabel.setBounds (options.removeFromLeft (48));
        options.removeFromLeft (6);
        fadeSlider.setBounds (options.removeFromLeft (juce::jmin (130, juce::jmax (40, options.getWidth() - 6))));
    }

    void paint (juce::Graphics& g) override
    {
        theme::drawCard (g, getLocalBounds());

        auto header = titleArea;
        g.setColour (titleColour);
        g.fillEllipse ((float) header.getX(), (float) header.getCentreY() - 5.0f, 10.0f, 10.0f);
        header.removeFromLeft (18);

        g.setColour (theme::text);
        g.setFont (theme::heading (17.0f));
        const int titleWidth = juce::roundToInt (juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), title)) + 12;
        g.drawText (title, header.removeFromLeft (juce::jmin (titleWidth, header.getWidth())), juce::Justification::centredLeft, false);

        g.setColour (theme::textDim);
        g.setFont (theme::font (12.0f));
        g.drawText (subtitle, header, juce::Justification::centredLeft, true);

        theme::drawSectionLabel (g, "Son", sectionArea);
        theme::drawSectionLabel (g, "Enveloppe", envelopeLabelArea);

        g.setColour (theme::outline.withAlpha (0.6f));
        g.fillRect (separatorX, sectionArea.getY(), 1, envelope.getBottom() - sectionArea.getY());
    }

private:
    std::array<Knob*, 7> getKnobs()
    {
        return { &gain, &pan, &pitch, &fine, &filter, &resonance, &drive };
    }

    void updateEnablement()
    {
        const bool enabled = proc.hasSample() && proc.getNumSlices() > 0;
        const bool fadeUseful = enabled && loopButton.getToggleState() && ! reverseButton.getToggleState();
        fadeSlider.setEnabled (fadeUseful);
        fadeLabel.setAlpha (fadeUseful ? 1.0f : 0.4f);
        resonance.setEnabled (enabled && std::abs (filter.slider.getValue()) >= 0.5);
    }

    SliceParams readControls()
    {
        SliceParams p;
        p.gainDb  = (float) gain.slider.getValue();
        p.pan     = (float) (pan.slider.getValue() / 100.0);
        p.pitch   = juce::roundToInt (pitch.slider.getValue());
        p.fine    = (float) fine.slider.getValue();
        p.filter  = (float) (filter.slider.getValue() / 100.0);
        p.resonance = (float) (resonance.slider.getValue() / 100.0);
        p.drive   = (float) (drive.slider.getValue() / 100.0);
        p.attack  = envelope.getAttack();
        p.decay   = envelope.getDecay();
        p.sustain = envelope.getSustain();
        p.release = envelope.getRelease();
        p.loopFade = (float) fadeSlider.getValue();
        p.loop    = loopButton.getToggleState();
        p.oneShot = oneShotButton.getToggleState();
        p.reverse = reverseButton.getToggleState();
        p.choke   = juce::jmax (0, chokeBox.getSelectedId() - 1);
        return p;
    }

    void pushToProcessor()
    {
        if (updating || ! proc.hasSample() || proc.getNumSlices() == 0)
            return;

        proc.setSliceParams (proc.getSelectedSlice(), readControls());
        subtitle = "Note " + noteName (baseNote + proc.getSelectedSlice()) + fr ("  ·  ")
                 + juce::String (proc.getSliceSeconds (proc.getSelectedSlice()), 2) + " s";
        updateEnablement();
        repaint (titleArea);

        if (onEdit != nullptr)
            onEdit();
    }

    SiampleProcessor& proc;
    bool updating = false;
    juce::String title, subtitle;
    juce::Colour titleColour { theme::outline };
    juce::Rectangle<int> titleArea, sectionArea, envelopeLabelArea;
    int separatorX = 0;

    Knob gain    { "Volume", -24.0, 12.0, 0.1, 0.0, " dB", fr ("Volume de la tranche") };
    Knob pan     { "Pan", -100.0, 100.0, 1.0, 0.0, "", fr ("Position gauche / droite") };
    Knob pitch   { "Pitch", -24.0, 24.0, 1.0, 0.0, " st", fr ("Hauteur en demi-tons. Sans Sync, la durée change avec.") };
    Knob fine    { "Fin", -100.0, 100.0, 1.0, 0.0, " ct", fr ("Accordage fin, en centièmes de demi-ton") };
    Knob filter  { "Filtre", -100.0, 100.0, 1.0, 0.0, "", fr ("Vers la gauche : coupe les aigus (LP). Vers la droite : coupe les graves (HP).") };
    Knob resonance { fr ("Réso"), 0.0, 100.0, 1.0, 0.0, " %", fr ("Résonance : accentue la fréquence de coupure du filtre") };
    Knob drive   { "Drive", 0.0, 100.0, 1.0, 0.0, " %", fr ("Saturation : du grain et de la chaleur, jusqu'à la distorsion") };

    EnvelopeEditor envelope;
    juce::TextButton oneShotButton, gateButton, loopButton, reverseButton, applyAllButton, resetButton;
    juce::Label fadeLabel, chokeLabel;
    juce::Slider fadeSlider;
    juce::ComboBox chokeBox;
    DragExportButton audioDrag { fr ("Glisser l'audio"),
                                 fr ("Attrape ce bouton et glisse-le dans la playlist du DAW : la tranche y est déposée "
                                     "en audio, avec tous ses réglages (pitch, filtre, enveloppe, grain, Sync).") };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SlicePanel)
};

} // namespace siample
