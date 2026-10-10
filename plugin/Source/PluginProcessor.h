#pragma once

#include "core/dsp/StereoWidth.h"

#include <juce_audio_processors/juce_audio_processors.h>

/**
 * Development wrapper around the first tested core invariant.
 *
 * This is deliberately not a released Holotoner UI. The production editor,
 * image state, analyser and Standalone file source are added together so an
 * incomplete shell is never presented as a product version.
 */
class HolotonerProcessor final : public juce::AudioProcessor {
public:
    HolotonerProcessor();

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Holotoner"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destination) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout parameterLayout();

    juce::AudioProcessorValueTreeState parameters_;
    std::atomic<float>* yaw_ = nullptr;
    std::atomic<float>* width_ = nullptr;
    std::atomic<float>* wet_ = nullptr;
    holotoner::StereoWidth engine_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HolotonerProcessor)
};
