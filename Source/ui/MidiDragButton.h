#pragma once

#include "../PluginProcessor.h"
#include "Theme.h"

namespace siample
{

/** Zone à attraper à la souris : on la glisse dans le DAW pour y déposer la boucle en notes MIDI. */
class MidiDragButton final : public juce::Component,
                             public juce::SettableTooltipClient
{
public:
    explicit MidiDragButton (SiampleProcessor& p) : proc (p)
    {
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
        setTooltip (fr ("Attrape ce bouton et glisse-le dans le piano roll ou la playlist : "
                        "la boucle y est déposée en notes MIDI, une note par tranche. "
                        "Un simple clic montre le fichier MIDI dans son dossier."));
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        const float alpha = isEnabled() ? 1.0f : 0.4f;

        g.setColour ((hovering && isEnabled() ? theme::panelLight.brighter (0.1f) : theme::panelLight).withMultipliedAlpha (alpha));
        g.fillRoundedRectangle (bounds, 5.0f);

        // Bordure en pointillés : signale que ça se glisse
        juce::Path outline;
        outline.addRoundedRectangle (bounds, 5.0f);
        juce::Path dashed;
        const float dashes[] = { 4.0f, 3.0f };
        juce::PathStrokeType (1.2f).createDashedStroke (dashed, outline, dashes, 2);
        g.setColour (theme::accent.withMultipliedAlpha (alpha));
        g.fillPath (dashed);

        g.setColour (theme::text.withMultipliedAlpha (alpha));
        g.setFont (theme::font (13.5f));
        g.drawText (fr ("Glisser le MIDI vers le DAW"), getLocalBounds(), juce::Justification::centred, false);
    }

    void mouseEnter (const juce::MouseEvent&) override   { hovering = true;  repaint(); }
    void mouseExit (const juce::MouseEvent&) override    { hovering = false; repaint(); }

    void mouseDown (const juce::MouseEvent&) override    { dragged = false; }

    void mouseUp (const juce::MouseEvent& e) override
    {
        dragging = false;

        // Simple clic, sans glisser : on montre le fichier, à glisser depuis son dossier si le DAW
        // n'accepte pas le dépôt direct depuis la fenêtre du plugin.
        if (! dragged && isEnabled() && ! e.mods.isPopupMenu() && getLocalBounds().contains (e.getPosition()))
        {
            const auto file = proc.createPatternMidiFile();

            if (file.existsAsFile())
                file.revealToUser();
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging || ! isEnabled() || e.getDistanceFromDragStart() < 6)
            return;

        dragged = true;

        const auto file = proc.createPatternMidiFile();

        if (file.existsAsFile())
        {
            dragging = true;
            juce::DragAndDropContainer::performExternalDragDropOfFiles ({ file.getFullPathName() }, false, this,
                                                                        [this] { dragging = false; });
        }
    }

private:
    SiampleProcessor& proc;
    bool hovering = false;
    bool dragging = false;
    bool dragged = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiDragButton)
};

} // namespace siample
