#include "PluginProcessor.h"

#include <juce_audio_utils/juce_audio_utils.h>

namespace {

constexpr int kStateVersion = 1;

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

    if (inputChannels == 0 || outputChannels < 2) {
        audio.clear();
        return;
    }
    if (inputChannels == 1)
        audio.copyFrom(1, 0, audio, 0, 0, samples);

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
    if (const auto xml = state.createXml())
        copyXmlToBinary(*xml, destination);
}

void HolotonerProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary(data, sizeInBytes);
    if (xml == nullptr || !xml->hasTagName(parameters_.state.getType()))
        return;
    const auto state = juce::ValueTree::fromXml(*xml);
    const auto version = static_cast<int>(state.getProperty("format_version", 0));
    if (version == kStateVersion)
        parameters_.replaceState(state);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new HolotonerProcessor(); }
