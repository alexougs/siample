#pragma once

#include "../PluginProcessor.h"
#include "Theme.h"

namespace siample
{

/** Forme d'onde avec la zone de travail (bandeau du haut) et les points de coupe. */
class WaveformView final : public juce::Component,
                           private juce::ScrollBar::Listener
{
public:
    std::function<void()> onEdit;     // tranches ou zone modifiées
    std::function<void()> onSelect;   // tranche sélectionnée modifiée

    explicit WaveformView (SiampleProcessor& p) : proc (p)
    {
        addAndMakeVisible (scrollBar);
        scrollBar.addListener (this);
        scrollBar.setAutoHide (false);
        setWantsKeyboardFocus (false);
    }

    ~WaveformView() override
    {
        scrollBar.removeListener (this);
    }

    /** À appeler quand un nouveau sample a été chargé. */
    void sampleChanged()
    {
        sample = proc.getSample();
        peakMin.clear();
        peakMax.clear();

        if (sample != nullptr)
        {
            const auto& buffer = sample->buffer;
            const int length = buffer.getNumSamples();
            const int blocks = (length + peakBlock - 1) / peakBlock;
            peakMin.resize ((size_t) blocks);
            peakMax.resize ((size_t) blocks);

            for (int b = 0; b < blocks; ++b)
            {
                const int from  = b * peakBlock;
                const int count = juce::jmin (peakBlock, length - from);
                float lo = 0.0f, hi = 0.0f;

                for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                {
                    const auto range = buffer.findMinMax (ch, from, count);
                    lo = juce::jmin (lo, range.getStart());
                    hi = juce::jmax (hi, range.getEnd());
                }

                peakMin[(size_t) b] = lo;
                peakMax[(size_t) b] = hi;
            }

            viewStart = 0.0;
            viewEnd   = (double) length;
        }

        dragMode = DragMode::none;
        updateScrollBar();
        repaint();
    }

    void setDropHighlight (bool shouldHighlight)
    {
        if (dropHighlight != shouldHighlight)
        {
            dropHighlight = shouldHighlight;
            repaint();
        }
    }

    void setActiveMask (juce::uint64 mask)
    {
        if (activeMask != mask)
        {
            activeMask = mask;
            repaint();
        }
    }

    //==============================================================================
    void resized() override
    {
        auto area = getLocalBounds();
        scrollBar.setBounds (area.removeFromBottom (scrollHeight).reduced (2, 2));
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        g.setColour (theme::panel);
        g.fillRoundedRectangle (bounds, 6.0f);

        if (sample == nullptr)
        {
            g.setColour (dropHighlight ? theme::accent : theme::textDim);
            g.setFont (theme::font (17.0f));
            g.drawFittedText (emptyMessage.isNotEmpty() ? emptyMessage
                                                         : fr ("Glisse un sample ici, ou clique sur « Charger un sample »"),
                              getLocalBounds().reduced (20), juce::Justification::centred, 3);

            if (dropHighlight)
            {
                g.setColour (theme::accent);
                g.drawRoundedRectangle (bounds.reduced (1.0f), 6.0f, 2.0f);
            }
            return;
        }

        const auto ruler = getRulerArea();
        const auto wave  = getWaveArea();
        const bool draggingRegion = dragMode == DragMode::regionStart || dragMode == DragMode::regionEnd || dragMode == DragMode::regionNew;
        const int rs = draggingRegion ? pendingRegionStart : proc.getRegionStart();
        const int re = draggingRegion ? pendingRegionEnd   : proc.getRegionEnd();
        const int selected = proc.getSelectedSlice();
        const int numSlices = proc.getNumSlices();

        g.saveState();
        g.reduceClipRegion (wave.getUnion (ruler));

        // Fond des tranches
        for (int i = 0; i < numSlices; ++i)
        {
            const float x1 = sampleToX (proc.getSlice (i).start);
            const float x2 = sampleToX (proc.getSliceEnd (i));

            if (x2 < (float) wave.getX() || x1 > (float) wave.getRight())
                continue;

            const bool playing = i < 64 && ((activeMask >> i) & 1) != 0;
            juce::Colour fill = (i % 2 == 0) ? juce::Colours::white.withAlpha (0.035f) : juce::Colours::transparentBlack;

            if (i == selected)
                fill = theme::accent.withAlpha (0.13f);

            if (playing)
                fill = theme::accent.withAlpha (i == selected ? 0.26f : 0.18f);

            g.setColour (fill);
            g.fillRect (juce::Rectangle<float> (x1, (float) wave.getY(), x2 - x1, (float) wave.getHeight()));
        }

        // Forme d'onde
        const float mid  = (float) wave.getCentreY();
        const float half = (float) wave.getHeight() * 0.46f;
        const int selStart = numSlices > 0 ? proc.getSlice (selected).start : -1;
        const int selEnd   = numSlices > 0 ? proc.getSliceEnd (selected) : -1;

        for (int px = wave.getX(); px < wave.getRight(); ++px)
        {
            const double s0 = xToSample ((float) px);
            const double s1 = xToSample ((float) px + 1.0f);
            float lo = 0.0f, hi = 0.0f;
            getMinMax (s0, s1, lo, hi);

            const bool inRegion = s0 >= (double) rs && s0 < (double) re;
            const bool inSelected = inRegion && s0 >= (double) selStart && s0 < (double) selEnd;

            g.setColour (inSelected ? theme::accent
                                    : inRegion ? theme::wave
                                               : theme::wave.withAlpha (0.25f));

            const float top    = mid - juce::jlimit (-1.0f, 1.0f, hi) * half;
            const float bottom = mid - juce::jlimit (-1.0f, 1.0f, lo) * half;
            g.fillRect ((float) px, top, 1.0f, juce::jmax (1.0f, bottom - top));
        }

        // Points de coupe
        g.setFont (theme::font (11.0f, true));

        for (int i = 0; i < numSlices; ++i)
        {
            const float x = sampleToX (proc.getSlice (i).start);

            if (x < (float) wave.getX() - 30.0f || x > (float) wave.getRight())
                continue;

            const bool isSelected = i == selected;
            g.setColour (isSelected ? theme::accent : theme::text.withAlpha (0.55f));
            g.fillRect (x - 0.5f, (float) wave.getY(), isSelected ? 2.0f : 1.0f, (float) wave.getHeight());

            const juce::Rectangle<float> tag (x, (float) wave.getY(), 22.0f, 14.0f);
            g.fillRoundedRectangle (tag, 2.0f);
            g.setColour (theme::background);
            g.drawText (juce::String (i + 1), tag, juce::Justification::centred, false);
        }

        // Tête de lecture
        if (playheadPosition >= 0)
        {
            const float x = sampleToX (playheadPosition);
            g.setColour (juce::Colours::white);
            g.fillRect (x - 0.75f, (float) wave.getY(), 1.5f, (float) wave.getHeight());
        }

        // Bandeau de la zone de travail
        g.setColour (theme::background.withAlpha (0.6f));
        g.fillRect (ruler);

        const float rx1 = sampleToX (rs);
        const float rx2 = sampleToX (re);
        g.setColour (theme::region.withAlpha (0.35f));
        g.fillRect (juce::Rectangle<float> (rx1, (float) ruler.getY() + 3.0f, rx2 - rx1, (float) ruler.getHeight() - 6.0f));

        g.setColour (theme::region);
        g.fillRoundedRectangle (rx1 - 1.0f, (float) ruler.getY() + 1.0f, handleWidth, (float) ruler.getHeight() - 2.0f, 2.0f);
        g.fillRoundedRectangle (rx2 - handleWidth + 1.0f, (float) ruler.getY() + 1.0f, handleWidth, (float) ruler.getHeight() - 2.0f, 2.0f);

        // Limites de la zone dans la forme d'onde
        g.setColour (theme::region.withAlpha (0.7f));
        g.fillRect (rx1 - 0.5f, (float) wave.getY(), 1.0f, (float) wave.getHeight());
        g.fillRect (rx2 - 0.5f, (float) wave.getY(), 1.0f, (float) wave.getHeight());

        g.restoreState();

        if (dropHighlight)
        {
            g.setColour (theme::accent);
            g.drawRoundedRectangle (bounds.reduced (1.0f), 6.0f, 2.0f);
        }
    }

    //==============================================================================
    void mouseMove (const juce::MouseEvent& e) override
    {
        if (sample == nullptr)
        {
            setMouseCursor (juce::MouseCursor::NormalCursor);
            return;
        }

        const bool overHandle = hitRegionHandle (e.position) != DragMode::none
                             || (getWaveArea().contains (e.getPosition()) && hitMarker (e.position.x) >= 0);

        setMouseCursor (overHandle ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragMode = DragMode::none;

        if (sample == nullptr)
            return;

        const auto handle = hitRegionHandle (e.position);

        if (handle != DragMode::none)
        {
            dragMode = handle;
            pendingRegionStart = proc.getRegionStart();
            pendingRegionEnd   = proc.getRegionEnd();
            return;
        }

        if (getRulerArea().contains (e.getPosition()))
        {
            // Glisser dans le bandeau dessine une nouvelle zone de travail
            dragMode   = DragMode::regionNew;
            dragAnchor = (int) juce::jlimit (0.0, (double) sample->buffer.getNumSamples(), xToSample (e.position.x));
            dragMoved  = false;
            pendingRegionStart = proc.getRegionStart();
            pendingRegionEnd   = proc.getRegionEnd();
            return;
        }

        if (! getWaveArea().contains (e.getPosition()))
            return;

        const int marker = hitMarker (e.position.x);

        if (marker >= 0)
        {
            if (e.mods.isPopupMenu())
            {
                if (proc.removeMarker (marker))
                    notifyEdit();
                return;
            }

            dragMode  = DragMode::marker;
            dragIndex = marker;
            proc.setSelectedSlice (marker);
            notifySelect();
            return;
        }

        if (e.mods.isPopupMenu())
            return;

        const int slice = proc.findSliceAt ((int) xToSample (e.position.x));

        if (slice >= 0)
        {
            dragMode  = DragMode::audition;
            dragIndex = slice;
            proc.setSelectedSlice (slice);
            proc.audition (slice, true);
            notifySelect();
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (sample == nullptr)
            return;

        const int position = (int) juce::jlimit (0.0, (double) sample->buffer.getNumSamples(), xToSample (e.position.x));

        if (dragMode == DragMode::marker)
        {
            proc.moveMarker (dragIndex, position);
            notifyEdit();
        }
        else if (dragMode == DragMode::regionStart)
        {
            pendingRegionStart = juce::jlimit (0, juce::jmax (0, pendingRegionEnd - 1024), position);
            repaint();
        }
        else if (dragMode == DragMode::regionEnd)
        {
            pendingRegionEnd = juce::jlimit (juce::jmin (sample->buffer.getNumSamples(), pendingRegionStart + 1024),
                                             sample->buffer.getNumSamples(), position);
            repaint();
        }
        else if (dragMode == DragMode::regionNew)
        {
            if (std::abs (e.getDistanceFromDragStartX()) >= 4)
            {
                dragMoved = true;
                pendingRegionStart = juce::jmin (dragAnchor, position);
                pendingRegionEnd   = juce::jmax (dragAnchor, position);
                repaint();
            }
        }
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        const auto mode = dragMode;
        dragMode = DragMode::none;

        if (mode == DragMode::audition)
        {
            proc.audition (dragIndex, false);
        }
        else if (mode == DragMode::regionNew && ! dragMoved)
        {
            repaint();   // simple clic dans le bandeau : rien ne change
        }
        else if (mode == DragMode::regionStart || mode == DragMode::regionEnd || mode == DragMode::regionNew)
        {
            // La découpe n'est recalculée qu'au relâchement, pour rester fluide sur les longs fichiers
            proc.setRegion (pendingRegionStart, pendingRegionEnd);
            notifyEdit();
        }
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (sample == nullptr)
            return;

        if (getRulerArea().contains (e.getPosition()))
        {
            proc.setRegion (0, sample->buffer.getNumSamples());
            notifyEdit();
            return;
        }

        if (getWaveArea().contains (e.getPosition()) && hitMarker (e.position.x) < 0)
        {
            if (proc.addMarker ((int) xToSample (e.position.x)) >= 0)
                notifyEdit();
        }
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        if (sample == nullptr)
            return;

        const double length = (double) sample->buffer.getNumSamples();
        const double width  = viewEnd - viewStart;

        if (std::abs (wheel.deltaX) > std::abs (wheel.deltaY) || e.mods.isShiftDown())
        {
            // Défilement horizontal
            const double delta = (std::abs (wheel.deltaX) > std::abs (wheel.deltaY) ? wheel.deltaX : wheel.deltaY) * width * -0.5;
            viewStart = juce::jlimit (0.0, juce::jmax (0.0, length - width), viewStart + delta);
            viewEnd   = viewStart + width;
        }
        else if (std::abs (wheel.deltaY) > 0.0f)
        {
            // Zoom autour de la souris
            const double anchor   = xToSample (e.position.x);
            const double fraction = width > 0.0 ? (anchor - viewStart) / width : 0.5;
            const double newWidth = juce::jlimit (juce::jmin (64.0, length), length, width * std::pow (2.0, -(double) wheel.deltaY * 2.0));

            viewStart = juce::jlimit (0.0, juce::jmax (0.0, length - newWidth), anchor - fraction * newWidth);
            viewEnd   = viewStart + newWidth;
        }

        updateScrollBar();
        repaint();
    }

    void setPlayhead (int samplePosition)
    {
        if (playheadPosition != samplePosition)
        {
            playheadPosition = samplePosition;
            repaint();
        }
    }

    void setEmptyMessage (const juce::String& message)
    {
        emptyMessage = message;
        repaint();
    }

private:
    enum class DragMode { none, marker, regionStart, regionEnd, regionNew, audition };

    static constexpr int peakBlock    = 256;
    static constexpr int rulerHeight  = 18;
    static constexpr int scrollHeight = 14;
    static constexpr float handleWidth = 7.0f;

    juce::Rectangle<int> getRulerArea() const
    {
        return getLocalBounds().withTrimmedBottom (scrollHeight).removeFromTop (rulerHeight);
    }

    juce::Rectangle<int> getWaveArea() const
    {
        return getLocalBounds().withTrimmedBottom (scrollHeight).withTrimmedTop (rulerHeight);
    }

    double xToSample (float x) const
    {
        const auto area = getWaveArea();
        const double width = juce::jmax (1.0, (double) area.getWidth());
        return viewStart + ((double) x - (double) area.getX()) / width * (viewEnd - viewStart);
    }

    float sampleToX (int samplePosition) const
    {
        const auto area = getWaveArea();
        const double span = juce::jmax (1.0, viewEnd - viewStart);
        return (float) ((double) area.getX() + ((double) samplePosition - viewStart) / span * (double) area.getWidth());
    }

    void getMinMax (double s0, double s1, float& lo, float& hi) const
    {
        const auto& buffer = sample->buffer;
        const int length = buffer.getNumSamples();
        lo = 0.0f;
        hi = 0.0f;

        if (s1 <= 0.0 || s0 >= (double) length)
            return;

        if (s1 - s0 >= (double) peakBlock * 2.0)
        {
            const int b0 = juce::jlimit (0, (int) peakMin.size() - 1, (int) (s0 / peakBlock));
            const int b1 = juce::jlimit (b0, (int) peakMin.size() - 1, (int) (s1 / peakBlock));

            for (int b = b0; b <= b1; ++b)
            {
                lo = juce::jmin (lo, peakMin[(size_t) b]);
                hi = juce::jmax (hi, peakMax[(size_t) b]);
            }
        }
        else
        {
            const int from  = juce::jlimit (0, length - 1, (int) s0);
            const int count = juce::jlimit (1, length - from, (int) s1 - from + 1);

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                const auto range = buffer.findMinMax (ch, from, count);
                lo = juce::jmin (lo, range.getStart());
                hi = juce::jmax (hi, range.getEnd());
            }
        }
    }

    int hitMarker (float x) const
    {
        int best = -1;
        float bestDistance = 6.0f;

        for (int i = 0; i < proc.getNumSlices(); ++i)
        {
            const float distance = std::abs (sampleToX (proc.getSlice (i).start) - x);

            if (distance < bestDistance)
            {
                bestDistance = distance;
                best = i;
            }
        }

        return best;
    }

    DragMode hitRegionHandle (juce::Point<float> position) const
    {
        if (! getRulerArea().toFloat().contains (position))
            return DragMode::none;

        const float x1 = sampleToX (proc.getRegionStart());
        const float x2 = sampleToX (proc.getRegionEnd());
        const float d1 = std::abs (position.x - (x1 + handleWidth * 0.5f));
        const float d2 = std::abs (position.x - (x2 - handleWidth * 0.5f));

        if (juce::jmin (d1, d2) > 9.0f)
            return DragMode::none;

        return d1 <= d2 ? DragMode::regionStart : DragMode::regionEnd;
    }

    void updateScrollBar()
    {
        const double length = sample != nullptr ? (double) sample->buffer.getNumSamples() : 1.0;
        scrollBar.setRangeLimits (0.0, length, juce::dontSendNotification);
        scrollBar.setCurrentRange (viewStart, viewEnd - viewStart, juce::dontSendNotification);
        scrollBar.setVisible (sample != nullptr);
    }

    void scrollBarMoved (juce::ScrollBar*, double newRangeStart) override
    {
        const double width = viewEnd - viewStart;
        viewStart = newRangeStart;
        viewEnd   = newRangeStart + width;
        repaint();
    }

    void notifyEdit()     { repaint(); if (onEdit != nullptr)   onEdit(); }
    void notifySelect()   { repaint(); if (onSelect != nullptr) onSelect(); }

    //==============================================================================
    SiampleProcessor& proc;
    SiampleProcessor::SampleData::Ptr sample;
    std::vector<float> peakMin, peakMax;

    double viewStart = 0.0, viewEnd = 1.0;
    juce::ScrollBar scrollBar { false };

    DragMode dragMode = DragMode::none;
    int dragIndex = 0;
    int dragAnchor = 0;
    bool dragMoved = false;
    int pendingRegionStart = 0, pendingRegionEnd = 0;
    bool dropHighlight = false;
    juce::uint64 activeMask = 0;
    int playheadPosition = -1;
    juce::String emptyMessage;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformView)
};

} // namespace siample
