#pragma once

#include "../PluginProcessor.h"
#include "Theme.h"

namespace siample
{

/** 16 pads façon MPC (le pad 1 est en bas à gauche) et 4 banques A-D. */
class PadGrid final : public juce::Component
{
public:
    std::function<void()> onSelect;

    explicit PadGrid (SiampleProcessor& p) : proc (p)
    {
        const char* names[] = { "A", "B", "C", "D" };

        for (int i = 0; i < 4; ++i)
        {
            auto& b = bankButtons[(size_t) i];
            b.setButtonText (names[i]);
            b.setClickingTogglesState (true);
            b.setRadioGroupId (7101);
            b.setConnectedEdges ((i > 0 ? juce::Button::ConnectedOnLeft : 0) | (i < 3 ? juce::Button::ConnectedOnRight : 0));
            b.setTooltip (fr ("Banque ") + names[i] + fr (" : tranches ") + juce::String (i * 16 + 1) + fr (" à ") + juce::String (i * 16 + 16));
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
        auto area = getLocalBounds();
        auto top = area.removeFromTop (26);
        top.removeFromLeft (64);
        const int width = juce::jmin (40, top.getWidth() / 4);

        for (auto& b : bankButtons)
            b.setBounds (top.removeFromLeft (width));
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (theme::textDim);
        g.setFont (theme::font (13.0f));
        g.drawText (fr ("Banque"), getLocalBounds().removeFromTop (26).removeFromLeft (60), juce::Justification::centredLeft, false);

        const int numSlices = proc.getNumSlices();
        const int selected  = proc.getSelectedSlice();

        for (int pad = 0; pad < 16; ++pad)
        {
            const int index = bank * 16 + pad;
            const auto r = getPadBounds (pad).toFloat().reduced (3.0f);
            const bool assigned = index < numSlices;
            const bool playing  = assigned && ((activeMask >> index) & 1) != 0;
            const bool isSelected = assigned && index == selected;

            g.setColour (playing ? theme::accent
                                 : assigned ? theme::panelLight
                                            : theme::panel);
            g.fillRoundedRectangle (r, 6.0f);

            g.setColour (isSelected ? theme::accent : theme::outline.withAlpha (assigned ? 1.0f : 0.5f));
            g.drawRoundedRectangle (r, 6.0f, isSelected ? 2.0f : 1.0f);

            if (assigned)
            {
                g.setColour (playing ? theme::background : theme::text);
                g.setFont (theme::font (juce::jmin (22.0f, r.getHeight() * 0.36f), true));
                g.drawText (juce::String (index + 1), r.withTrimmedBottom (r.getHeight() * 0.22f), juce::Justification::centred, false);

                g.setColour (playing ? theme::background.withAlpha (0.8f) : theme::textDim);
                g.setFont (theme::font (11.0f));
                g.drawText (noteName (baseNote + index),
                            r.reduced (4.0f, 5.0f), juce::Justification::centredBottom, false);
            }
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        pressed = -1;

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
        auto area = getLocalBounds().withTrimmedTop (32);
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
    juce::uint64 activeMask = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PadGrid)
};

} // namespace siample
