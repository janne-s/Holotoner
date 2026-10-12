#pragma once
#include "PluginProcessor.h"
#include "Style.h"
#include <juce_audio_utils/juce_audio_utils.h>

class HolotonerEditor final : public juce::AudioProcessorEditor,
    public juce::FileDragAndDropTarget, private juce::Timer {
public:
    explicit HolotonerEditor(HolotonerProcessor&);
    ~HolotonerEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;
    bool isInterestedInFileDrag(const juce::StringArray&) override;
    void filesDropped(const juce::StringArray&,int,int) override;
    void fileDragEnter(const juce::StringArray&,int,int) override;
    void fileDragExit(const juce::StringArray&) override;
private:
    class PlaneView;
    class PolarView;
    class KnobCell;
    class TextPanel;
    void timerCallback() override;
    void chooseAudioFile();
    void chooseImageFile();
    void resetTransform();
    void refresh();
    HolotonerProcessor& processor_;
    HolotonerLook look_;
    std::unique_ptr<PlaneView> planeView_;
    std::unique_ptr<PolarView> polarView_;
    std::unique_ptr<TextPanel> textPanel_;
    std::array<std::unique_ptr<KnobCell>,7> geometry_,axisRates_;
    std::unique_ptr<KnobCell> gain_,fill_,threshold_,depth_,frame_,textRate_,wet_,amount_,tempo_;
    juce::Label title_,status_;
    juce::ComboBox source_,channel_,projection_,shape_,textLayout_;
    juce::TextButton loadAudio_ {"Audio..."},restart_ {"Restart"},play_ {"Stop"},
        loadImage_ {"Image..."},reset_ {"Reset plane"},textButton_ {"Text"},
        applyText_ {"Use text"},closeText_ {"Close"};
    juce::ToggleButton normalize_ {"Normalize"},invert_ {"Invert"},binary_ {"B/W"},textPlay_ {"Cycle"};
    juce::TextEditor textInput_;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>> comboAttachments_;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>> buttonAttachments_;
    std::unique_ptr<juce::FileChooser> chooser_;
    std::shared_ptr<const HolotonerProcessor::ImageDocument> currentImage_;
    std::shared_ptr<const holotoner::ImageMask> currentMask_;
    juce::String lastText_;
    bool dragOver_ = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HolotonerEditor)
};
