#pragma once

#include "core/audio/SharedExchange.h"
#include "core/audio/SourcePlayer.h"
#include "core/dsp/StereoWidth.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <mutex>

/**
 * Development wrapper around the first tested core invariant.
 *
 * This is deliberately not a released Holotoner UI. The production editor,
 * image state and analyser are still built as coherent development stages so
 * an incomplete shell is never presented as a product version.
 */
class HolotonerProcessor final : public juce::AudioProcessor, private juce::Timer {
public:
    HolotonerProcessor();
    ~HolotonerProcessor() override;

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

    juce::AudioProcessorValueTreeState& state() { return parameters_; }

    void setSourceKind(holotoner::SourceKind kind) { sourceKind_.store(static_cast<int>(kind)); }
    holotoner::SourceKind sourceKind() const
    {
        return static_cast<holotoner::SourceKind>(sourceKind_.load());
    }
    void loadSourceFile(const juce::File& file, bool switchToFile = true);
    void restartSource() { restartRequested_.store(true); }
    void setSourcePlaying(bool playing) { sourcePlaying_.store(playing); }
    void toggleSourcePlaying() { sourcePlaying_.store(!sourcePlaying_.load()); }
    bool isSourcePlaying() const { return sourcePlaying_.load(); }
    bool isWaitingForHost() const { return waitingForHost_.load(std::memory_order_relaxed); }
    bool isLoadingSource() const { return sourceLoadsPending_.load() > 0; }
    juce::String sourceName() const;
    juce::File sourceFile() const;
    juce::String lastSourceMessage() const;
    static bool isSupportedAudioFile(const juce::File& file);

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout parameterLayout();
    void timerCallback() override;
    void setSourceMessage(const juce::String& message);

    juce::AudioProcessorValueTreeState parameters_;
    std::atomic<float>* yaw_ = nullptr;
    std::atomic<float>* width_ = nullptr;
    std::atomic<float>* wet_ = nullptr;
    holotoner::StereoWidth engine_;

    holotoner::SourcePlayer sourcePlayer_;
    holotoner::SharedExchange<holotoner::SourceAudio> sources_;
    std::atomic<int> sourceKind_ {static_cast<int>(holotoner::SourceKind::Input)};
    std::atomic<bool> restartRequested_ {false};
    std::atomic<bool> sourcePlaying_ {true};
    std::atomic<bool> waitingForHost_ {false};
    holotoner::SourceKind playedSourceKind_ = holotoner::SourceKind::Input;
    std::atomic<int> sourceLoadsPending_ {0};
    mutable std::mutex sourceInfoMutex_;
    juce::File sourceFile_;
    juce::String sourceMessage_;
    /** Jobs refer to the source members, so the destructor waits for them. */
    juce::ThreadPool sourceLoader_ {1};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HolotonerProcessor)
};
