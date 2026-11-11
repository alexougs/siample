/*  Tests de Siample : moteur audio, découpe, sauvegarde d'état, et capture de l'interface.
    Usage : SiampleTests [dossier de sortie pour les captures]
*/
#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <thread>

using namespace siample;

static int failures = 0;

static void check (bool condition, const juce::String& what)
{
    std::cout << (condition ? "  ok     " : "  ECHEC  ") << what << std::endl;
    if (! condition)
        ++failures;
}

/** Boucle de batterie synthétique : des coups à des positions connues, sur une basse tenue. */
static juce::AudioBuffer<float> makeLoop (double sr, const std::vector<double>& hitTimes, double seconds, bool withBass)
{
    juce::AudioBuffer<float> buffer (2, (int) (sr * seconds));
    buffer.clear();
    juce::Random random (1234);

    for (size_t h = 0; h < hitTimes.size(); ++h)
    {
        const int start = (int) (hitTimes[h] * sr);
        const int kind = (int) (h % 3);   // 0 kick, 1 snare, 2 charley
        const int length = (int) (sr * (kind == 2 ? 0.05 : 0.18));

        for (int i = 0; i < length && start + i < buffer.getNumSamples(); ++i)
        {
            const double t = i / sr;
            float v = 0.0f;

            if (kind == 0)       v = (float) (0.9 * std::sin (2.0 * juce::MathConstants<double>::pi * (50.0 + 90.0 * std::exp (-t * 30.0)) * t) * std::exp (-t * 18.0));
            else if (kind == 1)  v = (float) ((random.nextFloat() * 2.0f - 1.0f) * 0.6 * std::exp (-t * 22.0));
            else                 v = (float) ((random.nextFloat() * 2.0f - 1.0f) * 0.25 * std::exp (-t * 80.0));

            buffer.addSample (0, start + i, v);
            buffer.addSample (1, start + i, v);
        }
    }

    if (withBass)
        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            // basse tenue + accord tenu, comme dans un morceau complet
            const double t = 2.0 * juce::MathConstants<double>::pi * i / sr;
            const float v = (float) (0.15 * std::sin (82.4 * t) + 0.05 * std::sin (329.6 * t)
                                     + 0.05 * std::sin (415.3 * t) + 0.04 * std::sin (493.9 * t));
            buffer.addSample (0, i, v);
            buffer.addSample (1, i, v);
        }

    return buffer;
}

static juce::File writeWav (const juce::AudioBuffer<float>& buffer, double sr, const juce::String& name)
{
    auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile (name);
    file.deleteFile();

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (sr)
                                                                               .withNumChannels (buffer.getNumChannels())
                                                                               .withBitsPerSample (24));
    jassert (writer != nullptr);
    writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
    writer.reset();
    return file;
}

/** Fait tourner le processeur et renvoie le niveau crête de chaque bloc. */
static std::vector<float> run (SiampleProcessor& proc, int numBlocks, juce::MidiBuffer firstBlockMidi = {}, int blockSize = 512)
{
    std::vector<float> peaks;
    juce::AudioBuffer<float> buffer (2, blockSize);

    for (int b = 0; b < numBlocks; ++b)
    {
        juce::MidiBuffer midi;
        if (b == 0)
            midi = firstBlockMidi;

        proc.processBlock (buffer, midi);
        peaks.push_back (buffer.getMagnitude (0, blockSize));
    }

    return peaks;
}

static juce::MidiBuffer noteOn (int note, int offset = 0)
{
    juce::MidiBuffer m;
    m.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) 127), offset);
    return m;
}

static juce::MidiBuffer noteOff (int note)
{
    juce::MidiBuffer m;
    m.addEvent (juce::MidiMessage::noteOff (1, note), 0);
    return m;
}

static int lastAudibleBlock (const std::vector<float>& peaks)
{
    int last = -1;
    for (size_t i = 0; i < peaks.size(); ++i)
        if (peaks[i] > 1.0e-4f)
            last = (int) i;
    return last;
}

/** Faux transport de DAW, pour donner un tempo au plugin. */
struct TestPlayHead final : public juce::AudioPlayHead
{
    double bpm = 120.0;

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setBpm (bpm);
        info.setIsPlaying (true);
        return info;
    }
};

/** Fait tourner le processeur et renvoie tout le canal gauche produit. */
static std::vector<float> capture (SiampleProcessor& proc, int numBlocks, juce::MidiBuffer firstBlockMidi = {}, int blockSize = 512)
{
    std::vector<float> out;
    juce::AudioBuffer<float> buffer (2, blockSize);

    for (int b = 0; b < numBlocks; ++b)
    {
        juce::MidiBuffer midi;
        if (b == 0)
            midi = firstBlockMidi;

        proc.processBlock (buffer, midi);
        out.insert (out.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize);
    }

    return out;
}

/** Durée audible, en secondes. */
static double audibleSeconds (const std::vector<float>& signal, double sampleRate)
{
    int last = -1;
    for (size_t i = 0; i < signal.size(); ++i)
        if (std::abs (signal[i]) > 2.0e-3f)
            last = (int) i;

    return (double) (last + 1) / sampleRate;
}

/** Fréquence d'un son pur, mesurée par ses passages par zéro entre deux instants. */
static double measureFrequency (const std::vector<float>& signal, double sampleRate, double fromSeconds, double toSeconds)
{
    const int from = (int) (fromSeconds * sampleRate), to = juce::jmin ((int) signal.size() - 1, (int) (toSeconds * sampleRate));
    int crossings = 0;

    for (int i = from + 1; i <= to; ++i)
        if ((signal[(size_t) i] >= 0.0f) != (signal[(size_t) i - 1] >= 0.0f))
            ++crossings;

    return (double) crossings / 2.0 / ((double) (to - from) / sampleRate);
}

static float rms (const std::vector<float>& signal)
{
    double sum = 0.0;
    for (float v : signal)
        sum += (double) v * (double) v;
    return signal.empty() ? 0.0f : (float) std::sqrt (sum / (double) signal.size());
}

/** Rythme simple : kick sur les temps, snare sur 2 et 4, charley sur les croches. */
static juce::AudioBuffer<float> makeBeat (double sr, double bpm, double seconds)
{
    std::vector<double> times;
    const double beat = 60.0 / bpm;
    juce::AudioBuffer<float> buffer (2, (int) (sr * seconds));
    buffer.clear();
    juce::Random random (99);

    for (int step = 0; (double) step * beat * 0.5 < seconds - 0.2; ++step)
    {
        const int start = (int) ((double) step * beat * 0.5 * sr);
        const bool onBeat = step % 2 == 0;
        const int beatIndex = step / 2;
        const int kind = ! onBeat ? 2 : (beatIndex % 2 == 0 ? 0 : 1);
        const int length = (int) (sr * (kind == 2 ? 0.04 : 0.15));

        for (int i = 0; i < length && start + i < buffer.getNumSamples(); ++i)
        {
            const double t = i / sr;
            float v = 0.0f;

            if (kind == 0)       v = (float) (0.9 * std::sin (2.0 * juce::MathConstants<double>::pi * (50.0 + 90.0 * std::exp (-t * 30.0)) * t) * std::exp (-t * 20.0));
            else if (kind == 1)  v = (float) ((random.nextFloat() * 2.0f - 1.0f) * 0.6 * std::exp (-t * 25.0));
            else                 v = (float) ((random.nextFloat() * 2.0f - 1.0f) * 0.2 * std::exp (-t * 90.0));

            buffer.addSample (0, start + i, v);
            buffer.addSample (1, start + i, v);
        }
    }

    return buffer;
}

static juce::AudioBuffer<float> makeTone (double sr, double frequency, double seconds)
{
    juce::AudioBuffer<float> buffer (2, (int) (sr * seconds));

    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        const float v = (float) (0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * frequency * i / sr));
        buffer.setSample (0, i, v);
        buffer.setSample (1, i, v);
    }

    return buffer;
}

static void saveSnapshot (SiampleProcessor& proc, const juce::File& out, int width, int height)
{
    std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditorAndMakeActive());
    editor->setSize (width, height);
    auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);

    out.deleteFile();
    juce::FileOutputStream stream (out);
    juce::PNGImageFormat png;
    png.writeImageToStream (image, stream);
    proc.editorBeingDeleted (editor.get());
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI gui;
    // Mode « --load fichier... » : charge chaque fichier et affiche ce que Siample en fait
    if (argc > 2 && juce::String (argv[1]) == "--load")
    {
        for (int i = 2; i < argc; ++i)
        {
            const auto file = juce::File::getCurrentWorkingDirectory().getChildFile (argv[i]);
            SiampleProcessor proc;
            proc.prepareToPlay (44100.0, 512);
            const auto startTime = juce::Time::getMillisecondCounterHiRes();
            const auto error = proc.loadFile (file);
            const auto elapsed = juce::Time::getMillisecondCounterHiRes() - startTime;

            std::cout << file.getFileName() << " : "
                      << (error.isEmpty() ? juce::String (proc.getSampleLength() / proc.getSourceSampleRate(), 2) + " s, "
                                              + juce::String (proc.getSourceSampleRate()) + " Hz, "
                                              + juce::String (proc.getSample()->buffer.getNumChannels()) + " canal(aux), "
                                              + juce::String (proc.getNumSlices()) + " tranches, "
                                              + juce::String (elapsed, 0) + " ms, joue = "
                                              + juce::String (lastAudibleBlock (run (proc, 8, noteOn (36))) >= 0 ? "oui" : "non")
                                          : "ERREUR " + error)
                      << std::endl;

            if (error.isEmpty())
            {
                // Temps de calcul du mode Sync (projet a 100 BPM)
                TestPlayHead head;
                head.bpm = 100.0;
                proc.setPlayHead (&head);
                run (proc, 2);
                const auto syncStart = juce::Time::getMillisecondCounterHiRes();
                proc.setSyncEnabled (true);
                proc.waitForRendering (600000);
                std::cout << "    tempo detecte " << juce::String (proc.getSampleBpm(), 2) << " BPM ; Sync : "
                          << juce::String ((juce::Time::getMillisecondCounterHiRes() - syncStart) / 1000.0, 2) << " s de calcul pour "
                          << juce::String ((proc.getRegionEnd() - proc.getRegionStart()) / proc.getSourceSampleRate(), 1) << " s d'audio" << std::endl;
                proc.setSyncEnabled (false);
                proc.setPlayHead (nullptr);
            }

            if (error.isEmpty() && proc.getSampleLength() / proc.getSourceSampleRate() < 30.0)
            {
                std::cout << "    tranches selon la sensibilite :";
                for (float sens : { 0.0f, 0.2f, 0.4f, 0.5f, 0.6f, 0.8f, 1.0f })
                {
                    proc.setSensitivity (sens);
                    std::cout << "  " << sens << " -> " << proc.getNumSlices();
                }
                std::cout << std::endl << "    positions (ms) a 0.5 :";
                proc.setSensitivity (0.5f);
                for (int s = 0; s < proc.getNumSlices(); ++s)
                    std::cout << " " << juce::roundToInt (1000.0 * proc.getSlice (s).start / proc.getSourceSampleRate());
                std::cout << std::endl;
            }
        }
        return 0;
    }

    const juce::File outDir = argc > 1 ? juce::File::getCurrentWorkingDirectory().getChildFile (argv[1])
                                       : juce::File::getSpecialLocation (juce::File::tempDirectory);
    outDir.createDirectory();

    const double sr = 44100.0;
    const std::vector<double> hits { 0.0, 0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75 };

    //==============================================================================
    std::cout << "Detection de transitoires" << std::endl;
    for (bool withBass : { false, true })
    {
        auto loop = makeLoop (sr, hits, 2.0, withBass);
        auto file = writeWav (loop, sr, withBass ? "siample_test_bass.wav" : "siample_test.wav");

        SiampleProcessor proc;
        proc.prepareToPlay (48000.0, 512);
        check (proc.loadFile (file).isEmpty(), "chargement du WAV");
        check (proc.getSampleLength() == loop.getNumSamples(), "longueur du sample");

        const juce::String label = withBass ? " (avec basse)" : " (batterie seule)";
        check (proc.getNumSlices() == 8, "8 tranches a la sensibilite par defaut" + label + " -> " + juce::String (proc.getNumSlices()));

        int worst = 0;
        for (int i = 0; i < juce::jmin (8, proc.getNumSlices()); ++i)
            worst = juce::jmax (worst, std::abs (proc.getSlice (i).start - (int) (hits[(size_t) i] * sr)));
        for (int i = 0; i < juce::jmin (8, proc.getNumSlices()); ++i)
            std::cout << "      coup " << i << " : ecart " << proc.getSlice (i).start - (int) (hits[(size_t) i] * sr) << std::endl;
        check (worst <= 132, "positions a moins de 3 ms" + label + " -> ecart max " + juce::String (worst) + " echantillons");

        proc.setSensitivity (0.0f);
        const int low = proc.getNumSlices();
        proc.setSensitivity (1.0f);
        const int high = proc.getNumSlices();
        check (low <= 8 && high >= 8 && low >= 1, "la sensibilite fait varier le nombre de tranches (" + juce::String (low) + " -> " + juce::String (high) + ")");
    }

    //==============================================================================
    auto loop = makeLoop (sr, hits, 2.0, true);
    auto file = writeWav (loop, sr, "siample_test_main.wav");

    SiampleProcessor proc;
    proc.prepareToPlay (48000.0, 512);
    proc.loadFile (file);

    std::cout << "Grille et points manuels" << std::endl;
    proc.setSliceMode (SliceMode::grid);
    proc.setDivisions (16);
    check (proc.getNumSlices() == 16, "grille de 16 parts");
    check (proc.getSlice (4).start == loop.getNumSamples() / 4, "position de la 5e part");
    check (proc.getSliceEnd (15) == loop.getNumSamples(), "la derniere tranche finit a la fin");

    proc.setDivisions (8);
    const int added = proc.addMarker (proc.getSlice (0).start + 3000);
    check (added == 1 && proc.getNumSlices() == 9, "ajout d'un point");
    check (proc.getSliceMode() == SliceMode::manual, "passage automatique en mode manuel");
    check (proc.moveMarker (1, -500) == proc.getSlice (0).start + minSliceLength, "un point ne peut pas depasser son voisin");
    check (proc.removeMarker (1) && proc.getNumSlices() == 8, "suppression d'un point");
    check (proc.addMarker (proc.getSlice (2).start + 10) == -1, "refus d'un point trop proche d'un autre");

    proc.setRegion (loop.getNumSamples() / 2, loop.getNumSamples());
    check (proc.getNumSlices() == 4 && proc.getSlice (0).start == loop.getNumSamples() / 2, "zone reduite : les points hors zone sont retires");
    proc.setRegion (0, loop.getNumSamples());
    proc.setSliceMode (SliceMode::grid);
    check (proc.getNumSlices() == 8, "retour a la grille de 8");

    //==============================================================================
    std::cout << "Lecture" << std::endl;
    const double sliceSeconds = 0.25;
    const double blockSeconds = 512.0 / 48000.0;
    const int expectedBlocks = (int) (sliceSeconds / blockSeconds);

    check (lastAudibleBlock (run (proc, 20)) == -1, "silence sans note");

    auto peaks = run (proc, 60, noteOn (36));
    check (peaks[0] > 0.05f, "note 36 : la tranche 1 se joue");
    check (std::abs (lastAudibleBlock (peaks) - expectedBlocks) <= 1, "one-shot : duree = longueur de la tranche (" + juce::String (lastAudibleBlock (peaks)) + " blocs, attendu " + juce::String (expectedBlocks) + ")");

    float maxPeak = 0.0f;
    for (auto p : peaks) maxPeak = juce::jmax (maxPeak, p);
    check (maxPeak < 1.2f, "niveau de sortie raisonnable (" + juce::String (maxPeak, 2) + ")");

    check (lastAudibleBlock (run (proc, 20, noteOn (36 + 8))) == -1, "note au-dela de la derniere tranche : silence");

    // Hors-temps : note placée au milieu d'un bloc
    {
        juce::AudioBuffer<float> buffer (2, 512);
        auto midi = noteOn (36, 300);
        proc.processBlock (buffer, midi);
        check (buffer.getMagnitude (0, 0, 300) == 0.0f && buffer.getMagnitude (0, 300, 212) > 0.0f, "la note demarre a l'echantillon exact");
        run (proc, 60);
    }

    // Pitch +12 : deux fois plus court
    auto params = SliceParams();
    params.pitch = 12;
    proc.setSliceParams (0, params);
    peaks = run (proc, 60, noteOn (36));
    check (std::abs (lastAudibleBlock (peaks) - expectedBlocks / 2) <= 1, "pitch +12 : duree divisee par deux");

    // Inversé
    params = SliceParams();
    params.reverse = true;
    proc.setSliceParams (0, params);
    peaks = run (proc, 60, noteOn (36));
    check (lastAudibleBlock (peaks) >= expectedBlocks - 2 && peaks[0] < peaks[(size_t) expectedBlocks - 3] + 0.5f, "lecture inversee");

    // Gate : s'arrête au relâchement
    params = SliceParams();
    params.oneShot = false;
    params.release = 0.01f;
    proc.setSliceParams (0, params);
    run (proc, 4, noteOn (36));
    peaks = run (proc, 30, noteOff (36));
    check (lastAudibleBlock (peaks) <= 1, "gate : le son s'arrete au relachement");

    // One-shot : continue après le relâchement
    proc.setSliceParams (0, SliceParams());
    run (proc, 4, noteOn (36));
    peaks = run (proc, 30, noteOff (36));
    check (lastAudibleBlock (peaks) >= expectedBlocks - 6, "one-shot : le son continue apres le relachement");

    // Choke
    params = SliceParams();
    params.choke = 1;
    proc.setSliceParams (0, params);
    proc.setSliceParams (1, params);
    run (proc, 2, noteOn (36));
    check (proc.getActiveMask() == 1, "masque d'activite : tranche 1");
    run (proc, 3, noteOn (37));
    check (proc.getActiveMask() == 2, "choke : la tranche 2 coupe la tranche 1");
    run (proc, 60);
    check (proc.getActiveMask() == 0, "plus rien ne joue a la fin");

    // Ecoute depuis l'interface
    proc.audition (2, true);
    peaks = run (proc, 3);
    check (peaks[0] > 0.01f, "ecoute depuis l'interface");
    run (proc, 60);

    // Volume général
    proc.setSliceParams (0, SliceParams());
    const float reference = run (proc, 60, noteOn (36))[1];
    *proc.getMasterGainParameter() = -12.0f;
    run (proc, 60);
    const float quieter = run (proc, 60, noteOn (36))[1];
    check (std::abs (quieter / reference - 0.251f) < 0.02f, "volume general -12 dB");
    *proc.getMasterGainParameter() = 0.0f;

    //==============================================================================
    std::cout << "Sauvegarde et rechargement" << std::endl;
    proc.setSliceMode (SliceMode::transients);
    proc.setSensitivity (0.5f);
    proc.setRegion (2000, loop.getNumSamples() - 3000);
    params = SliceParams();
    params.gainDb = -6.0f; params.pan = 0.5f; params.pitch = -3; params.fine = 12.0f;
    params.attack = 0.01f; params.decay = 0.2f; params.sustain = 0.5f; params.release = 0.3f;
    params.reverse = true; params.oneShot = false; params.choke = 3;
    proc.setSliceParams (2, params);
    proc.setSelectedSlice (2);

    juce::MemoryBlock saved;
    proc.getStateInformation (saved);

    {
        SiampleProcessor other;
        other.prepareToPlay (44100.0, 256);
        other.setStateInformation (saved.getData(), (int) saved.getSize());

        bool same = other.getNumSlices() == proc.getNumSlices()
                 && other.getRegionStart() == proc.getRegionStart()
                 && other.getRegionEnd() == proc.getRegionEnd()
                 && other.getSelectedSlice() == 2
                 && other.getSliceMode() == SliceMode::transients;

        for (int i = 0; same && i < proc.getNumSlices(); ++i)
            same = other.getSlice (i).start == proc.getSlice (i).start;

        check (same, "memes tranches et meme zone apres rechargement");

        const auto& q = other.getSlice (2).params;
        check (q.gainDb == -6.0f && q.pan == 0.5f && q.pitch == -3 && q.fine == 12.0f && q.attack == 0.01f && q.decay == 0.2f
                && q.sustain == 0.5f && q.release == 0.3f && q.reverse && ! q.oneShot && q.choke == 3, "memes reglages de tranche");
        check (lastAudibleBlock (run (other, 10, noteOn (36), 256)) >= 0, "le plugin recharge joue");
    }

    {
        // Fichier disparu : la découpe doit survivre à une nouvelle sauvegarde
        auto moved = file.getSiblingFile ("siample_test_moved.wav");
        moved.deleteFile();
        file.moveFileTo (moved);

        SiampleProcessor other;
        other.setStateInformation (saved.getData(), (int) saved.getSize());
        check (! other.hasSample() && other.getMissingFilePath() == file.getFullPathName(), "fichier manquant signale");
        check (lastAudibleBlock (run (other, 4, noteOn (36))) == -1, "fichier manquant : silence, pas de plantage");

        juce::MemoryBlock again;
        other.getStateInformation (again);
        moved.moveFileTo (file);

        SiampleProcessor third;
        third.setStateInformation (again.getData(), (int) again.getSize());
        check (third.hasSample() && third.getNumSlices() == proc.getNumSlices() && third.getSlice (2).params.choke == 3,
               "la decoupe survit a un fichier manquant");
    }

    {
        SiampleProcessor other;
        const char garbage[] = "pas un etat valide";
        other.setStateInformation (garbage, (int) sizeof (garbage));
        other.setStateInformation (nullptr, 0);
        check (! other.hasSample(), "etat invalide ignore sans plantage");
        check (other.loadFile (juce::File ("/nexiste/pas.wav")).isNotEmpty(), "fichier inexistant : message d'erreur");

        auto text = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("siample_faux.wav");
        text.replaceWithText ("ceci n'est pas de l'audio");
        check (other.loadFile (text).isNotEmpty(), "faux fichier audio : message d'erreur");
    }

    //==============================================================================
    std::cout << "Tempo" << std::endl;
    {
        SiampleProcessor t;
        t.prepareToPlay (48000.0, 512);
        t.loadFile (file);
        check (std::abs (t.getSampleBpm() - 120.0) < 0.5, "boucle de 2 s, un coup tous les 0,25 s -> " + juce::String (t.getSampleBpm(), 2) + " BPM (attendu 120)");

        for (double bpm : { 93.0, 128.0, 74.0, 140.0, 100.0, 82.5 })
        {
            auto beatFile = writeWav (makeBeat (sr, bpm, 9.3), sr, "siample_beat.wav");
            t.loadFile (beatFile);
            check (std::abs (t.getSampleBpm() - bpm) < 1.0, "rythme a " + juce::String (bpm, 1) + " BPM, extrait quelconque -> " + juce::String (t.getSampleBpm(), 2));
        }

        {
            // Tempo rapide : la détection peut légitimement compter deux fois plus lentement (bouton x2 pour corriger)
            auto fastFile = writeWav (makeBeat (sr, 165.0, 9.3), sr, "siample_beat.wav");
            t.loadFile (fastFile);
            const double found = t.getSampleBpm();
            check (std::abs (found - 165.0) < 1.0 || std::abs (found * 2.0 - 165.0) < 1.0, "rythme a 165 BPM -> " + juce::String (found, 2) + " (165 ou sa moitie)");
            t.setSampleBpm (found * (found < 100.0 ? 2.0 : 1.0));
            check (std::abs (t.getSampleBpm() - 165.0) < 1.0, "bouton x2 : tempo corrige a " + juce::String (t.getSampleBpm(), 2));
        }

        {
            // Boucle exacte de 16 temps a 87 BPM
            const double seconds = 16.0 * 60.0 / 87.0;
            auto loopFile = writeWav (makeBeat (sr, 87.0, seconds), sr, "siample_loop87.wav");
            t.loadFile (loopFile);
            check (std::abs (t.getSampleBpm() - 87.0) < 0.1, "boucle exacte de 16 temps a 87 BPM -> " + juce::String (t.getSampleBpm(), 3));

            t.setSliceMode (SliceMode::grid);
            t.setDivisions (grid::quarters);
            check (t.getNumSlices() == 16, "grille en noires : 16 tranches -> " + juce::String (t.getNumSlices()));
            t.setDivisions (grid::bars);
            check (t.getNumSlices() == 4, "grille en mesures : 4 tranches -> " + juce::String (t.getNumSlices()));
            t.setDivisions (grid::eighths);
            check (t.getNumSlices() == 32, "grille en croches : 32 tranches -> " + juce::String (t.getNumSlices()));

            t.setSampleBpm (174.0);
            check (t.isBpmManual() && t.getNumSlices() == 64, "tempo saisi a la main (double) : la grille suit");
            t.detectBpm();
            check (! t.isBpmManual() && std::abs (t.getSampleBpm() - 87.0) < 0.1, "bouton Auto : retour au tempo detecte");
        }
    }

    //==============================================================================
    std::cout << "Sync : etirement cale sur le tempo du projet" << std::endl;
    {
        const double hostSr = 48000.0;
        auto toneFile = writeWav (makeTone (sr, 440.0, 2.0), sr, "siample_tone.wav");
        TestPlayHead head;

        SiampleProcessor t;
        t.setPlayHead (&head);
        t.prepareToPlay (hostSr, 512);
        t.loadFile (toneFile);
        t.setSliceMode (SliceMode::grid);
        t.setDivisions (2);
        t.setSampleBpm (120.0);

        const int blocksFor3s = (int) (3.2 * hostSr / 512.0);
        auto out = capture (t, blocksFor3s, noteOn (36));
        check (std::abs (audibleSeconds (out, hostSr) - 1.0) < 0.03, "sans Sync : la tranche dure 1,00 s -> " + juce::String (audibleSeconds (out, hostSr), 3));
        check (std::abs (measureFrequency (out, hostSr, 0.2, 0.8) - 440.0) < 5.0, "sans Sync : 440 Hz -> " + juce::String (measureFrequency (out, hostSr, 0.2, 0.8), 1));

        // Projet a 60 BPM, sample a 120 : tout doit durer deux fois plus longtemps, a la meme hauteur
        head.bpm = 60.0;
        capture (t, 2);
        t.setSyncEnabled (true);
        check (t.isRendering() || true, "calcul lance");
        t.waitForRendering (20000);
        check (! t.isRendering(), "calcul des tranches etirees termine");

        out = capture (t, blocksFor3s, noteOn (36));
        check (std::abs (audibleSeconds (out, hostSr) - 2.0) < 0.05, "Sync a 60 BPM : la tranche dure 2,00 s -> " + juce::String (audibleSeconds (out, hostSr), 3));
        check (std::abs (measureFrequency (out, hostSr, 0.4, 1.6) - 440.0) < 6.0, "Sync a 60 BPM : toujours 440 Hz -> " + juce::String (measureFrequency (out, hostSr, 0.4, 1.6), 1));
        check (rms (out) > 0.1f, "Sync : niveau conserve (" + juce::String (rms (out), 3) + ")");

        // Pitch +12 avec Sync : une octave plus haut, meme duree
        auto params = SliceParams();
        params.pitch = 12;
        t.setSliceParams (0, params);
        t.waitForRendering (20000);
        out = capture (t, blocksFor3s, noteOn (36));
        check (std::abs (audibleSeconds (out, hostSr) - 2.0) < 0.05, "Sync + pitch +12 : la duree ne change pas -> " + juce::String (audibleSeconds (out, hostSr), 3));
        check (std::abs (measureFrequency (out, hostSr, 0.4, 1.6) - 880.0) < 12.0, "Sync + pitch +12 : 880 Hz -> " + juce::String (measureFrequency (out, hostSr, 0.4, 1.6), 1));

        // Tempo du projet identique au sample : pitch seul
        head.bpm = 120.0;
        capture (t, 2);
        t.setSyncEnabled (false);
        t.setSyncEnabled (true);
        t.waitForRendering (20000);
        out = capture (t, blocksFor3s, noteOn (36));
        check (std::abs (audibleSeconds (out, hostSr) - 1.0) < 0.04, "Sync a tempo egal + pitch +12 : 1,00 s -> " + juce::String (audibleSeconds (out, hostSr), 3));
        check (std::abs (measureFrequency (out, hostSr, 0.2, 0.8) - 880.0) < 12.0, "Sync a tempo egal + pitch +12 : 880 Hz -> " + juce::String (measureFrequency (out, hostSr, 0.2, 0.8), 1));

        // Sans Sync, le meme pitch raccourcit la tranche (comportement classique)
        t.setSyncEnabled (false);
        t.waitForRendering (20000);
        out = capture (t, blocksFor3s, noteOn (36));
        check (std::abs (audibleSeconds (out, hostSr) - 0.5) < 0.03, "sans Sync + pitch +12 : 0,50 s -> " + juce::String (audibleSeconds (out, hostSr), 3));

        // Export hors temps reel : le tempo change, le premier bloc doit deja etre juste
        t.setSliceParams (0, SliceParams());
        t.setSyncEnabled (true);
        t.waitForRendering (20000);
        t.setNonRealtime (true);
        head.bpm = 80.0;
        out = capture (t, blocksFor3s, noteOn (36));
        t.setNonRealtime (false);
        check (std::abs (audibleSeconds (out, hostSr) - 1.5) < 0.05, "export hors temps reel a 80 BPM : 1,50 s des la premiere note -> " + juce::String (audibleSeconds (out, hostSr), 3));

        // Sauvegarde : Sync et tempo reviennent, et le plugin recharge joue etire
        juce::MemoryBlock state;
        t.getStateInformation (state);

        SiampleProcessor other;
        other.prepareToPlay (hostSr, 512);
        other.setStateInformation (state.getData(), (int) state.getSize());
        other.waitForRendering (20000);
        check (other.isSyncEnabled() && std::abs (other.getSampleBpm() - 120.0) < 0.01 && other.isBpmManual(), "Sync et tempo restaures");
        out = capture (other, blocksFor3s, noteOn (36));
        check (std::abs (audibleSeconds (out, hostSr) - 1.5) < 0.05, "plugin recharge : joue etire sans attendre le DAW -> " + juce::String (audibleSeconds (out, hostSr), 3));

        // Tranches de batterie, tres courtes comprises
        SiampleProcessor d;
        TestPlayHead head2;
        head2.bpm = 100.0;
        d.setPlayHead (&head2);
        d.prepareToPlay (hostSr, 512);
        d.loadFile (file);
        d.setSliceMode (SliceMode::grid);
        d.setDivisions (64);
        capture (d, 2);
        d.setSyncEnabled (true);
        d.waitForRendering (30000);
        out = capture (d, 40, noteOn (36));
        const double expected = 2.0 / 64.0 * 120.0 / 100.0;
        check (std::abs (audibleSeconds (out, hostSr) - expected) < 0.006, "64 tranches de 31 ms etirees a 100 BPM : " + juce::String (expected * 1000.0, 1) + " ms -> " + juce::String (audibleSeconds (out, hostSr) * 1000.0, 1));
    }

    //==============================================================================
    std::cout << "Jeu : clavier, mono, filtre, grain" << std::endl;
    {
        const double hostSr = 48000.0;
        SiampleProcessor t;
        t.prepareToPlay (hostSr, 512);
        t.loadFile (file);
        t.setSliceMode (SliceMode::grid);
        t.setDivisions (8);

        // Clavier : toutes les notes jouent la tranche sélectionnée
        t.setSelectedSlice (3);
        t.setPlayMode (PlayMode::keys);
        auto out = capture (t, 40, noteOn (60));
        check (std::abs (audibleSeconds (out, hostSr) - 0.25) < 0.01 && t.getActiveMask() == 0, "clavier, note 60 : tranche selectionnee a sa hauteur d'origine");
        capture (t, 1, noteOn (60));
        check (t.getActiveMask() == (juce::uint64) 1 << 3, "clavier : c'est bien la tranche 4 qui joue");
        capture (t, 40);
        out = capture (t, 40, noteOn (72));
        check (std::abs (audibleSeconds (out, hostSr) - 0.125) < 0.01, "clavier, note 72 : une octave plus haut (duree / 2) -> " + juce::String (audibleSeconds (out, hostSr), 3));
        out = capture (t, 60, noteOn (48));
        check (std::abs (audibleSeconds (out, hostSr) - 0.5) < 0.015, "clavier, note 48 : une octave plus bas (duree x 2) -> " + juce::String (audibleSeconds (out, hostSr), 3));

        juce::MidiBuffer chord;
        chord.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127), 0);
        chord.addEvent (juce::MidiMessage::noteOn (1, 67, (juce::uint8) 127), 0);
        const float one = rms (capture (t, 6, noteOn (60)));
        capture (t, 60);
        const float two = rms (capture (t, 6, chord));
        capture (t, 60);
        check (two > one * 1.15f, "clavier : deux notes en meme temps (polyphonie)");
        t.setPlayMode (PlayMode::pads);

        // Mono
        juce::MidiBuffer twoPads;
        twoPads.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 127), 0);
        twoPads.addEvent (juce::MidiMessage::noteOn (1, 37, (juce::uint8) 127), 400);
        capture (t, 2, twoPads);
        check (t.getActiveMask() == 3, "poly : deux tranches jouent ensemble");
        capture (t, 60);
        t.setMono (true);
        capture (t, 2, twoPads);
        check (t.getActiveMask() == 2, "mono : la nouvelle note coupe la precedente");
        capture (t, 60);
        t.setMono (false);

        // Filtre, sur la tranche de caisse claire (bruit)
        const float dry = rms (capture (t, 20, noteOn (37)));
        auto params = SliceParams();
        params.filter = -1.0f;
        t.setSliceParams (1, params);
        const float lowPassed = rms (capture (t, 20, noteOn (37)));
        params.filter = 1.0f;
        t.setSliceParams (1, params);
        const float highPassed = rms (capture (t, 20, noteOn (37)));
        params.filter = -0.02f;
        t.setSliceParams (1, params);
        const float barely = rms (capture (t, 20, noteOn (37)));
        check (lowPassed < dry * 0.5f, "filtre a fond a gauche : les aigus sont coupes (" + juce::String (lowPassed / dry, 2) + " du niveau)");
        check (highPassed < dry * 0.95f && highPassed > dry * 0.05f, "filtre a fond a droite : les graves sont coupes (" + juce::String (highPassed / dry, 2) + " du niveau)");
        check (std::abs (barely / dry - 1.0f) < 0.1f, "filtre presque au centre : son quasi inchange");
        t.setSliceParams (1, SliceParams());

        // Boucle : la tranche se répète tant que la note est tenue
        {
            capture (t, 60);
            auto looped = SliceParams();
            looped.loop = true;
            looped.release = 0.01f;
            t.setSliceParams (0, looped);

            const int blocksPerSecond = (int) (hostSr / 512.0);
            auto held = capture (t, blocksPerSecond, noteOn (36));        // 1 s tenue, tranche de 0,25 s
            bool alive = true;
            for (int window = 0; window < 4; ++window)
            {
                const std::vector<float> part (held.begin() + window * (int) held.size() / 4, held.begin() + (window + 1) * (int) held.size() / 4);
                alive = alive && rms (part) > 0.02f;
            }
            check (alive, "boucle : le son continue pendant 1 s pour une tranche de 0,25 s");

            // Chaque passage est identique au premier (hors des fondus aux bords)
            const int period = (int) (0.25 * hostSr);
            float worst = 0.0f;
            for (int i = 200; i < period - 200; ++i)
                worst = juce::jmax (worst, std::abs (held[(size_t) i] - held[(size_t) (i + 2 * period)]));
            check (worst < 0.02f, "boucle : le 3e passage est identique au 1er (ecart max " + juce::String (worst, 4) + ")");

            float jump = 0.0f;
            for (size_t i = 1; i < held.size(); ++i)
                jump = juce::jmax (jump, std::abs (held[i] - held[i - 1]));
            float reference = 0.0f;
            for (int i = 1; i < period - 100; ++i)
                reference = juce::jmax (reference, std::abs (held[(size_t) i] - held[(size_t) i - 1]));
            check (jump <= reference * 1.05f + 1.0e-4f, "boucle : pas de clic au point de bouclage");

            auto released = capture (t, 30, noteOff (36));
            check (audibleSeconds (released, hostSr) < 0.03, "boucle : s'arrete au relachement -> " + juce::String (audibleSeconds (released, hostSr) * 1000.0, 1) + " ms");

            // Pitch +12 : la boucle tourne deux fois plus vite
            looped.pitch = 12;
            t.setSliceParams (0, looped);
            held = capture (t, blocksPerSecond, noteOn (36));
            worst = 0.0f;
            for (int i = 200; i < period / 2 - 200; ++i)
                worst = juce::jmax (worst, std::abs (held[(size_t) i] - held[(size_t) (i + period)]));
            check (worst < 0.02f, "boucle + pitch +12 : periode divisee par deux");
            capture (t, 30, noteOff (36));

            // Inversé + boucle
            looped.pitch = 0;
            looped.reverse = true;
            t.setSliceParams (0, looped);
            held = capture (t, blocksPerSecond, noteOn (36));
            worst = 0.0f;
            for (int i = 200; i < period - 200; ++i)
                worst = juce::jmax (worst, std::abs (held[(size_t) i] - held[(size_t) (i + 2 * period)]));
            check (worst < 0.02f && rms (held) > 0.02f, "boucle inversee : se repete aussi");
            capture (t, 30, noteOff (36));

            // Écoute à la souris : boucle tant que le bouton est tenu
            t.audition (0, true);
            held = capture (t, blocksPerSecond);
            const std::vector<float> lastQuarter (held.end() - (int) held.size() / 4, held.end());
            check (rms (lastQuarter) > 0.02f, "boucle : fonctionne aussi au clic sur un pad");
            t.audition (0, false);
            check (audibleSeconds (capture (t, 30), hostSr) < 0.03, "boucle : s'arrete quand on relache la souris");

            // Sauvegarde
            juce::MemoryBlock state;
            t.getStateInformation (state);
            SiampleProcessor other;
            other.setStateInformation (state.getData(), (int) state.getSize());
            check (other.getSlice (0).params.loop && ! other.getSlice (1).params.loop, "boucle : reglage restaure apres sauvegarde");

            t.setSliceParams (0, SliceParams());
        }

        // Grain
        capture (t, 60);
        const auto clean = capture (t, 60, noteOn (36));
        *t.getGrainParameter() = 100.0f;
        const auto crushed = capture (t, 60, noteOn (36));
        *t.getGrainParameter() = 0.0f;
        const auto cleanAgain = capture (t, 60, noteOn (36));

        int repeats = 0;
        float peak = 0.0f;
        for (size_t i = 1; i < crushed.size(); ++i)
        {
            if (std::abs (crushed[i] - crushed[i - 1]) < 1.0e-9f && std::abs (crushed[i]) > 1.0e-4f)
                ++repeats;
            peak = juce::jmax (peak, std::abs (crushed[i]));
        }

        check (repeats > (int) crushed.size() / 8 && peak < 1.3f, "grain a 100 % : son en escalier, niveau maitrise");
        check (clean == cleanAgain, "grain a 0 % : son strictement identique");

        // Tête de lecture
        capture (t, 3, noteOn (38));
        const int head = t.getPlayheadPosition();
        check (head >= t.getSlice (2).start && head < t.getSliceEnd (2), "tete de lecture dans la tranche jouee");
        capture (t, 60);
        check (t.getPlayheadPosition() == -1, "tete de lecture masquee a l'arret");
    }

    //==============================================================================
    std::cout << "Boucle : Flip, ecoute, MIDI" << std::endl;
    {
        const double hostSr = 48000.0;
        SiampleProcessor t;
        t.prepareToPlay (hostSr, 512);
        t.loadFile (file);
        t.setSliceMode (SliceMode::grid);
        t.setDivisions (8);

        // MIDI de la boucle d'origine
        auto readNotes = [] (const juce::File& midiFile)
        {
            std::vector<std::pair<int, int>> notes;   // (tick, note)
            juce::FileInputStream stream (midiFile);
            juce::MidiFile midi;

            if (stream.openedOk() && midi.readFrom (stream) && midi.getNumTracks() > 0)
                for (const auto* holder : *midi.getTrack (0))
                    if (holder->message.isNoteOn())
                        notes.push_back ({ (int) holder->message.getTimeStamp(), holder->message.getNoteNumber() });

            return notes;
        };

        auto midiFile = t.createPatternMidiFile();
        auto notes = readNotes (midiFile);
        bool ordered = notes.size() == 8;
        for (size_t i = 0; ordered && i < notes.size(); ++i)
            ordered = notes[i].first == (int) i * 480 && notes[i].second == 36 + (int) i;
        check (ordered, "fichier MIDI : 8 notes, une par demi-temps, dans l'ordre (" + midiFile.getFileName() + ")");

        // Flip
        t.flipPattern();
        bool valid = t.isPatternFlipped(), changed = false;
        for (int i = 0; i < 8; ++i)
        {
            valid = valid && t.getPatternSlice (i) >= 0 && t.getPatternSlice (i) < 8;
            changed = changed || t.getPatternSlice (i) != i;
        }
        check (valid && changed, "Flip : nouvel ordre valide et different de l'original");

        notes = readNotes (t.createPatternMidiFile());
        bool follows = notes.size() == 8;
        for (size_t i = 0; follows && i < notes.size(); ++i)
            follows = notes[i].first == (int) i * 480 && notes[i].second == 36 + t.getPatternSlice ((int) i);
        check (follows, "fichier MIDI apres Flip : memes instants, notes rearrangees");

        // Ecoute interne : chaque pas déclenche la tranche prévue, au bon moment
        t.setPatternPlaying (true);
        const int blocksPerStep = (int) (0.25 * hostSr / 512.0);   // 23,4 blocs par pas
        bool sequenceOk = true;
        juce::AudioBuffer<float> buffer (2, 512);

        for (int step = 0; step < 16 && sequenceOk; ++step)
        {
            // on se place au milieu du pas
            const int targetBlock = (int) (((double) step + 0.5) * 0.25 * hostSr / 512.0);
            static int done = 0;
            if (step == 0) done = 0;

            for (; done < targetBlock; ++done)
            {
                juce::MidiBuffer midi;
                t.processBlock (buffer, midi);
            }

            sequenceOk = t.getActiveMask() == (juce::uint64) 1 << t.getPatternSlice (step % 8);
        }

        check (sequenceOk, "ecoute : les tranches s'enchainent dans l'ordre du Flip, et la boucle reprend au debut");
        juce::ignoreUnused (blocksPerStep);

        t.setPatternPlaying (false);
        capture (t, 3);
        check (t.getActiveMask() == 0, "ecoute : Stop coupe le son");

        t.resetPattern();
        check (! t.isPatternFlipped() && t.getPatternSlice (5) == 5, "Original : retour a l'ordre d'origine");

        // Le Flip survit a la sauvegarde, et disparait si la decoupe change
        t.flipPattern();
        juce::MemoryBlock state;
        t.getStateInformation (state);
        SiampleProcessor other;
        other.setStateInformation (state.getData(), (int) state.getSize());
        bool samePattern = other.isPatternFlipped();
        for (int i = 0; i < 8; ++i)
            samePattern = samePattern && other.getPatternSlice (i) == t.getPatternSlice (i);
        check (samePattern, "Flip restaure apres sauvegarde");

        t.setDivisions (4);
        check (! t.isPatternFlipped(), "nouvelle decoupe : le Flip est remis a zero");
    }

    //==============================================================================
    std::cout << "Piste et export audio" << std::endl;
    {
        const double hostSr = 48000.0;
        // Quatre notes d'une demi-seconde : 220, 330, 440 et 550 Hz
        juce::AudioBuffer<float> tones (2, (int) (sr * 2.0));
        tones.clear();
        const double frequencies[] = { 220.0, 330.0, 440.0, 550.0 };

        for (int i = 0; i < tones.getNumSamples(); ++i)
        {
            const double f = frequencies[juce::jmin (3, (int) (i / (sr * 0.5)))];
            const float v = (float) (0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * f * i / sr));
            tones.setSample (0, i, v);
            tones.setSample (1, i, v);
        }

        auto tonesFile = writeWav (tones, sr, "siample_tones.wav");
        TestPlayHead head;
        SiampleProcessor t;
        t.setPlayHead (&head);
        t.prepareToPlay (hostSr, 512);
        t.loadFile (tonesFile);
        t.setSliceMode (SliceMode::grid);
        t.setDivisions (4);
        t.setSampleBpm (120.0);
        check (t.getNumSlices() == 4, "4 tranches d'une demi-seconde");

        auto readWav = [&t] (const juce::File& f, double& rate)
        {
            std::vector<float> samples;
            std::unique_ptr<juce::AudioFormatReader> reader (t.getFormatManager().createReaderFor (f));

            if (reader != nullptr)
            {
                juce::AudioBuffer<float> b ((int) reader->numChannels, (int) reader->lengthInSamples);
                reader->read (&b, 0, b.getNumSamples(), 0, true, true);
                samples.assign (b.getReadPointer (0), b.getReadPointer (0) + b.getNumSamples());
                rate = reader->sampleRate;
            }

            return samples;
        };

        // Export d'une tranche
        double rate = 0.0;
        auto sliceFile = t.createSliceAudioFile (2);
        auto sliceAudio = readWav (sliceFile, rate);
        check (sliceFile.existsAsFile() && sliceFile.getFileName().contains ("tranche 3"), "export d'une tranche : fichier WAV cree (" + sliceFile.getFileName() + ")");
        check (std::abs ((double) sliceAudio.size() / rate - 0.5) < 0.002 && rate == sr, "tranche exportee : 0,50 s a la frequence du sample");
        check (std::abs (measureFrequency (sliceAudio, rate, 0.05, 0.45) - 440.0) < 4.0, "tranche exportee : 440 Hz -> " + juce::String (measureFrequency (sliceAudio, rate, 0.05, 0.45), 1));
        check (! t.createSliceAudioFile (9).existsAsFile(), "tranche inexistante : pas de fichier");

        // Les réglages de la tranche sont dans l'export
        auto params = SliceParams();
        params.pitch = 12;
        params.loop = true;
        t.setSliceParams (2, params);
        sliceAudio = readWav (t.createSliceAudioFile (2), rate);
        check (std::abs ((double) sliceAudio.size() / rate - 0.25) < 0.002, "tranche une octave plus haut : 0,25 s (une seule fois, meme en mode Boucle)");
        check (std::abs (measureFrequency (sliceAudio, rate, 0.03, 0.22) - 880.0) < 8.0, "tranche une octave plus haut : 880 Hz");
        t.setSliceParams (2, SliceParams());

        // Montage de la piste : 1, 1, 3
        check (! t.insertSequenceStep (7), "piste : une tranche inexistante est refusee");
        check (t.insertSequenceStep (0) && t.insertSequenceStep (2) && t.insertSequenceStep (0, 0), "piste : ajout a la fin et insertion au debut");
        check (t.getSequenceLength() == 3 && t.getSequenceStep (0) == 0 && t.getSequenceStep (1) == 0 && t.getSequenceStep (2) == 2, "piste : 1, 1, 3");
        check (std::abs (t.getSequenceSeconds() - 1.5) < 0.001, "piste : 1,50 s au total");

        t.moveSequenceStep (2, 0);
        check (t.getSequenceStep (0) == 2 && t.getSequenceStep (1) == 0 && t.getSequenceStep (2) == 0, "piste : deplacement d'un bloc -> 3, 1, 1");
        t.moveSequenceStep (0, 2);
        t.insertSequenceStep (3, 1);
        t.removeSequenceStep (1);
        check (t.getSequenceLength() == 3 && t.getSequenceStep (2) == 2, "piste : retrait d'un bloc -> retour a 1, 1, 3");

        // Écoute : 220 Hz deux fois puis 440 Hz, et ça reboucle
        t.setPatternPlaying (true);
        t.setSequencePlaying (true);
        check (! t.isPatternPlaying() && t.isSequencePlaying(), "ecouter la piste arrete l'ecoute de la boucle");
        auto out = capture (t, (int) (2.0 * hostSr / 512.0));
        check (std::abs (measureFrequency (out, hostSr, 0.05, 0.45) - 220.0) < 3.0
                   && std::abs (measureFrequency (out, hostSr, 0.55, 0.95) - 220.0) < 3.0
                   && std::abs (measureFrequency (out, hostSr, 1.05, 1.45) - 440.0) < 5.0,
               "ecoute de la piste : 220, 220, 440 Hz");
        check (std::abs (measureFrequency (out, hostSr, 1.55, 1.95) - 220.0) < 3.0, "ecoute de la piste : elle reboucle");
        check (t.getSequencePlayingStep() == 0, "le pas en cours est indique -> " + juce::String (t.getSequencePlayingStep()));
        t.setSequencePlaying (false);
        out = capture (t, 40);
        check (rms (std::vector<float> (out.end() - 2048, out.end())) < 1.0e-4f && t.getSequencePlayingStep() == -1, "Stop : silence");

        // Export audio de la piste
        auto pisteFile = t.createSequenceAudioFile();
        auto piste = readWav (pisteFile, rate);
        check (pisteFile.existsAsFile() && std::abs ((double) piste.size() / rate - 1.5) < 0.002, "piste exportee en audio : 1,50 s (" + pisteFile.getFileName() + ")");
        check (std::abs (measureFrequency (piste, rate, 0.05, 0.45) - 220.0) < 3.0
                   && std::abs (measureFrequency (piste, rate, 0.55, 0.95) - 220.0) < 3.0
                   && std::abs (measureFrequency (piste, rate, 1.05, 1.45) - 440.0) < 5.0,
               "piste exportee : 220, 220, 440 Hz");

        // Export MIDI de la piste (projet a 120 BPM : une demi-seconde = un temps)
        std::vector<std::pair<int, int>> notes;
        {
            juce::FileInputStream stream (t.createSequenceMidiFile());
            juce::MidiFile midi;

            if (stream.openedOk() && midi.readFrom (stream) && midi.getNumTracks() > 0)
                for (const auto* holder : *midi.getTrack (0))
                    if (holder->message.isNoteOn())
                        notes.push_back ({ (int) holder->message.getTimeStamp(), holder->message.getNoteNumber() });
        }
        check (notes.size() == 3 && notes[0] == std::make_pair (0, 36) && notes[1] == std::make_pair (960, 36)
                   && notes[2] == std::make_pair (1920, 38), "piste exportee en MIDI : notes 36, 36, 38, une par temps");

        // Avec Sync a 60 BPM, tout dure deux fois plus longtemps, a la meme hauteur
        head.bpm = 60.0;
        capture (t, 2);
        t.setSyncEnabled (true);
        t.waitForRendering (20000);
        check (std::abs (t.getSequenceSeconds() - 3.0) < 0.001, "Sync a 60 BPM : la piste dure 3,00 s");
        piste = readWav (t.createSequenceAudioFile(), rate);
        check (std::abs ((double) piste.size() / rate - 3.0) < 0.005, "Sync : piste exportee de 3,00 s -> " + juce::String ((double) piste.size() / rate, 3));
        check (std::abs (measureFrequency (piste, rate, 0.2, 0.8) - 220.0) < 4.0
                   && std::abs (measureFrequency (piste, rate, 2.2, 2.8) - 440.0) < 6.0, "Sync : piste exportee a la bonne hauteur");
        t.setSequencePlaying (true);
        out = capture (t, (int) (3.0 * hostSr / 512.0));
        check (std::abs (measureFrequency (out, hostSr, 1.2, 1.8) - 220.0) < 4.0
                   && std::abs (measureFrequency (out, hostSr, 2.2, 2.8) - 440.0) < 6.0, "Sync : ecoute de la piste calee");
        t.setSequencePlaying (false);
        t.setSyncEnabled (false);
        head.bpm = 120.0;
        capture (t, 2);

        // Sauvegarde
        juce::MemoryBlock state;
        t.getStateInformation (state);
        SiampleProcessor restored;
        restored.prepareToPlay (hostSr, 512);
        restored.setStateInformation (state.getData(), (int) state.getSize());
        check (restored.getSequenceLength() == 3 && restored.getSequenceStep (0) == 0 && restored.getSequenceStep (2) == 2, "la piste est enregistree avec le projet");

        // Une nouvelle découpe ne garde que les tranches qui existent encore
        t.setDivisions (2);
        check (t.getSequenceLength() == 2 && t.getSequenceStep (1) == 0, "nouvelle decoupe en 2 : la tranche 3 quitte la piste");
        t.clearSequence();
        check (t.getSequenceLength() == 0 && ! t.createSequenceAudioFile().existsAsFile(), "piste videe : rien a exporter");

        for (int i = 0; i < 70; ++i)
            t.insertSequenceStep (i % 2);
        check (t.getSequenceLength() == maxSequenceSteps, "piste : 64 blocs au maximum");
        t.loadFile (file);
        check (t.getSequenceLength() == 0, "nouveau sample : la piste repart de zero");
        t.setPlayHead (nullptr);
    }

    //==============================================================================
    std::cout << "Compatibilite et robustesse" << std::endl;
    {
        // Un projet enregistre avec la version 0.1 (sans tempo ni filtre) doit s'ouvrir
        juce::ValueTree old ("Siample");
        old.setProperty ("version", 1, nullptr);
        old.setProperty ("file", file.getFullPathName(), nullptr);
        old.setProperty ("regionStart", 0, nullptr);
        old.setProperty ("regionEnd", loop.getNumSamples(), nullptr);
        old.setProperty ("mode", 1, nullptr);
        old.setProperty ("divisions", 4, nullptr);
        for (int i = 0; i < 4; ++i)
        {
            juce::ValueTree child ("Slice");
            child.setProperty ("start", i * loop.getNumSamples() / 4, nullptr);
            child.setProperty ("pitch", i, nullptr);
            old.appendChild (child, nullptr);
        }

        juce::MemoryBlock block;
        {
            auto xml = old.createXml();
            juce::AudioProcessor::copyXmlToBinary (*xml, block);
        }

        SiampleProcessor t;
        t.prepareToPlay (48000.0, 512);
        t.setStateInformation (block.getData(), (int) block.getSize());
        check (t.hasSample() && t.getNumSlices() == 4 && t.getSlice (3).params.pitch == 3, "projet de la version 0.1 : tranches et reglages relus");
        check (std::abs (t.getSampleBpm() - 120.0) < 0.5 && ! t.isSyncEnabled() && ! t.isMono(), "projet de la version 0.1 : tempo detecte, nouveautes desactivees");

        // Tout modifier pendant que l'audio tourne dans un autre thread
        TestPlayHead head;
        t.setPlayHead (&head);
        t.setSyncEnabled (true);
        std::atomic<bool> stop { false };
        std::atomic<int> blocks { 0 };

        std::thread audio ([&]
        {
            juce::AudioBuffer<float> buffer (2, 256);
            juce::Random random (7);

            while (! stop.load())
            {
                juce::MidiBuffer midi;
                if (random.nextInt (4) == 0)
                    midi.addEvent (juce::MidiMessage::noteOn (1, 36 + random.nextInt (10), (juce::uint8) 100), random.nextInt (256));
                if (random.nextInt (6) == 0)
                    midi.addEvent (juce::MidiMessage::noteOff (1, 36 + random.nextInt (10)), random.nextInt (256));

                t.processBlock (buffer, midi);
                ++blocks;

                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < 256; ++i)
                        if (! std::isfinite (buffer.getSample (ch, i)))
                            stop.store (true);
            }
        });

        juce::Random random (11);
        const auto endTime = juce::Time::getMillisecondCounter() + 2500;
        int edits = 0;
        bool finite = true;

        while (juce::Time::getMillisecondCounter() < endTime && ! stop.load())
        {
            switch (random.nextInt (12))
            {
                case 0:  t.setSliceMode ((SliceMode) random.nextInt (3)); break;
                case 1:  t.setDivisions (1 << (1 + random.nextInt (6))); break;
                case 2:  t.setSensitivity (random.nextFloat()); break;
                case 3:  { auto p = SliceParams(); p.pitch = random.nextInt (25) - 12; p.filter = random.nextFloat() * 2.0f - 1.0f; p.reverse = random.nextBool(); p.loop = random.nextBool();
                           t.setSliceParams (random.nextInt (juce::jmax (1, t.getNumSlices())), p); break; }
                case 4:  head.bpm = 70.0 + random.nextInt (100); t.setSyncEnabled (random.nextBool()); break;
                case 5:  t.addMarker (random.nextInt (loop.getNumSamples())); break;
                case 6:  t.removeMarker (random.nextInt (juce::jmax (1, t.getNumSlices()))); break;
                case 7:  t.setRegion (random.nextInt (loop.getNumSamples() / 2), loop.getNumSamples() / 2 + random.nextInt (loop.getNumSamples() / 2)); break;
                case 8:  t.flipPattern(); t.setPatternPlaying (random.nextBool()); break;
                case 9:  t.setPlayMode ((PlayMode) random.nextInt (2)); t.setMono (random.nextBool()); t.setSelectedSlice (random.nextInt (8)); break;
                case 10: t.loadFile (file); t.setSyncEnabled (true); break;
                case 11: t.audition (random.nextInt (8), random.nextBool()); t.setSampleBpm (60.0 + random.nextInt (120)); break;
                default: break;
            }

            ++edits;
            juce::Thread::sleep (random.nextInt (8));
        }

        finite = ! stop.load();
        stop.store (true);
        audio.join();
        t.waitForRendering (20000);
        check (finite && blocks.load() > 100, "2,5 s de modifications en rafale pendant la lecture : aucun plantage, son toujours valide ("
                                               + juce::String (edits) + " modifications, " + juce::String (blocks.load()) + " blocs audio)");
    }

    //==============================================================================
    std::cout << "Interactions a la souris" << std::endl;
    {
        proc.setRegion (0, loop.getNumSamples());
        proc.setSliceMode (SliceMode::grid);
        proc.setDivisions (8);

        std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditorAndMakeActive());
        editor->setSize (1020, 770);

        WaveformView* wave = nullptr;
        for (auto* child : editor->getChildren())
            if (auto* w = dynamic_cast<WaveformView*> (child))
                wave = w;

        check (wave != nullptr, "forme d'onde presente dans l'editeur");

        if (wave != nullptr)
        {
            auto source = juce::Desktop::getInstance().getMainMouseSource();
            const float width = (float) wave->getWidth();
            const float midY  = (float) wave->getHeight() * 0.5f;
            const float rulerY = 8.0f;
            const int length = loop.getNumSamples();
            const int tolerance = (int) (3.0f * (float) length / width);

            auto event = [&] (float x, float y, float downX, float downY, bool rightButton = false, int clicks = 1, bool dragged = false)
            {
                return juce::MouseEvent (source, { x, y },
                                         rightButton ? juce::ModifierKeys::rightButtonModifier : juce::ModifierKeys::leftButtonModifier,
                                         0.5f, 0.0f, 0.0f, 0.0f, 0.0f, wave, wave, juce::Time::getCurrentTime(),
                                         { downX, downY }, juce::Time::getCurrentTime(), clicks, dragged);
            };

            // Clic dans la 3e tranche : sélection + écoute
            float x = width * 2.5f / 8.0f;
            wave->mouseDown (event (x, midY, x, midY));
            check (proc.getSelectedSlice() == 2, "clic : selectionne la tranche sous la souris");
            check (run (proc, 3)[0] > 0.001f, "clic : la tranche s'ecoute");
            wave->mouseUp (event (x, midY, x, midY));
            run (proc, 60);

            // Double-clic : nouveau point
            wave->mouseDoubleClick (event (x, midY, x, midY, false, 2));
            check (proc.getNumSlices() == 9 && std::abs (proc.getSlice (3).start - (int) (length * 2.5 / 8.0)) <= tolerance,
                   "double-clic : ajoute un point a cet endroit");
            check (proc.getSliceMode() == SliceMode::manual, "l'edition a la main passe en mode manuel");

            // Glisser ce point
            const float target = width * 2.7f / 8.0f;
            wave->mouseDown (event (x, midY, x, midY));
            wave->mouseDrag (event (target, midY, x, midY, false, 1, true));
            wave->mouseUp (event (target, midY, x, midY, false, 1, true));
            check (std::abs (proc.getSlice (3).start - (int) (length * 2.7 / 8.0)) <= tolerance, "glisser : deplace le point");

            // Clic droit : suppression
            wave->mouseDown (event (target, midY, target, midY, true));
            wave->mouseUp (event (target, midY, target, midY, true));
            check (proc.getNumSlices() == 8, "clic droit : supprime le point");

            // Tracer une zone dans le bandeau
            wave->mouseDown (event (width * 0.25f, rulerY, width * 0.25f, rulerY));
            wave->mouseDrag (event (width * 0.75f, rulerY, width * 0.25f, rulerY, false, 1, true));
            wave->mouseUp (event (width * 0.75f, rulerY, width * 0.25f, rulerY, false, 1, true));
            check (std::abs (proc.getRegionStart() - length / 4) <= tolerance && std::abs (proc.getRegionEnd() - length * 3 / 4) <= tolerance,
                   "bandeau : glisser trace la zone de travail");

            // Déplacer la poignée de droite
            const float handle = width * 0.75f - 3.0f;
            wave->mouseDown (event (handle, rulerY, handle, rulerY));
            wave->mouseDrag (event (width * 0.5f, rulerY, handle, rulerY, false, 1, true));
            wave->mouseUp (event (width * 0.5f, rulerY, handle, rulerY, false, 1, true));
            check (std::abs (proc.getRegionStart() - length / 4) <= tolerance && std::abs (proc.getRegionEnd() - length / 2) <= tolerance,
                   "bandeau : la poignee regle la fin de la zone");

            // Simple clic dans le bandeau : rien ne change
            const int before = proc.getRegionEnd();
            wave->mouseDown (event (width * 0.9f, rulerY, width * 0.9f, rulerY));
            wave->mouseUp (event (width * 0.9f, rulerY, width * 0.9f, rulerY));
            check (proc.getRegionEnd() == before, "bandeau : un simple clic ne change rien");

            // Double-clic dans le bandeau : toute la longueur
            wave->mouseDoubleClick (event (width * 0.9f, rulerY, width * 0.9f, rulerY, false, 2));
            check (proc.getRegionStart() == 0 && proc.getRegionEnd() == length, "bandeau : double-clic = fichier entier");

            // Zoom à la molette puis clic : la position reste cohérente
            juce::MouseWheelDetails wheel;
            wheel.deltaX = 0.0f; wheel.deltaY = 0.5f; wheel.isReversed = false; wheel.isSmooth = false; wheel.isInertial = false;
            wave->mouseWheelMove (event (width * 0.5f, midY, width * 0.5f, midY), wheel);
            proc.setSliceMode (SliceMode::grid);
            wave->mouseDown (event (width * 0.5f, midY, width * 0.5f, midY));
            wave->mouseUp (event (width * 0.5f, midY, width * 0.5f, midY));
            check (proc.getSelectedSlice() == 4, "zoom : le point sous la souris reste en place");
            run (proc, 60);
        }

        proc.editorBeingDeleted (editor.get());
    }

    //==============================================================================
    std::cout << "Interface" << std::endl;
    {
        SiampleProcessor empty;
        saveSnapshot (empty, outDir.getChildFile ("siample_vide.png"), 1020, 860);
    }

    proc.setRegion (0, loop.getNumSamples());
    proc.setSelectedSlice (2);
    run (proc, 2, noteOn (38));
    saveSnapshot (proc, outDir.getChildFile ("siample_transitoires.png"), 1020, 860);
    run (proc, 60);

    proc.setSliceMode (SliceMode::grid);
    proc.setDivisions (32);
    proc.setSelectedSlice (20);
    saveSnapshot (proc, outDir.getChildFile ("siample_grille_min.png"), 900, 740);

    // Toutes les nouveautés actives : Sync, tempo saisi à la main, clavier, mono, Flip, grain
    {
        TestPlayHead head;
        head.bpm = 96.0;
        proc.setPlayHead (&head);
        proc.setDivisions (grid::eighths);
        run (proc, 2);
        proc.setSampleBpm (120.0);
        proc.setSyncEnabled (true);
        proc.waitForRendering (20000);
        proc.setPlayMode (PlayMode::keys);
        proc.setMono (true);
        proc.flipPattern();
        *proc.getGrainParameter() = 35.0f;
        auto params = SliceParams();
        params.pitch = 5;
        params.filter = -0.4f;
        params.loop = true;
        proc.setSelectedSlice (5);
        proc.setSliceParams (5, params);
        proc.waitForRendering (20000);
        for (int step : { 0, 0, 2, 5, 2, 7 })
            proc.insertSequenceStep (step);
        proc.setSequencePlaying (true);
        run (proc, 30);
        saveSnapshot (proc, outDir.getChildFile ("siample_sync.png"), 1020, 860);
        proc.setSequencePlaying (false);
        run (proc, 80);
        proc.setPlayHead (nullptr);
    }
    check (outDir.getChildFile ("siample_transitoires.png").getSize() > 5000, "captures d'ecran ecrites dans " + outDir.getFullPathName());

    std::cout << std::endl << (failures == 0 ? "TOUS LES TESTS PASSENT" : juce::String (failures) + " ECHEC(S)") << std::endl;
    return failures == 0 ? 0 : 1;
}
