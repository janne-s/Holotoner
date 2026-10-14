#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "TextSource.h"
#include "core/image/ShapeGenerator.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <memory>

namespace {

constexpr int kStateVersion = 13;
constexpr int kOldestReadableStateVersion = 2;
constexpr juce::int64 kMaximumImageFileBytes = 32 * 1024 * 1024;
constexpr int kMaximumImageDimension = 1024;
static_assert(std::atomic<float>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

float boundedValue(const std::atomic<float>* value,float fallback,float minimum,float maximum)
{
    const auto raw = value->load(std::memory_order_relaxed);
    return std::clamp(std::isfinite(raw) ? raw : fallback,minimum,maximum);
}

juce::String parametersNameForAxis(const juce::String& key)
{
    if (key == "pitch") return "Rotation X";
    if (key == "yaw") return "Rotation Y";
    if (key == "roll") return "Rotation Z";
    if (key == "position_x") return "Position X";
    if (key == "position_y") return "Position Y";
    return key == "scale_x" ? "Scale X" : "Scale Y";
}

// Control/loader threads only. One normalization path for imported, rendered and restored media.
std::shared_ptr<const HolotonerProcessor::ImageDocument> makeDocument(juce::Image decoded, juce::String name,
                                                                  const juce::MemoryBlock* embeddedPng = nullptr)
{
    if (decoded.isNull()) return {};
    const auto largest = std::max(decoded.getWidth(), decoded.getHeight());
    if (largest > kMaximumImageDimension) {
        embeddedPng = nullptr;
        const auto scale = static_cast<double>(kMaximumImageDimension)/largest;
        decoded = decoded.rescaled(std::max(1, juce::roundToInt(scale*decoded.getWidth())),
            std::max(1, juce::roundToInt(scale*decoded.getHeight())), juce::Graphics::highResamplingQuality);
    }
    decoded = decoded.convertedToFormat(juce::Image::ARGB);
    auto document = std::make_shared<HolotonerProcessor::ImageDocument>();
    if (embeddedPng != nullptr) document->normalizedPng = *embeddedPng;
    else {
        juce::MemoryOutputStream encoded(document->normalizedPng, false);
        juce::PNGImageFormat png;
        if (!png.writeImageToStream(decoded, encoded)) return {};
    }
    // Analyze exactly the serialized bitmap, not a pre-encode premultiplied surface.
    // Restoring retains these bytes, avoiding repeated alpha rounding in colour emoji.
    decoded = juce::ImageFileFormat::loadFrom(document->normalizedPng.getData(),document->normalizedPng.getSize());
    if (decoded.isNull()) return {};
    document->image = decoded;
    document->name = std::move(name);
    auto& rgba = document->rgba;
    rgba.width = decoded.getWidth();
    rgba.height = decoded.getHeight();
    rgba.pixels.resize(static_cast<std::size_t>(rgba.width*rgba.height)*4);
    const juce::Image::BitmapData pixels(decoded, juce::Image::BitmapData::readOnly);
    for (int y = 0; y < rgba.height; ++y)
        for (int x = 0; x < rgba.width; ++x) {
            const auto colour = pixels.getPixelColour(x,y);
            const auto at = static_cast<std::size_t>((y*rgba.width+x)*4);
            rgba.pixels[at] = colour.getRed(); rgba.pixels[at+1] = colour.getGreen();
            rgba.pixels[at+2] = colour.getBlue(); rgba.pixels[at+3] = colour.getAlpha();
        }
    return document;
}

juce::String sourceKindKey(holotoner::SourceKind kind)
{
    switch (kind) {
    case holotoner::SourceKind::Input:
        return "input";
    case holotoner::SourceKind::File:
        return "file";
    case holotoner::SourceKind::Clicks:
        return "clicks";
    }
    return "input";
}

holotoner::SourceKind sourceKindFromKey(const juce::String& key)
{
    if (key == "file")
        return holotoner::SourceKind::File;
    if (key == "clicks")
        return holotoner::SourceKind::Clicks;
    return holotoner::SourceKind::Input;
}

struct BlockMeter {
    float left = 0.0f;
    float right = 0.0f;
    float correlation = 0.0f;
};

BlockMeter measureBlock(const float* left, const float* right, int samples)
{
    if (left == nullptr || right == nullptr || samples <= 0)
        return {};
    double leftSquare = 0.0;
    double rightSquare = 0.0;
    double cross = 0.0;
    for (int i = 0; i < samples; ++i) {
        const auto l = std::isfinite(left[i]) ? static_cast<double>(left[i]) : 0.0;
        const auto r = std::isfinite(right[i]) ? static_cast<double>(right[i]) : 0.0;
        leftSquare += l * l;
        rightSquare += r * r;
        cross += l * r;
    }
    const auto count = static_cast<double>(samples);
    const auto denominator = std::sqrt(leftSquare * rightSquare);
    return {static_cast<float>(std::sqrt(leftSquare / count)),
            static_cast<float>(std::sqrt(rightSquare / count)),
            denominator > 1.0e-20 ? static_cast<float>(std::clamp(cross / denominator, -1.0, 1.0)) : 0.0f};
}

} // namespace

HolotonerProcessor::HolotonerProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters_(*this, nullptr, "HolotonerState", parameterLayout())
{
    positionX_ = parameters_.getRawParameterValue("position_x");
    positionY_ = parameters_.getRawParameterValue("position_y");
    scaleX_ = parameters_.getRawParameterValue("scale_x");
    scaleY_ = parameters_.getRawParameterValue("scale_y");
    pitch_ = parameters_.getRawParameterValue("pitch");
    yaw_ = parameters_.getRawParameterValue("yaw");
    roll_ = parameters_.getRawParameterValue("roll");
    wet_ = parameters_.getRawParameterValue("wet");
    imageChannel_ = parameters_.getRawParameterValue("image_channel");
    imageGain_ = parameters_.getRawParameterValue("image_gain");
    imageFill_ = parameters_.getRawParameterValue("image_fill");
    imageNormalize_ = parameters_.getRawParameterValue("image_normalize");
    imageInvert_ = parameters_.getRawParameterValue("image_invert");
    imageAmount_ = parameters_.getRawParameterValue("image_amount");
    spaceFeedback_ = parameters_.getRawParameterValue("space_feedback");
    spaceEcho_ = parameters_.getRawParameterValue("space_echo");
    spaceResonate_ = parameters_.getRawParameterValue("space_resonate");
    spaceInterfere_ = parameters_.getRawParameterValue("space_interfere");
    imageEdge_ = parameters_.getRawParameterValue("image_edge");
    imageSmooth_ = parameters_.getRawParameterValue("image_smooth");
    imageRest_ = parameters_.getRawParameterValue("image_rest");
    outDelta_ = parameters_.getRawParameterValue("out_delta");
    echoTime_ = parameters_.getRawParameterValue("space_echo_time");
    echoSync_ = parameters_.getRawParameterValue("space_echo_time_sync");
    shapeKind_ = parameters_.getRawParameterValue("shape_kind");
    shapeOrder_ = parameters_.getRawParameterValue("shape_order");
    imageProjection_ = parameters_.getRawParameterValue("image_projection");
    imageThreshold_ = parameters_.getRawParameterValue("image_threshold");
    imageBinary_ = parameters_.getRawParameterValue("image_binary");
    motionShape_ = parameters_.getRawParameterValue("motion_shape");
    motionDepth_ = parameters_.getRawParameterValue("motion_depth");
    for (std::size_t i = 0; i < kMotionAxes.size(); ++i) {
        axisRate_[i] = parameters_.getRawParameterValue(juce::String(kMotionAxes[i])+"_rate");
        axisSync_[i] = parameters_.getRawParameterValue(juce::String(kMotionAxes[i])+"_rate_sync");
    }
    textLayout_ = parameters_.getRawParameterValue("text_layout");
    textPlay_ = parameters_.getRawParameterValue("text_play");
    textFrame_ = parameters_.getRawParameterValue("text_frame");
    textRate_ = parameters_.getRawParameterValue("text_rate");
    textSync_ = parameters_.getRawParameterValue("text_rate_sync");
    localBpm_ = parameters_.getRawParameterValue("tempo");
    jassert(positionX_ != nullptr && positionY_ != nullptr && scaleX_ != nullptr && scaleY_ != nullptr
            && pitch_ != nullptr && yaw_ != nullptr && roll_ != nullptr && wet_ != nullptr
            && imageChannel_ != nullptr && imageGain_ != nullptr
            && imageFill_ != nullptr && imageNormalize_ != nullptr && imageInvert_ != nullptr
            && imageAmount_ != nullptr && spaceFeedback_ != nullptr && spaceEcho_ != nullptr
            && spaceResonate_ != nullptr && spaceInterfere_ != nullptr && shapeKind_ != nullptr
            && shapeOrder_ != nullptr && imageEdge_ != nullptr && imageSmooth_ != nullptr && imageRest_ != nullptr && outDelta_ != nullptr
            && echoTime_ != nullptr && echoSync_ != nullptr);
    setLatencySamples(engine_.latencySamples());
    if (wrapperType == wrapperType_Standalone)
        setSourceKind(holotoner::SourceKind::Clicks);
    shapeActive_ = true; // new instances start with the selected default shape
    startTimerHz(30);
}

HolotonerProcessor::~HolotonerProcessor()
{
    stopTimer();
    mediaLoader_.removeAllJobs(true, 5000);
}

juce::AudioProcessorValueTreeState::ParameterLayout HolotonerProcessor::parameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    const auto normalized = juce::NormalisableRange<float> {-1.0f, 1.0f, 0.001f};
    const auto degrees = juce::NormalisableRange<float> {-180.0f, 180.0f, 0.01f};
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID {"position_x", 1}, "Position X", normalized, 0.0f));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID {"position_y", 1}, "Position Y", normalized, 0.0f));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID {"scale_x", 1}, "Scale X", juce::NormalisableRange<float> {0.05f, 4.0f, 0.001f}, 1.0f));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID {"scale_y", 1}, "Scale Y", juce::NormalisableRange<float> {0.05f, 4.0f, 0.001f}, 1.0f));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID {"pitch", 1}, "Rotation X", degrees, 0.0f,
        juce::AudioParameterFloatAttributes().withLabel("deg")));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID {"yaw", 1}, "Rotation Y", degrees, 0.0f,
        juce::AudioParameterFloatAttributes().withLabel("deg")));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID {"roll", 1}, "Rotation Z", degrees, 0.0f,
        juce::AudioParameterFloatAttributes().withLabel("deg")));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID {"wet", 1}, "Wet", juce::NormalisableRange<float> {0.0f, 1.0f, 0.001f}, 1.0f,
        juce::AudioParameterFloatAttributes()
            .withLabel("%")
            .withStringFromValueFunction([](float value, int) { return juce::String(100.0f * value, 0); })
            .withValueFromStringFunction([](const juce::String& text) { return 0.01f * text.getFloatValue(); })));
    layout.add(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID {"image_channel", 1}, "Image Channel",
        juce::StringArray {"Off", "Luma", "Alpha", "Red", "Green", "Blue"}, 1));
    layout.add(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID {"image_projection", 1}, "Image Projection",
        juce::StringArray {"XY rectangle", "Polar warp"}, 0));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID {"image_threshold", 1}, "Image Threshold",
        juce::NormalisableRange<float> {0.0f, 1.0f, 0.001f}, 0.0f));
    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID {"image_binary", 1}, "Binary Image", false));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID {"image_gain", 1}, "Image Gain",
        juce::NormalisableRange<float> {0.0f, 8.0f, 0.01f, 0.5f}, 1.0f));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID {"image_fill", 1}, "Image Fill",
        juce::NormalisableRange<float> {0.0f, 1.0f, 0.001f}, 0.0f,
        juce::AudioParameterFloatAttributes()
            .withLabel("%")
            .withStringFromValueFunction([](float value, int) { return juce::String(100.0f * value, 0); })
            .withValueFromStringFunction([](const juce::String& text) { return 0.01f * text.getFloatValue(); })));
    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID {"image_normalize", 1}, "Normalize Image", true));
    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID {"image_invert", 1}, "Invert Image", false));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID {"image_amount", 1}, "Image Amount",
        juce::NormalisableRange<float> {0.0f, 1.0f, 0.001f}, 1.0f,
        juce::AudioParameterFloatAttributes()
            .withLabel("%")
            .withStringFromValueFunction([](float value, int) { return juce::String(100.0f * value, 0); })
            .withValueFromStringFunction([](const juce::String& text) { return 0.01f * text.getFloatValue(); })));
    for (const auto& [id,name] : {std::pair {"space_interfere","Interfere"},std::pair {"space_echo","Echo"},
                                  std::pair {"space_feedback","Feedback"},std::pair {"space_resonate","Resonate"}})
        layout.add(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID {id, 1}, name, juce::NormalisableRange<float> {0.0f, 1.0f, 0.001f}, 0.0f,
            juce::AudioParameterFloatAttributes()
                .withLabel("%")
                .withStringFromValueFunction([](float value, int) { return juce::String(100.0f * value, 0); })
                .withValueFromStringFunction([](const juce::String& text) { return 0.01f * text.getFloatValue(); })));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID {"image_edge", 1}, "Snap", juce::NormalisableRange<float> {0.0f, 1.0f, 0.001f}, 1.0f,
        juce::AudioParameterFloatAttributes()
            .withLabel("%")
            .withStringFromValueFunction([](float value, int) { return juce::String(100.0f * value, 0); })
            .withValueFromStringFunction([](const juce::String& text) { return 0.01f * text.getFloatValue(); })));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID {"image_rest", 1}, "Rest", juce::NormalisableRange<float> {-1.0f, 1.0f, 0.001f}, 0.0f,
        juce::AudioParameterFloatAttributes()
            .withLabel("%")
            .withStringFromValueFunction([](float value, int) { return juce::String(100.0f * value, 0); })
            .withValueFromStringFunction([](const juce::String& text) { return 0.01f * text.getFloatValue(); })));
    layout.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID {"out_delta", 1}, "Delta", false));
    // Echo Time in seconds (shown in ms); its ♪ note value replaces it, resolved at the host tempo.
    layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID {"space_echo_time", 1}, "Echo Time",
        juce::NormalisableRange<float> {0.04f, 1.5f, 0.001f, 0.5f}, 0.25f,
        juce::AudioParameterFloatAttributes().withLabel("ms")
            .withStringFromValueFunction([](float value, int) { return juce::String(juce::roundToInt(1000.0f * value)); })
            .withValueFromStringFunction([](const juce::String& text) { return 0.001f * text.getFloatValue(); })));
    juce::StringArray shapeNames;
    for (const auto* name : holotoner::kShapeNames) shapeNames.add(name);
    layout.add(std::make_unique<juce::AudioParameterChoice>(juce::ParameterID {"shape_kind", 1},
        "Shape Kind", shapeNames, static_cast<int>(holotoner::ShapeKind::Smiley)));
    layout.add(std::make_unique<juce::AudioParameterInt>(juce::ParameterID {"shape_order", 1},
        "Shape Order", holotoner::kShapeMinimumOrder, holotoner::kShapeMaximumOrder, 3));
    layout.add(std::make_unique<juce::AudioParameterChoice>(juce::ParameterID {"motion_shape", 1},
        "Motion Shape", juce::StringArray {"Swing", "Spin"}, 0));
    layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID {"motion_depth", 1},
        "Motion Depth", juce::NormalisableRange<float> {0, 1, 0.001f}, 0.5f,
        juce::AudioParameterFloatAttributes().withLabel("%")
            .withStringFromValueFunction([](float value, int) { return juce::String(100.0f*value,0); })
            .withValueFromStringFunction([](const juce::String& value) { return 0.01f*value.getFloatValue(); })));
    layout.add(std::make_unique<juce::AudioParameterChoice>(juce::ParameterID {"text_layout", 1},
        "Text Layout", juce::StringArray {"Whole", "Characters", "Words"}, 0));
    layout.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID {"text_play", 1}, "Text Play", false));
    layout.add(std::make_unique<juce::AudioParameterInt>(juce::ParameterID {"text_frame", 1}, "Text Frame", 1, 32, 1));
    layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID {"tempo", 1}, "Tempo",
        juce::NormalisableRange<float> {20, 300, 0.1f}, 120,
        juce::AudioParameterFloatAttributes().withLabel("BPM")));
    juce::StringArray notes;
    for (const auto* name : holotoner::kSyncNames) notes.add(name);
    juce::StringArray rateKeys {"text"};
    for (const auto* key : kMotionAxes) rateKeys.add(key);
    for (const auto& key : rateKeys) {
        const auto title = key == "text" ? juce::String("Text") : parametersNameForAxis(key);
        layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID {key+"_rate", 1},
            title+" Rate", juce::NormalisableRange<float> {0, 8, 0.001f, 0.4f}, key == "text" ? 0.25f : 0.0f,
            juce::AudioParameterFloatAttributes().withLabel("Hz")));
        layout.add(std::make_unique<juce::AudioParameterChoice>(juce::ParameterID {key+"_rate_sync", 1},
            juce::String(title)+" Note", notes, 0));
    }
    layout.add(std::make_unique<juce::AudioParameterChoice>(juce::ParameterID {"space_echo_time_sync", 1},
        "Echo Note", notes, 0));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID {"image_smooth", 1}, "Smooth", juce::NormalisableRange<float> {0.0f, 0.005f, 0.00001f, 0.5f}, 0.0f,
        juce::AudioParameterFloatAttributes().withLabel("ms")
            .withStringFromValueFunction([](float value, int) { return juce::String(1000.0f * value, 2); })
            .withValueFromStringFunction([](const juce::String& text) { return 0.001f * text.getFloatValue(); })));
    return layout;
}

void HolotonerProcessor::prepareToPlay(double sampleRate, int)
{
    clockSampleRate_ = std::isfinite(sampleRate) && sampleRate>0 ? sampleRate : 48000;
    for (auto& clock : axisClocks_) clock.reset();
    textClock_.reset();
    motionSamples_ = 0;
    hasDisplayedPlane_.store(false);
    holotoner::ImageSpatializer::Parameters spatialParameters;
    spatialParameters.plane = planeTransform();
    spatialParameters.phaseProjection = imageProjection_->load(std::memory_order_relaxed) >= 0.5f
        ? holotoner::PhaseProjection::Polar : holotoner::PhaseProjection::Cartesian;
    spatialParameters.imageAmount = imageAmount_->load(std::memory_order_relaxed);
    spatialParameters.edge = imageEdge_->load(std::memory_order_relaxed);
    spatialParameters.smooth = imageSmooth_->load(std::memory_order_relaxed);
    spatialParameters.rest = imageRest_->load(std::memory_order_relaxed);
    spatialParameters.feedback = spaceFeedback_->load(std::memory_order_relaxed);
    spatialParameters.echo = spaceEcho_->load(std::memory_order_relaxed);
    spatialParameters.resonate = spaceResonate_->load(std::memory_order_relaxed);
    spatialParameters.interfere = spaceInterfere_->load(std::memory_order_relaxed);
    spatialParameters.wet = wet_->load(std::memory_order_relaxed);
    spatialParameters.delta = outDelta_->load(std::memory_order_relaxed) >= 0.5f;
    spatialParameters.echoTime = echoTime_->load(std::memory_order_relaxed);
    engine_.setParameters(spatialParameters);
    engine_.prepare(sampleRate);
    setLatencySamples(engine_.latencySamples());
    sourcePlayer_.prepare(sampleRate);
    meterInputLeft_ = meterInputRight_ = meterOutputLeft_ = meterOutputRight_ = 0.0f;
    meterDecayPerSample_ = static_cast<float>(std::exp(-1.0 / (std::max(1.0, sampleRate) * 0.35)));
    inputLeft_.store(0.0f, std::memory_order_relaxed);
    inputRight_.store(0.0f, std::memory_order_relaxed);
    outputLeft_.store(0.0f, std::memory_order_relaxed);
    outputRight_.store(0.0f, std::memory_order_relaxed);
    outputCorrelation_.store(0.0f, std::memory_order_relaxed);
    scopeWritten_ = 0;
    scopePublished_.store(0, std::memory_order_release);
}

bool HolotonerProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto input = layouts.getMainInputChannelSet();
    const auto output = layouts.getMainOutputChannelSet();
    return output == juce::AudioChannelSet::stereo()
        && (input == juce::AudioChannelSet::mono() || input == juce::AudioChannelSet::stereo());
}

void HolotonerProcessor::processBlock(juce::AudioBuffer<float>& audio, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const auto inputChannels = getTotalNumInputChannels();
    const auto outputChannels = getTotalNumOutputChannels();
    const auto samples = audio.getNumSamples();

    for (int channel = inputChannels; channel < outputChannels; ++channel)
        audio.clear(channel, 0, samples);

    if (outputChannels < 2) {
        audio.clear();
        return;
    }
    if (inputChannels == 1)
        audio.copyFrom(1, 0, audio, 0, 0, samples);

    bool hostKnowsTransport = false;
    bool hostPlaying = false;
    double bpm = boundedValue(localBpm_,120,20,300);
    double ppq = textClock_.beats;
    bool hostHasPpq = false;
    if (auto* playHead = wrapperType != wrapperType_Standalone ? getPlayHead() : nullptr)
        if (const auto position = playHead->getPosition()) {
            hostKnowsTransport = true;
            hostPlaying = position->getIsPlaying();
            if (const auto tempo = position->getBpm(); tempo && std::isfinite(*tempo) && *tempo > 0)
                bpm = std::clamp(*tempo, 1.0, 1000.0);
            if (const auto beat = position->getPpqPosition(); beat && std::isfinite(*beat) && std::abs(*beat) < 1.0e12) {
                ppq = *beat;
                hostHasPpq = true;
            }
        }
    const bool hostStopped = wrapperType != wrapperType_Standalone && hostKnowsTransport && !hostPlaying;
    waitingForHost_.store(hostStopped, std::memory_order_relaxed);
    const auto kind = sourceKind();
    if (restartRequested_.exchange(false) || kind != playedSourceKind_) {
        sourcePlayer_.restart();
        for (auto& clock : axisClocks_) clock.reset();
        textClock_.reset();
        if (!hostHasPpq) ppq = 0;
    }
    playedSourceKind_ = kind;
    const bool localPlaying = wrapperType != wrapperType_Standalone || sourcePlaying_.load();
    if ((kind == holotoner::SourceKind::Input && localPlaying)
        || (kind != holotoner::SourceKind::Input && sourcePlaying_.load() && !hostStopped)) {
        sourcePlayer_.render(kind, sources_.acquire(), audio.getWritePointer(0), audio.getWritePointer(1), samples);
    } else {
        audio.clear(0, 0, samples);
        audio.clear(1, 0, samples);
    }

    const auto inputMeter = measureBlock(audio.getReadPointer(0), audio.getReadPointer(1), samples);
    const auto meterDecay = std::pow(meterDecayPerSample_, static_cast<float>(samples));
    meterInputLeft_ = std::max(inputMeter.left, meterInputLeft_ * meterDecay);
    meterInputRight_ = std::max(inputMeter.right, meterInputRight_ * meterDecay);
    inputLeft_.store(meterInputLeft_, std::memory_order_relaxed);
    inputRight_.store(meterInputRight_, std::memory_order_relaxed);

    holotoner::ImageSpatializer::Parameters spatialParameters;
    spatialParameters.phaseProjection = imageProjection_->load(std::memory_order_relaxed) >= 0.5f
        ? holotoner::PhaseProjection::Polar : holotoner::PhaseProjection::Cartesian;
    spatialParameters.imageAmount = imageAmount_->load(std::memory_order_relaxed);
    spatialParameters.edge = imageEdge_->load(std::memory_order_relaxed);
    spatialParameters.smooth = imageSmooth_->load(std::memory_order_relaxed);
    spatialParameters.rest = imageRest_->load(std::memory_order_relaxed);
    spatialParameters.feedback = spaceFeedback_->load(std::memory_order_relaxed);
    spatialParameters.echo = spaceEcho_->load(std::memory_order_relaxed);
    spatialParameters.resonate = spaceResonate_->load(std::memory_order_relaxed);
    spatialParameters.interfere = spaceInterfere_->load(std::memory_order_relaxed);
    spatialParameters.wet = wet_->load(std::memory_order_relaxed);
    spatialParameters.delta = outDelta_->load(std::memory_order_relaxed) >= 0.5f;
    spatialParameters.echoTime = echoTime_->load(std::memory_order_relaxed);
    // ♪: one note at the host tempo (Standalone: Tempo) replaces the free time.
    if (const auto division = juce::roundToInt(boundedValue(echoSync_,0,0,23)); division > 0)
        spatialParameters.echoTime = static_cast<float>(holotoner::kSyncBeats[static_cast<std::size_t>(division)] * 60.0 / bpm);
    resolvedEchoTime_.store(spatialParameters.echoTime, std::memory_order_relaxed);
    const auto* bank = banks_.acquire();
    banks_.hold(0, bank);
    for (int i = 0; i < samples; ++i) {
        const std::array<float, 2> pair {audio.getReadPointer(0)[i], audio.getReadPointer(1)[i]};
        scopeInput_[(scopeWritten_ + static_cast<std::uint64_t>(i)) % kScopeHistory]
            .store(std::bit_cast<std::uint64_t>(pair), std::memory_order_relaxed);
    }
    const bool clockPlaying = !hostStopped && localPlaying;
    const auto textDivision = juce::roundToInt(boundedValue(textSync_,0,0,23));
    const auto textHz = boundedValue(textRate_,0.25f,0,8);
    const auto depth = boundedValue(motionDepth_,0.5f,0,1);
    for (int offset = 0; offset < samples;) {
        // Fixed audio-sample grid, independent of the host's block partition and UI rate.
        const auto count = std::min(samples-offset, 32-static_cast<int>(motionSamples_ % 32));
        const auto partial = clockPlaying ? static_cast<double>(motionSamples_ % 32) : 0.0;
        const auto beat = hostHasPpq ? ppq + (clockPlaying ? (offset-partial)*bpm/(60.0*clockSampleRate_) : 0.0)
                                    : textClock_.beats-partial*bpm/(60.0*clockSampleRate_);
        const auto textCycles = textClock_.cycles(textDivision, beat)
            - (textDivision == 0 ? partial*textHz/clockSampleRate_ : 0.0);
        const holotoner::ImageMask* mask = nullptr;
        spatialParameters.plane = planeTransform();
        for (std::size_t axis = 0; axis < axisClocks_.size(); ++axis) {
            const auto hz = boundedValue(axisRate_[axis],0,0,8);
            const auto division = juce::roundToInt(boundedValue(axisSync_[axis],0,0,23));
            if (hz > 0 || division > 0) {
                const auto cycles = axisClocks_[axis].cycles(division,beat)
                    - (division == 0 ? partial*hz/clockSampleRate_ : 0.0);
                spatialParameters.plane = holotoner::motionPlane(spatialParameters.plane,
                    {static_cast<int>(axis)+1,motionShape_->load()>=0.5f,depth},cycles);
            }
            axisClocks_[axis].advance(count,clockSampleRate_,hz,bpm,clockPlaying);
        }
        if (bank != nullptr && !bank->frames.empty()) {
            const auto frameCount = static_cast<int>(bank->frames.size());
            int frame = juce::jlimit(0, frameCount-1, juce::roundToInt(boundedValue(textFrame_,1,1,32))-1);
            if (textPlay_->load() >= 0.5f) {
                const auto integral = std::abs(textCycles-std::round(textCycles)) < 1.0e-9
                    ? std::round(textCycles) : std::floor(textCycles);
                const auto wrapped = std::fmod(integral, static_cast<double>(frameCount));
                frame = static_cast<int>(wrapped < 0 ? wrapped+frameCount : wrapped);
            }
            displayedTextFrame_.store(frame, std::memory_order_relaxed);
            const auto& selected = bank->frames[static_cast<std::size_t>(frame)];
            mask = selected.mask.get();
        }
        engine_.setParameters(spatialParameters);
        engine_.process(audio.getWritePointer(0)+offset, audio.getWritePointer(1)+offset, count, mask);
        const auto p = engine_.currentPlane();
        const std::array<float, 7> visible {p.positionX,p.positionY,p.scaleX,p.scaleY,
            p.pitchDegrees,p.yawDegrees,p.rollDegrees};
        for (std::size_t i = 0; i < visible.size(); ++i) displayedPlane_[i].store(visible[i], std::memory_order_relaxed);
        hasDisplayedPlane_.store(true, std::memory_order_release);
        textClock_.advance(count, clockSampleRate_, textHz, bpm, clockPlaying);
        motionSamples_ += static_cast<std::uint64_t>(count);
        offset += count;
    }
    for (int i = 0; i < samples; ++i) {
        const std::array<float, 2> pair {audio.getReadPointer(0)[i], audio.getReadPointer(1)[i]};
        scopeOutput_[scopeWritten_++ % kScopeHistory]
            .store(std::bit_cast<std::uint64_t>(pair), std::memory_order_relaxed);
    }
    scopePublished_.store(scopeWritten_, std::memory_order_release);
    const auto outputMeter = measureBlock(audio.getReadPointer(0), audio.getReadPointer(1), samples);
    meterOutputLeft_ = std::max(outputMeter.left, meterOutputLeft_ * meterDecay);
    meterOutputRight_ = std::max(outputMeter.right, meterOutputRight_ * meterDecay);
    outputLeft_.store(meterOutputLeft_, std::memory_order_relaxed);
    outputRight_.store(meterOutputRight_, std::memory_order_relaxed);
    if (outputMeter.left + outputMeter.right > 1.0e-7f)
        outputCorrelation_.store(outputMeter.correlation, std::memory_order_relaxed);
}

holotoner::PlaneTransform HolotonerProcessor::planeTransform() const
{
    return {positionX_->load(std::memory_order_relaxed), positionY_->load(std::memory_order_relaxed),
            scaleX_->load(std::memory_order_relaxed), scaleY_->load(std::memory_order_relaxed),
            pitch_->load(std::memory_order_relaxed), yaw_->load(std::memory_order_relaxed),
            roll_->load(std::memory_order_relaxed)};
}

holotoner::PlaneTransform HolotonerProcessor::displayedPlaneTransform() const
{
    if (!hasDisplayedPlane_.load(std::memory_order_acquire)) return planeTransform();
    return {displayedPlane_[0].load(),displayedPlane_[1].load(),displayedPlane_[2].load(),
        displayedPlane_[3].load(),displayedPlane_[4].load(),displayedPlane_[5].load(),displayedPlane_[6].load()};
}

HolotonerProcessor::MeterState HolotonerProcessor::meterState() const
{
    return {inputLeft_.load(std::memory_order_relaxed), inputRight_.load(std::memory_order_relaxed),
            outputLeft_.load(std::memory_order_relaxed), outputRight_.load(std::memory_order_relaxed),
            outputCorrelation_.load(std::memory_order_relaxed)};
}

int HolotonerProcessor::scopeSamples(ScopeSample* destination, int capacity) const
{
    if (destination == nullptr || capacity <= 0)
        return 0;
    const auto end = scopePublished_.load(std::memory_order_acquire);
    const auto available = std::min(end, static_cast<std::uint64_t>(kScopeHistory));
    const auto count = std::min(available, static_cast<std::uint64_t>(capacity));
    for (std::uint64_t i = 0; i < count; ++i) {
        const auto at = (end - available + i * available / count) % kScopeHistory;
        const auto input = std::bit_cast<std::array<float, 2>>(scopeInput_[at].load(std::memory_order_relaxed));
        const auto output = std::bit_cast<std::array<float, 2>>(scopeOutput_[at].load(std::memory_order_relaxed));
        destination[i] = {input[0], input[1], output[0], output[1]};
    }
    return static_cast<int>(count);
}

juce::AudioProcessorEditor* HolotonerProcessor::createEditor()
{
    return new HolotonerEditor(*this);
}

void HolotonerProcessor::getStateInformation(juce::MemoryBlock& destination)
{
    auto state = parameters_.copyState();
    state.setProperty("format_version", kStateVersion, nullptr);
    if (const auto xml = state.createXml()) {
        auto* source = xml->createNewChildElement("Source");
        source->setAttribute("kind", sourceKindKey(sourceKind()));
        {
            std::lock_guard lock(sourceInfoMutex_);
            source->setAttribute("path", sourceFile_.getFullPathName());
        }
        std::lock_guard imageLock(imageMutex_);
        if (shapeActive_) {
            xml->createNewChildElement("Shape"); // Kind and Order are parameters
        } else if (pendingMediaState_ != nullptr) {
            xml->addChildElement(new juce::XmlElement(*pendingMediaState_));
        } else if (text_.isNotEmpty() && !textFrames_.empty()) {
            auto* textState = xml->createNewChildElement("Text");
            textState->setAttribute("value", text_);
            textState->setAttribute("layout", renderedTextLayout_);
            textState->setAttribute("renderer_version",2);
            for (const auto& document : textFrames_) {
                auto* frame = textState->createNewChildElement("Frame");
                frame->setAttribute("name", document->name);
                frame->addTextElement(document->normalizedPng.toBase64Encoding());
            }
        } else if (const auto image = image_) {
            auto* imageState = xml->createNewChildElement("Image");
            imageState->setAttribute("name", image->name);
            imageState->addTextElement(image->normalizedPng.toBase64Encoding());
        }
        copyXmlToBinary(*xml, destination);
    }
}

void HolotonerProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0 || sizeInBytes > 64*1024*1024) return;
    const auto xml = getXmlFromBinary(data, sizeInBytes);
    if (xml == nullptr || !xml->hasTagName(parameters_.state.getType()))
        return;
    auto sourceKindToRestore = holotoner::SourceKind::Input;
    juce::File sourceFileToRestore;
    juce::MemoryBlock imageToRestore;
    juce::String imageNameToRestore;
    juce::String textToRestore;
    int textLayoutToRestore = 0;
    bool fitTextToRestore = false;
    std::vector<std::pair<juce::String, juce::MemoryBlock>> framesToRestore;
    std::unique_ptr<juce::XmlElement> mediaStateToRestore;
    if (auto* textState = xml->getChildByName("Text")) {
        textToRestore = textState->getStringAttribute("value");
        textLayoutToRestore = textState->getIntAttribute("layout");
        fitTextToRestore = textState->getIntAttribute("renderer_version",1)<2;
        if (textToRestore.length() > 256 || textLayoutToRestore < 0 || textLayoutToRestore > 2
            || textState->getNumChildElements() > textSource::kMaximumFrames) return;
        mediaStateToRestore = std::make_unique<juce::XmlElement>(*textState);
        for (auto* frame : textState->getChildIterator()) {
            juce::MemoryBlock bytes;
            if (frame->hasTagName("Frame") && bytes.fromBase64Encoding(frame->getAllSubText())
                && bytes.getSize() <= static_cast<std::size_t>(kMaximumImageFileBytes))
                framesToRestore.emplace_back(frame->getStringAttribute("name"), std::move(bytes));
            if (framesToRestore.size() >= textSource::kMaximumFrames) break;
        }
        xml->removeChildElement(textState, true);
    }
    bool shapeToRestore = false;
    if (auto* shape = xml->getChildByName("Shape")) {
        shapeToRestore = true;
        xml->removeChildElement(shape, true);
    }
    if (auto* source = xml->getChildByName("Source")) {
        sourceKindToRestore = sourceKindFromKey(source->getStringAttribute("kind"));
        sourceFileToRestore = juce::File(source->getStringAttribute("path"));
        xml->removeChildElement(source, true);
    }
    if (auto* image = xml->getChildByName("Image")) {
        if (mediaStateToRestore == nullptr) mediaStateToRestore = std::make_unique<juce::XmlElement>(*image);
        imageNameToRestore = image->getStringAttribute("name");
        imageToRestore.fromBase64Encoding(image->getAllSubText());
        if (imageToRestore.getSize() > static_cast<std::size_t>(kMaximumImageFileBytes)) return;
        xml->removeChildElement(image, true);
    }
    auto state = juce::ValueTree::fromXml(*xml);
    const auto version = static_cast<int>(state.getProperty("format_version", 0));
    if (version >= kOldestReadableStateVersion && version <= kStateVersion) {
        // Pre-release mode selection was removed. Preserve explicit Off through Amount,
        // but migrate Place/Level/Phase to the single image-field engine.
        bool oldOff = false;
        for (int i = state.getNumChildren()-1; i >= 0; --i) {
            const auto child = state.getChild(i);
            if (child.getProperty("id").toString() == "image_target") {
                oldOff = static_cast<float>(child.getProperty("value", 3.0f)) < 0.5f;
                state.removeChild(i, nullptr);
            }
        }
        if (oldOff)
            for (int i = 0; i < state.getNumChildren(); ++i) {
                auto child = state.getChild(i);
                if (child.getProperty("id").toString() == "image_amount")
                    child.setProperty("value", 0.0f, nullptr);
            }
        state.setProperty("format_version", kStateVersion, nullptr);
        if (version < 8) {
            // A legacy state loaded into an already-running processor must not inherit new motion/text playback.
            for (const auto* id : {"motion_shape", "motion_depth",
                                  "text_layout", "text_play", "text_frame", "text_rate", "text_rate_sync", "tempo"}) {
                auto child = juce::ValueTree("PARAM");
                auto* parameter = parameters_.getParameter(id);
                for (int i = state.getNumChildren()-1; i >= 0; --i)
                    if (state.getChild(i).getProperty("id").toString() == id) state.removeChild(i, nullptr);
                child.setProperty("id", id, nullptr);
                child.setProperty("value", parameter->convertFrom0to1(parameter->getDefaultValue()), nullptr);
                state.addChild(child, -1, nullptr);
            }
        }
        if (version < 9) {
            const auto value = [&state](const char* id,float fallback) {
                for (int i = 0; i < state.getNumChildren(); ++i)
                    if (state.getChild(i).getProperty("id").toString() == id)
                        return static_cast<float>(state.getChild(i).getProperty("value",fallback));
                return fallback;
            };
            const auto oldTarget = version >= 8 ? juce::roundToInt(value("motion_target",0)) : 0;
            const auto oldRate = value("motion_rate",0.25f), oldSync = value("motion_rate_sync",0);
            for (std::size_t axis = 0; axis < kMotionAxes.size(); ++axis)
                for (const auto& suffix : {juce::String("_rate"),juce::String("_rate_sync")}) {
                    const auto id = juce::String(kMotionAxes[axis])+suffix;
                    for (int i = state.getNumChildren()-1; i >= 0; --i)
                        if (state.getChild(i).getProperty("id").toString() == id) state.removeChild(i,nullptr);
                    const bool selected = oldTarget == static_cast<int>(axis)+1 || (oldTarget == 6 && axis == 6);
                    auto child = juce::ValueTree("PARAM");
                    child.setProperty("id",id,nullptr);
                    child.setProperty("value",selected ? (suffix == "_rate" ? oldRate : oldSync) : 0.0f,nullptr);
                    state.addChild(child,-1,nullptr);
                }
        }
        // Space (10) and Interfere/Shape (11) did not exist: older sessions reopen with their
        // defaults, even when loaded into a running processor.
        const auto reset = [this,&state](const char* id) {
            for (int i = state.getNumChildren()-1; i >= 0; --i)
                if (state.getChild(i).getProperty("id").toString() == id) state.removeChild(i,nullptr);
            auto* parameter = parameters_.getParameter(id);
            auto child = juce::ValueTree("PARAM");
            child.setProperty("id",id,nullptr);
            child.setProperty("value",parameter->convertFrom0to1(parameter->getDefaultValue()),nullptr);
            state.addChild(child,-1,nullptr);
        };
        if (version < 10)
            for (const auto* id : {"space_echo","space_feedback","space_resonate"}) reset(id);
        if (version < 11)
            for (const auto* id : {"space_interfere","shape_kind","shape_order"}) reset(id);
        if (version < 12)
            reset("image_edge"); // default 100% is the only edge older sessions had
        if (version < 13)
            reset("image_rest"); // 0%: unheld samples stay where they were
        for (int i = state.getNumChildren()-1; i >= 0; --i) {
            const auto id = state.getChild(i).getProperty("id").toString();
            if (id == "motion_target" || id == "motion_rate" || id == "motion_rate_sync") state.removeChild(i,nullptr);
        }
        if (!state.getChildWithProperty("id", "image_smooth").isValid())
            reset("image_smooth"); // pre-Smooth sessions retain their original steering
        parameters_.replaceState(state);
        const bool fileExists = sourceFileToRestore.existsAsFile();
        {
            std::lock_guard lock(sourceInfoMutex_);
            sourceFile_ = sourceFileToRestore;
        }
        if (sourceKindToRestore == holotoner::SourceKind::File) {
            setSourceKind(holotoner::SourceKind::Input);
            if (fileExists)
                loadSourceFile(sourceFileToRestore, true);
        } else {
            setSourceKind(sourceKindToRestore);
            if (fileExists)
                loadSourceFile(sourceFileToRestore, false);
        }
        if (!fileExists && sourceFileToRestore != juce::File())
            setSourceMessage("Not found: " + sourceFileToRestore.getFullPathName());

        const auto imageRequest = imageRequest_.fetch_add(1) + 1;
        {
            std::lock_guard lock(imageMutex_);
            image_.reset();
            text_.clear();
            textFrames_.clear();
            renderedTextLayout_ = -1;
            rejectedTextLayout_ = -1;
            pendingMediaState_ = shapeToRestore ? nullptr : std::move(mediaStateToRestore);
            shapeActive_ = shapeToRestore;
            renderedShape_ = -1; // the timer regenerates it from the restored parameters
            imageMessage_.clear();
            mediaVersion_.fetch_add(1);
        }
        if (!framesToRestore.empty() && textToRestore.isNotEmpty()) {
            imageLoadsPending_.fetch_add(1);
            mediaLoader_.addJob([this, frames = std::move(framesToRestore), value = std::move(textToRestore),
                                  textLayoutToRestore, imageRequest,fitTextToRestore]() mutable {
                std::vector<std::shared_ptr<const ImageDocument>> restored;
                for (auto& [name, bytes] : frames) {
                    if (imageRequest != imageRequest_.load()) break;
                    auto decoded = juce::ImageFileFormat::loadFrom(bytes.getData(),bytes.getSize());
                    if (fitTextToRestore && decoded.isValid()) decoded = textSource::fitInk(decoded);
                    const auto document = makeDocument(decoded,name,fitTextToRestore ? nullptr : &bytes);
                    if (document != nullptr) restored.push_back(document);
                }
                if (imageRequest == imageRequest_.load() && restored.size() == frames.size() && !restored.empty()) {
                    std::lock_guard lock(imageMutex_);
                    if (imageRequest != imageRequest_.load()) { imageLoadsPending_.fetch_sub(1); return; }
                    text_ = std::move(value);
                    renderedTextLayout_ = textLayoutToRestore;
                    textFrames_ = std::move(restored);
                    image_ = textFrames_.front();
                    pendingMediaState_.reset();
                    mediaVersion_.fetch_add(1);
                } else if (imageRequest == imageRequest_.load()) setImageMessage("Cannot restore embedded text frames");
                imageLoadsPending_.fetch_sub(1);
            });
        } else if (!imageToRestore.isEmpty()) {
            imageLoadsPending_.fetch_add(1);
            mediaLoader_.addJob([this, imageBytes = std::move(imageToRestore), name = std::move(imageNameToRestore), imageRequest]() mutable {
                loadImageData(std::move(imageBytes), std::move(name), imageRequest, true);
                imageLoadsPending_.fetch_sub(1);
            });
        }
    }
}

bool HolotonerProcessor::isSupportedAudioFile(const juce::File& file)
{
    return file.hasFileExtension("wav;aif;aiff;flac;mp3;m4a;caf");
}

void HolotonerProcessor::loadSourceFile(const juce::File& file, bool switchToFile)
{
    sourceLoadsPending_.fetch_add(1);
    mediaLoader_.addJob([this, file, switchToFile] {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
        juce::String message;
        if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0) {
            message = "Cannot read " + file.getFileName();
        } else {
            const auto maximum = static_cast<juce::int64>(holotoner::kMaxSourceSeconds * reader->sampleRate);
            const auto length = static_cast<int>(std::min(reader->lengthInSamples, maximum));
            juce::AudioBuffer<float> decoded(2, length);
            reader->read(&decoded, 0, length, 0, true, true);
            auto source = std::make_shared<holotoner::SourceAudio>();
            source->sampleRate = reader->sampleRate;
            source->left.assign(decoded.getReadPointer(0), decoded.getReadPointer(0) + length);
            source->right.assign(decoded.getReadPointer(1), decoded.getReadPointer(1) + length);
            sources_.publish(std::move(source));
            if (reader->lengthInSamples > length)
                message = "Cut to " + juce::String(holotoner::kMaxSourceSeconds, 0) + " s";
            {
                std::lock_guard lock(sourceInfoMutex_);
                sourceFile_ = file;
            }
            if (switchToFile)
                setSourceKind(holotoner::SourceKind::File);
        }
        setSourceMessage(message);
        sourceLoadsPending_.fetch_sub(1);
    });
}

juce::String HolotonerProcessor::sourceName() const
{
    std::lock_guard lock(sourceInfoMutex_);
    return sourceFile_.getFileName();
}

juce::File HolotonerProcessor::sourceFile() const
{
    std::lock_guard lock(sourceInfoMutex_);
    return sourceFile_;
}

juce::String HolotonerProcessor::lastSourceMessage() const
{
    std::lock_guard lock(sourceInfoMutex_);
    return sourceMessage_;
}

void HolotonerProcessor::setSourceMessage(const juce::String& message)
{
    std::lock_guard lock(sourceInfoMutex_);
    sourceMessage_ = message;
}

void HolotonerProcessor::timerCallback()
{
    sources_.collectGarbage();
    banks_.collectGarbage();
    juce::String value;
    bool needsText = false;
    const auto layout = juce::roundToInt(textLayout_->load());
    {
        std::lock_guard lock(imageMutex_);
        needsText = text_.isNotEmpty() && renderedTextLayout_ != layout && rejectedTextLayout_ != layout;
        value = text_;
    }
    if (needsText && imageLoadsPending_.load() == 0) setText(value);
    bool needsShape = false;
    {
        std::lock_guard lock(imageMutex_);
        needsShape = shapeActive_ && renderedShape_ != shapeKey();
    }
    if (needsShape && imageLoadsPending_.load() == 0) useShape();
    rebuildImageMaskIfNeeded();
}

bool HolotonerProcessor::isSupportedImageFile(const juce::File& file)
{
    return file.hasFileExtension("png;jpg;jpeg;gif");
}

void HolotonerProcessor::loadImageFile(const juce::File& file)
{
    const auto request = imageRequest_.fetch_add(1) + 1;
    imageLoadsPending_.fetch_add(1);
    mediaLoader_.addJob([this, file, request] {
        juce::String message;
        if (!file.existsAsFile() || file.getSize() <= 0 || file.getSize() > kMaximumImageFileBytes) {
            message = "Cannot read image " + file.getFileName();
        } else {
            juce::MemoryBlock data;
            if (!file.loadFileAsData(data))
                message = "Cannot read image " + file.getFileName();
            else {
                loadImageData(std::move(data), file.getFileName(), request);
                imageLoadsPending_.fetch_sub(1);
                return;
            }
        }
        if (request == imageRequest_.load())
            setImageMessage(message);
        imageLoadsPending_.fetch_sub(1);
    });
}

void HolotonerProcessor::loadImageData(juce::MemoryBlock data, juce::String name, std::uint64_t request, bool embedded)
{
    auto document = makeDocument(juce::ImageFileFormat::loadFrom(data.getData(), data.getSize()), name, embedded ? &data : nullptr);
    if (document == nullptr) {
        if (request == imageRequest_.load())
            setImageMessage("Cannot decode image " + name);
        return;
    }

    if (request != imageRequest_.load())
        return;
    {
        std::lock_guard lock(imageMutex_);
        if (request != imageRequest_.load()) return;
        image_ = std::move(document);
        shapeActive_ = false;
        text_.clear();
        textFrames_.clear();
        renderedTextLayout_ = -1;
        rejectedTextLayout_ = -1;
        pendingMediaState_.reset();
        imageMessage_.clear();
        mediaVersion_.fetch_add(1);
    }
}

void HolotonerProcessor::setText(const juce::String& value)
{
    const auto request = imageRequest_.fetch_add(1) + 1;
    const auto layout = juce::roundToInt(textLayout_->load());
    imageLoadsPending_.fetch_add(1);
    mediaLoader_.addJob([this, value, layout, request] {
        renderText(value, layout, request);
        imageLoadsPending_.fetch_sub(1);
    });
}

void HolotonerProcessor::renderText(juce::String value, int layout, std::uint64_t request)
{
    juce::String error;
    const auto pieces = textSource::segments(value, layout, error);
    std::vector<std::shared_ptr<const ImageDocument>> documents;
    for (const auto& piece : pieces) {
        if (request != imageRequest_.load()) return;
        auto document = makeDocument(textSource::render(piece), piece);
        if (document == nullptr) { error = "Cannot render text"; break; }
        documents.push_back(std::move(document));
    }
    if (request != imageRequest_.load()) return;
    std::lock_guard lock(imageMutex_);
    if (request != imageRequest_.load()) return;
    if (error.isNotEmpty()) {
        imageMessage_ = error;
        // Remember the rejected layout so an error does not queue a retry every UI tick.
        rejectedTextLayout_ = layout;
        return;
    }
    text_ = std::move(value);
    shapeActive_ = false;
    renderedTextLayout_ = layout;
    rejectedTextLayout_ = -1;
    textFrames_ = std::move(documents);
    image_ = textFrames_.front();
    pendingMediaState_.reset();
    imageMessage_.clear();
    mediaVersion_.fetch_add(1);
}

int HolotonerProcessor::shapeKey() const
{
    return juce::roundToInt(shapeKind_->load()) * 16 + juce::roundToInt(shapeOrder_->load());
}

bool HolotonerProcessor::shapeActive() const
{
    std::lock_guard lock(imageMutex_);
    return shapeActive_;
}

void HolotonerProcessor::useShape()
{
    const auto request = imageRequest_.fetch_add(1) + 1;
    const auto key = shapeKey();
    imageLoadsPending_.fetch_add(1);
    mediaLoader_.addJob([this, key, request] {
        const auto kind = static_cast<holotoner::ShapeKind>(juce::jlimit(0, static_cast<int>(holotoner::kShapeNames.size())-1, key / 16));
        const auto rgba = holotoner::generateShape(kind, key % 16);
        juce::Image bitmap(juce::Image::ARGB, rgba.width, rgba.height, true);
        for (int y = 0; y < rgba.height; ++y)
            for (int x = 0; x < rgba.width; ++x) {
                const auto at = static_cast<std::size_t>((y * rgba.width + x) * 4);
                bitmap.setPixelAt(x, y, juce::Colour(rgba.pixels[at], rgba.pixels[at+1], rgba.pixels[at+2], rgba.pixels[at+3]));
            }
        auto document = makeDocument(bitmap, juce::String(holotoner::kShapeNames[static_cast<std::size_t>(kind)])
                                                 + " " + juce::String(key % 16));
        if (document != nullptr && request == imageRequest_.load()) {
            std::lock_guard lock(imageMutex_);
            if (request == imageRequest_.load()) {
                image_ = std::move(document);
                shapeActive_ = true;
                renderedShape_ = key;
                text_.clear();
                textFrames_.clear();
                renderedTextLayout_ = rejectedTextLayout_ = -1;
                pendingMediaState_.reset();
                imageMessage_.clear();
                mediaVersion_.fetch_add(1);
            }
        }
        imageLoadsPending_.fetch_sub(1);
    });
}

juce::String HolotonerProcessor::text() const
{
    std::lock_guard lock(imageMutex_);
    return text_;
}

int HolotonerProcessor::textFrameCount() const
{
    const auto bank = banks_.currentShared();
    return bank != nullptr ? static_cast<int>(bank->frames.size()) : 0;
}

void HolotonerProcessor::rebuildImageMaskIfNeeded()
{
    if (bankPending_.load() || imageLoadsPending_.load() > 0) return;
    holotoner::ImageMaskSettings settings;
    settings.channel = static_cast<holotoner::PixelChannel>(
        juce::jlimit(0, 5, juce::roundToInt(imageChannel_->load(std::memory_order_relaxed))));
    settings.gain = imageGain_->load(std::memory_order_relaxed);
    settings.fill = imageFill_->load(std::memory_order_relaxed);
    settings.normalize = imageNormalize_->load(std::memory_order_relaxed) >= 0.5f;
    settings.invert = imageInvert_->load(std::memory_order_relaxed) >= 0.5f;
    settings.amount = 1.0f;
    settings.threshold = imageThreshold_->load(std::memory_order_relaxed);
    settings.binary = imageBinary_->load(std::memory_order_relaxed) >= 0.5f;
    const bool settingsChanged = !hasBuiltMaskSettings_
        || settings.channel != builtMaskSettings_.channel
        || std::abs(settings.gain - builtMaskSettings_.gain) > 1.0e-6f
        || std::abs(settings.fill - builtMaskSettings_.fill) > 1.0e-6f
        || settings.normalize != builtMaskSettings_.normalize
        || settings.invert != builtMaskSettings_.invert
        || settings.binary != builtMaskSettings_.binary
        || std::abs(settings.threshold - builtMaskSettings_.threshold) > 1.0e-6f;
    std::vector<std::shared_ptr<const ImageDocument>> images;
    std::uint64_t mediaVersion = 0, request = 0;
    {
        std::lock_guard lock(imageMutex_);
        mediaVersion = mediaVersion_.load();
        // Compare in place first: this runs at 30 Hz and almost never needs a rebuild.
        const bool sameImages = textFrames_.empty()
            ? (image_ == nullptr ? maskImages_.empty() : maskImages_.size() == 1 && maskImages_.front() == image_)
            : textFrames_ == maskImages_;
        if (sameImages && !settingsChanged && mediaVersion == bankVersion_.load())
            return;
        images = textFrames_;
        if (images.empty() && image_ != nullptr) images.push_back(image_);
        request = imageRequest_.load();
    }
    maskImages_ = images;
    builtMaskSettings_ = settings;
    hasBuiltMaskSettings_ = true;
    bankPending_.store(true);
    mediaLoader_.addJob([this, documents = std::move(images), settings, request, mediaVersion] {
        auto bank = std::make_shared<ImageBank>();
        for (const auto& image : documents) {
            if (request != imageRequest_.load()) break;
            ImageFrame frame;
            frame.document = image;
            auto mask = std::make_shared<holotoner::ImageMask>(holotoner::buildImageMask(image->rgba, settings));
            frame.mask = mask;
            bank->frames.push_back(std::move(frame));
        }
        if (request == imageRequest_.load() && mediaVersion == mediaVersion_.load()) {
            banks_.publish(std::move(bank));
            bankVersion_.store(mediaVersion);
        }
        else {
            // Force the next control tick to compile the replacement, even if its settings match.
            hasBuiltMaskSettings_ = false;
        }
        bankPending_.store(false, std::memory_order_release);
    });
}

std::shared_ptr<const HolotonerProcessor::ImageDocument> HolotonerProcessor::imageDocument() const
{
    const auto bank = banks_.currentShared();
    if (bank != nullptr && !bank->frames.empty()) {
        const auto at = juce::jlimit(0, static_cast<int>(bank->frames.size())-1, displayedTextFrame_.load());
        return bank->frames[static_cast<std::size_t>(at)].document;
    }
    std::lock_guard lock(imageMutex_);
    return image_;
}

std::shared_ptr<const holotoner::ImageMask> HolotonerProcessor::imageMask() const
{
    const auto bank = banks_.currentShared();
    if (bank == nullptr || bank->frames.empty()) return {};
    const auto at = juce::jlimit(0, static_cast<int>(bank->frames.size())-1, displayedTextFrame_.load());
    return bank->frames[static_cast<std::size_t>(at)].mask;
}

juce::String HolotonerProcessor::lastImageMessage() const
{
    std::lock_guard lock(imageMutex_);
    return imageMessage_;
}

void HolotonerProcessor::setImageMessage(const juce::String& message)
{
    std::lock_guard lock(imageMutex_);
    imageMessage_ = message;
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new HolotonerProcessor(); }
