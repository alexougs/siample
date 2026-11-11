#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Stretcher.h"

using namespace siample;

namespace
{
    constexpr double maxFileMinutes = 15.0;
    constexpr int minRegionLength = 1024;
    constexpr double minBpm = 30.0, maxBpm = 300.0;

    enum EventType { noteOnEvent = 0, noteOffEvent, allOffEvent, patternEvent, sequenceEvent };

    /** Grain des vieux samplers : fréquence d'échantillonnage réduite + moins de bits. */
    void processGrain (juce::AudioBuffer<float>& buffer, float amount, double rate,
                       float& phase, float& heldL, float& heldR)
    {
        if (amount < 0.005f || buffer.getNumChannels() == 0)
            return;

        const float hold   = (1.0f + amount * amount * 9.0f) * (float) (rate / 44100.0);
        const float levels = std::pow (2.0f, 15.0f - 8.0f * amount);

        float* left  = buffer.getWritePointer (0);
        float* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : left;

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            phase += 1.0f;

            if (phase >= hold)
            {
                phase -= hold;
                heldL = std::round (left[i] * levels) / levels;
                heldR = std::round (right[i] * levels) / levels;
            }

            left[i]  = heldL;
            right[i] = heldR;
        }
    }
}

//==============================================================================
/** Ce qu'il faut calculer : une photo des tranches au moment de la demande. */
struct SiampleProcessor::RenderJob
{
    struct Item { int index, start, end, pitch; float fine; };

    SampleData::Ptr sample;
    std::vector<Item> items;
    double ratio = 1.0;
    bool enabled = false;
};

/** Thread de fond qui étire les tranches quand le mode Sync est actif. */
class SiampleProcessor::RenderWorker final : public juce::Thread
{
public:
    explicit RenderWorker (SiampleProcessor& p) : juce::Thread ("Siample render"), owner (p) {}

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
            owner.collectGarbage();
        }
    }

private:
    bool hasNewerJob()
    {
        const juce::ScopedLock lock (jobLock);
        return pending != nullptr;
    }

    void process (const RenderJob& job)
    {
        const bool stretching = std::abs (job.ratio - 1.0) > 1.0e-4;

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
                const juce::SpinLock::ScopedLockType lock (owner.audioLock);

                if (owner.sample != job.sample)
                    return;   // le sample a changé entre-temps

                const auto* current = owner.rendered[(size_t) index].get();

                if (! needed)
                {
                    upToDate = current == nullptr;
                }
                else if (current != nullptr && current->matches (item->start, item->end, item->pitch, item->fine, job.ratio))
                {
                    upToDate = true;
                }
                else
                {
                    // Une autre case contient peut-être déjà exactement ce passage (tranches décalées d'un cran)
                    for (const auto& other : owner.rendered)
                        if (other != nullptr && other->matches (item->start, item->end, item->pitch, item->fine, job.ratio))
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

                if (stretchSegment (job.sample->buffer, job.sample->sampleRate, item->start, item->end,
                                    job.ratio, semitones, fresh->buffer))
                    result = fresh;
            }

            RenderedSlice::Ptr previous;

            {
                const juce::SpinLock::ScopedLockType lock (owner.audioLock);

                if (owner.sample != job.sample)
                    return;

                previous = owner.rendered[(size_t) index];
                owner.rendered[(size_t) index] = result;

                if (previous != nullptr)
                    owner.retired.push_back (previous);   // une voix la lit peut-être encore
            }
        }
    }

    SiampleProcessor& owner;
    juce::CriticalSection jobLock;
    std::unique_ptr<RenderJob> pending;
    std::atomic<bool> busy { false };
};

//==============================================================================
SiampleProcessor::SiampleProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    formatManager.registerBasicFormats();

    masterGain = new juce::AudioParameterFloat (juce::ParameterID { "master", 1 }, "Volume",
                                                juce::NormalisableRange<float> (-36.0f, 12.0f, 0.1f), 0.0f,
                                                juce::AudioParameterFloatAttributes().withLabel ("dB"));
    addParameter (masterGain);

    grain = new juce::AudioParameterFloat (juce::ParameterID { "grain", 1 }, "Grain",
                                           juce::NormalisableRange<float> (0.0f, 100.0f, 1.0f), 0.0f,
                                           juce::AudioParameterFloatAttributes().withLabel ("%"));
    addParameter (grain);

    for (int i = 0; i < maxSlices; ++i)
        patternSlices[(size_t) i] = i;

    retired.reserve (256);
    worker = std::make_unique<RenderWorker> (*this);
    startTimerHz (10);
}

SiampleProcessor::~SiampleProcessor()
{
    stopTimer();
    worker.reset();   // arrête le thread de fond avant de détruire le reste
}

//==============================================================================
SiampleProcessor::SampleData::Ptr SiampleProcessor::getSample() const
{
    return sample;
}

SiampleProcessor::SampleData::Ptr SiampleProcessor::readFile (const juce::File& file, juce::String& error)
{
    if (! file.existsAsFile())
    {
        error = fr ("Fichier introuvable");
        return nullptr;
    }

    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));

    if (reader == nullptr)
    {
        error = fr ("Format audio non reconnu (essaie WAV, AIFF, FLAC ou MP3)");
        return nullptr;
    }

    if (reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
    {
        error = fr ("Ce fichier ne contient pas d'audio");
        return nullptr;
    }

    if ((double) reader->lengthInSamples > reader->sampleRate * 60.0 * maxFileMinutes)
    {
        error = fr ("Fichier trop long (15 minutes maximum)");
        return nullptr;
    }

    SampleData::Ptr data = new SampleData();
    const int channels = juce::jlimit (1, 2, (int) reader->numChannels);
    const int length   = (int) reader->lengthInSamples;

    data->buffer.setSize (channels, length);
    data->buffer.clear();
    reader->read (&data->buffer, 0, length, 0, true, channels > 1);
    data->sampleRate = reader->sampleRate;
    data->file = file;
    return data;
}

void SiampleProcessor::installSample (SampleData::Ptr newSample)
{
    std::array<RenderedSlice::Ptr, maxSlices> oldRendered;
    std::vector<RenderedSlice::Ptr> oldRetired;
    oldRetired.reserve (256);

    {
        const juce::SpinLock::ScopedLockType lock (audioLock);

        for (auto& v : voices)
            v.active = false;

        std::swap (sample, newSample);
        std::swap (rendered, oldRendered);
        std::swap (retired, oldRetired);

        numSlices   = 0;
        regionStart = 0;
        regionEnd   = sample != nullptr ? sample->buffer.getNumSamples() : 0;
        syncActive  = false;

        for (int i = 0; i < maxSlices; ++i)
            patternSlices[(size_t) i] = i;

        patternFlipped = false;
        sequenceLength = 0;
    }

    // l'ancien sample et ses tranches étirées sont libérés ici, hors du verrou
    patternPlaying.store (false);
    sequencePlaying.store (false);
    candidates.clear();
    analysisValid = false;
    selectedSlice = 0;
}

juce::String SiampleProcessor::loadFile (const juce::File& file)
{
    juce::String error;
    auto data = readFile (file, error);

    if (data == nullptr)
        return error;

    installSample (data);

    {
        const juce::SpinLock::ScopedLockType lock (audioLock);
        for (auto& s : slices)
            s = Slice();
    }

    missingFilePath.clear();
    orphanState = {};
    bpmManual = false;
    analyseRegion();
    reslice();
    sendChangeMessage();
    return {};
}

//==============================================================================
void SiampleProcessor::setRegion (int newStart, int newEnd)
{
    if (sample == nullptr)
        return;

    const int length = sample->buffer.getNumSamples();
    const int minLen = juce::jmin (minRegionLength, length);

    newStart = juce::jlimit (0, length - minLen, newStart);
    newEnd   = juce::jlimit (newStart + minLen, length, newEnd);

    if (newStart == regionStart && newEnd == regionEnd)
        return;

    {
        const juce::SpinLock::ScopedLockType lock (audioLock);
        regionStart = newStart;
        regionEnd   = newEnd;
    }

    analysisValid = false;
    analyseRegion();
    reslice();
}

void SiampleProcessor::analyseRegion()
{
    if (analysisValid)
        return;

    candidates.clear();
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

    if (! bpmManual)
    {
        const double bpm = estimateTempo (envelope, length, sample->sampleRate);
        sampleBpm = bpm > 0.0 ? juce::jlimit (minBpm, maxBpm, bpm) : 120.0;
    }
}

void SiampleProcessor::reslice()
{
    if (sample == nullptr)
    {
        {
            const juce::SpinLock::ScopedLockType lock (audioLock);
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
                const juce::SpinLock::ScopedLockType lock (audioLock);

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

void SiampleProcessor::applyPositions (const std::vector<int>& positions)
{
    const int count = juce::jmin (maxSlices, (int) positions.size());

    {
        const juce::SpinLock::ScopedLockType lock (audioLock);

        for (int i = 0; i < count; ++i)
            slices[(size_t) i].start = positions[(size_t) i];   // les réglages restent attachés au numéro de tranche

        numSlices = count;
    }

    slicesChanged();
}

void SiampleProcessor::slicesChanged()
{
    selectedSlice = juce::jlimit (0, juce::jmax (0, numSlices - 1), selectedSlice.load());

    // La piste ne garde que les tranches qui existent encore
    {
        const juce::SpinLock::ScopedLockType lock (audioLock);
        int kept = 0;

        for (int i = 0; i < sequenceLength; ++i)
            if (sequence[(size_t) i] < numSlices)
                sequence[(size_t) kept++] = sequence[(size_t) i];

        sequenceLength = kept;
    }

    if (sequenceLength == 0)
        sequencePlaying.store (false);

    // Un réarrangement ne vaut que pour la découpe sur laquelle il a été fait
    resetPattern();
    scheduleRender();
}

//==============================================================================
int SiampleProcessor::getSliceEnd (int index) const
{
    if (index < 0 || index >= numSlices)
        return regionEnd;

    return index + 1 < numSlices ? slices[(size_t) index + 1].start : regionEnd;
}

int SiampleProcessor::findSliceAt (int samplePosition) const
{
    if (samplePosition < regionStart || samplePosition >= regionEnd)
        return -1;

    int found = -1;

    for (int i = 0; i < numSlices; ++i)
        if (slices[(size_t) i].start <= samplePosition)
            found = i;

    return found;
}

void SiampleProcessor::setSliceParams (int index, const SliceParams& params)
{
    if (index < 0 || index >= numSlices)
        return;

    const auto& old = slices[(size_t) index].params;
    const bool pitchChanged = old.pitch != params.pitch || std::abs (old.fine - params.fine) > 0.001f;

    {
        const juce::SpinLock::ScopedLockType lock (audioLock);
        slices[(size_t) index].params = params;
    }

    if (pitchChanged)
        scheduleRender();
}

void SiampleProcessor::setAllSliceParams (const SliceParams& params)
{
    {
        const juce::SpinLock::ScopedLockType lock (audioLock);

        for (auto& s : slices)
            s.params = params;
    }

    scheduleRender();
}

int SiampleProcessor::moveMarker (int index, int newPosition)
{
    if (index < 0 || index >= numSlices)
        return 0;

    const int lowest  = index > 0 ? slices[(size_t) index - 1].start + minSliceLength : regionStart;
    const int highest = getSliceEnd (index) - minSliceLength;

    if (lowest > highest)
        return slices[(size_t) index].start;

    newPosition = juce::jlimit (lowest, highest, newPosition);

    {
        const juce::SpinLock::ScopedLockType lock (audioLock);
        slices[(size_t) index].start = newPosition;
    }

    sliceMode = SliceMode::manual;
    scheduleRender();
    return newPosition;
}

int SiampleProcessor::addMarker (int position)
{
    if (sample == nullptr || numSlices >= maxSlices)
        return -1;

    if (position < regionStart || position > regionEnd - minSliceLength)
        return -1;

    const int owner = findSliceAt (position);
    int insertAt = 0;
    Slice added;
    added.start = position;

    if (owner >= 0)
    {
        if (position - slices[(size_t) owner].start < minSliceLength || getSliceEnd (owner) - position < minSliceLength)
            return -1;

        added.params = slices[(size_t) owner].params;   // la nouvelle tranche hérite des réglages de celle qu'elle coupe
        insertAt = owner + 1;
    }
    else if (numSlices > 0 && slices[0].start - position < minSliceLength)
    {
        return -1;
    }

    {
        const juce::SpinLock::ScopedLockType lock (audioLock);

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

bool SiampleProcessor::removeMarker (int index)
{
    if (index < 0 || index >= numSlices || numSlices <= 1)
        return false;

    {
        const juce::SpinLock::ScopedLockType lock (audioLock);

        for (int i = index; i < numSlices - 1; ++i)
            slices[(size_t) i] = slices[(size_t) i + 1];

        --numSlices;
        slices[(size_t) numSlices] = Slice();
    }

    sliceMode = SliceMode::manual;

    if (selectedSlice.load() >= index && selectedSlice.load() > 0)
        selectedSlice = selectedSlice.load() - 1;

    slicesChanged();
    return true;
}

void SiampleProcessor::setSelectedSlice (int index)
{
    selectedSlice = juce::jlimit (0, juce::jmax (0, numSlices - 1), index);
}

//==============================================================================
void SiampleProcessor::setSliceMode (SliceMode newMode)
{
    if (sliceMode == newMode)
        return;

    sliceMode = newMode;
    reslice();
}

void SiampleProcessor::setSensitivity (float newSensitivity)
{
    sensitivity = juce::jlimit (0.0f, 1.0f, newSensitivity);

    if (sliceMode == SliceMode::transients)
        reslice();
}

void SiampleProcessor::setDivisions (int newDivisions)
{
    const bool musical = newDivisions == grid::bars || newDivisions == grid::quarters
                      || newDivisions == grid::eighths || newDivisions == grid::sixteenths;

    divisions = musical ? newDivisions : juce::jlimit (1, maxSlices, newDivisions);

    if (sliceMode == SliceMode::grid)
        reslice();
}

//==============================================================================
void SiampleProcessor::setSampleBpm (double bpm)
{
    sampleBpm = juce::jlimit (minBpm, maxBpm, bpm);
    bpmManual = true;

    if (sliceMode == SliceMode::grid && divisions < 0)
        reslice();
    else
        scheduleRender();
}

void SiampleProcessor::detectBpm()
{
    bpmManual = false;
    analysisValid = false;
    analyseRegion();

    if (sliceMode == SliceMode::grid && divisions < 0)
        reslice();
    else
        scheduleRender();
}

void SiampleProcessor::setSyncEnabled (bool shouldSync)
{
    if (syncEnabled == shouldSync)
        return;

    syncEnabled = shouldSync;
    scheduleRender();
}

bool SiampleProcessor::isRendering() const
{
    return worker->isBusy();
}

void SiampleProcessor::waitForRendering (int timeoutMs)
{
    worker->waitUntilIdle (timeoutMs);
}

void SiampleProcessor::scheduleRender()
{
    const juce::ScopedLock schedule (scheduleLock);

    auto job = std::make_unique<RenderJob>();
    job->items.reserve ((size_t) maxSlices);
    const double target = juce::jlimit (minBpm, 999.0, hostBpm.load());

    {
        const juce::SpinLock::ScopedLockType lock (audioLock);

        syncActive   = syncEnabled && sample != nullptr;
        stretchRatio = syncActive ? juce::jlimit (0.25, 4.0, sampleBpm / target) : 1.0;

        job->sample  = sample;
        job->ratio   = stretchRatio;
        job->enabled = syncActive;

        if (syncActive)
            for (int i = 0; i < numSlices; ++i)
                job->items.push_back ({ i, slices[(size_t) i].start, getSliceEnd (i),
                                        slices[(size_t) i].params.pitch, slices[(size_t) i].params.fine });
    }

    renderedBpm = target;
    lastSeenHostBpm = target;
    worker->post (std::move (job));
}

void SiampleProcessor::collectGarbage()
{
    std::vector<RenderedSlice::Ptr> garbage;
    garbage.reserve (256);

    {
        const juce::SpinLock::ScopedLockType lock (audioLock);

        for (size_t i = retired.size(); i-- > 0;)
        {
            bool inUse = false;

            for (const auto& v : voices)
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

void SiampleProcessor::timerCallback()
{
    collectGarbage();

    // Le tempo du projet a changé : on recalcule quand il s'est stabilisé
    if (syncEnabled && sample != nullptr)
    {
        const double current = hostBpm.load();

        if (std::abs (current - renderedBpm) > 0.005)
        {
            if (std::abs (current - lastSeenHostBpm) < 0.005)
            {
                scheduleRender();
                sendChangeMessage();
            }

            lastSeenHostBpm = current;
        }
    }
}

//==============================================================================
void SiampleProcessor::setPlayMode (PlayMode newMode)
{
    const juce::SpinLock::ScopedLockType lock (audioLock);
    playMode = newMode;
}

void SiampleProcessor::setMono (bool shouldBeMono)
{
    const juce::SpinLock::ScopedLockType lock (audioLock);
    monoMode = shouldBeMono;
}

//==============================================================================
void SiampleProcessor::resetPattern()
{
    const juce::SpinLock::ScopedLockType lock (audioLock);

    for (int i = 0; i < maxSlices; ++i)
        patternSlices[(size_t) i] = i;

    patternFlipped = false;
}

void SiampleProcessor::flipPattern()
{
    const int count = numSlices;

    if (count < 2)
        return;

    auto& random = juce::Random::getSystemRandom();
    std::array<int, maxSlices> flipped {};

    for (int attempt = 0; attempt < 8; ++attempt)
    {
        // Chaque tranche est utilisée une fois, dans le désordre…
        for (int i = 0; i < count; ++i)
            flipped[(size_t) i] = i;

        for (int i = count - 1; i > 0; --i)
            std::swap (flipped[(size_t) i], flipped[(size_t) random.nextInt (i + 1)]);

        // …avec de temps en temps une répétition, façon « stutter »
        for (int i = 1; i < count; ++i)
            if (random.nextFloat() < 0.15f)
                flipped[(size_t) i] = flipped[(size_t) i - 1];

        bool identical = true;
        for (int i = 0; i < count; ++i)
            if (flipped[(size_t) i] != i)
                identical = false;

        if (! identical)
            break;
    }

    const juce::SpinLock::ScopedLockType lock (audioLock);

    for (int i = 0; i < maxSlices; ++i)
        patternSlices[(size_t) i] = i < count ? flipped[(size_t) i] : i;

    patternFlipped = true;
}

void SiampleProcessor::setPatternPlaying (bool shouldPlay)
{
    patternPlaying.store (shouldPlay && sample != nullptr && numSlices > 0);

    if (patternPlaying.load())
        sequencePlaying.store (false);
}

juce::File SiampleProcessor::createPatternMidiFile()
{
    if (sample == nullptr || numSlices == 0)
        return {};

    const int ticksPerBeat = 960;
    const int origin = slices[0].start;
    const double ticksPerSample = sampleBpm / 60.0 / sample->sampleRate * (double) ticksPerBeat;

    juce::MidiMessageSequence notes;

    for (int i = 0; i < numSlices; ++i)
    {
        const double on  = (double) (slices[(size_t) i].start - origin) * ticksPerSample;
        const double off = juce::jmax (on + 1.0, (double) (getSliceEnd (i) - origin) * ticksPerSample - 1.0);
        const int note = juce::jlimit (0, 127, baseNote + patternSlices[(size_t) i]);

        notes.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) 100), std::round (on));
        notes.addEvent (juce::MidiMessage::noteOff (1, note), std::round (off));
    }

    notes.sort();
    notes.updateMatchedPairs();

    juce::MidiFile midi;
    midi.setTicksPerQuarterNote (ticksPerBeat);
    midi.addTrack (notes);

    const auto name = "Siample - " + sample->file.getFileNameWithoutExtension()
                    + (patternFlipped ? " (flip)" : "") + ".mid";
    auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile (juce::File::createLegalFileName (name));

    file.deleteFile();

    if (auto stream = file.createOutputStream())
    {
        if (midi.writeTo (*stream))
        {
            stream->flush();
            return file;
        }
    }

    return {};
}

//==============================================================================
bool SiampleProcessor::insertSequenceStep (int sliceIndex, int position)
{
    if (sliceIndex < 0 || sliceIndex >= numSlices || sequenceLength >= maxSequenceSteps)
        return false;

    position = position < 0 ? sequenceLength : juce::jmin (position, sequenceLength);

    const juce::SpinLock::ScopedLockType lock (audioLock);

    for (int i = sequenceLength; i > position; --i)
        sequence[(size_t) i] = sequence[(size_t) i - 1];

    sequence[(size_t) position] = sliceIndex;
    ++sequenceLength;
    return true;
}

void SiampleProcessor::removeSequenceStep (int position)
{
    if (position < 0 || position >= sequenceLength)
        return;

    {
        const juce::SpinLock::ScopedLockType lock (audioLock);

        for (int i = position; i < sequenceLength - 1; ++i)
            sequence[(size_t) i] = sequence[(size_t) i + 1];

        --sequenceLength;
    }

    if (sequenceLength == 0)
        sequencePlaying.store (false);
}

void SiampleProcessor::moveSequenceStep (int from, int to)
{
    if (from < 0 || from >= sequenceLength)
        return;

    to = juce::jlimit (0, sequenceLength - 1, to);

    if (from == to)
        return;

    const juce::SpinLock::ScopedLockType lock (audioLock);
    const int moved = sequence[(size_t) from];

    if (from < to)
        for (int i = from; i < to; ++i)
            sequence[(size_t) i] = sequence[(size_t) i + 1];
    else
        for (int i = from; i > to; --i)
            sequence[(size_t) i] = sequence[(size_t) i - 1];

    sequence[(size_t) to] = moved;
}

void SiampleProcessor::clearSequence()
{
    {
        const juce::SpinLock::ScopedLockType lock (audioLock);
        sequenceLength = 0;
    }

    sequencePlaying.store (false);
}

void SiampleProcessor::setSequencePlaying (bool shouldPlay)
{
    sequencePlaying.store (shouldPlay && sample != nullptr && sequenceLength > 0);

    if (sequencePlaying.load())
        patternPlaying.store (false);
}

double SiampleProcessor::sliceSourceDuration (int sliceIndex) const
{
    if (sliceIndex < 0 || sliceIndex >= numSlices)
        return 0.0;

    const auto& s = slices[(size_t) sliceIndex];
    const double length = (double) (getSliceEnd (sliceIndex) - s.start);

    // Avec Sync, la durée suit le tempo du projet ; sinon c'est le pitch qui la change
    if (syncActive)
        return length * stretchRatio;

    return length / std::pow (2.0, ((double) s.params.pitch + (double) s.params.fine / 100.0) / 12.0);
}

double SiampleProcessor::getSliceSeconds (int sliceIndex) const
{
    return sample != nullptr ? sliceSourceDuration (sliceIndex) / sample->sampleRate : 0.0;
}

double SiampleProcessor::getSequenceSeconds() const
{
    double total = 0.0;

    for (int i = 0; i < sequenceLength; ++i)
        total += getSliceSeconds (sequence[(size_t) i]);

    return total;
}

//==============================================================================
bool SiampleProcessor::renderSliceAudio (int sliceIndex, juce::AudioBuffer<float>& result, bool withGrain)
{
    const SampleData::Ptr source = sample;

    if (source == nullptr || sliceIndex < 0 || sliceIndex >= numSlices)
        return false;

    if (syncEnabled)
        worker->waitUntilIdle (10000);

    const auto& s = slices[(size_t) sliceIndex];
    const int sliceStart = s.start;
    const int sliceEnd   = juce::jmin (getSliceEnd (sliceIndex), source->buffer.getNumSamples());

    if (sliceEnd - sliceStart < 4)
        return false;

    RenderedSlice::Ptr stretched;
    double ratio = 1.0;

    {
        const juce::SpinLock::ScopedLockType lock (audioLock);

        if (syncActive)
            stretched = rendered[(size_t) sliceIndex];

        ratio = stretchRatio;
    }

    // À l'export, la tranche est toujours jouée en entier, une seule fois
    SliceParams params = s.params;
    params.oneShot = true;
    params.loop = false;

    VoiceStart start;
    start.slice    = sliceIndex;
    start.params   = &params;
    start.velocity = 1.0f;
    start.hostRate = source->sampleRate;
    start.originalStart = sliceStart;
    start.originalEnd   = sliceEnd;

    if (stretched != nullptr && stretched->matches (sliceStart, getSliceEnd (sliceIndex), params.pitch, params.fine, ratio))
    {
        start.source = &stretched->buffer;
        start.start  = 0;
        start.end    = stretched->buffer.getNumSamples();
        start.rate   = 1.0;
    }
    else
    {
        start.source = &source->buffer;
        start.start  = sliceStart;
        start.end    = sliceEnd;
        start.rate   = std::pow (2.0, ((double) params.pitch + (double) params.fine / 100.0) / 12.0);
    }

    const int length = juce::jmax (1, (int) std::ceil ((double) (start.end - start.start) / start.rate));
    result.setSize (2, length);
    result.clear();

    Voice voice;
    voice.begin (start);
    voice.render (result.getWritePointer (0), result.getWritePointer (1), length);

    if (withGrain)
    {
        float phase = 0.0f, heldL = 0.0f, heldR = 0.0f;
        processGrain (result, grain->get() / 100.0f, source->sampleRate, phase, heldL, heldR);
    }

    return true;
}

bool SiampleProcessor::renderSequenceAudio (juce::AudioBuffer<float>& result)
{
    const SampleData::Ptr source = sample;
    const int count = sequenceLength;

    if (source == nullptr || count == 0)
        return false;

    if (syncEnabled)
        worker->waitUntilIdle (10000);

    std::array<juce::AudioBuffer<float>, maxSlices> parts;
    std::vector<int> offsets;
    double position = 0.0;

    for (int i = 0; i < count; ++i)
    {
        const int slice = sequence[(size_t) i];
        offsets.push_back ((int) std::llround (position));
        position += sliceSourceDuration (slice);

        if (slice >= 0 && slice < numSlices && parts[(size_t) slice].getNumSamples() == 0)
            renderSliceAudio (slice, parts[(size_t) slice], false);   // le grain est appliqué sur le tout, comme à l'écoute
    }

    const int total = juce::jmax (1, (int) std::llround (position));
    result.setSize (2, total);
    result.clear();

    for (int i = 0; i < count; ++i)
    {
        const int slice = sequence[(size_t) i];

        if (slice < 0 || slice >= numSlices)
            continue;

        const auto& part = parts[(size_t) slice];
        const int offset = offsets[(size_t) i];

        // Chaque tranche laisse la place à la suivante, comme à l'écoute
        const int next = i + 1 < count ? offsets[(size_t) i + 1] : total;
        const int length = juce::jmin (part.getNumSamples(), next - offset, total - offset);

        if (length <= 0)
            continue;

        const int fade = juce::jmin (32, length);

        for (int ch = 0; ch < 2; ++ch)
        {
            result.addFrom (ch, offset, part, ch, 0, length);

            if (length < part.getNumSamples())
                result.applyGainRamp (ch, offset + length - fade, fade, 1.0f, 0.0f);
        }
    }

    float phase = 0.0f, heldL = 0.0f, heldR = 0.0f;
    processGrain (result, grain->get() / 100.0f, source->sampleRate, phase, heldL, heldR);
    return true;
}

juce::File SiampleProcessor::writeTempWav (const juce::AudioBuffer<float>& buffer, const juce::String& suffix)
{
    if (sample == nullptr || buffer.getNumSamples() == 0)
        return {};

    const auto name = "Siample - " + sample->file.getFileNameWithoutExtension() + " - " + suffix + ".wav";
    auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile (juce::File::createLegalFileName (name));

    file.deleteFile();

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());

    if (stream == nullptr)
        return {};

    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (sample->sampleRate)
                                                                               .withNumChannels (buffer.getNumChannels())
                                                                               .withBitsPerSample (24));

    if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples()))
        return {};

    writer.reset();   // termine l'écriture du fichier
    return file;
}

juce::File SiampleProcessor::createSliceAudioFile (int sliceIndex)
{
    juce::AudioBuffer<float> audio;

    if (! renderSliceAudio (sliceIndex, audio))
        return {};

    return writeTempWav (audio, "tranche " + juce::String (sliceIndex + 1));
}

juce::File SiampleProcessor::createSequenceAudioFile()
{
    juce::AudioBuffer<float> audio;

    if (! renderSequenceAudio (audio))
        return {};

    return writeTempWav (audio, "piste");
}

juce::File SiampleProcessor::createSequenceMidiFile()
{
    if (sample == nullptr || sequenceLength == 0)
        return {};

    // Les notes sont placées pour le tempo du projet : chaque tranche laisse la place à la suivante
    const int ticksPerBeat = 960;
    const double ticksPerSample = hostBpm.load() / 60.0 / sample->sampleRate * (double) ticksPerBeat;

    juce::MidiMessageSequence notes;
    double position = 0.0;

    for (int i = 0; i < sequenceLength; ++i)
    {
        const int slice = sequence[(size_t) i];
        const double on = std::round (position * ticksPerSample);
        position += sliceSourceDuration (slice);
        const double off = juce::jmax (on + 1.0, std::round (position * ticksPerSample) - 1.0);
        const int note = juce::jlimit (0, 127, baseNote + slice);

        notes.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) 100), on);
        notes.addEvent (juce::MidiMessage::noteOff (1, note), off);
    }

    notes.sort();
    notes.updateMatchedPairs();

    juce::MidiFile midi;
    midi.setTicksPerQuarterNote (ticksPerBeat);
    midi.addTrack (notes);

    const auto name = "Siample - " + sample->file.getFileNameWithoutExtension() + " - piste.mid";
    auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile (juce::File::createLegalFileName (name));

    file.deleteFile();

    if (auto stream = file.createOutputStream())
    {
        if (midi.writeTo (*stream))
        {
            stream->flush();
            return file;
        }
    }

    return {};
}

//==============================================================================
void SiampleProcessor::audition (int sliceIndex, bool noteOn)
{
    const juce::SpinLock::ScopedLockType lock (uiLock);

    if (numUiEvents < (int) uiEvents.size())
        uiEvents[(size_t) numUiEvents++] = { sliceIndex, noteOn };
}

//==============================================================================
void SiampleProcessor::prepareToPlay (double sampleRate, int)
{
    const juce::SpinLock::ScopedLockType lock (audioLock);

    hostRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    lastMasterGain = juce::Decibels::decibelsToGain (masterGain->get());
    grainPhase = grainHeldL = grainHeldR = 0.0f;

    for (auto& v : voices)
        v.active = false;
}

void SiampleProcessor::releaseResources()
{
    killAllVoices();
}

void SiampleProcessor::killAllVoices()
{
    const juce::SpinLock::ScopedLockType lock (audioLock);

    for (auto& v : voices)
        v.active = false;

    activeMask.store (0, std::memory_order_relaxed);
    playhead.store (-1, std::memory_order_relaxed);
}

bool SiampleProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void SiampleProcessor::startVoice (int sliceIndex, int key, float velocity, double extraSemitones)
{
    if (sample == nullptr || sliceIndex < 0 || sliceIndex >= numSlices)
        return;

    const auto& s = slices[(size_t) sliceIndex];
    const int originalStart = s.start;
    const int originalEnd   = juce::jmin (getSliceEnd (sliceIndex), sample->buffer.getNumSamples());

    if (originalEnd - originalStart < 4)
        return;

    // Rejouer la même note coupe sa lecture précédente ; un groupe de choke coupe les autres membres
    // du groupe ; en mode mono, toute nouvelle note coupe tout le reste.
    for (auto& v : voices)
        if (v.active && (monoMode || v.key == key || (s.params.choke > 0 && v.choke == s.params.choke)))
            v.kill (hostRate);

    Voice* target = nullptr;

    for (auto& v : voices)
        if (! v.active)
        {
            target = &v;
            break;
        }

    if (target == nullptr)
    {
        // Plus de voix libre : on prend la plus ancienne
        target = &voices[0];

        for (auto& v : voices)
            if (v.age < target->age)
                target = &v;
    }

    VoiceStart start;
    start.slice    = sliceIndex;
    start.key      = key;
    start.params   = &s.params;
    start.velocity = velocity;
    start.hostRate = hostRate;
    start.age      = ++voiceCounter;
    start.originalStart = originalStart;
    start.originalEnd   = originalEnd;

    const double baseRate = sample->sampleRate / hostRate;
    const auto* stretched = syncActive ? rendered[(size_t) sliceIndex].get() : nullptr;

    if (stretched != nullptr && stretched->matches (s.start, getSliceEnd (sliceIndex), s.params.pitch, s.params.fine, stretchRatio))
    {
        // Tranche déjà étirée et transposée : on la lit telle quelle
        start.source = &stretched->buffer;
        start.owner  = stretched;
        start.start  = 0;
        start.end    = stretched->buffer.getNumSamples();
        start.rate   = baseRate * std::pow (2.0, extraSemitones / 12.0);
    }
    else
    {
        // Lecture classique : le pitch change la vitesse, donc la durée
        const double semitones = (double) s.params.pitch + (double) s.params.fine / 100.0 + extraSemitones;
        start.source = &sample->buffer;
        start.owner  = nullptr;
        start.start  = originalStart;
        start.end    = originalEnd;
        start.rate   = baseRate * std::pow (2.0, semitones / 12.0);
    }

    target->begin (start);
}

void SiampleProcessor::releaseKey (int key)
{
    for (auto& v : voices)
        if (v.active && v.key == key)
            v.noteOff();
}

void SiampleProcessor::handleEvent (const Event& event)
{
    switch (event.type)
    {
        case noteOnEvent:
            if (playMode == PlayMode::keys)
                startVoice (selectedSlice.load (std::memory_order_relaxed), event.value, event.velocity,
                            (double) (event.value - keyboardRootNote));
            else
                startVoice (event.value - baseNote, event.value, event.velocity, 0.0);
            break;

        case noteOffEvent:
            releaseKey (event.value);
            break;

        case allOffEvent:
            for (auto& v : voices)
                v.kill (hostRate);
            break;

        case patternEvent:
            startVoice (patternSlices[(size_t) juce::jlimit (0, maxSlices - 1, event.value)], patternKey, 0.9f, 0.0);
            break;

        case sequenceEvent:
            startVoice (event.value, sequenceKey, 1.0f, 0.0);
            break;

        default:
            break;
    }
}

void SiampleProcessor::renderVoices (juce::AudioBuffer<float>& buffer, int startSample, int numSamples)
{
    if (numSamples <= 0 || buffer.getNumChannels() == 0)
        return;

    float* left  = buffer.getWritePointer (0, startSample);
    float* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1, startSample) : left;

    for (auto& v : voices)
        if (v.active)
            v.render (left, right, numSamples);
}

void SiampleProcessor::applyGrain (juce::AudioBuffer<float>& buffer)
{
    processGrain (buffer, grain->get() / 100.0f, hostRate, grainPhase, grainHeldL, grainHeldR);
}

void SiampleProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();
    buffer.clear();

    // Tempo du projet
    if (auto* head = getPlayHead())
        if (const auto position = head->getPosition())
            if (const auto bpm = position->getBpm())
                if (*bpm >= minBpm && *bpm <= 999.0)
                    hostBpm.store (*bpm);

    // Export hors temps réel : on attend que les tranches étirées soient prêtes, pour un rendu exact
    if (isNonRealtime())
    {
        if (syncEnabled && sample != nullptr && std::abs (hostBpm.load() - renderedBpm) > 0.005)
            scheduleRender();

        if (worker->isBusy())
            worker->waitUntilIdle (15000);
    }

    {
        const juce::SpinLock::ScopedLockType lock (audioLock);

        std::array<Event, 512> events;
        int numEvents = 0;
        auto addEvent = [&] (int time, int type, int value, float velocity)
        {
            if (numEvents < (int) events.size())
                events[(size_t) numEvents++] = { juce::jlimit (0, juce::jmax (0, numSamples - 1), time), type, value, velocity };
        };

        // Écoutes demandées par l'interface
        {
            const juce::SpinLock::ScopedTryLockType uiTry (uiLock);

            if (uiTry.isLocked())
            {
                for (int i = 0; i < numUiEvents; ++i)
                {
                    const int key = uiKeyBase + uiEvents[(size_t) i].slice;

                    if (uiEvents[(size_t) i].on)
                        startVoice (uiEvents[(size_t) i].slice, key, 1.0f, 0.0);
                    else
                        releaseKey (key);
                }

                numUiEvents = 0;
            }
        }

        // Notes MIDI
        for (const auto metadata : midi)
        {
            const auto m = metadata.getMessage();

            if (m.isNoteOn())
                addEvent (metadata.samplePosition, noteOnEvent, m.getNoteNumber(), m.getFloatVelocity());
            else if (m.isNoteOff())
                addEvent (metadata.samplePosition, noteOffEvent, m.getNoteNumber(), 0.0f);
            else if (m.isAllNotesOff() || m.isAllSoundOff())
                addEvent (metadata.samplePosition, allOffEvent, 0, 0.0f);
        }

        // Lecture interne de la boucle
        const bool playing = patternPlaying.load() && sample != nullptr && numSlices > 0;

        if (playing)
        {
            const int origin = slices[0].start;
            const double loopLength = (double) (regionEnd - origin);
            const double advance = (sample->sampleRate / hostRate) / (syncActive ? stretchRatio : 1.0);

            if (! patternWasPlaying)
                patternPosition = 0.0;

            if (loopLength >= 1.0 && advance > 0.0)
            {
                const double from = patternPosition;
                const double to   = from + advance * (double) numSamples;

                for (int i = 0; i < numSlices; ++i)
                    for (double t = (double) (slices[(size_t) i].start - origin); t < to; t += loopLength)
                        if (t >= from)
                            addEvent ((int) ((t - from) / advance), patternEvent, i, 0.9f);

                patternPosition = std::fmod (to, loopLength);
            }
        }
        else if (patternWasPlaying)
        {
            for (auto& v : voices)
                if (v.active && v.key == patternKey)
                    v.kill (hostRate);
        }

        patternWasPlaying = playing;

        // Lecture de la piste : les tranches s'enchaînent bout à bout, puis on repart du début
        const bool sequencing = sequencePlaying.load() && sample != nullptr && sequenceLength > 0;

        if (sequencing)
        {
            if (! sequenceWasPlaying)
            {
                sequenceIndex = 0;
                sequenceCountdown = 0.0;
            }

            const double toHost = hostRate / sample->sampleRate;

            for (int guard = 0; sequenceCountdown < (double) numSamples && guard < 128; ++guard)
            {
                if (sequenceIndex >= sequenceLength)
                    sequenceIndex = 0;

                const int slice = sequence[(size_t) sequenceIndex];

                if (slice >= 0 && slice < numSlices)
                {
                    addEvent ((int) sequenceCountdown, sequenceEvent, slice, 1.0f);
                    sequencePlayingStep.store (sequenceIndex, std::memory_order_relaxed);
                }

                sequenceCountdown += juce::jmax (32.0, sliceSourceDuration (slice) * toHost);
                ++sequenceIndex;
            }

            sequenceCountdown -= (double) numSamples;
        }
        else if (sequenceWasPlaying)
        {
            for (auto& v : voices)
                if (v.active && v.key == sequenceKey)
                    v.kill (hostRate);

            sequencePlayingStep.store (-1, std::memory_order_relaxed);
        }

        sequenceWasPlaying = sequencing;

        // Les événements sont joués dans l'ordre du temps (à temps égal, dans l'ordre d'arrivée)
        for (int i = 1; i < numEvents; ++i)
        {
            const Event moved = events[(size_t) i];
            int j = i - 1;

            while (j >= 0 && events[(size_t) j].time > moved.time)
            {
                events[(size_t) j + 1] = events[(size_t) j];
                --j;
            }

            events[(size_t) j + 1] = moved;
        }

        int position = 0;

        for (int i = 0; i < numEvents; ++i)
        {
            const int time = juce::jlimit (position, numSamples, events[(size_t) i].time);
            renderVoices (buffer, position, time - position);
            position = time;
            handleEvent (events[(size_t) i]);
        }

        renderVoices (buffer, position, numSamples - position);

        juce::uint64 mask = 0;
        const Voice* newest = nullptr;

        for (const auto& v : voices)
        {
            if (! v.active)
                continue;

            if (v.slice >= 0 && v.slice < 64)
                mask |= (juce::uint64) 1 << v.slice;

            if (newest == nullptr || v.age > newest->age)
                newest = &v;
        }

        activeMask.store (mask, std::memory_order_relaxed);
        playhead.store (newest != nullptr ? newest->originalPosition() : -1, std::memory_order_relaxed);
    }

    applyGrain (buffer);

    const float gain = juce::Decibels::decibelsToGain (masterGain->get());
    buffer.applyGainRamp (0, numSamples, lastMasterGain, gain);
    lastMasterGain = gain;

    midi.clear();
}

//==============================================================================
juce::AudioProcessorEditor* SiampleProcessor::createEditor()
{
    return new SiampleEditor (*this);
}

//==============================================================================
void SiampleProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::ValueTree state ("Siample");
    state.setProperty ("version", 2, nullptr);
    state.setProperty ("file", sample != nullptr ? sample->file.getFullPathName() : missingFilePath, nullptr);
    state.setProperty ("regionStart", regionStart, nullptr);
    state.setProperty ("regionEnd", regionEnd, nullptr);
    state.setProperty ("mode", (int) sliceMode, nullptr);
    state.setProperty ("sensitivity", sensitivity, nullptr);
    state.setProperty ("divisions", divisions, nullptr);
    state.setProperty ("selected", selectedSlice.load(), nullptr);
    state.setProperty ("master", masterGain->get(), nullptr);
    state.setProperty ("grain", grain->get(), nullptr);
    state.setProperty ("bpm", sampleBpm, nullptr);
    state.setProperty ("bpmManual", bpmManual, nullptr);
    state.setProperty ("sync", syncEnabled, nullptr);
    state.setProperty ("hostBpm", hostBpm.load(), nullptr);
    state.setProperty ("mono", monoMode, nullptr);
    state.setProperty ("playMode", (int) playMode, nullptr);

    if (patternFlipped)
    {
        juce::StringArray order;
        for (int i = 0; i < numSlices; ++i)
            order.add (juce::String (patternSlices[(size_t) i]));

        state.setProperty ("pattern", order.joinIntoString (","), nullptr);
    }

    if (sequenceLength > 0)
    {
        juce::StringArray steps;
        for (int i = 0; i < sequenceLength; ++i)
            steps.add (juce::String (sequence[(size_t) i]));

        state.setProperty ("sequence", steps.joinIntoString (","), nullptr);
    }

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
        state.appendChild (child, nullptr);
    }

    // Si le fichier audio était introuvable au chargement, on réécrit l'état d'origine tel quel
    // pour ne pas perdre la découpe.
    if (sample == nullptr && orphanState.isValid())
        state = orphanState.createCopy();

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void SiampleProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);

    if (xml == nullptr)
        return;

    auto state = juce::ValueTree::fromXml (*xml);

    if (! state.hasType ("Siample"))
        return;

    sliceMode   = (SliceMode) juce::jlimit (0, 2, (int) state.getProperty ("mode", 0));
    sensitivity = juce::jlimit (0.0f, 1.0f, (float) state.getProperty ("sensitivity", 0.5f));
    *masterGain = (float) state.getProperty ("master", 0.0f);
    *grain      = (float) state.getProperty ("grain", 0.0f);

    {
        const int saved = (int) state.getProperty ("divisions", 16);
        const bool musical = saved == grid::bars || saved == grid::quarters || saved == grid::eighths || saved == grid::sixteenths;
        divisions = musical ? saved : juce::jlimit (1, maxSlices, saved);
    }

    const double savedHostBpm = (double) state.getProperty ("hostBpm", 120.0);
    if (savedHostBpm >= minBpm && savedHostBpm <= 999.0)
        hostBpm.store (savedHostBpm);

    const auto path = state.getProperty ("file").toString();
    juce::String error;
    SampleData::Ptr loaded = path.isNotEmpty() ? readFile (juce::File (path), error) : nullptr;

    installSample (loaded);
    orphanState = {};
    missingFilePath.clear();

    syncEnabled = (bool) state.getProperty ("sync", false);
    bpmManual   = (bool) state.getProperty ("bpmManual", false);
    sampleBpm   = juce::jlimit (minBpm, maxBpm, (double) state.getProperty ("bpm", 120.0));

    {
        const juce::SpinLock::ScopedLockType lock (audioLock);
        monoMode = (bool) state.getProperty ("mono", false);
        playMode = (PlayMode) juce::jlimit (0, 1, (int) state.getProperty ("playMode", 0));
    }

    if (loaded != nullptr)
    {
        const int length = loaded->buffer.getNumSamples();
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
            restored.push_back (s);
        }

        {
            const juce::SpinLock::ScopedLockType lock (audioLock);
            regionStart = rs;
            regionEnd   = re;

            for (size_t i = 0; i < slices.size(); ++i)
                slices[i] = i < restored.size() ? restored[i] : Slice();

            numSlices = (int) restored.size();
        }

        // Un projet de la version 0.1 n'a pas de tempo enregistré : on le détecte
        if (! state.hasProperty ("bpm"))
        {
            bpmManual = false;
            analyseRegion();
        }

        if (numSlices == 0)
            reslice();

        selectedSlice = juce::jlimit (0, juce::jmax (0, numSlices - 1), (int) state.getProperty ("selected", 0));

        // Réarrangement de la boucle
        juce::StringArray order;
        order.addTokens (state.getProperty ("pattern").toString(), ",", "");
        order.removeEmptyStrings();

        if (order.size() == numSlices && numSlices > 0)
        {
            const juce::SpinLock::ScopedLockType lock (audioLock);

            for (int i = 0; i < numSlices; ++i)
                patternSlices[(size_t) i] = juce::jlimit (0, numSlices - 1, order[i].getIntValue());

            patternFlipped = true;
        }

        // Piste
        juce::StringArray steps;
        steps.addTokens (state.getProperty ("sequence").toString(), ",", "");
        steps.removeEmptyStrings();

        {
            const juce::SpinLock::ScopedLockType lock (audioLock);
            sequenceLength = 0;

            for (const auto& step : steps)
            {
                const int slice = step.getIntValue();

                if (slice >= 0 && slice < numSlices && sequenceLength < maxSequenceSteps)
                    sequence[(size_t) sequenceLength++] = slice;
            }
        }

        scheduleRender();
    }
    else if (path.isNotEmpty())
    {
        missingFilePath = path;
        orphanState = state.createCopy();
    }

    sendChangeMessage();
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SiampleProcessor();
}
