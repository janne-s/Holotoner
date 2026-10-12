#pragma once

#include "core/audio/SharedExchange.h"
#include "core/audio/SourcePlayer.h"
#include "core/dsp/ImageSpatializer.h"
#include "core/image/ImageMask.h"
#include "core/geometry/Motion.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <memory>
#include <mutex>

/**
 * Native wrapper: immutable media exchange, portable image DSP, real L/R
 * scope history, embedded image state, and shared Standalone/plug-in editor.
 */
class HolotonerProcessor final : public juce::AudioProcessor, private juce::Timer {
public:
    struct MeterState {
        float inputLeft = 0.0f;
        float inputRight = 0.0f;
        float outputLeft = 0.0f;
        float outputRight = 0.0f;
        float outputCorrelation = 0.0f;
    };

    struct ImageDocument {
        juce::Image image;
        juce::MemoryBlock normalizedPng;
        juce::String name;
        holotoner::RgbaImage rgba;
    };

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
    double getTailLengthSeconds() const override { return 0.1; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destination) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& state() { return parameters_; }
    holotoner::PlaneTransform planeTransform() const;
    holotoner::PlaneTransform displayedPlaneTransform() const;
    static constexpr std::array<const char*,7> kMotionAxes {"pitch","yaw","roll","position_x","position_y","scale_x","scale_y"};
    MeterState meterState() const;
    struct ScopeSample { float inputLeft, inputRight, outputLeft, outputRight; };
    // UI-only read of a bounded, non-tearing atomic sample history.
    int scopeSamples(ScopeSample* destination, int capacity) const;

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

    void loadImageFile(const juce::File& file);
    void setText(const juce::String& text);
    juce::String text() const;
    int textFrameCount() const;
    std::shared_ptr<const ImageDocument> imageDocument() const;
    std::shared_ptr<const holotoner::ImageMask> imageMask() const;
    bool isLoadingImage() const { return imageLoadsPending_.load() > 0 || bankPending_.load()
        || mediaVersion_.load() != bankVersion_.load(); }
    juce::String lastImageMessage() const;
    static bool isSupportedImageFile(const juce::File& file);

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout parameterLayout();
    void timerCallback() override;
    void setSourceMessage(const juce::String& message);
    void loadImageData(juce::MemoryBlock data, juce::String name, std::uint64_t request, bool embedded = false);
    void setImageMessage(const juce::String& message);
    void rebuildImageMaskIfNeeded();
    void renderText(juce::String text, int layout, std::uint64_t request);

    juce::AudioProcessorValueTreeState parameters_;
    std::atomic<float>* positionX_ = nullptr;
    std::atomic<float>* positionY_ = nullptr;
    std::atomic<float>* scaleX_ = nullptr;
    std::atomic<float>* scaleY_ = nullptr;
    std::atomic<float>* pitch_ = nullptr;
    std::atomic<float>* yaw_ = nullptr;
    std::atomic<float>* roll_ = nullptr;
    std::atomic<float>* wet_ = nullptr;
    std::atomic<float>* imageChannel_ = nullptr;
    std::atomic<float>* imageGain_ = nullptr;
    std::atomic<float>* imageFill_ = nullptr;
    std::atomic<float>* imageNormalize_ = nullptr;
    std::atomic<float>* imageInvert_ = nullptr;
    std::atomic<float>* imageAmount_ = nullptr;
    std::atomic<float>* imageProjection_ = nullptr;
    std::atomic<float>* imageThreshold_ = nullptr;
    std::atomic<float>* imageBinary_ = nullptr;
    std::atomic<float> *motionShape_, *motionDepth_,
        *textLayout_, *textPlay_, *textFrame_, *textRate_, *textSync_, *localBpm_;
    std::array<std::atomic<float>*,7> axisRate_ {}, axisSync_ {};
    std::array<holotoner::CycleClock,7> axisClocks_;
    holotoner::CycleClock textClock_;
    double clockSampleRate_ = 48000;
    std::uint64_t motionSamples_ = 0;
    std::array<std::atomic<float>, 7> displayedPlane_ {};
    std::atomic<bool> hasDisplayedPlane_ {false};
    std::atomic<int> displayedTextFrame_ {0};
    holotoner::ImageSpatializer engine_;
    std::atomic<float> inputLeft_ {0.0f};
    std::atomic<float> inputRight_ {0.0f};
    std::atomic<float> outputLeft_ {0.0f};
    std::atomic<float> outputRight_ {0.0f};
    std::atomic<float> outputCorrelation_ {0.0f};
    float meterInputLeft_ = 0.0f;       ///< audio thread
    float meterInputRight_ = 0.0f;      ///< audio thread
    float meterOutputLeft_ = 0.0f;      ///< audio thread
    float meterOutputRight_ = 0.0f;     ///< audio thread
    float meterDecayPerSample_ = 0.0f;  ///< audio thread
    static constexpr std::size_t kScopeHistory = 32768;
    std::array<std::atomic<std::uint64_t>, kScopeHistory> scopeInput_ {}, scopeOutput_ {};
    std::atomic<std::uint64_t> scopePublished_ {0};
    std::uint64_t scopeWritten_ = 0; ///< audio thread only

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
    std::atomic<int> imageLoadsPending_ {0};
    std::atomic<std::uint64_t> imageRequest_ {0};
    std::atomic<std::uint64_t> mediaVersion_ {0}, bankVersion_ {0};
    mutable std::mutex imageMutex_;
    std::shared_ptr<const ImageDocument> image_;
    juce::String text_;
    int renderedTextLayout_ = -1;
    int rejectedTextLayout_ = -1;
    std::vector<std::shared_ptr<const ImageDocument>> textFrames_;
    // Preserve embedded media if a host saves immediately while asynchronous restore is still decoding.
    std::unique_ptr<juce::XmlElement> pendingMediaState_;
    juce::String imageMessage_;
    struct ImageFrame {
        std::shared_ptr<const ImageDocument> document;
        std::shared_ptr<const holotoner::ImageMask> mask;
    };
    struct ImageBank {
        std::vector<ImageFrame> frames;
    };
    holotoner::SharedExchange<ImageBank> banks_;
    std::vector<std::shared_ptr<const ImageDocument>> maskImages_;
    std::atomic<bool> bankPending_ {false};
    holotoner::ImageMaskSettings builtMaskSettings_;
    std::atomic<bool> hasBuiltMaskSettings_ {false};
    /** Jobs refer to media members, so the destructor waits for them. */
    juce::ThreadPool mediaLoader_ {1};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HolotonerProcessor)
};
