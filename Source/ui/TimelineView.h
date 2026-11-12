#pragma once

#include "../PluginProcessor.h"
#include "Theme.h"

namespace siample
{

/** Le montage : une ligne par sample, une grille en mesures, des blocs à poser, déplacer et raccourcir.

    - glisser un pad sur le montage : poser un bloc (il va sur la ligne de son sample) ;
    - glisser un bloc : le déplacer ; glisser son bord droit : changer sa durée ;
    - Alt + glisser : le dupliquer ; clic droit : le retirer ; double-clic dans le vide : poser la tranche sélectionnée ;
    - clic sur un bloc : l'écouter et afficher sa tranche.
*/
class TimelineView final : public juce::Component,
                           public juce::DragAndDropTarget,
                           public juce::SettableTooltipClient
{
public:
    std::function<void()> onEdit;     // le montage a changé
    std::function<void()> onSelect;   // un bloc (donc un sample et une tranche) a été choisi

    explicit TimelineView (SiampleProcessor& p) : proc (p)
    {
        setTooltip (fr ("Glisse des pads ici. Glisser un bloc : le déplacer. Bord droit : sa durée. "
                        "Alt + glisser : dupliquer. Clic droit : retirer. Double-clic : poser la tranche sélectionnée."));
    }

    void setPlayhead (double beats)
    {
        if (std::abs (beats - playhead) > 1.0e-6)
        {
            playhead = beats;
            repaint();
        }
    }

    int getSelectedClip() const   { return selectedClip; }

    void clipsChanged()
    {
        selectedClip = juce::jmin (selectedClip, proc.getNumClips() - 1);
        repaint();
    }

    //==============================================================================
    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        g.setColour (theme::well);
        g.fillRoundedRectangle (bounds, 8.0f);

        const auto content = getContentArea();
        const double total = proc.getTimelineBeats();
        const double step = proc.getSnapStep();
        const float pixelsPerBeat = (float) (content.getWidth() / total);

        g.saveState();
        g.reduceClipRegion (getLocalBounds().reduced (1));

        // Lignes des samples
        for (int d = 0; d < numDecks; ++d)
        {
            const auto lane = getLaneArea (d);
            const bool active = d == proc.getActiveDeck();
            const bool loaded = proc.getDeck (d).hasSample();

            g.setColour (juce::Colours::white.withAlpha (d % 2 == 0 ? 0.025f : 0.0f));
            g.fillRect (lane.withLeft (0.0f));

            auto header = lane.withLeft (0.0f).withWidth ((float) headerWidth);
            g.setColour (active ? theme::accent : theme::panelLight);
            g.fillRoundedRectangle (header.reduced (5.0f, juce::jmax (4.0f, (header.getHeight() - 24.0f) * 0.5f)), 6.0f);
            g.setColour (active ? theme::onAccent : (loaded ? theme::text : theme::textDim));
            g.setFont (theme::heading (13.0f));
            g.drawText (deckName (d), header, juce::Justification::centred, false);

            if (! loaded)
            {
                g.setColour (theme::textDim.withAlpha (0.55f));
                g.setFont (theme::font (11.5f));
                g.drawText (fr ("Sample ") + deckName (d) + fr (" vide"), lane.reduced (10.0f, 0.0f),
                            juce::Justification::centredLeft, false);
            }
        }

        // Grille
        const int subdivisions = step > 0.0 ? juce::roundToInt (total / step) : 0;

        if (subdivisions > 0 && pixelsPerBeat * (float) step >= 7.0f)
        {
            g.setColour (juce::Colours::white.withAlpha (0.045f));

            for (int i = 1; i < subdivisions; ++i)
                g.fillRect (content.getX() + (float) (i * step) * pixelsPerBeat, content.getY(), 1.0f, content.getHeight());
        }

        g.setFont (theme::font (10.5f));

        for (int beat = 0; beat <= (int) total; ++beat)
        {
            const bool bar = beat % beatsPerBar == 0;
            const float x = content.getX() + (float) beat * pixelsPerBeat;

            g.setColour (juce::Colours::white.withAlpha (bar ? 0.22f : 0.1f));
            g.fillRect (x, bar ? 0.0f : content.getY(), 1.0f, bar ? bounds.getHeight() : content.getHeight());

            if (bar && beat < (int) total)
            {
                g.setColour (theme::textDim);
                g.drawText (juce::String (beat / beatsPerBar + 1), juce::Rectangle<float> (x + 5.0f, 0.0f, 40.0f, (float) rulerHeight),
                            juce::Justification::centredLeft, false);
            }
        }

        // Blocs
        for (int i = 0; i < proc.getNumClips(); ++i)
            if (i != selectedClip)
                paintClip (g, i);

        if (selectedClip >= 0 && selectedClip < proc.getNumClips())
            paintClip (g, selectedClip);

        // Aperçu du bloc qu'on est en train de déposer
        if (dropDeck >= 0)
        {
            const auto r = getLaneArea (dropDeck).withX (beatsToX (dropStart)).withWidth ((float) dropLength * pixelsPerBeat).reduced (0.0f, 3.0f);
            g.setColour (sliceColour (dropSlice).withAlpha (0.3f));
            g.fillRoundedRectangle (r, 5.0f);
            g.setColour (sliceColour (dropSlice));
            g.drawRoundedRectangle (r, 5.0f, 1.5f);
        }

        // Tête de lecture
        if (playhead >= 0.0)
        {
            g.setColour (juce::Colours::white);
            g.fillRect (beatsToX (playhead) - 0.75f, 0.0f, 1.5f, bounds.getHeight());
        }

        g.restoreState();

        if (proc.getNumClips() == 0 && dropDeck < 0)
        {
            g.setColour (theme::textDim);
            g.setFont (theme::font (13.0f));
            g.drawText (fr ("Glisse des pads ici pour monter ta boucle, ou clique sur « Dans l'ordre »"),
                        content.toNearestInt(), juce::Justification::centred, true);
        }
    }

    //==============================================================================
    void mouseMove (const juce::MouseEvent& e) override
    {
        const int clip = clipAt (e.position);
        setMouseCursor (clip >= 0 && isOnRightEdge (clip, e.position.x) ? juce::MouseCursor::LeftRightResizeCursor
                                                                         : juce::MouseCursor::NormalCursor);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        mode = Mode::none;
        auditionDeck = -1;

        // Clic sur la lettre d'une ligne : afficher ce sample
        if (e.position.x < (float) headerWidth)
        {
            const int d = laneAt (e.position.y);

            if (d >= 0)
            {
                proc.setActiveDeck (d);
                notifySelect();
            }
            return;
        }

        const int clip = clipAt (e.position);

        if (clip < 0)
        {
            selectedClip = -1;
            repaint();
            return;
        }

        if (e.mods.isPopupMenu())
        {
            proc.removeClip (clip);
            selectedClip = -1;
            notifyEdit();
            return;
        }

        const auto c = proc.getClip (clip);
        selectedClip = clip;
        dragOffset = xToBeats (e.position.x) - c.start;
        dragMoved = false;
        mode = isOnRightEdge (clip, e.position.x) ? Mode::resize : Mode::move;
        duplicate = e.mods.isAltDown() && mode == Mode::move;

        proc.setActiveDeck (c.deck);
        proc.getDeck (c.deck).setSelectedSlice (c.slice);

        if (mode == Mode::move)
        {
            auditionDeck = c.deck;
            auditionSlice = c.slice;
            proc.audition (c.deck, c.slice, true);
        }

        notifySelect();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (mode == Mode::none || selectedClip < 0 || selectedClip >= proc.getNumClips())
            return;

        if (! dragMoved && e.getDistanceFromDragStart() < 4)
            return;

        if (! dragMoved)
        {
            dragMoved = true;
            stopAudition();

            if (duplicate)
            {
                const auto c = proc.getClip (selectedClip);
                const int copy = proc.addClip (c.deck, c.slice, c.start, c.length);

                if (copy >= 0)
                    selectedClip = copy;
            }
        }

        const bool free = e.mods.isShiftDown();
        const auto c = proc.getClip (selectedClip);
        const double position = xToBeats (e.position.x);

        if (mode == Mode::move)
        {
            const double start = position - dragOffset;
            proc.moveClip (selectedClip, free ? start : proc.snapBeats (start));
        }
        else
        {
            const double end = free ? position : proc.snapBeats (position);
            const double minimum = proc.getSnapStep() > 0.0 && ! free ? proc.getSnapStep() : 1.0 / 16.0;
            proc.resizeClip (selectedClip, juce::jmax (minimum, end - c.start));
        }

        notifyEdit();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        stopAudition();
        mode = Mode::none;
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (e.position.x < (float) headerWidth || clipAt (e.position) >= 0)
            return;

        const int d = laneAt (e.position.y);

        if (d < 0 || ! proc.getDeck (d).hasSample())
            return;

        const double start = proc.getSnapStep() > 0.0 ? std::floor (xToBeats (e.position.x) / proc.getSnapStep()) * proc.getSnapStep()
                                                      : xToBeats (e.position.x);
        selectedClip = proc.addClip (d, proc.getDeck (d).getSelectedSlice(), start);
        notifyEdit();
    }

    //==============================================================================
    bool isInterestedInDragSource (const SourceDetails& details) override
    {
        return details.description.toString().startsWith ("slice:");
    }

    void itemDragEnter (const SourceDetails& details) override   { itemDragMove (details); }

    void itemDragMove (const SourceDetails& details) override
    {
        if (! parseDrag (details))
            return;

        dropStart = dropPosition ((float) details.localPosition.x);
        dropLength = juce::jmin (proc.naturalBeats (dropDeck, dropSlice), proc.getTimelineBeats() - dropStart);
        repaint();
    }

    void itemDragExit (const SourceDetails&) override
    {
        dropDeck = -1;
        repaint();
    }

    void itemDropped (const SourceDetails& details) override
    {
        if (parseDrag (details))
            selectedClip = proc.addClip (dropDeck, dropSlice, dropPosition ((float) details.localPosition.x));

        dropDeck = -1;
        notifyEdit();
    }

private:
    enum class Mode { none, move, resize };

    static constexpr int headerWidth = 34;
    static constexpr int rulerHeight = 18;

    juce::Rectangle<float> getContentArea() const
    {
        return getLocalBounds().toFloat().withTrimmedLeft ((float) headerWidth).withTrimmedTop ((float) rulerHeight)
                               .withTrimmedRight (4.0f).withTrimmedBottom (3.0f);
    }

    juce::Rectangle<float> getLaneArea (int deckIndex) const
    {
        const auto content = getContentArea();
        const float height = content.getHeight() / (float) numDecks;
        return { content.getX(), content.getY() + (float) deckIndex * height, content.getWidth(), height };
    }

    float beatsToX (double beats) const
    {
        const auto content = getContentArea();
        return content.getX() + (float) (beats / proc.getTimelineBeats()) * content.getWidth();
    }

    double xToBeats (float x) const
    {
        const auto content = getContentArea();
        return (double) ((x - content.getX()) / juce::jmax (1.0f, content.getWidth())) * proc.getTimelineBeats();
    }

    juce::Rectangle<float> getClipBounds (const Clip& clip) const
    {
        const auto lane = getLaneArea (clip.deck);
        const float x1 = beatsToX (clip.start);
        const float x2 = beatsToX (juce::jmin (proc.getTimelineBeats(), clip.start + clip.length));
        return juce::Rectangle<float> (x1, lane.getY(), juce::jmax (3.0f, x2 - x1), lane.getHeight()).reduced (0.5f, 3.0f);
    }

    int laneAt (float y) const
    {
        for (int d = 0; d < numDecks; ++d)
        {
            const auto lane = getLaneArea (d);

            if (y >= lane.getY() && y < lane.getBottom())
                return d;
        }

        return -1;
    }

    int clipAt (juce::Point<float> position) const
    {
        // le bloc sélectionné est dessiné par-dessus : il est prioritaire
        if (selectedClip >= 0 && selectedClip < proc.getNumClips() && isClipVisible (proc.getClip (selectedClip))
            && getClipBounds (proc.getClip (selectedClip)).contains (position))
            return selectedClip;

        for (int i = proc.getNumClips(); --i >= 0;)
            if (isClipVisible (proc.getClip (i)) && getClipBounds (proc.getClip (i)).contains (position))
                return i;

        return -1;
    }

    bool isClipVisible (const Clip& clip) const
    {
        return clip.start < proc.getTimelineBeats() - 0.03;
    }

    bool isOnRightEdge (int clip, float x) const
    {
        const auto r = getClipBounds (proc.getClip (clip));
        return x > r.getRight() - juce::jmin (7.0f, r.getWidth() * 0.35f);
    }

    bool parseDrag (const SourceDetails& details)
    {
        juce::StringArray parts;
        parts.addTokens (details.description.toString(), ":", "");

        if (parts.size() != 3 || parts[0] != "slice")
            return false;

        const int d = juce::jlimit (0, numDecks - 1, parts[1].getIntValue());
        const int slice = parts[2].getIntValue();

        if (! proc.getDeck (d).hasSample() || slice < 0 || slice >= proc.getDeck (d).getNumSlices())
        {
            dropDeck = -1;
            return false;
        }

        dropDeck  = d;
        dropSlice = slice;
        return true;
    }

    /** Le bloc déposé commence sur la case de la grille qui se trouve sous la souris. */
    double dropPosition (float x) const
    {
        const double beats = juce::jlimit (0.0, proc.getTimelineBeats() - 1.0 / 16.0, xToBeats (x));
        const double step = proc.getSnapStep();
        return step > 0.0 ? std::floor (beats / step + 1.0e-6) * step : beats;
    }

    void paintClip (juce::Graphics& g, int index)
    {
        const auto clip = proc.getClip (index);

        if (! isClipVisible (clip))
            return;

        const auto r = getClipBounds (clip);
        const auto colour = sliceColour (clip.slice);
        const bool selected = index == selectedClip;
        const auto& deck = proc.getDeck (clip.deck);

        // Le bloc joue-t-il en ce moment ?
        const bool playing = playhead >= clip.start && playhead < clip.start + clip.length;

        g.setColour (playing ? colour.withAlpha (0.42f) : theme::panelLight.withAlpha (0.96f));
        g.fillRoundedRectangle (r, 5.0f);

        // Aperçu de la forme d'onde : ce que le bloc laisse entendre de la tranche
        const auto sample = deck.getSample();

        if (sample != nullptr && clip.slice < deck.getNumSlices() && r.getWidth() >= 8.0f)
        {
            const int start = deck.getSlice (clip.slice).start;
            const int length = juce::jmax (1, deck.getSliceEnd (clip.slice) - start);
            const double natural = juce::jmax (1.0e-6, proc.naturalBeats (clip.deck, clip.slice));
            const float* data = sample->buffer.getReadPointer (0);
            const int total = sample->buffer.getNumSamples();
            const int columns = (int) r.getWidth() - 2;
            const float centre = r.getCentreY() + 2.0f;
            const float half = r.getHeight() * 0.34f;
            const bool loops = deck.getSlice (clip.slice).params.loop;

            g.setColour (colour.withAlpha (playing ? 0.9f : 0.5f));

            for (int x = 0; x < columns; ++x)
            {
                double f0 = (double) x / columns * clip.length / natural;
                double f1 = (double) (x + 1) / columns * clip.length / natural;

                if (f0 >= 1.0)
                {
                    if (! loops)
                        break;   // la tranche est finie : le reste du bloc est du silence

                    f0 = std::fmod (f0, 1.0);
                    f1 = f0 + clip.length / natural / columns;
                }

                const int from = start + (int) (f0 * length);
                const int to   = juce::jmin (total, start + (int) (juce::jmin (1.0, f1) * length) + 1);
                const int stride = juce::jmax (1, (to - from) / 40);
                float peak = 0.0f;

                for (int n = from; n < to; n += stride)
                    peak = juce::jmax (peak, std::abs (data[n]));

                const float h = juce::jmax (0.5f, juce::jmin (1.0f, peak) * half);
                g.fillRect (r.getX() + 1.0f + (float) x, centre - h, 1.0f, h * 2.0f);
            }
        }

        g.setColour (colour);
        g.fillRoundedRectangle (r.withHeight (4.0f), 2.0f);

        g.setColour (selected ? theme::text : colour.withAlpha (0.55f));
        g.drawRoundedRectangle (r, 5.0f, selected ? 1.6f : 1.0f);

        if (r.getWidth() >= 15.0f)
        {
            g.setColour (theme::text);
            g.setFont (theme::font (juce::jmin (12.0f, r.getHeight() * 0.3f), true));
            g.drawText (juce::String (clip.slice + 1), r.reduced (5.0f, 0.0f).withTrimmedTop (5.0f).withHeight (15.0f),
                        juce::Justification::centredLeft, false);
        }
    }

    void stopAudition()
    {
        if (auditionDeck >= 0)
        {
            proc.audition (auditionDeck, auditionSlice, false);
            auditionDeck = -1;
        }
    }

    void notifyEdit()     { repaint(); if (onEdit != nullptr)   onEdit(); }
    void notifySelect()   { repaint(); if (onSelect != nullptr) onSelect(); }

    SiampleProcessor& proc;
    double playhead = -1.0;
    int selectedClip = -1;

    Mode mode = Mode::none;
    double dragOffset = 0.0;
    bool dragMoved = false;
    bool duplicate = false;
    int auditionDeck = -1, auditionSlice = 0;

    int dropDeck = -1, dropSlice = 0;
    double dropStart = 0.0, dropLength = 1.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimelineView)
};

} // namespace siample
