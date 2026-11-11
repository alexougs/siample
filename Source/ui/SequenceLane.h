#pragma once

#include "../PluginProcessor.h"
#include "Theme.h"

namespace siample
{

/** La piste : des tranches posées bout à bout. On y dépose des pads, on réordonne en glissant,
    on retire d'un clic droit. La largeur d'un bloc suit la durée de la tranche. */
class SequenceLane final : public juce::Component,
                           public juce::DragAndDropTarget,
                           public juce::SettableTooltipClient
{
public:
    std::function<void()> onEdit;
    std::function<void()> onSelect;

    explicit SequenceLane (SiampleProcessor& p) : proc (p)
    {
        setTooltip (fr ("Glisse des pads ici pour les enchaîner. Glisser un bloc : le déplacer. "
                        "Clic : l'écouter. Clic droit : le retirer."));
    }

    void setPlayingStep (int step)
    {
        if (playingStep != step)
        {
            playingStep = step;
            repaint();
        }
    }

    //==============================================================================
    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        g.setColour (theme::panel);
        g.fillRoundedRectangle (bounds, 6.0f);

        if (dropIndex >= 0)
        {
            g.setColour (theme::accent.withAlpha (0.9f));
            g.drawRoundedRectangle (bounds.reduced (0.75f), 6.0f, 1.5f);
        }

        const auto blocks = layout();

        if (blocks.empty())
        {
            g.setColour (theme::textDim);
            g.setFont (theme::font (13.0f));
            g.drawText (proc.hasSample() ? fr ("Glisse des pads ici pour les enchaîner : le 1, encore le 1, puis le 3…")
                                         : juce::String(),
                        getLocalBounds().reduced (10, 0), juce::Justification::centred, true);
            return;
        }

        const auto sample = proc.getSample();

        for (size_t i = 0; i < blocks.size(); ++i)
        {
            const int slice = proc.getSequenceStep ((int) i);
            const auto r = blocks[i].reduced (1.0f, 0.0f);
            const bool playing = (int) i == playingStep;

            g.setColour (playing ? theme::accent : theme::panelLight);
            g.fillRoundedRectangle (r, 4.0f);

            // Aperçu de la forme d'onde de la tranche
            if (sample != nullptr && r.getWidth() >= 6.0f)
            {
                const int start = proc.getSlice (slice).start;
                const int length = juce::jmax (1, proc.getSliceEnd (slice) - start);
                const float* data = sample->buffer.getReadPointer (0);
                const int total = sample->buffer.getNumSamples();
                const int columns = (int) r.getWidth();
                const float centre = r.getCentreY();
                const float half = r.getHeight() * 0.36f;

                g.setColour ((playing ? theme::background : theme::wave).withAlpha (playing ? 0.45f : 0.3f));

                for (int x = 0; x < columns; ++x)
                {
                    const int from = start + (int) ((juce::int64) length * x / columns);
                    const int to   = juce::jmin (total, start + (int) ((juce::int64) length * (x + 1) / columns));
                    const int step = juce::jmax (1, (to - from) / 48);
                    float peak = 0.0f;

                    for (int n = from; n < to; n += step)
                        peak = juce::jmax (peak, std::abs (data[n]));

                    const float h = juce::jmax (0.5f, juce::jmin (1.0f, peak) * half);
                    g.fillRect (r.getX() + (float) x, centre - h, 1.0f, h * 2.0f);
                }
            }

            // Une couleur par tranche : on repère d'un coup d'œil les répétitions
            g.setColour (sliceColour (slice));
            g.fillRoundedRectangle (r.withHeight (4.0f), 2.0f);

            if (r.getWidth() >= 14.0f)
            {
                g.setColour (playing ? theme::background : theme::text);
                g.setFont (theme::font (juce::jmin (15.0f, r.getHeight() * 0.36f), true));
                g.drawText (juce::String (slice + 1), r.reduced (2.0f, 0.0f).withTrimmedTop (4.0f),
                            juce::Justification::centred, false);
            }
        }

        // Repère d'insertion pendant un glisser
        if (dropIndex >= 0)
        {
            const float x = dropIndex < (int) blocks.size() ? blocks[(size_t) dropIndex].getX()
                                                            : blocks.back().getRight();
            g.setColour (theme::accent);
            g.fillRect (juce::jlimit (1.0f, bounds.getRight() - 3.0f, x - 1.0f), 3.0f, 2.0f, bounds.getHeight() - 6.0f);
        }
    }

    //==============================================================================
    void mouseDown (const juce::MouseEvent& e) override
    {
        pressedStep = stepAt (e.position.x);
        dragStarted = false;

        if (pressedStep < 0)
            return;

        if (e.mods.isPopupMenu())
        {
            proc.removeSequenceStep (pressedStep);
            pressedStep = -1;

            if (onEdit != nullptr)
                onEdit();

            return;
        }

        auditioned = proc.getSequenceStep (pressedStep);
        proc.setSelectedSlice (auditioned);
        proc.audition (auditioned, true);

        if (onSelect != nullptr)
            onSelect();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (pressedStep < 0 || dragStarted || e.getDistanceFromDragStart() < 6)
            return;

        if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor (this))
        {
            dragStarted = true;
            stopAudition();
            container->startDragging ("step:" + juce::String (pressedStep), this,
                                      makeSliceDragImage (proc.getSequenceStep (pressedStep)));
        }
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        stopAudition();
        pressedStep = -1;
    }

    //==============================================================================
    bool isInterestedInDragSource (const SourceDetails& details) override
    {
        const auto description = details.description.toString();
        return proc.hasSample() && (description.startsWith ("slice:") || description.startsWith ("step:"));
    }

    void itemDragEnter (const SourceDetails& details) override   { itemDragMove (details); }

    void itemDragMove (const SourceDetails& details) override
    {
        const int index = insertIndexAt ((float) details.localPosition.x);

        if (index != dropIndex)
        {
            dropIndex = index;
            repaint();
        }
    }

    void itemDragExit (const SourceDetails&) override
    {
        dropIndex = -1;
        repaint();
    }

    void itemDropped (const SourceDetails& details) override
    {
        const auto description = details.description.toString();
        const int index = insertIndexAt ((float) details.localPosition.x);
        const int value = description.fromFirstOccurrenceOf (":", false, false).getIntValue();

        if (description.startsWith ("slice:"))
            proc.insertSequenceStep (value, index);
        else if (description.startsWith ("step:"))
            proc.moveSequenceStep (value, index > value ? index - 1 : index);

        dropIndex = -1;
        repaint();

        if (onEdit != nullptr)
            onEdit();
    }

private:
    void stopAudition()
    {
        if (auditioned >= 0)
        {
            proc.audition (auditioned, false);
            auditioned = -1;
        }
    }

    /** Un rectangle par pas de la piste, de gauche à droite. */
    std::vector<juce::Rectangle<float>> layout() const
    {
        std::vector<juce::Rectangle<float>> blocks;
        const int count = proc.getSequenceLength();

        if (count == 0 || ! proc.hasSample())
            return blocks;

        const auto inner = getLocalBounds().toFloat().reduced (4.0f);
        const double total = juce::jmax (0.001, proc.getSequenceSeconds());
        const double pixelsPerSecond = juce::jmin ((double) inner.getWidth() / total, 240.0);
        double x = inner.getX();

        for (int i = 0; i < count; ++i)
        {
            const double width = proc.getSliceSeconds (proc.getSequenceStep (i)) * pixelsPerSecond;
            blocks.emplace_back ((float) x, inner.getY(), (float) width, inner.getHeight());
            x += width;
        }

        return blocks;
    }

    int stepAt (float x) const
    {
        const auto blocks = layout();

        for (size_t i = 0; i < blocks.size(); ++i)
            if (x >= blocks[i].getX() && x < blocks[i].getRight())
                return (int) i;

        return -1;
    }

    int insertIndexAt (float x) const
    {
        const auto blocks = layout();

        for (size_t i = 0; i < blocks.size(); ++i)
            if (x < blocks[i].getCentreX())
                return (int) i;

        return (int) blocks.size();
    }

    SiampleProcessor& proc;
    int playingStep = -1;
    int dropIndex = -1;
    int pressedStep = -1;
    int auditioned = -1;
    bool dragStarted = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SequenceLane)
};

} // namespace siample
