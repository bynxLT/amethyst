#pragma once

#include "AudioEngine.h"
#include "Widgets.h"

// Nightcore / slowed + reverb maker (AMETHYST Ultra Pro).
// Load a song, pick speed / reverb / bass / volume, preview it, then export it or drop it into the project.
// Speed changes pitch together with tempo, the classic nightcore sound.
class NightcoreMaker : public juce::Component,
                       public juce::FileDragAndDropTarget,
                       private juce::Timer
{
public:
    NightcoreMaker (AudioEngine&, std::function<void (const juce::File&)> addToProject);
    ~NightcoreMaker() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int, int) override;

    void loadFile (const juce::File&);

    // Offline render used by the window and the self-test.
    struct Settings { double speed = 1.25; float reverb = 0.10f; float bassDb = 0.0f; float volume = 1.0f; };
    static juce::AudioBuffer<float> render (const juce::AudioBuffer<float>& source, double sampleRate, const Settings&);

    static void show (AudioEngine&, juce::Component* centreAround, std::function<void (const juce::File&)> addToProject);

private:
    void timerCallback() override;
    void settingsChanged();
    void startRender();
    void setMode (int mode);
    void togglePlay();
    void exportResult();
    void addResultToProject();
    juce::Rectangle<int> waveArea() const;
    void seekToX (int x);
    Settings currentSettings() const;

    AudioEngine& engine;
    std::function<void (const juce::File&)> onAddToProject;

    std::shared_ptr<juce::AudioBuffer<float>> source;
    std::shared_ptr<const juce::AudioBuffer<float>> rendered;
    std::vector<float> peaks;          // waveform overview of `rendered`
    juce::String songName;
    double sampleRate = 48000.0;

    juce::TextButton loadBtn { "Load song..." }, playBtn { "Play" }, exportBtn { "Export..." }, addBtn { "Add to project" };
    juce::TextButton nightcoreBtn { "Nightcore" }, slowedBtn { "Slowed + Reverb" }, customBtn { "Custom" };
    ValueSlider speed, reverb, bass, volume;
    juce::Label speedL, reverbL, bassL, volumeL;

    juce::ThreadPool pool { 1 };
    std::atomic<int> generation { 0 };
    bool dirty = false, rendering = false;
    juce::uint32 lastChange = 0;
    std::unique_ptr<juce::FileChooser> chooser;
    std::shared_ptr<int> alive = std::make_shared<int> (0);

    JUCE_DECLARE_NON_COPYABLE (NightcoreMaker)
};
