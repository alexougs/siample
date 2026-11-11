#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include "Theme.h"

namespace siample
{

/** Zone à attraper à la souris : on la glisse dans le DAW pour y déposer un fichier (audio ou MIDI)
    fabriqué au moment où l'on commence à glisser. */
class DragExportButton final : public juce::Component,
                               public juce::SettableTooltipClient
{
public:
    /** Fabrique le fichier à déposer. Un fichier inexistant annule le glisser. */
    std::function<juce::File()> createFile;

    DragExportButton (const juce::String& label, const juce::String& tooltip) : text (label)
    {
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
        setTooltip (tooltip + fr (" Un simple clic montre le fichier dans son dossier."));
    }

    void setText (const juce::String& newText)
    {
        if (text != newText)
        {
            text = newText;
            repaint();
        }
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
        g.drawText (text, getLocalBounds().reduced (4, 0), juce::Justification::centred, true);
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
            const auto file = makeFile();

            if (file.existsAsFile())
                file.revealToUser();
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging || ! isEnabled() || e.getDistanceFromDragStart() < 6)
            return;

        dragged = true;

        const auto file = makeFile();

        if (file.existsAsFile())
        {
            dragging = true;
            juce::DragAndDropContainer::performExternalDragDropOfFiles ({ file.getFullPathName() }, false, this,
                                                                        [this] { dragging = false; });
        }
    }

private:
    juce::File makeFile() const   { return createFile != nullptr ? createFile() : juce::File(); }

    juce::String text;
    bool hovering = false;
    bool dragging = false;
    bool dragged = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DragExportButton)
};

} // namespace siample
