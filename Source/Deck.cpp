#include "Deck.h"
#include "PluginProcessor.h"
#include "Stretcher.h"

namespace siample
{

namespace
{
    constexpr int minRegionLength = 1024;
    constexpr double minBpm = 30.0, maxBpm = 300.0;

    /** Le fondu de boucle a besoin de l'audio qui précède la tranche. */
    bool wantsLead (const SliceParams& p)
    {
        return p.loop && ! p.reverse && p.loopFade > 0.5f;
    }

    bool isMusicalDivision (int d)
    {
        return d == grid::bars || d == grid::quarters || d == grid::eighths || d == grid::sixteenths;
    }
}

//==============================================================================
/** Ce qu'il faut calculer : une photo des tranches au moment de la demande. */
struct Deck::RenderJob
{
    struct Item { int index, start, end, pitch; float fine; bool lead; };

    SampleData::Ptr sample;
    std::vector<Item> items;
    double ratio = 1.0;
    bool enabled = false;
};

/** Thread de fond qui étire les tranches quand le mode Sync est actif. */
class Deck::RenderWorker final : public juce::Thread
{
public:
    explicit RenderWorker (Deck& d) : juce::Thread ("Siample render"), deck (d) {}

    ~RenderWorker() override
    {
        stopThread (10000);
    }

    void post (std::unique_ptr<RenderJob> job)
    {
        {
            const juce::ScopedLock lock (jobLock);
            pending = std::move (job);
            busy.store (true);
        }

        if (! isThreadRunning())
            startThread (juce::Thread::Priority::low);

        notify();
    }

    bool isBusy() const   { return busy.load(); }

    void waitUntilIdle (int timeoutMs)
    {
        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) juce::jmax (0, timeoutMs);

        while (busy.load() && juce::Time::getMillisecondCounter() < deadline)
            juce::Thread::sleep (2);
    }

    void run() override
    {
        while (! threadShouldExit())
        {
            std::unique_ptr<RenderJob> job;

            {
                const juce::ScopedLock lock (jobLock);
                job = std::move (pending);

                if (job == nullptr)
                    busy.store (false);
            }

            if (job == nullptr)
            {
                wait (-1);
                continue;
            }

            process (*job);
            deck.collectGarbage();
        }
    }

private:
    bool hasNewerJob()
    {
        const juce::ScopedLock lock (jobLock);
        return pending != nullptr;
    }

    static bool fits (const RenderedSlice& r, const RenderJob::Item& item, double ratio)
    {
        return r.matches (item.start, item.end, item.pitch, item.fine, ratio) && (r.lead > 0 || ! item.lead || item.start == 0);
    }

    void process (const RenderJob& job)
    {
        const bool stretching = std::abs (job.ratio - 1.0) > 1.0e-4;
        auto& lock = deck.owner.audioLock;

        for (int index = 0; index < maxSlices; ++index)
        {
            if (threadShouldExit() || hasNewerJob())
                return;

            const RenderJob::Item* item = nullptr;

            if (job.enabled)
                for (const auto& candidate : job.items)
                    if (candidate.index == index)
                        item = &candidate;

            const bool needed = item != nullptr && (stretching || item->pitch != 0 || std::abs (item->fine) > 0.001f);

            RenderedSlice::Ptr result;
            bool upToDate = false;

            {
                const juce::SpinLock::ScopedLockType scoped (lock);

                if (deck.sample != job.sample)
                    return;   // le sample a changé entre-temps

                const auto* current = deck.rendered[(size_t) index].get();

                if (! needed)
                {
                    upToDate = current == nullptr;
                }
                else if (current != nullptr && fits (*current, *item, job.ratio))
                {
                    upToDate = true;
                }
                else
                {
                    // Une autre case contient peut-être déjà exactement ce passage (tranches décalées d'un cran)
                    for (const auto& other : deck.rendered)
                        if (other != nullptr && fits (*other, *item, job.ratio))
                        {
                            result = other;
                            break;
                        }
                }
            }

            if (upToDate)
                continue;

            if (needed && result == nullptr)
            {
                RenderedSlice::Ptr fresh = new RenderedSlice();
                fresh->start = item->start;
                fresh->end   = item->end;
                fresh->pitch = item->pitch;
                fresh->fine  = item->fine;
                fresh->ratio = job.ratio;

                const double semitones = (double) item->pitch + (double) item->fine / 100.0;
                const int before = item->lead ? juce::jmin (item->start, (int) (0.3 * job.sample->sampleRate)) : 0;

                if (stretchSegment (job.sample->buffer, job.sample->sampleRate, item->start - before, item->end,
                                    job.ratio, semitones, fresh->buffer))
                {
                    const int body = juce::roundToInt ((double) (item->end - item->start) * job.ratio);
                    fresh->lead = before > 0 ? juce::jlimit (0, fresh->buffer.getNumSamples() - 2, fresh->buffer.getNumSamples() - body) : 0;
                    result = fresh;
                }
            }

            RenderedSlice::Ptr previous;

            {
                const juce::SpinLock::ScopedLockType scoped (lock);

                if (deck.sample != job.sample)
                    return;

                previous = deck.rendered[(size_t) index];
                deck.rendered[(size_t) index] = result;

                if (previous != nullptr)
                    deck.retired.push_back (previous);   // une voix la lit peut-être encore
            }
        }
    }

    Deck& deck;
    juce::CriticalSection jobLock;
    std::unique_ptr<RenderJob> pending;
    std::atomic<bool> busy { false };
};

//==============================================================================
Deck::Deck (SiampleProcessor& processor, int deckIndex) : owner (processor), index (deckIndex)
{
    retired.reserve (256);
    worker = std::make_unique<RenderWorker> (*this);
}

Deck::~Deck()
{
    worker.reset();   // arrête le thread de fond avant de détruire le reste
}

//==============================================================================
void Deck::installSample (SampleData::Ptr newSample)
{
    std::array<RenderedSlice::Ptr, maxSlices> oldRendered;
    std::vector<RenderedSlice::Ptr> oldRetired;
    oldRetired.reserve (256);

    {
        const juce::SpinLock::ScopedLockType lock (owner.audioLock);

        owner.killDeckVoices (index);

        std::swap (sample, newSample);
        std::swap (rendered, oldRendered);
        std::swap (retired, oldRetired);

        numSlices   = 0;
        regionStart = 0;
        regionEnd   = sample != nullptr ? sample->buffer.getNumSamples() : 0;
        syncActive  = false;
    }

    // l'ancien sample et ses tranches étirées sont libérés ici, hors du verrou
    owner.deckSampleReplaced (index);
    candidates.clear();
    detectedKey = {};
    analysisValid = false;
    selectedSlice = 0;
    activeMask.store (0, std::memory_order_relaxed);
    playhead.store (-1, std::memory_order_relaxed);
}

juce::String Deck::loadFile (const juce::File& file)
{
    juce::String error;
    auto data = owner.readFile (file, error);

    if (data == nullptr)
        return error;

    owner.beginEdit ("load");
    installSample (data);

    {
        const juce::SpinLock::ScopedLockType lock (owner.audioLock);
        for (auto& s : slices)
            s = Slice();

        transpose = 0;
    }

    missingFilePath.clear();
    orphanState = {};
    bpmManual = false;
    analyseRegion();
    reslice();
    owner.sendChangeMessage();
    return {};
}

void Deck::unload()
{
    if (sample == nullptr && missingFilePath.isEmpty())
        return;

    owner.beginEdit ("unload");
    installSample (nullptr);

    {
        const juce::SpinLock::ScopedLockType lock (owner.audioLock);
        for (auto& s : slices)
            s = Slice();

        transpose = 0;
    }

    missingFilePath.clear();
    orphanState = {};
    scheduleRender();
    owner.sendChangeMessage();
}

//==============================================================================
void Deck::setRegion (int newStart, int newEnd)
{
    if (sample == nullptr)
        return;

    const int length = sample->buffer.getNumSamples();
    const int minLen = juce::jmin (minRegionLength, length);

    newStart = juce::jlimit (0, length - minLen, newStart);
    newEnd   = juce::jlimit (newStart + minLen, length, newEnd);

    if (newStart == regionStart && newEnd == regionEnd)
        return;

    owner.beginEdit ("region");

    {
        const juce::SpinLock::ScopedLockType lock (owner.audioLock);
        regionStart = newStart;
        regionEnd   = newEnd;
    }

    analysisValid = false;
    analyseRegion();
    reslice();
}

void Deck::analyseRegion()
{
    if (analysisValid)
        return;

    candidates.clear();
    detectedKey = {};
    analysisValid = true;

    if (sample == nullptr)
        return;

    const int length = regionEnd - regionStart;

    if (length <= 0)
        return;

    // Mélange mono de la zone de travail
    std::vector<float> mono ((size_t) length, 0.0f);
    const int channels = sample->buffer.getNumChannels();

    for (int ch = 0; ch < channels; ++ch)
    {
        const float* in = sample->buffer.getReadPointer (ch, regionStart);

        for (int i = 0; i < length; ++i)
            mono[(size_t) i] += in[i];
    }

    const auto envelope = computeOnsetEnvelope (mono.data(), length, sample->sampleRate);
    candidates = findOnsetCandidates (mono.data(), length, sample->sampleRate, envelope);
    detectedKey = detectKey (mono.data(), length, sample->sampleRate);

    if (! bpmManual)
    {
        const double bpm = estimateTempo (envelope, length, sample->sampleRate);
        sampleBpm = bpm > 0.0 ? juce::jlimit (minBpm, maxBpm, bpm) : 120.0;
    }
}

void Deck::reslice()
{
    if (sample == nullptr)
    {
        {
            const juce::SpinLock::ScopedLockType lock (owner.audioLock);
            numSlices = 0;
        }

        slicesChanged();
        return;
    }

    const int length = regionEnd - regionStart;
    std::vector<int> positions;

    switch (sliceMode)
    {
        case SliceMode::transients:
        {
            analyseRegion();

            const int minGap = (int) (0.05 * sample->sampleRate);
            const int snap   = (int) (0.002 * sample->sampleRate);
            const int lead   = (int) (0.06 * sample->sampleRate);

            for (int p : selectOnsets (candidates, sensitivity, maxSlices, minGap))
            {
                const int absolute = regionStart + p;

                if (absolute < regionEnd - minSliceLength)
                    positions.push_back (absolute <= regionStart + snap ? regionStart : absolute);
            }

            // La première tranche part du début de la zone, sauf si une attaque arrive presque tout de suite
            // (cas typique : les quelques millisecondes de silence au début d'un MP3) : on part alors de l'attaque.
            if (positions.empty() || positions.front() > regionStart + lead)
            {
                positions.insert (positions.begin(), regionStart);

                if ((int) positions.size() > maxSlices)
                    positions.pop_back();
            }
            break;
        }

        case SliceMode::grid:
        {
            if (divisions > 0)
            {
                // Parts égales
                const int count = juce::jlimit (1, maxSlices, juce::jmin (divisions, length / minSliceLength));

                for (int i = 0; i < count; ++i)
                    positions.push_back (regionStart + (int) ((juce::int64) length * i / count));
            }
            else
            {
                // Valeur musicale, d'après le tempo du sample
                const double beats = divisions == grid::bars ? 4.0 : 4.0 / (double) -divisions;
                const double step  = sample->sampleRate * 60.0 / juce::jmax (minBpm, sampleBpm) * beats;

                for (int i = 0; i < maxSlices; ++i)
                {
                    const int position = regionStart + (int) std::llround ((double) i * step);

                    if (position > regionEnd - minSliceLength)
                        break;

                    positions.push_back (position);
                }

                if (positions.empty())
                    positions.push_back (regionStart);
            }
            break;
        }

        case SliceMode::manual:
        {
            // On garde les points existants (avec leurs réglages) qui sont encore dans la zone
            std::vector<Slice> kept;

            for (int i = 0; i < numSlices; ++i)
                if (slices[(size_t) i].start >= regionStart && slices[(size_t) i].start < regionEnd - minSliceLength)
                    kept.push_back (slices[(size_t) i]);

            if (kept.empty())
            {
                Slice s;
                s.start = regionStart;
                kept.push_back (s);
            }

            {
                const juce::SpinLock::ScopedLockType lock (owner.audioLock);

                for (size_t i = 0; i < kept.size(); ++i)
                    slices[i] = kept[i];

                numSlices = (int) kept.size();
            }

            slicesChanged();
            return;
        }
    }

    applyPositions (positions);
}

void Deck::applyPositions (const std::vector<int>& positions)
{
    const int count = juce::jmin (maxSlices, (int) positions.size());

    {
        const juce::SpinLock::ScopedLockType lock (owner.audioLock);

        for (int i = 0; i < count; ++i)
            slices[(size_t) i].start = positions[(size_t) i];   // les réglages restent attachés au numéro de tranche

        numSlices = count;
    }

    slicesChanged();
}

void Deck::slicesChanged()
{
    selectedSlice = juce::jlimit (0, juce::jmax (0, numSlices - 1), selectedSlice.load());
    owner.deckSlicesChanged (index);
    scheduleRender();
}

//==============================================================================
int Deck::getSliceEnd (int sliceIndex) const
{
    if (sliceIndex < 0 || sliceIndex >= numSlices)
        return regionEnd;

    return sliceIndex + 1 < numSlices ? slices[(size_t) sliceIndex + 1].start : regionEnd;
}

int Deck::findSliceAt (int samplePosition) const
{
    if (samplePosition < regionStart || samplePosition >= regionEnd)
        return -1;

    int found = -1;

    for (int i = 0; i < numSlices; ++i)
        if (slices[(size_t) i].start <= samplePosition)
            found = i;

    return found;
}

void Deck::setSliceParams (int sliceIndex, const SliceParams& params)
{
    if (sliceIndex < 0 || sliceIndex >= numSlices)
        return;

    const auto& old = slices[(size_t) sliceIndex].params;
    const bool renderChanged = old.pitch != params.pitch || std::abs (old.fine - params.fine) > 0.001f
                            || wantsLead (old) != wantsLead (params);

    owner.beginEdit ("params" + juce::String (index * 100 + sliceIndex), true);

    {
        const juce::SpinLock::ScopedLockType lock (owner.audioLock);
        slices[(size_t) sliceIndex].params = params;
    }

    if (renderChanged)
        scheduleRender();
}

void Deck::setAllSliceParams (const SliceParams& params)
{
    owner.beginEdit ("allParams");

    {
        const juce::SpinLock::ScopedLockType lock (owner.audioLock);

        for (auto& s : slices)
            s.params = params;
    }

    scheduleRender();
}

int Deck::moveMarker (int sliceIndex, int newPosition)
{
    if (sliceIndex < 0 || sliceIndex >= numSlices)
        return 0;

    const int lowest  = sliceIndex > 0 ? slices[(size_t) sliceIndex - 1].start + minSliceLength : regionStart;
    const int highest = getSliceEnd (sliceIndex) - minSliceLength;

    if (lowest > highest)
        return slices[(size_t) sliceIndex].start;

    newPosition = juce::jlimit (lowest, highest, newPosition);

    if (newPosition == slices[(size_t) sliceIndex].start)
        return newPosition;

    owner.beginEdit ("marker" + juce::String (index * 100 + sliceIndex), true);

    {
        const juce::SpinLock::ScopedLockType lock (owner.audioLock);
        slices[(size_t) sliceIndex].start = newPosition;
    }

    sliceMode = SliceMode::manual;
    scheduleRender();
    return newPosition;
}

int Deck::addMarker (int position)
{
    if (sample == nullptr || numSlices >= maxSlices)
        return -1;

    if (position < regionStart || position > regionEnd - minSliceLength)
        return -1;

    const int parent = findSliceAt (position);
    int insertAt = 0;
    Slice added;
    added.start = position;

    if (parent >= 0)
    {
        if (position - slices[(size_t) parent].start < minSliceLength || getSliceEnd (parent) - position < minSliceLength)
            return -1;

        added.params = slices[(size_t) parent].params;   // la nouvelle tranche hérite des réglages de celle qu'elle coupe
        insertAt = parent + 1;
    }
    else if (numSlices > 0 && slices[0].start - position < minSliceLength)
    {
        return -1;
    }

    owner.beginEdit ("addMarker");

    {
        const juce::SpinLock::ScopedLockType lock (owner.audioLock);

        for (int i = numSlices; i > insertAt; --i)
            slices[(size_t) i] = slices[(size_t) i - 1];

        slices[(size_t) insertAt] = added;
        ++numSlices;
    }

    sliceMode = SliceMode::manual;
    selectedSlice = insertAt;
    slicesChanged();
    return insertAt;
}

bool Deck::removeMarker (int sliceIndex)
{
    if (sliceIndex < 0 || sliceIndex >= numSlices || numSlices <= 1)
        return false;

    owner.beginEdit ("removeMarker");

    {
        const juce::SpinLock::ScopedLockType lock (owner.audioLock);

        for (int i = sliceIndex; i < numSlices - 1; ++i)
            slices[(size_t) i] = slices[(size_t) i + 1];

        --numSlices;
        slices[(size_t) numSlices] = Slice();
    }

    sliceMode = SliceMode::manual;

    if (selectedSlice.load() >= sliceIndex && selectedSlice.load() > 0)
        selectedSlice = selectedSlice.load() - 1;

    slicesChanged();
    return true;
}

void Deck::setSelectedSlice (int sliceIndex)
{
    selectedSlice = juce::jlimit (0, juce::jmax (0, numSlices - 1), sliceIndex);
}

void Deck::normaliseSlices()
{
    if (sample == nullptr || numSlices == 0)
        return;

    owner.beginEdit ("normalise");
    std::array<float, maxSlices> gains {};

    for (int i = 0; i < numSlices; ++i)
    {
        const int from = slices[(size_t) i].start;
        const int length = juce::jmin (getSliceEnd (i), sample->buffer.getNumSamples()) - from;
        const float peak = length > 0 ? sample->buffer.getMagnitude (from, length) : 0.0f;

        // Crête à -1 dB ; une tranche quasi silencieuse n'est pas remontée
        gains[(size_t) i] = peak > 0.003f ? juce::jlimit (-24.0f, 12.0f, -1.0f - juce::Decibels::gainToDecibels (peak))
                                          : slices[(size_t) i].params.gainDb;
    }

    const juce::SpinLock::ScopedLockType lock (owner.audioLock);

    for (int i = 0; i < numSlices; ++i)
        slices[(size_t) i].params.gainDb = std::round (gains[(size_t) i] * 10.0f) / 10.0f;
}

//==============================================================================
void Deck::setSliceMode (SliceMode newMode)
{
    if (sliceMode == newMode)
        return;

    owner.beginEdit ("mode");
    sliceMode = newMode;
    reslice();
}

void Deck::setSensitivity (float newSensitivity)
{
    owner.beginEdit ("sensitivity", true);
    sensitivity = juce::jlimit (0.0f, 1.0f, newSensitivity);

    if (sliceMode == SliceMode::transients)
        reslice();
}

void Deck::setDivisions (int newDivisions)
{
    owner.beginEdit ("divisions");
    divisions = isMusicalDivision (newDivisions) ? newDivisions : juce::jlimit (1, maxSlices, newDivisions);

    if (sliceMode == SliceMode::grid)
        reslice();
}

//==============================================================================
void Deck::setSampleBpm (double bpm)
{
    owner.beginEdit ("bpm");
    sampleBpm = juce::jlimit (minBpm, maxBpm, bpm);
    bpmManual = true;

    if (sliceMode == SliceMode::grid && divisions < 0)
        reslice();
    else
        scheduleRender();
}

void Deck::detectBpm()
{
    owner.beginEdit ("bpm");
    bpmManual = false;
    analysisValid = false;
    analyseRegion();

    if (sliceMode == SliceMode::grid && divisions < 0)
        reslice();
    else
        scheduleRender();
}

void Deck::setSyncEnabled (bool shouldSync)
{
    if (syncEnabled == shouldSync)
        return;

    owner.beginEdit ("sync");
    syncEnabled = shouldSync;
    scheduleRender();
}

void Deck::setTranspose (int semitones)
{
    semitones = juce::jlimit (-12, 12, semitones);

    if (semitones == transpose)
        return;

    owner.beginEdit ("transpose");

    {
        const juce::SpinLock::ScopedLockType lock (owner.audioLock);
        transpose = semitones;
    }

    scheduleRender();
}

bool Deck::isRendering() const
{
    return worker->isBusy();
}

void Deck::waitForRendering (int timeoutMs)
{
    worker->waitUntilIdle (timeoutMs);
}

void Deck::scheduleRender()
{
    const juce::ScopedLock schedule (scheduleLock);

    auto job = std::make_unique<RenderJob>();
    job->items.reserve ((size_t) maxSlices);
    const double target = juce::jlimit (minBpm, 999.0, owner.hostBpm.load());

    {
        const juce::SpinLock::ScopedLockType lock (owner.audioLock);

        syncActive   = syncEnabled && sample != nullptr;
        stretchRatio = syncActive ? juce::jlimit (0.25, 4.0, sampleBpm / target) : 1.0;

        job->sample  = sample;
        job->ratio   = stretchRatio;
        job->enabled = syncActive;

        if (syncActive)
            for (int i = 0; i < numSlices; ++i)
                job->items.push_back ({ i, slices[(size_t) i].start, getSliceEnd (i), effectivePitch (i),
                                        slices[(size_t) i].params.fine, wantsLead (slices[(size_t) i].params) });
    }

    renderedBpm = target;
    lastSeenHostBpm = target;
    worker->post (std::move (job));
}

void Deck::collectGarbage()
{
    std::vector<RenderedSlice::Ptr> garbage;
    garbage.reserve (256);

    {
        const juce::SpinLock::ScopedLockType lock (owner.audioLock);

        for (size_t i = retired.size(); i-- > 0;)
        {
            bool inUse = false;

            for (const auto& v : owner.voices)
                if (v.active && v.owner == retired[i].get())
                    inUse = true;

            if (! inUse && garbage.size() < garbage.capacity())
            {
                garbage.push_back (std::move (retired[i]));
                retired.erase (retired.begin() + (std::ptrdiff_t) i);
            }
        }
    }

    // `garbage` est détruit ici, hors du verrou
}

void Deck::hostTempoTick()
{
    collectGarbage();

    // Le tempo du projet a changé : on recalcule quand il s'est stabilisé
    if (syncEnabled && sample != nullptr)
    {
        const double current = owner.hostBpm.load();

        if (std::abs (current - renderedBpm) > 0.005)
        {
            if (std::abs (current - lastSeenHostBpm) < 0.005)
            {
                scheduleRender();
                owner.sendChangeMessage();
            }

            lastSeenHostBpm = current;
        }
    }
}

//==============================================================================
void Deck::setPlayMode (PlayMode newMode)
{
    const juce::SpinLock::ScopedLockType lock (owner.audioLock);
    playMode = newMode;
}

void Deck::setMono (bool shouldBeMono)
{
    const juce::SpinLock::ScopedLockType lock (owner.audioLock);
    monoMode = shouldBeMono;
}

double Deck::sliceSourceDuration (int sliceIndex) const
{
    if (sliceIndex < 0 || sliceIndex >= numSlices)
        return 0.0;

    const auto& s = slices[(size_t) sliceIndex];
    const double length = (double) (getSliceEnd (sliceIndex) - s.start);

    // Avec Sync, la durée suit le tempo du projet ; sinon c'est le pitch qui la change
    if (syncActive)
        return length * stretchRatio;

    return length / std::pow (2.0, ((double) effectivePitch (sliceIndex) + (double) s.params.fine / 100.0) / 12.0);
}

double Deck::getSliceSeconds (int sliceIndex) const
{
    return sample != nullptr ? sliceSourceDuration (sliceIndex) / sample->sampleRate : 0.0;
}

//==============================================================================
juce::ValueTree Deck::toTree() const
{
    // Si le fichier audio était introuvable au chargement, on réécrit l'état d'origine tel quel
    // pour ne pas perdre la découpe.
    if (sample == nullptr && orphanState.isValid())
    {
        auto copy = orphanState.createCopy();
        copy.setProperty ("index", index, nullptr);
        return copy;
    }

    juce::ValueTree state ("Deck");
    state.setProperty ("index", index, nullptr);
    state.setProperty ("file", sample != nullptr ? sample->file.getFullPathName() : missingFilePath, nullptr);
    state.setProperty ("regionStart", regionStart, nullptr);
    state.setProperty ("regionEnd", regionEnd, nullptr);
    state.setProperty ("mode", (int) sliceMode, nullptr);
    state.setProperty ("sensitivity", sensitivity, nullptr);
    state.setProperty ("divisions", divisions, nullptr);
    state.setProperty ("selected", selectedSlice.load(), nullptr);
    state.setProperty ("bpm", sampleBpm, nullptr);
    state.setProperty ("bpmManual", bpmManual, nullptr);
    state.setProperty ("sync", syncEnabled, nullptr);
    state.setProperty ("mono", monoMode, nullptr);
    state.setProperty ("playMode", (int) playMode, nullptr);
    state.setProperty ("transpose", transpose, nullptr);

    for (int i = 0; i < numSlices; ++i)
    {
        const auto& s = slices[(size_t) i];
        juce::ValueTree child ("Slice");
        child.setProperty ("start",   s.start, nullptr);
        child.setProperty ("gain",    s.params.gainDb, nullptr);
        child.setProperty ("pan",     s.params.pan, nullptr);
        child.setProperty ("pitch",   s.params.pitch, nullptr);
        child.setProperty ("fine",    s.params.fine, nullptr);
        child.setProperty ("attack",  s.params.attack, nullptr);
        child.setProperty ("decay",   s.params.decay, nullptr);
        child.setProperty ("sustain", s.params.sustain, nullptr);
        child.setProperty ("release", s.params.release, nullptr);
        child.setProperty ("filter",  s.params.filter, nullptr);
        child.setProperty ("reverse", s.params.reverse, nullptr);
        child.setProperty ("oneShot", s.params.oneShot, nullptr);
        child.setProperty ("loop",    s.params.loop, nullptr);
        child.setProperty ("choke",   s.params.choke, nullptr);
        child.setProperty ("resonance", s.params.resonance, nullptr);
        child.setProperty ("drive",     s.params.drive, nullptr);
        child.setProperty ("loopFade",  s.params.loopFade, nullptr);
        state.appendChild (child, nullptr);
    }

    return state;
}

void Deck::fromTree (const juce::ValueTree& state, bool reuseLoadedSample)
{
    sliceMode   = (SliceMode) juce::jlimit (0, 2, (int) state.getProperty ("mode", 0));
    sensitivity = juce::jlimit (0.0f, 1.0f, (float) state.getProperty ("sensitivity", 0.5f));

    {
        const int saved = (int) state.getProperty ("divisions", 16);
        divisions = isMusicalDivision (saved) ? saved : juce::jlimit (1, maxSlices, saved);
    }

    const auto path = state.getProperty ("file").toString();
    const bool reuse = reuseLoadedSample && sample != nullptr && path.isNotEmpty()
                    && sample->file.getFullPathName() == path;

    if (! reuse)
    {
        juce::String error;
        installSample (path.isNotEmpty() ? owner.readFile (juce::File (path), error) : nullptr);
        analysisValid = false;
    }

    orphanState = {};
    missingFilePath.clear();

    syncEnabled = (bool) state.getProperty ("sync", false);
    bpmManual   = (bool) state.getProperty ("bpmManual", false);
    sampleBpm   = juce::jlimit (minBpm, maxBpm, (double) state.getProperty ("bpm", 120.0));

    {
        const juce::SpinLock::ScopedLockType lock (owner.audioLock);
        monoMode  = (bool) state.getProperty ("mono", false);
        playMode  = (PlayMode) juce::jlimit (0, 1, (int) state.getProperty ("playMode", 0));
        transpose = juce::jlimit (-12, 12, (int) state.getProperty ("transpose", 0));
    }

    if (sample == nullptr)
    {
        {
            const juce::SpinLock::ScopedLockType lock (owner.audioLock);
            numSlices = 0;
        }

        if (path.isNotEmpty())
        {
            missingFilePath = path;
            orphanState = juce::ValueTree ("Deck");
            orphanState.copyPropertiesAndChildrenFrom (state, nullptr);
        }

        return;
    }

    const int length = sample->buffer.getNumSamples();
    const int rs = juce::jlimit (0, juce::jmax (0, length - 1), (int) state.getProperty ("regionStart", 0));
    const int re = juce::jlimit (rs + 1, length, (int) state.getProperty ("regionEnd", length));

    std::vector<Slice> restored;

    for (const auto& child : state)
    {
        if (! child.hasType ("Slice") || (int) restored.size() >= maxSlices)
            continue;

        Slice s;
        s.start = (int) child.getProperty ("start", 0);

        if (s.start < 0 || s.start >= length || (! restored.empty() && s.start <= restored.back().start))
            continue;

        s.params.gainDb  = juce::jlimit (-24.0f, 12.0f, (float) child.getProperty ("gain", 0.0f));
        s.params.pan     = juce::jlimit (-1.0f, 1.0f,   (float) child.getProperty ("pan", 0.0f));
        s.params.pitch   = juce::jlimit (-24, 24,       (int)   child.getProperty ("pitch", 0));
        s.params.fine    = juce::jlimit (-100.0f, 100.0f, (float) child.getProperty ("fine", 0.0f));
        s.params.attack  = juce::jlimit (0.0f, 2.0f,    (float) child.getProperty ("attack", 0.0f));
        s.params.decay   = juce::jlimit (0.0f, 2.0f,    (float) child.getProperty ("decay", 0.0f));
        s.params.sustain = juce::jlimit (0.0f, 1.0f,    (float) child.getProperty ("sustain", 1.0f));
        s.params.release = juce::jlimit (0.0f, 3.0f,    (float) child.getProperty ("release", 0.02f));
        s.params.filter  = juce::jlimit (-1.0f, 1.0f,   (float) child.getProperty ("filter", 0.0f));
        s.params.reverse = (bool) child.getProperty ("reverse", false);
        s.params.oneShot = (bool) child.getProperty ("oneShot", true);
        s.params.loop    = (bool) child.getProperty ("loop", false);
        s.params.choke   = juce::jlimit (0, numChokeGroups, (int) child.getProperty ("choke", 0));
        s.params.resonance = juce::jlimit (0.0f, 1.0f,   (float) child.getProperty ("resonance", 0.0f));
        s.params.drive     = juce::jlimit (0.0f, 1.0f,   (float) child.getProperty ("drive", 0.0f));
        s.params.loopFade  = juce::jlimit (0.0f, 250.0f, (float) child.getProperty ("loopFade", 0.0f));
        restored.push_back (s);
    }

    const bool regionMoved = rs != regionStart || re != regionEnd;

    {
        const juce::SpinLock::ScopedLockType lock (owner.audioLock);
        regionStart = rs;
        regionEnd   = re;

        for (size_t i = 0; i < slices.size(); ++i)
            slices[i] = i < restored.size() ? restored[i] : Slice();

        numSlices = (int) restored.size();
    }

    if (regionMoved)
        analysisValid = false;

    // La détection ne doit pas écraser le tempo enregistré ; un projet de la version 0.1 n'en a pas
    {
        const bool manual = bpmManual;
        const double bpm = sampleBpm;
        bpmManual = state.hasProperty ("bpm");
        analyseRegion();
        bpmManual = manual;

        if (state.hasProperty ("bpm"))
            sampleBpm = bpm;
    }

    if (numSlices == 0)
        reslice();

    selectedSlice = juce::jlimit (0, juce::jmax (0, numSlices - 1), (int) state.getProperty ("selected", 0));
    scheduleRender();
}

} // namespace siample
