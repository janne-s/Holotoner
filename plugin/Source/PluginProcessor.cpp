#include "PluginProcessor.h"

#include <juce_audio_utils/juce_audio_utils.h>

namespace {

constexpr int kStateVersion = 1;

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

} // namespace

HolotonerProcessor::HolotonerProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters_(*this, nullptr, "HolotonerState", parameterLayout())
{
    yaw_ = parameters_.getRawParameterValue("yaw");
    width_ = parameters_.getRawParameterValue("width");
    wet_ = parameters_.getRawParameterValue("wet");
    jassert(yaw_ != nullptr && width_ != nullptr && wet_ != nullptr);
    if (wrapperType == wrapperType_Standalone)
        setSourceKind(holotoner::SourceKind::Clicks);
    startTimer(250);
}

HolotonerProcessor::~HolotonerProcessor()
{
    stopTimer();
    sourceLoader_.removeAllJobs(true, 5000);
}

juce::AudioProcessorValueTreeState::ParameterLayout HolotonerProcessor::parameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID {"yaw", 1}, "Yaw", juce::NormalisableRange<float> {-180.0f, 180.0f, 0.01f}, 0.0f,
        juce::AudioParameterFloatAttributes().withLabel("deg")));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID {"width", 1}, "Width", juce::NormalisableRange<float> {0.0f, 2.0f, 0.001f}, 1.0f));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID {"wet", 1}, "Wet", juce::NormalisableRange<float> {0.0f, 1.0f, 0.001f}, 1.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));
    return layout;
}

void HolotonerProcessor::prepareToPlay(double sampleRate, int)
{
    engine_.prepare(sampleRate);
    engine_.setParameters({yaw_->load(std::memory_order_relaxed), width_->load(std::memory_order_relaxed),
                           wet_->load(std::memory_order_relaxed)});
    engine_.reset();
    sourcePlayer_.prepare(sampleRate);
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

    engine_.setParameters({yaw_->load(std::memory_order_relaxed), width_->load(std::memory_order_relaxed),
                           wet_->load(std::memory_order_relaxed)});
    engine_.process(audio.getWritePointer(0), audio.getWritePointer(1), samples);
}

juce::AudioProcessorEditor* HolotonerProcessor::createEditor()
{
    // The generic editor keeps this unreleased wrapper inspectable without
    // pretending that the agreed two-panel production editor already exists.
    return new juce::GenericAudioProcessorEditor(*this);
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
    if (auto* source = xml->getChildByName("Source")) {
        sourceKindToRestore = sourceKindFromKey(source->getStringAttribute("kind"));
        sourceFileToRestore = juce::File(source->getStringAttribute("path"));
        xml->removeChildElement(source, true);
    }
    const auto state = juce::ValueTree::fromXml(*xml);
    const auto version = static_cast<int>(state.getProperty("format_version", 0));
    if (version == kStateVersion) {
        parameters_.replaceState(state);
        const bool fileExists = sourceFileToRestore.existsAsFile();
        setSourceKind(sourceKindToRestore == holotoner::SourceKind::File && !fileExists
                          ? holotoner::SourceKind::Input
                          : sourceKindToRestore);
        if (fileExists)
            loadSourceFile(sourceFileToRestore, false);
        else if (sourceFileToRestore != juce::File())
            setSourceMessage("Not found: " + sourceFileToRestore.getFullPathName());
    }
}

bool HolotonerProcessor::isSupportedAudioFile(const juce::File& file)
{
    return file.hasFileExtension("wav;aif;aiff;flac;mp3;m4a;caf");
}

void HolotonerProcessor::loadSourceFile(const juce::File& file, bool switchToFile)
{
    sourceLoadsPending_.fetch_add(1);
    sourceLoader_.addJob([this, file, switchToFile] {
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

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new HolotonerProcessor(); }
