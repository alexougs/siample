#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace siample;

namespace
{
    constexpr double maxFileMinutes = 15.0;
    constexpr double minBpm = 30.0;
    constexpr double minClipBeats = 1.0 / 16.0;
    constexpr int ticksPerBeat = 960;
    constexpr int stateVersion = 3;
    constexpr size_t maxUndoSteps = 60;

    enum EventType { noteOnEvent = 0, noteOffEvent, allOffEvent, clipEvent, repeatEvent };

    bool isDeckChannel (int channel)
    {
        return channel >= firstDeckChannel && channel < firstDeckChannel + numDecks;
    }

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

    bool writeMidiFile (const juce::MidiMessageSequence& notes, const juce::File& file)
    {
        juce::MidiFile midi;
        midi.setTicksPerQuarterNote (ticksPerBeat);
        midi.addTrack (notes);
        file.deleteFile();

        if (auto stream = file.createOutputStream())
        {
            if (midi.writeTo (*stream))
            {
                stream->flush();
                return true;
            }
        }

        return false;
    }
}

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

    for (int i = 0; i < numDecks; ++i)
        decks[(size_t) i] = std::make_unique<Deck> (*this, i);

    startTimerHz (10);
}

SiampleProcessor::~SiampleProcessor()
{
    stopTimer();

    for (auto& d : decks)
        d.reset();   // arrête les threads de fond avant de détruire le reste
}

void SiampleProcessor::setActiveDeck (int index)
{
    activeDeck.store (juce::jlimit (0, numDecks - 1, index));
}

bool SiampleProcessor::isRendering() const
{
    for (const auto& d : decks)
        if (d->isRendering())
            return true;

    return false;
}

void SiampleProcessor::waitForRendering (int timeoutMs)
{
    const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) juce::jmax (0, timeoutMs);

    for (auto& d : decks)
        d->waitForRendering (juce::jmax (0, (int) (deadline - juce::Time::getMillisecondCounter())));
}

void SiampleProcessor::setProjectBpm (double bpm)
{
    if (tempoFromHost.load())
        return;

    hostBpm.store (juce::jlimit (minBpm, 300.0, bpm));
}

void SiampleProcessor::setNoteRepeat (int division)
{
    if (division != 4 && division != 8 && division != 16 && division != 32)
        division = 0;

    noteRepeat.store (division);

    if (division == 0)
    {
        const juce::SpinLock::ScopedLockType lock (audioLock);

        for (auto& h : held)
            h.active = false;
    }
}

//==============================================================================
SiampleProcessor::SampleData::Ptr SiampleProcessor::readFile (const juce::File& file, juce::String& error, double maxSeconds)
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

    juce::int64 wanted = reader->lengthInSamples;

    if (maxSeconds > 0.0)
        wanted = juce::jmin (wanted, (juce::int64) (reader->sampleRate * maxSeconds));
    else if ((double) wanted > reader->sampleRate * 60.0 * maxFileMinutes)
    {
        error = fr ("Fichier trop long (15 minutes maximum)");
        return nullptr;
    }

    SampleData::Ptr data = new SampleData();
    const int channels = juce::jlimit (1, 2, (int) reader->numChannels);
    const int length   = (int) wanted;

    data->buffer.setSize (channels, length);
    data->buffer.clear();
    reader->read (&data->buffer, 0, length, 0, true, channels > 1);
    data->sampleRate = reader->sampleRate;
    data->file = file;
    return data;
}

//==============================================================================
// Montage
void SiampleProcessor::setVariant (int index)
{
    index = juce::jlimit (0, numVariants - 1, index);

    if (index == variant.load())
        return;

    beginEdit ("variant");
    variant.store (index);
}

void SiampleProcessor::copyVariant (int from, int to)
{
    if (from < 0 || from >= numVariants || to < 0 || to >= numVariants || from == to)
        return;

    beginEdit ("copyVariant");
    const juce::SpinLock::ScopedLockType lock (audioLock);
    clips[(size_t) to] = clips[(size_t) from];
    clipCounts[(size_t) to] = clipCounts[(size_t) from];
}

double SiampleProcessor::naturalBeats (int deckIndex, int sliceIndex) const
{
    return getDeck (deckIndex).getSliceSeconds (sliceIndex) * hostBpm.load() / 60.0;
}

double SiampleProcessor::snapBeats (double beats) const
{
    const double step = getSnapStep();
    return step > 0.0 ? std::round (beats / step) * step : beats;
}

int SiampleProcessor::addClip (int deckIndex, int sliceIndex, double startBeats, double lengthBeats)
{
    const int v = variant.load();
    const double total = getTimelineBeats();

    if (deckIndex < 0 || deckIndex >= numDecks || clipCounts[(size_t) v] >= maxClips)
        return -1;

    const auto& d = getDeck (deckIndex);

    if (! d.hasSample() || sliceIndex < 0 || sliceIndex >= d.getNumSlices())
        return -1;

    if (lengthBeats < 0.0)
    {
        lengthBeats = naturalBeats (deckIndex, sliceIndex);

        // Une durée presque ronde est arrondie à la grille : le montage tombe juste
        const double step = getSnapStep();

        if (step > 0.0)
        {
            const double snapped = std::round (lengthBeats / step) * step;

            if (snapped >= step && std::abs (snapped - lengthBeats) < step * 0.15)
                lengthBeats = snapped;
        }
    }

    Clip clip;
    clip.deck   = deckIndex;
    clip.slice  = sliceIndex;
    clip.start  = juce::jlimit (0.0, total - minClipBeats, startBeats);
    clip.length = juce::jlimit (minClipBeats, total - clip.start, lengthBeats);

    beginEdit ("addClip");
    const juce::SpinLock::ScopedLockType lock (audioLock);
    clips[(size_t) v][(size_t) clipCounts[(size_t) v]] = clip;
    return clipCounts[(size_t) v]++;
}

void SiampleProcessor::moveClip (int index, double newStartBeats)
{
    const int v = variant.load();

    if (index < 0 || index >= clipCounts[(size_t) v])
        return;

    auto& clip = clips[(size_t) v][(size_t) index];
    newStartBeats = juce::jlimit (0.0, juce::jmax (0.0, getTimelineBeats() - clip.length), newStartBeats);

    if (std::abs (newStartBeats - clip.start) < 1.0e-9)
        return;

    beginEdit ("moveClip" + juce::String (index), true);
    const juce::SpinLock::ScopedLockType lock (audioLock);
    clip.start = newStartBeats;
}

void SiampleProcessor::resizeClip (int index, double newLengthBeats)
{
    const int v = variant.load();

    if (index < 0 || index >= clipCounts[(size_t) v])
        return;

    auto& clip = clips[(size_t) v][(size_t) index];
    newLengthBeats = juce::jlimit (minClipBeats, juce::jmax (minClipBeats, getTimelineBeats() - clip.start), newLengthBeats);

    if (std::abs (newLengthBeats - clip.length) < 1.0e-9)
        return;

    beginEdit ("resizeClip" + juce::String (index), true);
    const juce::SpinLock::ScopedLockType lock (audioLock);
    clip.length = newLengthBeats;
}

void SiampleProcessor::removeClip (int index)
{
    const int v = variant.load();

    if (index < 0 || index >= clipCounts[(size_t) v])
        return;

    beginEdit ("removeClip");
    const juce::SpinLock::ScopedLockType lock (audioLock);

    for (int i = index; i < clipCounts[(size_t) v] - 1; ++i)
        clips[(size_t) v][(size_t) i] = clips[(size_t) v][(size_t) i + 1];

    --clipCounts[(size_t) v];
}

void SiampleProcessor::clearClips (int deckIndex)
{
    const int v = variant.load();

    if (clipCounts[(size_t) v] == 0)
        return;

    beginEdit ("clearClips");
    const juce::SpinLock::ScopedLockType lock (audioLock);
    int kept = 0;

    if (deckIndex >= 0)
        for (int i = 0; i < clipCounts[(size_t) v]; ++i)
            if (clips[(size_t) v][(size_t) i].deck != deckIndex)
                clips[(size_t) v][(size_t) kept++] = clips[(size_t) v][(size_t) i];

    clipCounts[(size_t) v] = kept;
}

void SiampleProcessor::fillLane (int deckIndex, bool shuffled)
{
    if (deckIndex < 0 || deckIndex >= numDecks)
        return;

    const auto& d = getDeck (deckIndex);
    const int count = d.getNumSlices();

    if (! d.hasSample() || count == 0)
        return;

    // Avec Sync, la boucle se mesure dans le tempo du sample ; sinon elle garde sa durée réelle
    const double tempo = d.isSyncEnabled() ? d.getSampleBpm() : hostBpm.load();
    const double beatsPerSample = tempo / 60.0 / d.getSourceSampleRate();
    const int origin = d.getSlice (0).start;
    const double loopBeats = (double) (d.getRegionEnd() - origin) * beatsPerSample;

    std::array<int, maxSlices> order {};

    for (int i = 0; i < count; ++i)
        order[(size_t) i] = i;

    if (shuffled && count > 1)
    {
        auto& random = juce::Random::getSystemRandom();

        for (int attempt = 0; attempt < 8; ++attempt)
        {
            // Chaque tranche est utilisée une fois, dans le désordre…
            for (int i = 0; i < count; ++i)
                order[(size_t) i] = i;

            for (int i = count - 1; i > 0; --i)
                std::swap (order[(size_t) i], order[(size_t) random.nextInt (i + 1)]);

            // …avec de temps en temps une répétition, façon « stutter »
            for (int i = 1; i < count; ++i)
                if (random.nextFloat() < 0.15f)
                    order[(size_t) i] = order[(size_t) i - 1];

            bool identical = true;
            for (int i = 0; i < count; ++i)
                if (order[(size_t) i] != i)
                    identical = false;

            if (! identical)
                break;
        }
    }

    beginEdit ("fillLane");

    // Le montage s'allonge si la boucle n'y tient pas
    int bars = timelineBars;

    while (bars < maxTimelineBars && (double) (bars * beatsPerBar) < loopBeats - 0.01)
        bars *= 2;

    const double total = (double) (bars * beatsPerBar);
    const int v = variant.load();
    const juce::SpinLock::ScopedLockType lock (audioLock);
    timelineBars = bars;

    int kept = 0;

    for (int i = 0; i < clipCounts[(size_t) v]; ++i)
        if (clips[(size_t) v][(size_t) i].deck != deckIndex)
            clips[(size_t) v][(size_t) kept++] = clips[(size_t) v][(size_t) i];

    for (int i = 0; i < count && kept < maxClips; ++i)
    {
        Clip clip;
        clip.deck   = deckIndex;
        clip.slice  = order[(size_t) i];
        clip.start  = std::round ((double) (d.getSlice (i).start - origin) * beatsPerSample * 960.0) / 960.0;
        clip.length = std::round ((double) (d.getSliceEnd (i) - d.getSlice (i).start) * beatsPerSample * 960.0) / 960.0;

        if (clip.start > total - minClipBeats)
            break;

        clip.length = juce::jlimit (minClipBeats, total - clip.start, clip.length);
        clips[(size_t) v][(size_t) kept++] = clip;
    }

    clipCounts[(size_t) v] = kept;
}

void SiampleProcessor::setTimelineBars (int bars)
{
    bars = juce::jlimit (1, maxTimelineBars, bars);

    if (bars == timelineBars)
        return;

    beginEdit ("bars");
    const juce::SpinLock::ScopedLockType lock (audioLock);
    timelineBars = bars;
    const double total = (double) (bars * beatsPerBar);

    // Les blocs qui dépassent sont raccourcis ; ceux qui commencent après la fin restent en réserve
    for (auto v = 0; v < numVariants; ++v)
        for (int i = 0; i < clipCounts[(size_t) v]; ++i)
        {
            auto& clip = clips[(size_t) v][(size_t) i];

            if (clip.start < total - minClipBeats)
                clip.length = juce::jmin (clip.length, total - clip.start);
        }
}

void SiampleProcessor::setSnap (int division)
{
    snapDivision = (division == 4 || division == 8 || division == 16 || division == 32) ? division : 0;
}

void SiampleProcessor::setTimelinePlaying (bool shouldPlay)
{
    timelinePlaying.store (shouldPlay);
}

void SiampleProcessor::deckSlicesChanged (int deckIndex)
{
    const int count = getDeck (deckIndex).getNumSlices();
    const juce::SpinLock::ScopedLockType lock (audioLock);

    for (int v = 0; v < numVariants; ++v)
    {
        int kept = 0;

        for (int i = 0; i < clipCounts[(size_t) v]; ++i)
        {
            const auto& clip = clips[(size_t) v][(size_t) i];

            if (clip.deck != deckIndex || clip.slice < count)
                clips[(size_t) v][(size_t) kept++] = clip;
        }

        clipCounts[(size_t) v] = kept;
    }
}

void SiampleProcessor::deckSampleReplaced (int deckIndex)
{
    const juce::SpinLock::ScopedLockType lock (audioLock);

    for (int v = 0; v < numVariants; ++v)
    {
        int kept = 0;

        for (int i = 0; i < clipCounts[(size_t) v]; ++i)
            if (clips[(size_t) v][(size_t) i].deck != deckIndex)
                clips[(size_t) v][(size_t) kept++] = clips[(size_t) v][(size_t) i];

        clipCounts[(size_t) v] = kept;
    }

    for (auto& h : held)
        if (h.deck == deckIndex)
            h.active = false;
}

//==============================================================================
// Export
juce::File SiampleProcessor::tempFile (const juce::String& name) const
{
    return juce::File::getSpecialLocation (juce::File::tempDirectory)
               .getChildFile (juce::File::createLegalFileName (name));
}

juce::File SiampleProcessor::writeTempWav (const juce::AudioBuffer<float>& buffer, double sampleRate, const juce::File& file)
{
    if (buffer.getNumSamples() == 0)
        return {};

    file.getParentDirectory().createDirectory();
    file.deleteFile();

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());

    if (stream == nullptr)
        return {};

    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (sampleRate)
                                                                               .withNumChannels (buffer.getNumChannels())
                                                                               .withBitsPerSample (24));

    if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples()))
        return {};

    writer.reset();   // termine l'écriture du fichier
    return file;
}

bool SiampleProcessor::prepareVoice (int deckIndex, int sliceIndex, const RenderedSlice* stretched, double outputRate,
                                     double extraSemitones, VoiceStart& start) const
{
    const auto& d = *decks[(size_t) deckIndex];

    if (d.sample == nullptr || sliceIndex < 0 || sliceIndex >= d.numSlices)
        return false;

    const auto& s = d.slices[(size_t) sliceIndex];
    const int originalStart = s.start;
    const int originalEnd   = juce::jmin (d.getSliceEnd (sliceIndex), d.sample->buffer.getNumSamples());

    if (originalEnd - originalStart < 4)
        return false;

    start.deck     = deckIndex;
    start.slice    = sliceIndex;
    start.params   = &s.params;
    start.hostRate = outputRate;
    start.originalStart = originalStart;
    start.originalEnd   = originalEnd;

    const double baseRate = d.sample->sampleRate / outputRate;
    const int pitch = d.effectivePitch (sliceIndex);

    if (stretched != nullptr && stretched->matches (s.start, d.getSliceEnd (sliceIndex), pitch, s.params.fine, d.stretchRatio))
    {
        // Tranche déjà étirée et transposée : on la lit telle quelle
        start.source = &stretched->buffer;
        start.owner  = stretched;
        start.start  = stretched->lead;
        start.end    = stretched->buffer.getNumSamples();
        start.lead   = stretched->lead;
        start.rate   = baseRate * std::pow (2.0, extraSemitones / 12.0);
    }
    else
    {
        // Lecture classique : le pitch change la vitesse, donc la durée
        const double semitones = (double) pitch + (double) s.params.fine / 100.0 + extraSemitones;
        start.source = &d.sample->buffer;
        start.owner  = nullptr;
        start.start  = originalStart;
        start.end    = originalEnd;
        start.lead   = originalStart;
        start.rate   = baseRate * std::pow (2.0, semitones / 12.0);
    }

    return true;
}

bool SiampleProcessor::renderSliceAudio (int deckIndex, int sliceIndex, juce::AudioBuffer<float>& result, bool withGrain)
{
    if (deckIndex < 0 || deckIndex >= numDecks)
        return false;

    auto& d = getDeck (deckIndex);
    const SampleData::Ptr source = d.sample;

    if (source == nullptr || sliceIndex < 0 || sliceIndex >= d.numSlices)
        return false;

    if (d.syncEnabled)
        d.waitForRendering (10000);

    RenderedSlice::Ptr stretched;

    {
        const juce::SpinLock::ScopedLockType lock (audioLock);

        if (d.syncActive)
            stretched = d.rendered[(size_t) sliceIndex];
    }

    VoiceStart start;

    if (! prepareVoice (deckIndex, sliceIndex, stretched.get(), source->sampleRate, 0.0, start))
        return false;

    // À l'export, la tranche est toujours jouée en entier, une seule fois
    SliceParams params = d.slices[(size_t) sliceIndex].params;
    params.oneShot = true;
    params.loop = false;
    start.params = &params;

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

bool SiampleProcessor::renderTimelineAudio (juce::AudioBuffer<float>& result, double& sampleRate)
{
    const int v = variant.load();
    const double total = getTimelineBeats();
    std::vector<Clip> list;

    for (int i = 0; i < clipCounts[(size_t) v]; ++i)
        if (clips[(size_t) v][(size_t) i].start < total - minClipBeats * 0.5)
            list.push_back (clips[(size_t) v][(size_t) i]);

    if (list.empty())
        return false;

    for (auto& d : decks)
        if (d->syncEnabled)
            d->waitForRendering (10000);

    // Les tranches étirées utilisées sont gardées en vie pendant tout le rendu
    std::array<std::array<RenderedSlice::Ptr, maxSlices>, numDecks> stretched;

    {
        const juce::SpinLock::ScopedLockType lock (audioLock);

        for (int d = 0; d < numDecks; ++d)
            if (decks[(size_t) d]->syncActive)
                stretched[(size_t) d] = decks[(size_t) d]->rendered;
    }

    sampleRate = hostRate;
    const double samplesPerBeat = 60.0 / hostBpm.load() * sampleRate;
    const int length = juce::jmax (1, (int) std::llround (total * samplesPerBeat));

    result.setSize (2, length);
    result.clear();

    for (size_t i = 0; i < list.size(); ++i)
    {
        const auto& clip = list[i];

        // Sur une même ligne, un bloc laisse la place au suivant
        double beats = juce::jmin (clip.length, total - clip.start);

        for (const auto& other : list)
            if (other.deck == clip.deck && other.start > clip.start + 1.0e-9)
                beats = juce::jmin (beats, other.start - clip.start);

        VoiceStart start;

        if (! prepareVoice (clip.deck, clip.slice, stretched[(size_t) clip.deck][(size_t) juce::jlimit (0, maxSlices - 1, clip.slice)].get(),
                            sampleRate, 0.0, start))
            continue;

        start.velocity = 1.0f;
        start.life = beats * samplesPerBeat;

        const int offset = juce::jlimit (0, length - 1, (int) std::llround (clip.start * samplesPerBeat));

        Voice voice;
        voice.begin (start);
        voice.render (result.getWritePointer (0, offset), result.getWritePointer (1, offset), length - offset);
    }

    // Pas de clic si un son est encore en cours à la toute fin
    const int fade = juce::jmin (length, (int) (0.004 * sampleRate));
    result.applyGainRamp (length - fade, fade, 1.0f, 0.0f);

    float phase = 0.0f, heldL = 0.0f, heldR = 0.0f;
    processGrain (result, grain->get() / 100.0f, sampleRate, phase, heldL, heldR);
    return true;
}

juce::File SiampleProcessor::createSliceAudioFile (int sliceIndex)
{
    juce::AudioBuffer<float> audio;
    const auto& d = deck();

    if (! renderSliceAudio (d.getIndex(), sliceIndex, audio))
        return {};

    return writeTempWav (audio, d.getSourceSampleRate(),
                         tempFile ("Siample - " + d.getSample()->file.getFileNameWithoutExtension()
                                   + " - tranche " + juce::String (sliceIndex + 1) + ".wav"));
}

juce::Array<juce::File> SiampleProcessor::createAllSliceAudioFiles()
{
    juce::Array<juce::File> files;
    const auto& d = deck();

    if (! d.hasSample())
        return files;

    const auto name = d.getSample()->file.getFileNameWithoutExtension();
    const auto folder = tempFile ("Siample - " + name + " - tranches");
    folder.deleteRecursively();

    for (int i = 0; i < d.getNumSlices(); ++i)
    {
        juce::AudioBuffer<float> audio;

        if (! renderSliceAudio (d.getIndex(), i, audio))
            continue;

        const auto file = writeTempWav (audio, d.getSourceSampleRate(),
                                        folder.getChildFile (juce::File::createLegalFileName (
                                            name + " - " + juce::String (i + 1).paddedLeft ('0', 2) + ".wav")));

        if (file.existsAsFile())
            files.add (file);
    }

    return files;
}

juce::File SiampleProcessor::createTimelineAudioFile()
{
    juce::AudioBuffer<float> audio;
    double rate = 44100.0;

    if (! renderTimelineAudio (audio, rate))
        return {};

    return writeTempWav (audio, rate, tempFile ("Siample - montage " + juce::String::charToString ((juce::juce_wchar) ('A' + variant.load()))
                                                + " - " + juce::String (juce::roundToInt (hostBpm.load())) + " BPM.wav"));
}

juce::File SiampleProcessor::createTimelineMidiFile()
{
    const int v = variant.load();
    const double total = getTimelineBeats();
    juce::MidiMessageSequence notes;

    for (int i = 0; i < clipCounts[(size_t) v]; ++i)
    {
        const auto& clip = clips[(size_t) v][(size_t) i];

        if (clip.start >= total - minClipBeats * 0.5)
            continue;

        // Un canal MIDI par sample : 2 = A, 3 = B, 4 = C, 5 = D
        const int channel = firstDeckChannel + clip.deck;
        const int note = juce::jlimit (0, 127, baseNote + clip.slice);
        const double on  = std::round (clip.start * ticksPerBeat);
        const double off = juce::jmax (on + 1.0, std::round (juce::jmin (total, clip.start + clip.length) * ticksPerBeat) - 1.0);

        notes.addEvent (juce::MidiMessage::noteOn (channel, note, (juce::uint8) 100), on);
        notes.addEvent (juce::MidiMessage::noteOff (channel, note), off);
    }

    if (notes.getNumEvents() == 0)
        return {};

    notes.sort();
    notes.updateMatchedPairs();

    const auto file = tempFile ("Siample - montage " + juce::String::charToString ((juce::juce_wchar) ('A' + v)) + ".mid");
    return writeMidiFile (notes, file) ? file : juce::File();
}

//==============================================================================
// Annuler / rétablir
void SiampleProcessor::beginEdit (const juce::String& tag, bool continuous)
{
    if (editSuspended > 0)
        return;

    const auto now = juce::Time::getMillisecondCounter();

    if (continuous && tag == lastEditTag && now - lastEditTime < 1200)
    {
        lastEditTime = now;
        return;
    }

    lastEditTag = tag;
    lastEditTime = now;

    undoStack.push_back (createState());

    if (undoStack.size() > maxUndoSteps)
        undoStack.erase (undoStack.begin());

    redoStack.clear();
}

void SiampleProcessor::undo()
{
    if (undoStack.empty())
        return;

    redoStack.push_back (createState());
    const auto state = undoStack.back();
    undoStack.pop_back();

    ++editSuspended;
    applyState (state, true);
    --editSuspended;
    lastEditTag.clear();
    sendChangeMessage();
}

void SiampleProcessor::redo()
{
    if (redoStack.empty())
        return;

    undoStack.push_back (createState());
    const auto state = redoStack.back();
    redoStack.pop_back();

    ++editSuspended;
    applyState (state, true);
    --editSuspended;
    lastEditTag.clear();
    sendChangeMessage();
}

//==============================================================================
// Presets
bool SiampleProcessor::savePreset (const juce::File& file)
{
    if (auto xml = createState().createXml())
        return xml->writeTo (file);

    return false;
}

juce::String SiampleProcessor::loadPreset (const juce::File& file)
{
    auto xml = juce::XmlDocument::parse (file);

    if (xml == nullptr)
        return fr ("Ce fichier n'est pas un preset Siample");

    const auto state = juce::ValueTree::fromXml (*xml);

    if (! state.hasType ("Siample"))
        return fr ("Ce fichier n'est pas un preset Siample");

    beginEdit ("preset");
    ++editSuspended;
    applyState (state, false);
    --editSuspended;
    lastEditTag.clear();
    sendChangeMessage();

    juce::StringArray missing;

    for (const auto& d : decks)
        if (d->getMissingFilePath().isNotEmpty())
            missing.add (juce::File (d->getMissingFilePath()).getFileName());

    return missing.isEmpty() ? juce::String()
                             : fr ("Preset chargé, mais fichier audio introuvable : ") + missing.joinIntoString (", ");
}

//==============================================================================
// Pré-écoute
juce::String SiampleProcessor::startPreview (const juce::File& file)
{
    juce::String error;
    auto data = readFile (file, error, 45.0);

    if (data == nullptr)
        return error;

    {
        const juce::SpinLock::ScopedLockType lock (audioLock);
        std::swap (preview, data);
        previewPosition = 0.0;
        previewPlaying.store (true);
    }

    return {};   // l'ancienne pré-écoute (dans `data`) est libérée ici, hors du verrou
}

void SiampleProcessor::stopPreview()
{
    previewPlaying.store (false);
}

void SiampleProcessor::renderPreview (juce::AudioBuffer<float>& buffer)
{
    if (preview == nullptr || ! previewPlaying.load (std::memory_order_relaxed) || buffer.getNumChannels() == 0)
        return;

    const auto& source = preview->buffer;
    const int last = source.getNumSamples() - 1;
    const float* in0 = source.getReadPointer (0);
    const float* in1 = source.getNumChannels() > 1 ? source.getReadPointer (1) : in0;
    float* left  = buffer.getWritePointer (0);
    float* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : left;
    const double rate = preview->sampleRate / hostRate;
    const double fade = juce::jmax (1.0, 0.005 * preview->sampleRate);

    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        if (previewPosition >= (double) last)
        {
            previewPlaying.store (false, std::memory_order_relaxed);
            break;
        }

        const int a = (int) previewPosition;
        const float f = (float) (previewPosition - (double) a);
        const float gain = 0.7f * (float) juce::jmin (1.0, previewPosition / fade, ((double) last - previewPosition) / fade);

        left[i]  += gain * (in0[a] + f * (in0[a + 1] - in0[a]));
        right[i] += gain * (in1[a] + f * (in1[a + 1] - in1[a]));
        previewPosition += rate;
    }
}

//==============================================================================
void SiampleProcessor::audition (int deckIndex, int sliceIndex, bool noteOn)
{
    const juce::SpinLock::ScopedLockType lock (uiLock);

    if (numUiEvents < (int) uiEvents.size())
        uiEvents[(size_t) numUiEvents++] = { deckIndex, sliceIndex, noteOn };
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

    for (auto& h : held)
        h.active = false;
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

    for (auto& h : held)
        h.active = false;

    for (auto& d : decks)
    {
        d->activeMask.store (0, std::memory_order_relaxed);
        d->playhead.store (-1, std::memory_order_relaxed);
    }
}

void SiampleProcessor::killDeckVoices (int deckIndex)
{
    for (auto& v : voices)
        if (v.deck == deckIndex)
            v.active = false;
}

bool SiampleProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void SiampleProcessor::startVoice (int deckIndex, int sliceIndex, int key, float velocity, double extraSemitones,
                                   int lane, double life)
{
    if (deckIndex < 0 || deckIndex >= numDecks)
        return;

    const auto& d = *decks[(size_t) deckIndex];
    VoiceStart start;
    const auto* stretched = d.syncActive && sliceIndex >= 0 && sliceIndex < maxSlices ? d.rendered[(size_t) sliceIndex].get() : nullptr;

    if (! prepareVoice (deckIndex, sliceIndex, stretched, hostRate, extraSemitones, start))
        return;

    const int choke = start.params->choke;

    // Rejouer la même note coupe sa lecture précédente ; un groupe de choke coupe les autres membres du groupe ;
    // en mode mono, toute nouvelle note coupe le reste du sample ; sur le montage, une ligne ne joue qu'un bloc à la fois.
    for (auto& v : voices)
        if (v.active && (v.key == key
                         || (lane >= 0 && v.lane == lane)
                         || (v.deck == deckIndex && (d.monoMode || (choke > 0 && v.choke == choke)))))
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

    start.key      = key;
    start.velocity = velocity;
    start.age      = ++voiceCounter;
    start.lane     = lane;
    start.life     = life;
    target->begin (start);
}

void SiampleProcessor::releaseKey (int key, bool force)
{
    for (auto& v : voices)
        if (v.active && v.key == key)
        {
            if (force)
                v.release();
            else
                v.noteOff();
        }
}

void SiampleProcessor::holdNote (int key, int deckIndex, int sliceIndex, float velocity, int time)
{
    const int division = noteRepeat.load (std::memory_order_relaxed);

    if (division <= 0)
        return;

    const double interval = 60.0 / hostBpm.load() * 4.0 / (double) division * hostRate;
    Held* slot = nullptr;

    for (auto& h : held)
        if (h.active && h.key == key)
            slot = &h;

    if (slot == nullptr)
        for (auto& h : held)
            if (! h.active)
            {
                slot = &h;
                break;
            }

    // L'intervalle se compte à partir de la note ; le décompte, lui, repart au début du bloc audio suivant
    if (slot != nullptr)
        *slot = { true, key, deckIndex, sliceIndex, velocity, interval + (double) (time - currentBlockSize) };
}

void SiampleProcessor::unholdNote (int key)
{
    for (auto& h : held)
        if (h.key == key)
            h.active = false;
}

void SiampleProcessor::handleEvent (const Event& event)
{
    switch (event.type)
    {
        case noteOnEvent:
        {
            const int key = event.channel * 128 + event.value;
            const float velocity = fullVelocity.load (std::memory_order_relaxed) ? 1.0f : event.velocity;

            if (isDeckChannel (event.channel))
            {
                // Canaux « montage » : un canal par sample, une tranche à la fois
                const int d = event.channel - firstDeckChannel;
                startVoice (d, event.value - baseNote, key, velocity, 0.0, d);
                break;
            }

            const int d = activeDeck.load (std::memory_order_relaxed);
            const auto& current = *decks[(size_t) d];

            if (current.playMode == PlayMode::keys)
            {
                startVoice (d, current.selectedSlice.load (std::memory_order_relaxed), key, velocity,
                            (double) (event.value - keyboardRootNote));
            }
            else
            {
                startVoice (d, event.value - baseNote, key, velocity, 0.0);
                holdNote (key, d, event.value - baseNote, velocity, event.time);
            }
            break;
        }

        case noteOffEvent:
        {
            const int key = event.channel * 128 + event.value;
            releaseKey (key, isDeckChannel (event.channel));
            unholdNote (key);
            break;
        }

        case allOffEvent:
            for (auto& v : voices)
                v.kill (hostRate);

            for (auto& h : held)
                h.active = false;
            break;

        case clipEvent:
        {
            const int v = variant.load (std::memory_order_relaxed);

            if (event.value >= 0 && event.value < clipCounts[(size_t) v])
            {
                const auto& clip = clips[(size_t) v][(size_t) event.value];
                const double life = clip.length * 60.0 / hostBpm.load() * hostRate;
                startVoice (clip.deck, clip.slice, timelineKey + clip.deck, 1.0f, 0.0, clip.deck, life);
            }
            break;
        }

        case repeatEvent:
        {
            const auto& h = held[(size_t) juce::jlimit (0, (int) held.size() - 1, event.value)];

            if (h.active)
                startVoice (h.deck, h.slice, h.key, h.velocity, 0.0);
            break;
        }

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

void SiampleProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();
    currentBlockSize = numSamples;
    buffer.clear();

    // Tempo du projet
    if (auto* head = getPlayHead())
        if (const auto position = head->getPosition())
            if (const auto bpm = position->getBpm())
                if (*bpm >= minBpm && *bpm <= 999.0)
                {
                    hostBpm.store (*bpm);
                    tempoFromHost.store (true);
                }

    // Export hors temps réel : on attend que les tranches étirées soient prêtes, pour un rendu exact
    if (isNonRealtime())
    {
        for (auto& d : decks)
        {
            if (d->syncEnabled && d->sample != nullptr && std::abs (hostBpm.load() - d->renderedBpm) > 0.005)
                d->scheduleRender();

            if (d->isRendering())
                d->waitForRendering (15000);
        }
    }

    {
        const juce::SpinLock::ScopedLockType lock (audioLock);

        std::array<Event, 512> events;
        int numEvents = 0;
        auto addEvent = [&] (int time, int type, int value, float velocity, int channel)
        {
            if (numEvents < (int) events.size())
                events[(size_t) numEvents++] = { juce::jlimit (0, juce::jmax (0, numSamples - 1), time), type, value, velocity, channel };
        };

        // Répétition des notes tenues
        const int division = noteRepeat.load (std::memory_order_relaxed);

        if (division > 0)
        {
            const double interval = juce::jmax (64.0, 60.0 / hostBpm.load() * 4.0 / (double) division * hostRate);

            for (size_t i = 0; i < held.size(); ++i)
            {
                auto& h = held[i];

                if (! h.active)
                    continue;

                for (int guard = 0; h.countdown < (double) numSamples && guard < 64; ++guard)
                {
                    addEvent ((int) h.countdown, repeatEvent, (int) i, h.velocity, 0);
                    h.countdown += interval;
                }

                h.countdown -= (double) numSamples;
            }
        }

        // Écoutes demandées par l'interface
        {
            const juce::SpinLock::ScopedTryLockType uiTry (uiLock);

            if (uiTry.isLocked())
            {
                for (int i = 0; i < numUiEvents; ++i)
                {
                    const auto& e = uiEvents[(size_t) i];
                    const int key = uiKeyBase + e.deck * 100 + e.slice;

                    if (e.on)
                    {
                        startVoice (e.deck, e.slice, key, 1.0f, 0.0);
                        holdNote (key, e.deck, e.slice, 1.0f, 0);
                    }
                    else
                    {
                        releaseKey (key, false);
                        unholdNote (key);
                    }
                }

                numUiEvents = 0;
            }
        }

        // Notes MIDI
        for (const auto metadata : midi)
        {
            const auto m = metadata.getMessage();

            if (m.isNoteOn())
                addEvent (metadata.samplePosition, noteOnEvent, m.getNoteNumber(), m.getFloatVelocity(), m.getChannel());
            else if (m.isNoteOff())
                addEvent (metadata.samplePosition, noteOffEvent, m.getNoteNumber(), 0.0f, m.getChannel());
            else if (m.isAllNotesOff() || m.isAllSoundOff())
                addEvent (metadata.samplePosition, allOffEvent, 0, 0.0f, 0);
        }

        // Lecture du montage, en boucle, au tempo du projet
        const bool playing = timelinePlaying.load();

        if (playing)
        {
            const double loopBeats = (double) (timelineBars * beatsPerBar);
            const double beatsPerSample = hostBpm.load() / 60.0 / hostRate;

            if (! timelineWasPlaying || timelinePosition >= loopBeats)
                timelinePosition = 0.0;

            const double from = timelinePosition;
            const double to   = from + beatsPerSample * (double) numSamples;
            const int v = variant.load (std::memory_order_relaxed);

            for (int i = 0; i < clipCounts[(size_t) v]; ++i)
            {
                const double start = clips[(size_t) v][(size_t) i].start;

                if (start >= loopBeats - minClipBeats * 0.5)
                    continue;

                for (double t = start; t < to; t += loopBeats)
                    if (t >= from)
                        addEvent ((int) ((t - from) / beatsPerSample), clipEvent, i, 1.0f, 0);
            }

            timelinePosition = std::fmod (to, loopBeats);
            timelinePositionUi.store (timelinePosition, std::memory_order_relaxed);
        }
        else if (timelineWasPlaying)
        {
            for (auto& v : voices)
                if (v.active && v.key >= timelineKey && v.key < timelineKey + numDecks)
                    v.kill (hostRate);

            timelinePositionUi.store (-1.0, std::memory_order_relaxed);
        }

        timelineWasPlaying = playing;

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

        // Ce que l'interface affiche : pads allumés et tête de lecture, par sample
        std::array<juce::uint64, numDecks> masks {};
        std::array<const Voice*, numDecks> newest {};

        for (const auto& v : voices)
        {
            if (! v.active || v.deck < 0 || v.deck >= numDecks)
                continue;

            if (v.slice >= 0 && v.slice < 64)
                masks[(size_t) v.deck] |= (juce::uint64) 1 << v.slice;

            if (newest[(size_t) v.deck] == nullptr || v.age > newest[(size_t) v.deck]->age)
                newest[(size_t) v.deck] = &v;
        }

        for (int d = 0; d < numDecks; ++d)
        {
            decks[(size_t) d]->activeMask.store (masks[(size_t) d], std::memory_order_relaxed);
            decks[(size_t) d]->playhead.store (newest[(size_t) d] != nullptr ? newest[(size_t) d]->originalPosition() : -1,
                                               std::memory_order_relaxed);
        }

        processGrain (buffer, grain->get() / 100.0f, hostRate, grainPhase, grainHeldL, grainHeldR);
        renderPreview (buffer);
    }

    const float gain = juce::Decibels::decibelsToGain (masterGain->get());
    buffer.applyGainRamp (0, numSamples, lastMasterGain, gain);
    lastMasterGain = gain;

    midi.clear();
}

void SiampleProcessor::timerCallback()
{
    for (auto& d : decks)
        d->hostTempoTick();
}

//==============================================================================
juce::AudioProcessorEditor* SiampleProcessor::createEditor()
{
    return new SiampleEditor (*this);
}

//==============================================================================
juce::ValueTree SiampleProcessor::createState() const
{
    juce::ValueTree state ("Siample");
    state.setProperty ("version", stateVersion, nullptr);
    state.setProperty ("master", masterGain->get(), nullptr);
    state.setProperty ("grain", grain->get(), nullptr);
    state.setProperty ("hostBpm", hostBpm.load(), nullptr);
    state.setProperty ("activeDeck", activeDeck.load(), nullptr);
    state.setProperty ("fullVelocity", fullVelocity.load(), nullptr);
    state.setProperty ("noteRepeat", noteRepeat.load(), nullptr);
    state.setProperty ("bars", timelineBars, nullptr);
    state.setProperty ("snap", snapDivision, nullptr);
    state.setProperty ("variant", variant.load(), nullptr);
    state.setProperty ("view", shownView, nullptr);

    for (const auto& d : decks)
        state.appendChild (d->toTree(), nullptr);

    for (int v = 0; v < numVariants; ++v)
    {
        juce::ValueTree arrangement ("Arrangement");
        arrangement.setProperty ("index", v, nullptr);

        for (int i = 0; i < clipCounts[(size_t) v]; ++i)
        {
            const auto& clip = clips[(size_t) v][(size_t) i];
            juce::ValueTree child ("Clip");
            child.setProperty ("deck", clip.deck, nullptr);
            child.setProperty ("slice", clip.slice, nullptr);
            child.setProperty ("start", clip.start, nullptr);
            child.setProperty ("length", clip.length, nullptr);
            arrangement.appendChild (child, nullptr);
        }

        state.appendChild (arrangement, nullptr);
    }

    return state;
}

void SiampleProcessor::applyState (const juce::ValueTree& state, bool reuseLoadedSamples)
{
    const int version = (int) state.getProperty ("version", 1);
    const bool legacy = version < 3;   // jusqu'à la 0.4 : un seul sample, décrit à la racine

    // Annuler ou rétablir n'interrompt pas l'écoute ; ouvrir un projet ou un preset, si
    if (! reuseLoadedSamples)
    {
        timelinePlaying.store (false);
        *masterGain = (float) state.getProperty ("master", 0.0f);
        *grain      = (float) state.getProperty ("grain", 0.0f);
    }

    const double savedHostBpm = (double) state.getProperty ("hostBpm", 120.0);

    if (! tempoFromHost.load() && savedHostBpm >= minBpm && savedHostBpm <= 999.0)
        hostBpm.store (savedHostBpm);

    // Les samples
    for (int d = 0; d < numDecks; ++d)
    {
        juce::ValueTree tree;

        if (legacy)
            tree = d == 0 ? state : juce::ValueTree ("Deck");
        else
            for (const auto& child : state)
                if (child.hasType ("Deck") && (int) child.getProperty ("index", -1) == d)
                    tree = child;

        if (! tree.isValid())
            tree = juce::ValueTree ("Deck");

        decks[(size_t) d]->fromTree (tree, reuseLoadedSamples);
    }

    activeDeck.store (juce::jlimit (0, numDecks - 1, (int) state.getProperty ("activeDeck", 0)));
    fullVelocity.store ((bool) state.getProperty ("fullVelocity", false));
    setNoteRepeat ((int) state.getProperty ("noteRepeat", 0));
    setSnap ((int) state.getProperty ("snap", 16));

    // Le montage
    std::array<std::vector<Clip>, numVariants> restored;
    int bars = juce::jlimit (1, maxTimelineBars, (int) state.getProperty ("bars", 2));

    if (legacy)
    {
        // La « piste » de la 0.4 : des tranches bout à bout
        juce::StringArray steps;
        steps.addTokens (state.getProperty ("sequence").toString(), ",", "");
        steps.removeEmptyStrings();
        double position = 0.0;

        for (const auto& step : steps)
        {
            const int slice = step.getIntValue();

            if (slice < 0 || slice >= decks[0]->getNumSlices())
                continue;

            const double length = juce::jmax (minClipBeats, naturalBeats (0, slice));
            restored[0].push_back ({ 0, slice, position, length });
            position += length;
        }

        while (bars < maxTimelineBars && (double) (bars * beatsPerBar) < position - 0.01)
            bars *= 2;
    }
    else
    {
        for (const auto& arrangement : state)
        {
            const int v = (int) arrangement.getProperty ("index", -1);

            if (! arrangement.hasType ("Arrangement") || v < 0 || v >= numVariants)
                continue;

            for (const auto& child : arrangement)
            {
                Clip clip;
                clip.deck   = (int) child.getProperty ("deck", 0);
                clip.slice  = (int) child.getProperty ("slice", 0);
                clip.start  = (double) child.getProperty ("start", 0.0);
                clip.length = (double) child.getProperty ("length", 1.0);

                if (child.hasType ("Clip") && clip.deck >= 0 && clip.deck < numDecks && clip.slice >= 0
                    && clip.slice < decks[(size_t) clip.deck]->getNumSlices()
                    && clip.start >= 0.0 && clip.length > 0.0 && (int) restored[(size_t) v].size() < maxClips)
                    restored[(size_t) v].push_back (clip);
            }
        }
    }

    {
        const juce::SpinLock::ScopedLockType lock (audioLock);
        timelineBars = bars;
        const double total = (double) (bars * beatsPerBar);

        for (int v = 0; v < numVariants; ++v)
        {
            int count = 0;

            for (auto clip : restored[(size_t) v])
            {
                if (clip.start > total - minClipBeats || count >= maxClips)
                    continue;

                clip.length = juce::jlimit (minClipBeats, total - clip.start, clip.length);
                clips[(size_t) v][(size_t) count++] = clip;
            }

            clipCounts[(size_t) v] = count;
        }
    }

    variant.store (juce::jlimit (0, numVariants - 1, (int) state.getProperty ("variant", 0)));

    if (! reuseLoadedSamples)
        setShownView ((int) state.getProperty ("view", 0));
}

void SiampleProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = createState().createXml())
        copyXmlToBinary (*xml, destData);
}

void SiampleProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);

    if (xml == nullptr)
        return;

    const auto state = juce::ValueTree::fromXml (*xml);

    if (! state.hasType ("Siample"))
        return;

    ++editSuspended;
    applyState (state, false);
    --editSuspended;

    undoStack.clear();
    redoStack.clear();
    lastEditTag.clear();
    sendChangeMessage();
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SiampleProcessor();
}
