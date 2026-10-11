#include "plugin/Source/PluginProcessor.h"
#include <juce_audio_utils/juce_audio_utils.h>
#include <chrono>
#include <cmath>
#include <iostream>

namespace {
int failures = 0;
void check(bool condition, const char* message)
{
    if (!condition) { std::cerr << message << '\n'; ++failures; }
}
void set(HolotonerProcessor& processor, const char* id, float value)
{
    auto* parameter = processor.state().getParameter(id);
    parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}
bool waitForMask(HolotonerProcessor& processor)
{
    for (int i = 0; i < 300; ++i) {
        juce::Thread::sleep(10);
        juce::Timer::callPendingTimersSynchronously();
        if (const auto mask = processor.imageMask(); mask != nullptr && mask->hasSource)
            return true;
    }
    return false;
}
}

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    HolotonerProcessor processor;
    processor.setSourceKind(holotoner::SourceKind::Input);
    set(processor, "image_target", 3.0f);
    set(processor, "image_amount", 1.0f);
    set(processor, "image_binary", 1.0f);
    set(processor, "image_threshold", 0.6f);
    set(processor, "scale_x", 0.8f);
    set(processor, "scale_y", 0.8f);
    juce::Image fixture(juce::Image::RGB, 128, 128, true);
    juce::Graphics drawing(fixture);
    drawing.setColour(juce::Colours::white);
    drawing.drawEllipse(18, 18, 92, 92, 5);
    drawing.fillEllipse(40, 44, 10, 10);
    drawing.fillEllipse(78, 44, 10, 10);
    drawing.drawLine(44, 82, 84, 82, 5);
    juce::TemporaryFile file(".png");
    {
        juce::FileOutputStream stream(file.getFile());
        juce::PNGImageFormat png;
        check(png.writeImageToStream(fixture, stream), "fixture PNG encoding failed");
    }
    processor.loadImageFile(file.getFile());
    check(waitForMask(processor), "image/mask publication timed out");
    const auto mask = processor.imageMask();
    check(mask != nullptr && mask->hasPhasePoints, "native phase distribution missing");

    juce::MemoryBlock state;
    processor.getStateInformation(state);
    HolotonerProcessor restored;
    restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    check(waitForMask(restored), "embedded state image restore timed out");
    const auto restoredMask = restored.imageMask();
    check(mask != nullptr && restoredMask != nullptr && mask->values == restoredMask->values,
          "native state round-trip changed image map");
    check(mask != nullptr && restoredMask != nullptr && mask->fieldAngles == restoredMask->fieldAngles,
          "native state round-trip changed projected steering field");
    check(restored.state().getRawParameterValue("image_target")->load() == 3.0f,
          "phase target did not restore");
    check(std::abs(restored.state().getRawParameterValue("image_threshold")->load() - 0.6f) < 1.0e-6f,
          "threshold did not restore");
    set(processor, "image_projection", 1.0f);
    processor.getStateInformation(state);
    restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    check(restored.state().getRawParameterValue("image_projection")->load() == 1.0f,
          "polar projection did not restore");
    set(processor, "image_projection", 0.0f);
    const auto beforeMove = processor.imageMask();
    set(processor, "position_x", 0.25f);
    for (int i = 0; i < 300 && processor.imageMask() == beforeMove; ++i) {
        juce::Thread::sleep(10);
        juce::Timer::callPendingTimersSynchronously();
    }
    const auto moved = processor.imageMask();
    check(moved != nullptr && beforeMove != nullptr && moved->fieldAngles != beforeMove->fieldAngles,
          "geometry-only gesture did not rebuild steering field");
    set(processor, "position_x", 0.0f);
    for (int i = 0; i < 300 && processor.imageMask() == moved; ++i) {
        juce::Thread::sleep(10);
        juce::Timer::callPendingTimersSynchronously();
    }

    processor.prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;
    std::uint64_t position = 0;
    const auto started = std::chrono::steady_clock::now();
    for (int block = 0; block < 5625; ++block) {
        for (int i = 0; i < 512; ++i) {
            const auto value = static_cast<float>(0.2 * std::sin(2.0 * 3.141592653589793 * 440.0
                                                               * static_cast<double>(position++) / 48000.0));
            buffer.setSample(0, i, value);
            buffer.setSample(1, i, value);
        }
        processor.processBlock(buffer, midi);
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::cout << "60s native Phase map including sample history: " << elapsed << "s ("
              << 100.0 * elapsed / 60.0 << "% of real-time budget)\n";
    std::array<HolotonerProcessor::ScopeSample, 8192> samples {};
    const auto count = processor.scopeSamples(samples.data(), static_cast<int>(samples.size()));
    check(count == static_cast<int>(samples.size()), "actual scope history missing");
    bool stereo = false;
    for (int i = 0; i < count; ++i) {
        const auto& sample = samples[static_cast<std::size_t>(i)];
        check(std::isfinite(sample.outputLeft) && std::isfinite(sample.outputRight), "invalid scope sample");
        stereo = stereo || std::abs(sample.outputLeft - sample.outputRight) > 0.001f;
    }
    check(stereo, "native actual scope contains no image stereo");
    // Exercise the same off-thread file-source path used by Standalone,
    // without opening an audio device or retaining user media.
    juce::TemporaryFile audioFile(".wav");
    {
        juce::AudioBuffer<float> tone(2, 48000);
        for (int i = 0; i < tone.getNumSamples(); ++i) {
            double mixture = 0.0;
            for (int partial = 1; partial <= 16; ++partial)
                mixture += 0.009*std::sin(2.0*3.141592653589793*(97.0*partial+7.0*partial*partial)
                    *static_cast<double>(i)/48000.0 + partial*2.399963229728653);
            const auto value = static_cast<float>(mixture);
            tone.setSample(0, i, value);
            tone.setSample(1, i, value);
        }
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = audioFile.getFile().createOutputStream();
        auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions {}
            .withSampleRate(48000.0).withChannelLayout(juce::AudioChannelSet::stereo()).withBitsPerSample(24));
        check(writer != nullptr && writer->writeFromAudioSampleBuffer(tone, 0, tone.getNumSamples()),
              "file-source fixture encoding failed");
    }
    processor.loadSourceFile(audioFile.getFile());
    for (int i = 0; i < 300 && processor.isLoadingSource(); ++i)
        juce::Thread::sleep(10);
    check(processor.sourceKind() == holotoner::SourceKind::File, "native file source failed to load");
    float filePeak = 0.0f;
    for (int block = 0; block < 300; ++block) {
        buffer.clear();
        processor.processBlock(buffer, midi);
        filePeak = std::max(filePeak, buffer.getMagnitude(0, 512));
    }
    check(filePeak > 0.001f, "file source did not reach phase-map output");
    auto editor = std::unique_ptr<juce::AudioProcessorEditor>(processor.createEditor());
    juce::Timer::callPendingTimersSynchronously();
    const auto renderStarted = std::chrono::steady_clock::now();
    auto snapshot = editor->createComponentSnapshot(editor->getLocalBounds(), true, 1.0f);
    const auto renderElapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - renderStarted).count();
    std::cout << "Editor snapshot: " << renderElapsed << "ms\n";
    juce::FileOutputStream output(juce::File("/tmp/holotoner-phase-editor.png"));
    check(output.setPosition(0) && output.truncate().wasOk(), "diagnostic snapshot reset failed");
    juce::PNGImageFormat png;
    check(png.writeImageToStream(snapshot, output), "editor snapshot failed");
    return failures == 0 ? 0 : 1;
}
