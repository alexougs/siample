#pragma once

#include "../PluginProcessor.h"
#include "Theme.h"

namespace siample
{

/** Navigateur de samples : un dossier favori (les téléchargements Tracklib, par exemple),
    pré-écoute d'un clic, chargement d'un double-clic. Le dossier est retenu d'une session à l'autre. */
class BrowserPanel final : public juce::Component,
                           private juce::ListBoxModel
{
public:
    std::function<void (const juce::File&)> onLoad;

    explicit BrowserPanel (SiampleProcessor& p) : proc (p)
    {
        juce::PropertiesFile::Options options;
        options.applicationName     = "Siample";
        options.filenameSuffix      = ".settings";
        options.folderName          = "Siample";
        options.osxLibrarySubFolder = "Application Support";
        settings = std::make_unique<juce::PropertiesFile> (options);

        folderButton.setButtonText (fr ("Dossier…"));
        folderButton.setTooltip (fr ("Choisis le dossier où tu ranges tes samples. Siample s'en souvient."));
        folderButton.onClick = [this] { chooseFolder(); };
        addAndMakeVisible (folderButton);

        upButton.setButtonText (fr ("Remonter"));
        upButton.setTooltip (fr ("Dossier parent"));
        upButton.onClick = [this] { setFolder (folder.getParentDirectory()); };
        addAndMakeVisible (upButton);

        stopButton.setButtonText ("Stop");
        stopButton.setTooltip (fr ("Arrête la pré-écoute"));
        stopButton.onClick = [this] { proc.stopPreview(); playingRow = -1; list.repaint(); };
        addAndMakeVisible (stopButton);

        list.setModel (this);
        list.setRowHeight (24);
        list.setColour (juce::ListBox::backgroundColourId, theme::well);
        list.setColour (juce::ListBox::outlineColourId, juce::Colours::transparentBlack);
        addAndMakeVisible (list);

        const juce::File saved (settings->getValue ("browserFolder"));
        const auto music = juce::File::getSpecialLocation (juce::File::userMusicDirectory);
        setFolder (saved.isDirectory() ? saved
                                       : music.isDirectory() ? music
                                                             : juce::File::getSpecialLocation (juce::File::userHomeDirectory));
    }

    ~BrowserPanel() override
    {
        proc.stopPreview();
        list.setModel (nullptr);
    }

    void setFolder (const juce::File& newFolder)
    {
        if (! newFolder.isDirectory())
            return;

        folder = newFolder;
        entries.clear();
        playingRow = -1;

        juce::Array<juce::File> folders, files;

        for (const auto& entry : juce::RangedDirectoryIterator (folder, false, "*", juce::File::findFilesAndDirectories | juce::File::ignoreHiddenFiles))
        {
            const auto file = entry.getFile();

            if (entry.isDirectory())
                folders.add (file);
            else if (proc.getFormatManager().findFormatForFileExtension (file.getFileExtension()) != nullptr)
                files.add (file);
        }

        folders.sort();
        files.sort();
        entries.addArray (folders);
        numFolders = folders.size();
        entries.addArray (files);

        settings->setValue ("browserFolder", folder.getFullPathName());
        settings->saveIfNeeded();

        list.updateContent();
        list.deselectAllRows();
        list.scrollToEnsureRowIsOnscreen (0);
        repaint();
    }

    juce::File getFolder() const   { return folder; }

    void timerTick()
    {
        if (playingRow >= 0 && ! proc.isPreviewPlaying())
        {
            playingRow = -1;
            list.repaint();
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (10, 10);
        area.removeFromTop (24);
        pathArea = area.removeFromTop (18);
        area.removeFromTop (6);

        auto buttons = area.removeFromTop (28);
        const int width = (buttons.getWidth() - 12) / 3;
        folderButton.setBounds (buttons.removeFromLeft (width));
        buttons.removeFromLeft (6);
        upButton.setBounds (buttons.removeFromLeft (width));
        buttons.removeFromLeft (6);
        stopButton.setBounds (buttons);

        area.removeFromTop (8);
        hintArea = area.removeFromBottom (30);
        area.removeFromBottom (4);
        list.setBounds (area);
    }

    void paint (juce::Graphics& g) override
    {
        theme::drawCard (g, getLocalBounds());
        theme::drawSectionLabel (g, "Samples", getLocalBounds().reduced (12, 10).removeFromTop (24));

        g.setColour (theme::text);
        g.setFont (theme::font (12.0f, true));
        g.drawText (folder.getFileName(), pathArea.reduced (2, 0), juce::Justification::centredLeft, true);

        g.setColour (theme::textDim);
        g.setFont (theme::font (11.0f));
        g.drawFittedText (entries.isEmpty() ? fr ("Aucun fichier audio dans ce dossier")
                                            : fr ("Clic : écouter. Double-clic : charger dans le sample ") + deckName (proc.getActiveDeck()) + ".",
                          hintArea.reduced (2, 0), juce::Justification::centredLeft, 2);
    }

private:
    int getNumRows() override   { return entries.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        if (row < 0 || row >= entries.size())
            return;

        const bool isFolder = row < numFolders;
        const bool playing = row == playingRow;

        if (selected || playing)
        {
            g.setColour (playing ? theme::accent.withAlpha (0.22f) : theme::panelLight);
            g.fillRoundedRectangle (1.0f, 1.0f, (float) width - 2.0f, (float) height - 2.0f, 4.0f);
        }

        // Petite icône : dossier, ou triangle de lecture
        const float cy = (float) height * 0.5f;
        juce::Path icon;

        if (isFolder)
        {
            icon.addRoundedRectangle (7.0f, cy - 4.0f, 12.0f, 9.0f, 1.5f);
            icon.addRoundedRectangle (7.0f, cy - 6.0f, 6.0f, 4.0f, 1.0f);
            g.setColour (theme::textDim);
        }
        else
        {
            icon.addTriangle (9.0f, cy - 5.0f, 9.0f, cy + 5.0f, 17.0f, cy);
            g.setColour (playing ? theme::accent : theme::outline.brighter (0.4f));
        }

        g.fillPath (icon);

        g.setColour (isFolder ? theme::textDim : theme::text);
        g.setFont (theme::font (12.5f, playing));
        g.drawText (isFolder ? entries[row].getFileName() : entries[row].getFileNameWithoutExtension(),
                    26, 0, width - 30, height, juce::Justification::centredLeft, true);
    }

    void listBoxItemClicked (int row, const juce::MouseEvent&) override
    {
        if (row < 0 || row >= entries.size() || row < numFolders)
            return;

        if (row == playingRow && proc.isPreviewPlaying())
        {
            proc.stopPreview();
            playingRow = -1;
        }
        else
        {
            playingRow = proc.startPreview (entries[row]).isEmpty() ? row : -1;
        }

        list.repaint();
    }

    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override
    {
        if (row < 0 || row >= entries.size())
            return;

        if (row < numFolders)
        {
            setFolder (entries[row]);
            return;
        }

        proc.stopPreview();
        playingRow = -1;

        if (onLoad != nullptr)
            onLoad (entries[row]);
    }

    void chooseFolder()
    {
        chooser = std::make_unique<juce::FileChooser> (fr ("Choisis ton dossier de samples"), folder);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [this] (const juce::FileChooser& fc)
                              {
                                  if (fc.getResult().isDirectory())
                                      setFolder (fc.getResult());
                              });
    }

    SiampleProcessor& proc;
    std::unique_ptr<juce::PropertiesFile> settings;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::File folder;
    juce::Array<juce::File> entries;
    int numFolders = 0;
    int playingRow = -1;

    juce::TextButton folderButton, upButton, stopButton;
    juce::ListBox list;
    juce::Rectangle<int> pathArea, hintArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BrowserPanel)
};

} // namespace siample
