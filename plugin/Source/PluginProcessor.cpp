#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <algorithm>
#include <cmath>
#include <memory>

namespace {

constexpr int kStateVersion = 2;
constexpr juce::int64 kMaximumImageFileBytes = 32 * 1024 * 1024;
constexpr int kMaximumImageDimension = 1024;

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
    jassert(positionX_ != nullptr && positionY_ != nullptr && scaleX_ != nullptr && scaleY_ != nullptr
            && pitch_ != nullptr && yaw_ != nullptr && roll_ != nullptr && wet_ != nullptr);
    if (wrapperType == wrapperType_Standalone)
        setSourceKind(holotoner::SourceKind::Clicks);
    startTimer(250);
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
        juce::ParameterID {"scale_x", 1}, "Scale X", juce::NormalisableRange<float> {0.05f, 2.0f, 0.001f}, 1.0f));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID {"scale_y", 1}, "Scale Y", juce::NormalisableRange<float> {0.05f, 2.0f, 0.001f}, 1.0f));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID {"pitch", 1}, "Pitch", degrees, 0.0f,
        juce::AudioParameterFloatAttributes().withLabel("deg")));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID {"yaw", 1}, "Yaw", degrees, 0.0f,
        juce::AudioParameterFloatAttributes().withLabel("deg")));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID {"roll", 1}, "Roll", degrees, 0.0f,
        juce::AudioParameterFloatAttributes().withLabel("deg")));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID {"wet", 1}, "Wet", juce::NormalisableRange<float> {0.0f, 1.0f, 0.001f}, 1.0f,
        juce::AudioParameterFloatAttributes()
            .withLabel("%")
            .withStringFromValueFunction([](float value, int) { return juce::String(100.0f * value, 0); })
            .withValueFromStringFunction([](const juce::String& text) { return 0.01f * text.getFloatValue(); })));
    return layout;
}

void HolotonerProcessor::prepareToPlay(double sampleRate, int)
{
    engine_.prepare(sampleRate);
    engine_.setParameters({planeTransform(), wet_->load(std::memory_order_relaxed)});
    engine_.reset();
    sourcePlayer_.prepare(sampleRate);
    meterInputLeft_ = meterInputRight_ = meterOutputLeft_ = meterOutputRight_ = 0.0f;
    meterDecayPerSample_ = static_cast<float>(std::exp(-1.0 / (std::max(1.0, sampleRate) * 0.35)));
    inputLeft_.store(0.0f, std::memory_order_relaxed);
    inputRight_.store(0.0f, std::memory_order_relaxed);
    outputLeft_.store(0.0f, std::memory_order_relaxed);
    outputRight_.store(0.0f, std::memory_order_relaxed);
    outputCorrelation_.store(0.0f, std::memory_order_relaxed);
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
    if (auto* playHead = getPlayHead())
        if (const auto position = playHead->getPosition()) {
            hostKnowsTransport = true;
            hostPlaying = position->getIsPlaying();
        }
    const bool hostStopped = wrapperType != wrapperType_Standalone && hostKnowsTransport && !hostPlaying;
    waitingForHost_.store(hostStopped, std::memory_order_relaxed);
    const auto kind = sourceKind();
    if (restartRequested_.exchange(false) || kind != playedSourceKind_)
        sourcePlayer_.restart();
    playedSourceKind_ = kind;
    if (kind == holotoner::SourceKind::Input || (sourcePlaying_.load() && !hostStopped)) {
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

    engine_.setParameters({planeTransform(), wet_->load(std::memory_order_relaxed)});
    engine_.process(audio.getWritePointer(0), audio.getWritePointer(1), samples);
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

HolotonerProcessor::MeterState HolotonerProcessor::meterState() const
{
    return {inputLeft_.load(std::memory_order_relaxed), inputRight_.load(std::memory_order_relaxed),
            outputLeft_.load(std::memory_order_relaxed), outputRight_.load(std::memory_order_relaxed),
            outputCorrelation_.load(std::memory_order_relaxed)};
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
        if (const auto image = imageDocument()) {
            auto* imageState = xml->createNewChildElement("Image");
            imageState->setAttribute("name", image->name);
            imageState->addTextElement(image->normalizedPng.toBase64Encoding());
        }
        copyXmlToBinary(*xml, destination);
    }
}

void HolotonerProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary(data, sizeInBytes);
    if (xml == nullptr || !xml->hasTagName(parameters_.state.getType()))
        return;
    auto sourceKindToRestore = holotoner::SourceKind::Input;
    juce::File sourceFileToRestore;
    juce::MemoryBlock imageToRestore;
    juce::String imageNameToRestore;
    if (auto* source = xml->getChildByName("Source")) {
        sourceKindToRestore = sourceKindFromKey(source->getStringAttribute("kind"));
        sourceFileToRestore = juce::File(source->getStringAttribute("path"));
        xml->removeChildElement(source, true);
    }
    if (auto* image = xml->getChildByName("Image")) {
        imageNameToRestore = image->getStringAttribute("name");
        imageToRestore.fromBase64Encoding(image->getAllSubText());
        xml->removeChildElement(image, true);
    }
    const auto state = juce::ValueTree::fromXml(*xml);
    const auto version = static_cast<int>(state.getProperty("format_version", 0));
    if (version == kStateVersion) {
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
            imageMessage_.clear();
        }
        if (!imageToRestore.isEmpty()) {
            imageLoadsPending_.fetch_add(1);
            mediaLoader_.addJob([this, imageBytes = std::move(imageToRestore), name = std::move(imageNameToRestore), imageRequest]() mutable {
                loadImageData(std::move(imageBytes), std::move(name), imageRequest);
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

void HolotonerProcessor::timerCallback() { sources_.collectGarbage(); }

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

void HolotonerProcessor::loadImageData(juce::MemoryBlock data, juce::String name, std::uint64_t request)
{
    auto decoded = juce::ImageFileFormat::loadFrom(data.getData(), data.getSize());
    if (decoded.isNull()) {
        if (request == imageRequest_.load())
            setImageMessage("Cannot decode image " + name);
        return;
    }

    const auto largest = std::max(decoded.getWidth(), decoded.getHeight());
    if (largest > kMaximumImageDimension) {
        const auto scale = static_cast<double>(kMaximumImageDimension) / static_cast<double>(largest);
        decoded = decoded.rescaled(std::max(1, juce::roundToInt(scale * decoded.getWidth())),
                                   std::max(1, juce::roundToInt(scale * decoded.getHeight())),
                                   juce::Graphics::highResamplingQuality);
    }
    decoded = decoded.convertedToFormat(juce::Image::ARGB);

    holotoner::RgbaImage rgba;
    rgba.width = decoded.getWidth();
    rgba.height = decoded.getHeight();
    rgba.pixels.resize(static_cast<std::size_t>(rgba.width) * static_cast<std::size_t>(rgba.height) * 4);
    const juce::Image::BitmapData pixels(decoded, juce::Image::BitmapData::readOnly);
    for (int y = 0; y < rgba.height; ++y)
        for (int x = 0; x < rgba.width; ++x) {
            const auto colour = pixels.getPixelColour(x, y);
            const auto offset = static_cast<std::size_t>((y * rgba.width + x) * 4);
            rgba.pixels[offset] = colour.getRed();
            rgba.pixels[offset + 1] = colour.getGreen();
            rgba.pixels[offset + 2] = colour.getBlue();
            rgba.pixels[offset + 3] = colour.getAlpha();
        }

    holotoner::ImageMaskSettings maskSettings;
    maskSettings.channel = holotoner::PixelChannel::Luma;
    auto document = std::make_shared<ImageDocument>();
    document->image = decoded;
    document->name = std::move(name);
    document->lumaMask = holotoner::buildImageMask(rgba, maskSettings);
    juce::MemoryOutputStream encoded(document->normalizedPng, false);
    juce::PNGImageFormat png;
    if (!png.writeImageToStream(decoded, encoded)) {
        if (request == imageRequest_.load())
            setImageMessage("Cannot normalize image " + document->name);
        return;
    }
    if (request != imageRequest_.load())
        return;
    {
        std::lock_guard lock(imageMutex_);
        image_ = std::move(document);
        imageMessage_.clear();
    }
}

std::shared_ptr<const HolotonerProcessor::ImageDocument> HolotonerProcessor::imageDocument() const
{
    std::lock_guard lock(imageMutex_);
    return image_;
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
