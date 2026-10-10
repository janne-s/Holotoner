#pragma once

#include "PluginProcessor.h"
#include "Style.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <memory>

class HolotonerEditor final : public juce::AudioProcessorEditor,
                              public juce::FileDragAndDropTarget,
                              private juce::Timer {
public:
    explicit HolotonerEditor(HolotonerProcessor& owner);
    ~HolotonerEditor() override;

    void paint(juce::Graphics& graphics) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;

    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;
    void fileDragEnter(const juce::StringArray&, int, int) override;
    void fileDragExit(const juce::StringArray&) override;

private:
    class PlaneView;
    class PolarView;

    void timerCallback() override;
    void chooseAudioFile();
    void resetTransform();
    void refresh();

    HolotonerProcessor& processor_;
    HolotonerLook look_;
    std::unique_ptr<PlaneView> planeView_;
    std::unique_ptr<PolarView> polarView_;
    juce::Label title_;
    juce::ComboBox source_;
    juce::TextButton loadAudio_ {"Audio..."};
    juce::TextButton restart_ {"Restart"};
    juce::TextButton play_ {"Stop"};
    juce::TextButton reset_ {"Reset plane"};
    juce::Label wetLabel_;
    juce::Slider wet_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> wetAttachment_;
    juce::Label status_;
    std::unique_ptr<juce::FileChooser> chooser_;
    bool dragOver_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HolotonerEditor)
};
