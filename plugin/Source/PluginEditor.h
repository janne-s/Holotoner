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
    void chooseImageFile();
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
    juce::TextButton loadImage_ {"Image..."};
    juce::TextButton reset_ {"Reset plane"};
    juce::Label wetLabel_;
    juce::Slider wet_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> wetAttachment_;
    juce::Label channelLabel_;
    juce::ComboBox channel_;
    juce::Label gainLabel_;
    juce::Slider gain_;
    juce::Label fillLabel_;
    juce::Slider fill_;
    juce::Label amountLabel_;
    juce::Slider amount_;
    juce::ToggleButton normalize_ {"Normalize"};
    juce::ToggleButton invert_ {"Invert"};
    juce::ComboBox projection_;
    juce::Label projectionLabel_, thresholdLabel_;
    juce::Slider threshold_;
    juce::ToggleButton binary_ {"B/W"};
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> projectionAttachment_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> thresholdAttachment_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> binaryAttachment_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> channelAttachment_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> gainAttachment_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> fillAttachment_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> amountAttachment_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> normalizeAttachment_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> invertAttachment_;
    juce::Label status_;
    std::unique_ptr<juce::FileChooser> chooser_;
    std::shared_ptr<const HolotonerProcessor::ImageDocument> currentImage_;
    std::shared_ptr<const holotoner::ImageMask> currentMask_;
    bool dragOver_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HolotonerEditor)
};
