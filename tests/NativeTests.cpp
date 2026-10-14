#include "core/image/ShapeGenerator.h"
#include "plugin/Source/PluginProcessor.h"
#include "plugin/Source/TextSource.h"
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
        if (const auto mask = processor.imageMask(); mask != nullptr && mask->hasSource
            && !processor.isLoadingImage() && processor.imageDocument() != nullptr)
            return true;
    }
    return false;
}

class TestPlayHead final : public juce::AudioPlayHead {
public:
    double ppq = 0, bpm = 120;
    bool playing = true;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setPpqPosition(ppq); info.setBpm(bpm); info.setIsPlaying(playing);
        return info;
    }
};

bool waitForImage(HolotonerProcessor& processor, const juce::String& name)
{
    for (int i = 0; i < 300; ++i) {
        juce::Thread::sleep(10);
        juce::Timer::callPendingTimersSynchronously();
        if (const auto document = processor.imageDocument(); document != nullptr && document->name == name
            && !processor.isLoadingImage() && processor.imageMask() != nullptr)
            return true;
    }
    return false;
}

void smoothStateTests()
{
    auto processor = std::make_unique<HolotonerProcessor>();
    auto restored = std::make_unique<HolotonerProcessor>();
    set(*processor, "image_smooth", 0.0007f);
    juce::MemoryBlock state;
    processor->getStateInformation(state);
    restored->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    check(std::abs(restored->state().getRawParameterValue("image_smooth")->load()-0.0007f)<1.0e-8f,
          "Smooth milliseconds did not restore");
    auto legacy = juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
    for (auto* child = legacy->getFirstChildElement(); child;) {
        auto* next = child->getNextElement();
        if (child->getStringAttribute("id") == "image_smooth") legacy->removeChildElement(child,true);
        child = next;
    }
    juce::AudioProcessor::copyXmlToBinary(*legacy,state);
    restored->setStateInformation(state.getData(),static_cast<int>(state.getSize()));
    check(restored->state().getRawParameterValue("image_smooth")->load()==0.0f,
          "Pre-Smooth state retained the running processor's Smooth");
}

// Motion's Reset: every axis Rate and note off, Swing and Depth to defaults; the base
// geometry and other sections stay as they were.
void motionResetTests()
{
    HolotonerProcessor processor;
    set(processor, "roll_rate", 1.5f); set(processor, "yaw_rate_sync", 9.0f); set(processor, "scale_y_rate", 0.3f);
    set(processor, "motion_depth", 0.9f); set(processor, "motion_shape", 1.0f);
    set(processor, "yaw", 40.0f); set(processor, "wet", 0.5f);
    auto editor = std::unique_ptr<juce::AudioProcessorEditor>(processor.createEditor());
    juce::TextButton* reset = nullptr;
    std::function<void(juce::Component&)> find = [&](juce::Component& c) {
        if (auto* button = dynamic_cast<juce::TextButton*>(&c); button != nullptr && button->getButtonText() == "Reset")
            reset = button;
        for (auto* child : c.getChildren()) find(*child);
    };
    find(*editor);
    check(reset != nullptr, "Motion has no Reset button");
    if (reset == nullptr) return;
    reset->onClick();
    const auto value = [&](const juce::String& id) { return processor.state().getRawParameterValue(id)->load(); };
    for (const auto* axis : HolotonerProcessor::kMotionAxes)
        check(value(juce::String(axis)+"_rate") == 0.0f && value(juce::String(axis)+"_rate_sync") == 0.0f,
              "Motion Reset left an axis moving");
    check(std::abs(value("motion_depth")-0.5f) < 1.0e-6f && value("motion_shape") == 0.0f,
          "Motion Reset did not restore Swing and Depth");
    check(std::abs(value("yaw")-40.0f) < 1.0e-4f && std::abs(value("wet")-0.5f) < 1.0e-6f,
          "Motion Reset changed base geometry or another section");
}

void shapeSourceTests()
{
    HolotonerProcessor shape;
    check(juce::roundToInt(shape.state().getRawParameterValue("shape_kind")->load()) == static_cast<int>(holotoner::ShapeKind::Smiley),
          "default shape choice is not Smiley");
    check(waitForImage(shape, "Smiley 3") && shape.shapeActive(), "default Smiley did not publish");
    set(shape, "shape_kind", 4.0f); // Sierpinski
    set(shape, "shape_order", 5.0f);
    shape.useShape();
    check(waitForImage(shape, "Sierpinski 5") && shape.shapeActive(), "shape source did not publish");
    set(shape, "shape_order", 2.0f); // automation alone regenerates an active shape
    check(waitForImage(shape, "Sierpinski 2"), "shape order change did not regenerate");
    juce::MemoryBlock state;
    shape.getStateInformation(state);
    const auto xml = juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
    check(xml != nullptr && xml->getChildByName("Shape") != nullptr && xml->getChildByName("Image") == nullptr,
          "shape state is not parametric");
    HolotonerProcessor restored;
    restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    check(waitForImage(restored, "Sierpinski 2") && restored.shapeActive()
              && restored.imageMask()->values == shape.imageMask()->values,
          "shape did not restore identically");
    set(restored, "shape_kind", static_cast<float>(holotoner::ShapeKind::Smiley));
    check(waitForImage(restored, "Smiley 2"), "smiley choice did not generate");
    juce::MemoryBlock smileyState;
    restored.getStateInformation(smileyState);
    auto smileyRestored = std::make_unique<HolotonerProcessor>();
    smileyRestored->setStateInformation(smileyState.getData(), static_cast<int>(smileyState.getSize()));
    check(waitForImage(*smileyRestored, "Smiley 2")
              && smileyRestored->imageMask()->values == restored.imageMask()->values,
          "smiley shape did not restore identically");
    restored.setText("Hi");
    for (int i = 0; i < 300 && (restored.isLoadingImage() || restored.text() != "Hi"); ++i) {
        juce::Thread::sleep(10);
        juce::Timer::callPendingTimersSynchronously();
    }
    check(!restored.shapeActive(), "text did not replace the shape source");

    // Echo ♪: a quarter note at 120 BPM (no host tempo here) resolves to 0.5 s on the audio thread.
    {
        HolotonerProcessor synced;
        set(synced, "space_echo_time_sync", 15.0f); // 1/4
        synced.prepareToPlay(48000.0, 512);
        juce::AudioBuffer<float> block(2, 512);
        block.clear();
        juce::MidiBuffer none;
        synced.processBlock(block, none);
        check(std::abs(synced.echoTimeSeconds() - 0.5) < 1.0e-6, "echo note did not resolve at the tempo");
    }
    // Diagnostic snapshots: a shape source with every Space cue on, plane moved left and turned.
    set(shape, "position_x", -0.6f);
    set(shape, "yaw", 35.0f);
    for (const auto* id : {"space_interfere", "space_echo", "space_feedback", "space_resonate"}) set(shape, id, 0.8f);
    set(shape, "pitch_rate_sync", 12.0f);       // 1/8: shows the compact note menus
    set(shape, "space_echo_time_sync", 3.0f);   // 1/64T: the longest note name
    shape.motionPanelOpen = true;
    auto editor = std::unique_ptr<juce::AudioProcessorEditor>(shape.createEditor());
    juce::PNGImageFormat png;
    for (const auto& [name, small] : {std::pair {"/tmp/holotoner-space-editor.png", false},
                                      std::pair {"/tmp/holotoner-space-small.png", true}}) {
        if (small) editor->setSize(900, editor->getConstrainer()->getMinimumHeight());
        juce::Thread::sleep(80);
        juce::Timer::callPendingTimersSynchronously();
        auto snapshot = editor->createComponentSnapshot(editor->getLocalBounds(), true, 1.0f);
        juce::FileOutputStream output {juce::File(name)};
        output.setPosition(0); output.truncate();
        check(png.writeImageToStream(snapshot, output), "space snapshot failed");
    }
}

void textAndMotionTests()
{
    juce::String error;
    const auto emoji = juce::String::fromUTF8("👨‍👩‍👧‍👦 👍🏽 🇫🇮 á");
    check(textSource::segments(emoji,1,error).size() == 4 && error.isEmpty(),
          "grapheme playback split a joined emoji, modifier, flag or combining character");
    check(textSource::segments("one  two\nthree",2,error).size() == 3,"word segmentation failed");
    auto family = textSource::render(juce::String::fromUTF8("👨‍👩‍👧‍👦"));
    int opaque = 0;
    for (int y = 0; y < family.getHeight(); ++y)
        for (int x = 0; x < family.getWidth(); ++x) opaque += family.getPixelAt(x,y).getAlpha() > 0 ? 1 : 0;
    check(opaque > 1000, "emoji rendering produced an empty image");
    juce::FileOutputStream emojiOutput(juce::File("/tmp/holotoner-emoji.png"));
    emojiOutput.setPosition(0); emojiOutput.truncate();
    juce::PNGImageFormat png;
    png.writeImageToStream(family,emojiOutput);

    HolotonerProcessor text;
    set(text,"text_layout",1);
    text.setText(emoji);
    check(waitForMask(text) && text.textFrameCount() == 4,"text source frames failed to publish");
    const auto first = text.imageMask();
    juce::MemoryBlock saved;
    text.getStateInformation(saved);
    HolotonerProcessor restored;
    restored.setStateInformation(saved.getData(),static_cast<int>(saved.getSize()));
    juce::MemoryBlock immediate;
    restored.getStateInformation(immediate);
    auto immediateXml = juce::AudioProcessor::getXmlFromBinary(immediate.getData(),static_cast<int>(immediate.getSize()));
    check(immediateXml != nullptr && immediateXml->getChildByName("Text") != nullptr,
        "saving during asynchronous text restore lost embedded media");
    check(waitForMask(restored) && restored.text() == emoji && restored.textFrameCount() == 4,
          "text metadata or embedded frames did not restore");
    check(first != nullptr && restored.imageMask() != nullptr && first->values == restored.imageMask()->values,
          "text restore re-rasterized or changed embedded bitmap");
    {
        auto textEditor = std::unique_ptr<juce::AudioProcessorEditor>(restored.createEditor());
        for (auto* child : textEditor->getChildren())
            if (auto* button = dynamic_cast<juce::TextButton*>(child);
                button != nullptr && button->getButtonText() == "Text")
                { button->setToggleState(true,juce::dontSendNotification); button->onClick(); }
        textEditor->resized();
        auto snapshot = textEditor->createComponentSnapshot(textEditor->getLocalBounds(),true,1.0f);
        juce::FileOutputStream output(juce::File("/tmp/holotoner-text-motion-editor.png"));
        output.setPosition(0); output.truncate();
        check(png.writeImageToStream(snapshot,output),"expanded text/motion snapshot failed");
        textEditor->setSize(900,textEditor->getConstrainer()->getMinimumHeight());
        auto small = textEditor->createComponentSnapshot(textEditor->getLocalBounds(),true,1.0f);
        juce::FileOutputStream smallOutput(juce::File("/tmp/holotoner-text-motion-small.png"));
        smallOutput.setPosition(0); smallOutput.truncate();
        check(png.writeImageToStream(small,smallOutput),"small text/motion snapshot failed");
    }
    {
        auto legacy = juce::AudioProcessor::getXmlFromBinary(saved.getData(),static_cast<int>(saved.getSize()));
        legacy->setAttribute("format_version",8);
        auto* oldText = legacy->getChildByName("Text");
        oldText->removeAttribute("renderer_version");
        oldText->setAttribute("value","A"); oldText->setAttribute("layout",0);
        while (auto* child = oldText->getFirstChildElement()) oldText->removeChildElement(child,true);
        juce::Image inset(juce::Image::ARGB,512,512,true);
        juce::Graphics g(inset); g.setColour(juce::Colours::white); g.setFont(juce::FontOptions(180.0f));
        g.drawFittedText("A",inset.getBounds().reduced(28),juce::Justification::centred,8,0.05f);
        juce::MemoryBlock bytes; juce::MemoryOutputStream encoded(bytes,false);
        png.writeImageToStream(inset,encoded);
        auto* frame = oldText->createNewChildElement("Frame"); frame->setAttribute("name","A");
        frame->addTextElement(bytes.toBase64Encoding());
        for (const auto& [id,value] : std::array<std::pair<const char*,float>,3> {
            std::pair {"motion_target",6.0f},{"motion_rate",0.7f},{"motion_rate_sync",12.0f}}) {
            auto* parameter = legacy->createNewChildElement("PARAM");
            parameter->setAttribute("id",id); parameter->setAttribute("value",value);
        }
        juce::MemoryBlock oldState; juce::AudioProcessor::copyXmlToBinary(*legacy,oldState);
        restored.setStateInformation(oldState.getData(),static_cast<int>(oldState.getSize()));
        check(waitForMask(restored),"version 8 text/motion migration timed out");
        int active = 0;
        if (const auto mask = restored.imageMask())
            for (const auto value : mask->values) active += value>0.5f ? 1 : 0;
        check(active>300,"saved inset A did not migrate to fitted ink");
        check(std::abs(restored.state().getRawParameterValue("scale_x_rate")->load()-0.7f)<1.0e-6f
            && std::abs(restored.state().getRawParameterValue("scale_y_rate")->load()-0.7f)<1.0e-6f
            && std::abs(restored.state().getRawParameterValue("scale_y_rate_sync")->load()-12)<1.0e-6f,
            "legacy Scale motion did not migrate to both independent axes");
        restored.getStateInformation(oldState);
        auto clean = juce::AudioProcessor::getXmlFromBinary(oldState.getData(),static_cast<int>(oldState.getSize()));
        check(clean->getChildByName("Text")->getIntAttribute("renderer_version")==2
            && !clean->toString().contains("motion_target"),"legacy text/motion metadata remained in resaved state");
    }
    text.setText("newest");
    text.setText("OK");
    check(waitForMask(text) && text.text() == "OK" && text.textFrameCount() == 2,"stale text job replaced latest source");
    text.setText(juce::String::repeatedString("a",33));
    for (int i = 0; i < 100 && text.isLoadingImage(); ++i) { juce::Thread::sleep(10); juce::Timer::callPendingTimersSynchronously(); }
    check(text.text() == "OK" && text.lastImageMessage().isNotEmpty(),"oversized text silently truncated or lost source");

    for (const auto rate : {44100.0,48000.0,96000.0}) {
        std::vector<float> reference;
        for (const auto blockSize : {512,1,31,97}) {
            HolotonerProcessor moving;
            moving.setSourceKind(holotoner::SourceKind::Input);
            set(moving,"text_layout",2);
            set(moving,"motion_shape",1);
            set(moving,"roll_rate_sync",15);
            set(moving,"position_x_rate_sync",20);
            set(moving,"scale_y_rate",0.17f);
            set(moving,"text_play",1);
            set(moving,"text_rate_sync",9); // Sixteenth-note word steps.
            moving.setText("L T");
            check(waitForMask(moving),"motion field compilation timed out");
            TestPlayHead head;
            moving.setPlayHead(&head);
            moving.prepareToPlay(rate,blockSize);
            const auto baseRoll = moving.planeTransform().rollDegrees;
            std::vector<float> result;
            juce::MidiBuffer midi;
            constexpr int total = 32768;
            result.reserve(total*2);
            for (int offset = 0; offset < total;) {
                const int count = std::min(blockSize,total-offset);
                juce::AudioBuffer<float> audio(2,count);
                for (int i = 0; i < count; ++i) {
                    const float value = static_cast<float>(0.1*std::sin(2*3.141592653589793*173*(offset+i)/rate)
                        + 0.08*std::sin(2*3.141592653589793*619*(offset+i)/rate));
                    audio.setSample(0,i,value); audio.setSample(1,i,value);
                }
                head.ppq = 2.0*offset/rate;
                moving.processBlock(audio,midi);
                for (int i = 0; i < count; ++i) {
                    result.push_back(audio.getSample(0,i)); result.push_back(audio.getSample(1,i));
                }
                offset += count;
            }
            if (reference.empty()) reference = result;
            else {
                float difference = 0;
                for (std::size_t i = 0; i < result.size(); ++i) difference = std::max(difference,std::abs(result[i]-reference[i]));
                std::cout << "Synced text/motion partition " << rate << '/' << blockSize << ": " << difference << '\n';
                check(difference < 1.0e-5f,"audio-timed text/motion depends on host block size");
            }
            check(std::abs(moving.planeTransform().rollDegrees-baseRoll) < 1.0e-6f,"Motion wrote host base rotation");
            head.ppq = 0.25; // One sixteenth step: frame T.
            juce::AudioBuffer<float> audio(2,512); audio.clear();
            moving.processBlock(audio,midi);
            check(moving.imageDocument()->name == "T","host seek did not select the PPQ text frame");
            head.ppq = -0.25; moving.processBlock(audio,midi);
            check(moving.imageDocument()->name == "T","negative PPQ frame selection failed");
            head.playing = false;
            // Let the normal geometry/field smoothing settle after the seek.
            for (int i = 0; i < 64; ++i) moving.processBlock(audio,midi);
            const auto paused = moving.displayedPlaneTransform();
            moving.processBlock(audio,midi);
            check(std::abs(paused.rollDegrees-moving.displayedPlaneTransform().rollDegrees)<1.0e-6f,"stopped transport advanced synced motion");
            moving.setPlayHead(nullptr);
        }
    }
    juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Standalone);
    HolotonerProcessor local;
    juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Undefined);
    set(local,"text_layout",2); set(local,"text_play",1); set(local,"text_rate_sync",15);
    set(local,"motion_shape",1); set(local,"roll_rate_sync",15);
    local.setText("L T");
    check(waitForMask(local),"Standalone text/motion failed to compile");
    local.prepareToPlay(48000,512);
    juce::AudioBuffer<float> audio(2,512); juce::MidiBuffer midi;
    for (int i = 0; i < 48; ++i) local.processBlock(audio,midi);
    check(local.imageDocument()->name == "T","Standalone 120 BPM clock failed");
    local.setSourcePlaying(false);
    for (int i = 0; i < 64; ++i) local.processBlock(audio,midi);
    const auto stopped = local.displayedPlaneTransform();
    for (int i = 0; i < 64; ++i) local.processBlock(audio,midi);
    check(local.imageDocument()->name == "T"
        && std::abs(stopped.rollDegrees-local.displayedPlaneTransform().rollDegrees)<1.0e-6f,
        "Standalone Stop advanced text or Motion");
    set(local,"tempo",60);
    local.setSourcePlaying(true);
    for (int i = 0; i < 48; ++i) local.processBlock(audio,midi);
    check(local.imageDocument()->name == "T","Standalone tempo change ignored or reset the beat clock");
    local.restartSource(); local.processBlock(audio,midi);
    check(local.imageDocument()->name == "L","Standalone Restart did not reset the text clock");
    set(local,"text_rate_sync",0); set(local,"text_rate",1);
    local.prepareToPlay(48000,512);
    for (int i = 0; i < 100; ++i) local.processBlock(audio,midi);
    check(local.imageDocument()->name == "T","free-Hz text playback failed");
}

void letterARegression()
{
    HolotonerProcessor processor;
    processor.setSourceKind(holotoner::SourceKind::Input);
    set(processor,"image_gain",0.02f);
    set(processor,"image_binary",1);
    set(processor,"image_threshold",0.967f);
    processor.setText("A");
    check(waitForMask(processor),"A raster/mask failed to publish");
    const auto mask = processor.imageMask();
    if (mask == nullptr) return;
    int active = 0;
    std::array<bool,4096> visited {};
    std::vector<int> queue;
    int largest = 0;
    for (int i = 0; i < 4096; ++i) {
        if (mask->values[static_cast<std::size_t>(i)]<0.5f) continue;
        ++active;
        if (visited[static_cast<std::size_t>(i)]) continue;
        queue.clear(); queue.push_back(i); visited[static_cast<std::size_t>(i)] = true;
        for (std::size_t next = 0; next < queue.size(); ++next) {
            const int px = queue[next]%64,py = queue[next]/64;
            for (int y = -1; y <= 1; ++y)
                for (int x = -1; x <= 1; ++x) {
                    const int nx = px+x,ny = py+y;
                    if (nx<0 || nx>=64 || ny<0 || ny>=64) continue;
                    const auto at = static_cast<std::size_t>(ny*64+nx);
                    if (!visited[at] && mask->values[at]>=0.5f) {
                        visited[at] = true; queue.push_back(static_cast<int>(at));
                    }
                }
        }
        largest = std::max(largest,static_cast<int>(queue.size()));
    }
    std::cout << "A active/largest-connected pixels: " << active << '/' << largest << '\n';
    check(active>300 && largest>=active*9/10,"A was reduced to disconnected dots by thresholding");
    set(processor,"image_threshold",1);
    const auto beforeEndpoint = processor.imageMask();
    for (int i = 0; i < 300 && processor.imageMask()==beforeEndpoint; ++i) {
        juce::Thread::sleep(10); juce::Timer::callPendingTimersSynchronously();
    }
    check(processor.imageMask()!=nullptr && processor.imageMask()->hasNearestSupport,"threshold 100% erased pure-white ink");
    set(processor,"image_threshold",0.967f);
    const auto endpoint = processor.imageMask();
    for (int i = 0; i < 300 && processor.imageMask()==endpoint; ++i) {
        juce::Thread::sleep(10); juce::Timer::callPendingTimersSynchronously();
    }
    processor.prepareToPlay(48000,512);
    juce::AudioBuffer<float> buffer(2,512); juce::MidiBuffer midi;
    int hits = 0,total = 0,left = 0,right = 0;
    for (int block = 0; block < 300; ++block) {
        for (int i = 0; i < 512; ++i) {
            double value = 0;
            for (int p = 1; p <= 16; ++p)
                value += 0.009*std::sin(2*3.141592653589793*(97.0*p+7.0*p*p)*(block*512+i)/48000+p*2.399963229728653);
            buffer.setSample(0,i,static_cast<float>(value)); buffer.setSample(1,i,static_cast<float>(value));
        }
        processor.processBlock(buffer,midi);
        if (block<64) continue;
        for (int i = 0; i < 512; ++i) {
            const auto point = holotoner::stereoToScopePoint(buffer.getSample(0,i)*5,buffer.getSample(1,i)*5);
            const auto radius = std::hypot(point.x,point.y);
            if (radius<0.35f || radius>0.8f) continue;
            const int px = static_cast<int>(std::lround((point.x*2+1)*31.5f));
            const int py = static_cast<int>(std::lround((2-2*point.y)*31.5f));
            bool supported = false;
            for (int y = -2; y <= 2; ++y)
                for (int x = -2; x <= 2; ++x) supported = supported || mask->at(px+x,py+y)>0.5f;
            ++total; hits += supported ? 1 : 0;
            left += supported && point.x < -0.05f ? 1 : 0;
            right += supported && point.x > 0.05f ? 1 : 0;
        }
    }
    std::cout << "A measured support: " << hits << '/' << total << ", left/right: " << left << '/' << right << '\n';
    check(total>1000 && hits>total*8/10 && left>500 && right>500,"actual polar audio did not retain both legs of A");
    auto editor = std::unique_ptr<juce::AudioProcessorEditor>(processor.createEditor());
    const auto before = editor->getBounds();
    // Decision 19: the seven geometry axes are linear rows in the Motion panel; everything else is a knob.
    std::function<void(juce::Component&,int&,int&,int&)> inspect = [&](juce::Component& c,int& knobs,int& rows,int& other) {
        if (auto* slider = dynamic_cast<juce::Slider*>(&c)) {
            const auto style = slider->getSliderStyle();
            ++(style == juce::Slider::RotaryHorizontalVerticalDrag ? knobs : style == juce::Slider::LinearHorizontal ? rows : other);
        }
        for (auto* child : c.getChildren()) inspect(*child,knobs,rows,other);
    };
    int knobs = 0, rows = 0, other = 0;
    inspect(*editor,knobs,rows,other);
    check(knobs>=19 && rows==7 && other==0,"editor controls do not match the Motion-panel layout");
    for (auto* child : editor->getChildren())
        if (auto* button = dynamic_cast<juce::TextButton*>(child); button != nullptr && button->getButtonText()=="Text") {
            button->setToggleState(true,juce::dontSendNotification); button->onClick();
            check(editor->getBounds()==before,"Text overlay resized the editor");
            button->setToggleState(false,juce::dontSendNotification); button->onClick();
        }
    auto snapshot = editor->createComponentSnapshot(editor->getLocalBounds(),true,1);
    juce::FileOutputStream output(juce::File("/tmp/holotoner-letter-a-editor.png"));
    output.setPosition(0); output.truncate();
    juce::PNGImageFormat png; check(png.writeImageToStream(snapshot,output),"A snapshot failed");
    // Base automation is now consumed on audio, not waiting for a message-thread map rebuild.
    set(processor,"position_x",0.25f);
    for (int i = 0; i < 100; ++i) processor.processBlock(buffer,midi);
    check(processor.displayedPlaneTransform().positionX>0.24f,"geometry automation waited for a GUI timer");
}
}

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    HolotonerProcessor processor;
    processor.setSourceKind(holotoner::SourceKind::Input);
    set(processor, "image_amount", 1.0f);
    set(processor, "image_binary", 1.0f);
    set(processor, "image_threshold", 0.6f);
    set(processor, "scale_x", 0.8f);
    set(processor, "scale_y", 0.8f);
    auto editor = std::unique_ptr<juce::AudioProcessorEditor>(processor.createEditor());
    auto emptyScope = editor->createComponentSnapshot(editor->getLocalBounds(), true, 1.0f);
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
    juce::Thread::sleep(40);
    juce::Timer::callPendingTimersSynchronously();
    const auto imageOnlyScope = editor->createComponentSnapshot(editor->getLocalBounds(), true, 1.0f);
    bool unchangedSilentPolar = true;
    for (int y = 85; y < 470; ++y)
        for (int x = editor->getWidth()/2+14; x < editor->getWidth()-20; ++x)
            unchangedSilentPolar = unchangedSilentPolar
                && emptyScope.getPixelAt(x,y) == imageOnlyScope.getPixelAt(x,y);
    check(unchangedSilentPolar, "loading an image changed the silent polar view");

    juce::MemoryBlock state;
    processor.getStateInformation(state);
    HolotonerProcessor restored;
    restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    check(waitForMask(restored), "embedded state image restore timed out");
    const auto restoredMask = restored.imageMask();
    check(mask != nullptr && restoredMask != nullptr && mask->values == restoredMask->values,
          "native state round-trip changed image map");
    check(mask != nullptr && restoredMask != nullptr
        && std::equal(mask->nearestSupport.begin(),mask->nearestSupport.end(),restoredMask->nearestSupport.begin(),
            [](auto a,auto b) { return std::abs(a.x-b.x)<1.0e-6f && std::abs(a.y-b.y)<1.0e-6f; }),
          "native state round-trip changed image-coordinate support");
    check(restored.state().getParameter("image_target") == nullptr, "obsolete mode parameter still exposed");
    check(std::abs(restored.state().getRawParameterValue("image_threshold")->load() - 0.6f) < 1.0e-6f,
          "threshold did not restore");
    // Legacy modes are metadata migration, not dormant DSP/host parameters.
    auto legacy = juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
    legacy->setAttribute("format_version", 6);
    auto* oldMode = legacy->createNewChildElement("PARAM");
    oldMode->setAttribute("id", "image_target");
    oldMode->setAttribute("value", 0.0);
    juce::MemoryBlock legacyState;
    juce::AudioProcessor::copyXmlToBinary(*legacy, legacyState);
    restored.setStateInformation(legacyState.getData(), static_cast<int>(legacyState.getSize()));
    check(waitForMask(restored), "legacy Off state image restore failed");
    check(std::abs(restored.state().getRawParameterValue("image_amount")->load()) < 1.0e-6f,
          "old Off mode did not migrate to Amount zero");
    oldMode->setAttribute("value", 1.0);
    juce::AudioProcessor::copyXmlToBinary(*legacy, legacyState);
    restored.setStateInformation(legacyState.getData(), static_cast<int>(legacyState.getSize()));
    check(waitForMask(restored), "legacy spectral state image restore failed");
    check(std::abs(restored.state().getRawParameterValue("image_amount")->load()-1.0f) < 1.0e-6f,
          "old spectral mode did not migrate to the single phase engine");
    juce::MemoryBlock resaved;
    restored.getStateInformation(resaved);
    const auto cleanState = juce::AudioProcessor::getXmlFromBinary(resaved.getData(), static_cast<int>(resaved.getSize()));
    check(!cleanState->toString().contains("image_target"), "retired mode persisted in new state");
    set(processor, "image_projection", 1.0f);
    processor.getStateInformation(state);
    restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    check(restored.state().getRawParameterValue("image_projection")->load() == 1.0f,
          "polar projection did not restore");
    set(processor, "image_projection", 0.0f);
    const auto beforeMove = processor.imageMask();
    set(processor, "position_x", 0.25f);
    juce::Timer::callPendingTimersSynchronously();
    const auto moved = processor.imageMask();
    check(moved == beforeMove,"geometry unnecessarily rebuilt a pose-independent image field");
    set(processor, "position_x", 0.0f);

    processor.prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;
    std::uint64_t position = 0;
    const auto measure = [&](const char* name) {
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
        std::cout << "60s native " << name << ": " << elapsed << "s ("
                  << 100.0 * elapsed / 60.0 << "% of real-time budget)\n";
    };
    measure("Phase map including sample history");
    set(processor, "image_smooth", 0.001f);
    measure("Phase map with Smooth 1 ms");
    set(processor, "image_smooth", 0.0f);
    for (const auto* id : {"space_interfere", "space_echo", "space_feedback", "space_resonate"}) set(processor, id, 1.0f);
    measure("Phase map with all Space cues at 100%");
    for (const auto* id : {"space_interfere", "space_echo", "space_feedback", "space_resonate"}) set(processor, id, 0.0f);
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
    // Refresh the left image preview only; polar must always use actual samples.
    juce::Thread::sleep(40);
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
    textAndMotionTests();
    shapeSourceTests();
    smoothStateTests();
    motionResetTests();
    letterARegression();
    return failures == 0 ? 0 : 1;
}
