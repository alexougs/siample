#pragma once

#include "../PluginProcessor.h"
#include "Theme.h"

namespace siample
{

/** 16 pads façon MPC (le pad 1 est en bas à gauche) et 4 banques A-D, pour le sample affiché.
    Un pad se glisse vers le montage. */
class PadGrid final : public juce::Component
{
public:
    std::function<void()> onSelect;

    explicit PadGrid (SiampleProcessor& p) : proc (p)
    {
        for (int i = 0; i < 4; ++i)
        {
            auto& b = bankButtons[(size_t) i];
            b.setButtonText (juce::String (i + 1));
            b.setTooltip (fr ("Banque ") + juce::String (i + 1) + fr (" : tranches ") + juce::String (i * 16 + 1)
                          + fr (" à ") + juce::String (i * 16 + 16));
            b.onClick = [this, i]
            {
                if (bankButtons[(size_t) i].getToggleState())
                {
                    bank = i;
                    repaint();
                }
            };
            addAndMakeVisible (b);
        }

        makeSegment ({ &bankButtons[0], &bankButtons[1], &bankButtons[2], &bankButtons[3] }, 7101);
        bankButtons[0].setToggleState (true, juce::dontSendNotification);
    }

    void setActiveMask (juce::uint64 mask)
    {
        if (activeMask != mask)
        {
            activeMask = mask;
            repaint();
        }
    }

    /** Affiche la banque qui contient cette tranche. */
    void showSlice (int index)
    {
        const int wanted = juce::jlimit (0, 3, index / 16);

        if (wanted != bank)
        {
            bank = wanted;
            bankButtons[(size_t) bank].setToggleState (true, juce::dontSendNotification);
        }

        repaint();
    }

    void resized() override
    {
        auto top = getLocalBounds().reduced (12, 10).removeFromTop (28);
        auto buttons = top.removeFromRight (4 * 34);

        for (auto& b : bankButtons)
            b.setBounds (buttons.removeFromLeft (34));
    }

    void paint (juce::Graphics& g) override
    {
        theme::drawCard (g, getLocalBounds());

        auto top = getLocalBounds().reduced (12, 10).removeFromTop (28);
        theme::drawSectionLabel (g, "Pads  " + deckName (proc.getActiveDeck()), top.removeFromLeft (90));
        theme::drawSectionLabel (g, "Banque", top.withTrimmedRight (4 * 34 + 8), juce::Justification::centredRight);

        const int numSlices = proc.getNumSlices();
        const int selected  = proc.getSelectedSlice();

        for (int pad = 0; pad < 16; ++pad)
        {
            const int index = bank * 16 + pad;
            const auto r = getPadBounds (pad).toFloat().reduced (3.0f);
            const bool assigned = index < numSlices;
            const bool playing  = assigned && ((activeMask >> index) & 1) != 0;
            const bool isSelected = assigned && index == selected;
            const auto colour = sliceColour (index);

            g.setColour (playing ? colour
                                 : assigned ? theme::panelLight
                                            : theme::well);
            g.fillRoundedRectangle (r, 9.0f);

            g.setColour (isSelected ? colour : (assigned ? theme::outline : theme::cardOutline));
            g.drawRoundedRectangle (r.reduced (isSelected ? 0.5f : 0.0f), 9.0f, isSelected ? 2.0f : 1.0f);

            if (assigned)
            {
                if (! playing)
                {
                    g.setColour (colour);
                    g.fillEllipse (r.getX() + 8.0f, r.getY() + 8.0f, 7.0f, 7.0f);
                }

                g.setColour (playing ? theme::onAccent : theme::text);
                g.setFont (theme::heading (juce::jmin (20.0f, r.getHeight() * 0.36f)));
                g.drawText (juce::String (index + 1), r.withTrimmedBottom (r.getHeight() * 0.2f), juce::Justification::centred, false);

                g.setColour (playing ? theme::onAccent.withAlpha (0.8f) : theme::textDim);
                g.setFont (theme::font (10.0f));
                g.drawText (noteName (baseNote + index), r.reduced (4.0f, 5.0f), juce::Justification::centredBottom, false);
            }
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        pressed = -1;
        dragStarted = false;

        for (int pad = 0; pad < 16; ++pad)
        {
            if (getPadBounds (pad).contains (e.getPosition()))
            {
                const int index = bank * 16 + pad;

                if (index < proc.getNumSlices())
                {
                    pressed = index;
                    proc.setSelectedSlice (index);
                    proc.audition (index, true);

                    if (onSelect != nullptr)
                        onSelect();

                    repaint();
                }
                break;
            }
        }
    }

    /** Glisser un pad : on l'emporte vers le montage. */
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (pressed < 0 || dragStarted || e.getDistanceFromDragStart() < 8)
            return;

        if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor (this))
        {
            dragStarted = true;
            proc.audition (pressed, false);
            container->startDragging ("slice:" + juce::String (proc.getActiveDeck()) + ":" + juce::String (pressed),
                                      this, makeSliceDragImage (pressed));
        }
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (pressed >= 0)
        {
            proc.audition (pressed, false);
            pressed = -1;
        }
    }

private:
    juce::Rectangle<int> getPadBounds (int pad) const
    {
        auto area = getLocalBounds().reduced (9, 9).withTrimmedTop (34);
        const int cellW = area.getWidth() / 4;
        const int cellH = area.getHeight() / 4;
        const int column = pad % 4;
        const int row    = 3 - pad / 4;   // pad 1 en bas à gauche
        return { area.getX() + column * cellW, area.getY() + row * cellH, cellW, cellH };
    }

    SiampleProcessor& proc;
    std::array<juce::TextButton, 4> bankButtons;
    int bank = 0;
    int pressed = -1;
    bool dragStarted = false;
    juce::uint64 activeMask = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PadGrid)
};

} // namespace siample
