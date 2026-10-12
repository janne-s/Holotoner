#include "PluginEditor.h"

#include "core/geometry/PlaneProjection.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace {

using holotoner::PlaneTransform;
using holotoner::ProjectedPlane;
using holotoner::SourceKind;
namespace style = holotoner_style;

constexpr float pi = 3.14159265358979323846f;

int sourceItem(SourceKind kind) { return static_cast<int>(kind) + 1; }

float clamp(float value, float minimum, float maximum) { return std::clamp(value, minimum, maximum); }

juce::RangedAudioParameter* parameter(HolotonerProcessor& processor, const char* id)
{
    return processor.state().getParameter(id);
}

void setParameter(HolotonerProcessor& processor, const char* id, float value)
{
    if (auto* target = parameter(processor, id))
        target->setValueNotifyingHost(target->convertTo0to1(value));
}

void beginParameter(HolotonerProcessor& processor, const char* id)
{
    if (auto* target = parameter(processor, id))
        target->beginChangeGesture();
}

void endParameter(HolotonerProcessor& processor, const char* id)
{
    if (auto* target = parameter(processor, id))
        target->endChangeGesture();
}


} // namespace

class HolotonerEditor::KnobCell final : public juce::Component {
public:
    KnobCell(HolotonerProcessor& owner,const juce::String& key,const juce::String& name,bool synced = false)
        : state_(owner.state()),parameter_(*state_.getParameter(key)),syncKey_(key+"_sync")
    {
        name_.setText(name,juce::dontSendNotification);
        name_.setFont(juce::FontOptions(9.5f));
        name_.setColour(juce::Label::textColourId,style::textDim);
        name_.setJustificationType(juce::Justification::centred);
        value_.setFont(juce::FontOptions(10.0f));
        value_.setJustificationType(juce::Justification::centred);
        value_.setEditable(false,true,false);
        value_.onTextChange = [this] {
            parameter_.beginChangeGesture();
            parameter_.setValueNotifyingHost(parameter_.getValueForText(value_.getText()));
            parameter_.endChangeGesture();
        };
        knob_.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        knob_.setTextBoxStyle(juce::Slider::NoTextBox,false,0,0);
        attachment_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(state_,key,knob_);
        knob_.setDoubleClickReturnValue(true,parameter_.convertFrom0to1(parameter_.getDefaultValue()));
        addAndMakeVisible(name_); addAndMakeVisible(knob_); addAndMakeVisible(value_);
        if (synced) {
            sync_ = std::make_unique<juce::TextButton>(juce::String::fromUTF8("♪"));
            notes_ = std::make_unique<juce::ComboBox>();
            for (std::size_t i = 1; i < holotoner::kSyncNames.size(); ++i)
                notes_->addItem(holotoner::kSyncNames[i],static_cast<int>(i)+1);
            notes_->onChange = [this] { writeSync(notes_->getSelectedId()-1); };
            sync_->onClick = [this] {
                const auto current = juce::roundToInt(state_.getRawParameterValue(syncKey_)->load());
                if (current > 0) lastDivision_ = current;
                writeSync(current > 0 ? 0 : lastDivision_);
                refresh();
            };
            addAndMakeVisible(*sync_); addChildComponent(*notes_);
        }
        refresh();
    }
    void refresh()
    {
        const auto number = knob_.getValue();
        const auto unit = parameter_.getLabel();
        const auto digits = parameter_.isDiscrete() || std::abs(number)>=100 ? 0 : std::abs(number)>=10 ? 1 : 2;
        auto text = unit == "%" ? juce::String(number*100,0)+"%" : juce::String(std::abs(number)<0.005 ? 0.0 : number,digits);
        if (unit == "Hz") text = number<=0 ? "off" : text+" Hz";
        else if (unit == "BPM") text += " BPM";
        else if (unit == "deg") text += juce::String::fromUTF8("°");
        if (!value_.isBeingEdited()) value_.setText(text,juce::dontSendNotification);
        if (sync_ != nullptr) {
            const auto division = juce::roundToInt(state_.getRawParameterValue(syncKey_)->load());
            if (division > 0) lastDivision_ = division;
            knob_.setVisible(division == 0); value_.setVisible(division == 0);
            notes_->setVisible(division > 0);
            notes_->setSelectedId(division+1,juce::dontSendNotification);
            sync_->setToggleState(division>0,juce::dontSendNotification);
        }
    }
    void setFrameCount(int count)
    {
        if (juce::roundToInt(knob_.getMaximum()) != std::max(2,count)) knob_.setRange(1,std::max(2,count),1);
    }
    void resized() override
    {
        auto area = getLocalBounds();
        if (sync_ != nullptr) {
            auto side = area.removeFromRight(20);
            sync_->setBounds(side.withSizeKeepingCentre(20,20));
            notes_->setBounds(area.withTrimmedTop(12).withTrimmedBottom(12).withSizeKeepingCentre(area.getWidth()-2,20));
        }
        name_.setBounds(area.removeFromTop(12));
        value_.setBounds(area.removeFromBottom(14));
        knob_.setBounds(area.withSizeKeepingCentre(28,28));
    }
private:
    void writeSync(int division)
    {
        auto* target = state_.getParameter(syncKey_);
        target->beginChangeGesture();
        target->setValueNotifyingHost(target->convertTo0to1(static_cast<float>(division)));
        target->endChangeGesture();
    }
    juce::AudioProcessorValueTreeState& state_;
    juce::RangedAudioParameter& parameter_;
    juce::String syncKey_;
    juce::Label name_,value_;
    juce::Slider knob_;
    std::unique_ptr<juce::TextButton> sync_;
    std::unique_ptr<juce::ComboBox> notes_;
    int lastDivision_ = 12;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment_;
};

class HolotonerEditor::TextPanel final : public juce::Component {
    void paint(juce::Graphics& g) override
    {
        g.setColour(style::card);
        g.fillRoundedRectangle(getLocalBounds().toFloat(),style::corner);
        g.setColour(style::link);
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f),style::corner,1);
    }
};

class HolotonerEditor::PlaneView final : public juce::Component {
public:
    explicit PlaneView(HolotonerProcessor& processor) : processor_(processor)
    {
        setMouseCursor(juce::MouseCursor::DraggingHandCursor);
    }

    void setMask(const holotoner::ImageMask* mask)
    {
        if (mask == nullptr || !mask->hasSource) {
            image_ = {};
            return;
        }
        image_ = juce::Image(juce::Image::RGB, holotoner::kImageMaskWidth, holotoner::kImageMaskHeight, false);
        aspectX_ = mask->aspectX;
        aspectY_ = mask->aspectY;
        for (int y = 0; y < holotoner::kImageMaskHeight; ++y)
            for (int x = 0; x < holotoner::kImageMaskWidth; ++x) {
                const auto value = juce::jlimit(0.0f, 1.0f, mask->at(x, y));
                image_.setPixelAt(x, y, juce::Colour::fromFloatRGBA(value, value, value, 1.0f));
            }
        repaint();
    }

    void paint(juce::Graphics& graphics) override
    {
        const auto well = getLocalBounds().toFloat();
        style::paintWell(graphics, well);
        const auto area = drawingArea();
        const auto centre = area.getCentre();
        const auto unit = viewUnit();

        graphics.saveState();
        graphics.reduceClipRegion(area.toNearestInt());
        graphics.setColour(style::grid.withAlpha(0.65f));
        for (int i = -2; i <= 2; ++i) {
            const auto offset = static_cast<float>(i) * unit;
            graphics.drawVerticalLine(juce::roundToInt(centre.x + offset), area.getY(), area.getBottom());
            graphics.drawHorizontalLine(juce::roundToInt(centre.y + offset), area.getX(), area.getRight());
        }
        graphics.setColour(style::textFaint.withAlpha(0.8f));
        graphics.drawLine(area.getX(), centre.y, area.getRight(), centre.y, 1.0f);
        graphics.drawLine(centre.x, area.getY(), centre.x, area.getBottom(), 1.0f);

        const auto projected = holotoner::projectPlane(processor_.displayedPlaneTransform());
        const auto points = screenPoints(projected);
        juce::Path plane;
        plane.startNewSubPath(points[0]);
        for (std::size_t i = 1; i < points.size(); ++i)
            plane.lineTo(points[i]);
        plane.closeSubPath();

        if (image_.isValid()) {
            graphics.saveState();
            graphics.reduceClipRegion(plane);
            graphics.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
            const auto transform = juce::AffineTransform::fromTargetPoints(
                juce::Point<float> {0.0f, static_cast<float>(image_.getHeight())}, points[0],
                juce::Point<float> {static_cast<float>(image_.getWidth()), static_cast<float>(image_.getHeight())}, points[1],
                juce::Point<float> {static_cast<float>(image_.getWidth()), 0.0f}, points[2]);
            graphics.drawImageTransformed(image_, transform);
            graphics.setColour(style::signal.withAlpha(0.10f));
            graphics.fillPath(plane);
            graphics.restoreState();
        } else {
            const juce::ColourGradient surface(style::signal.withAlpha(0.42f), points[3].x, points[3].y,
                                               style::link.withAlpha(0.24f), points[1].x, points[1].y, false);
            graphics.setGradientFill(surface);
            graphics.fillPath(plane);
            graphics.setColour(style::text.withAlpha(0.28f));
            for (int i = 1; i < 5; ++i) {
                juce::Path line;
                const auto t = static_cast<float>(i) / 5.0f;
                line.startNewSubPath(points[0] + (points[3] - points[0]) * t);
                line.lineTo(points[1] + (points[2] - points[1]) * t);
                graphics.strokePath(line, juce::PathStrokeType(0.75f));
            }
        }
        graphics.setColour(style::signal);
        graphics.strokePath(plane, juce::PathStrokeType(1.5f));

        const auto handles = handlePoints(points);
        graphics.setColour(style::textDim);
        graphics.drawLine(juce::Line<float>(handles.topEdge, handles.roll), 1.0f);
        drawHandle(graphics, handles.move, style::linkLight, 5.0f);
        drawHandle(graphics, handles.rotate, style::text, 5.0f);
        drawHandle(graphics, handles.scale, style::signal, 6.0f);
        drawHandle(graphics, handles.roll, style::textDim, 5.0f);
        graphics.restoreState();

        graphics.setColour(style::text);
        graphics.setFont(juce::FontOptions(11.0f, juce::Font::bold));
        graphics.drawText("PROCESSED IMAGE", 12, 7, 150, 18, juce::Justification::centredLeft);
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        start_ = processor_.planeTransform();
        const auto points = screenPoints(holotoner::projectPlane(start_));
        const auto handles = handlePoints(points);
        const auto position = event.position;
        if (position.getDistanceFrom(handles.scale) <= 15.0f)
            drag_ = Drag::Scale;
        else if (position.getDistanceFrom(handles.roll) <= 15.0f)
            drag_ = Drag::Roll;
        else if (position.getDistanceFrom(handles.rotate) <= 15.0f)
            drag_ = Drag::Rotate;
        else
            drag_ = Drag::Move;
        beginGesture();
    }

    void mouseDrag(const juce::MouseEvent& event) override
    {
        const auto delta = event.getOffsetFromDragStart().toFloat();
        const auto unit = std::max(1.0f, viewUnit());
        switch (drag_) {
        case Drag::Move:
            setParameter(processor_, "position_x", clamp(start_.positionX + delta.x / unit, -1.0f, 1.0f));
            setParameter(processor_, "position_y", clamp(start_.positionY - delta.y / unit, -1.0f, 1.0f));
            break;
        case Drag::Scale:
            setParameter(processor_, "scale_x", clamp(start_.scaleX + delta.x / unit, 0.05f, 4.0f));
            setParameter(processor_, "scale_y", clamp(start_.scaleY - delta.y / unit, 0.05f, 4.0f));
            break;
        case Drag::Rotate:
            setParameter(processor_, "yaw", clamp(start_.yawDegrees + 0.75f * delta.x, -180.0f, 180.0f));
            setParameter(processor_, "pitch", clamp(start_.pitchDegrees - 0.75f * delta.y, -180.0f, 180.0f));
            break;
        case Drag::Roll:
            setParameter(processor_, "roll", clamp(start_.rollDegrees + 0.75f * delta.x, -180.0f, 180.0f));
            break;
        case Drag::None:
            break;
        }
        repaint();
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        endGesture();
        drag_ = Drag::None;
    }

private:
    enum class Drag { None, Move, Scale, Rotate, Roll };

    struct Handles {
        juce::Point<float> move;
        juce::Point<float> scale;
        juce::Point<float> rotate;
        juce::Point<float> topEdge;
        juce::Point<float> roll;
    };

    juce::Rectangle<float> drawingArea() const { return getLocalBounds().toFloat().reduced(12.0f).withTrimmedTop(22.0f); }

    float viewUnit() const
    {
        const auto area = drawingArea();
        return std::max(1.0f, std::min(area.getWidth(), area.getHeight()) / 3.0f);
    }

    std::array<juce::Point<float>, 4> screenPoints(const ProjectedPlane& projected) const
    {
        std::array<juce::Point<float>, 4> points;
        const auto centre = drawingArea().getCentre();
        const auto unit = viewUnit();
        const auto& a = projected.corners[0];
        const auto& b = projected.corners[1];
        const auto& d = projected.corners[3];
        constexpr std::array<std::array<float, 2>, 4> corners {{{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}};
        for (std::size_t i = 0; i < points.size(); ++i)
        {
            const auto u = 0.5f * (corners[i][0] * aspectX_ + 1.0f);
            const auto v = 0.5f * (corners[i][1] * aspectY_ + 1.0f);
            points[i] = {centre.x + (a.stereo + u * (b.stereo - a.stereo) + v * (d.stereo - a.stereo)) * unit,
                         centre.y - (a.frequency + u * (b.frequency - a.frequency) + v * (d.frequency - a.frequency)) * unit};
        }
        return points;
    }

    Handles handlePoints(const std::array<juce::Point<float>, 4>& points) const
    {
        const auto centre = 0.25f * (points[0] + points[1] + points[2] + points[3]);
        const auto right = 0.5f * (points[1] + points[2]);
        const auto top = 0.5f * (points[2] + points[3]);
        auto direction = top - centre;
        const auto length = std::max(1.0f, direction.getDistanceFromOrigin());
        direction *= 22.0f / length;
        const auto bounds = drawingArea().reduced(8.0f);
        const auto constrain = [bounds](juce::Point<float> point) {
            return juce::Point<float> {clamp(point.x, bounds.getX(), bounds.getRight()),
                                       clamp(point.y, bounds.getY(), bounds.getBottom())};
        };
        return {constrain(centre), constrain(points[2]), constrain(right), constrain(top), constrain(top + direction)};
    }

    static void drawHandle(juce::Graphics& graphics, juce::Point<float> point, juce::Colour colour, float radius)
    {
        graphics.setColour(style::well);
        graphics.fillEllipse(juce::Rectangle<float>(2.0f * radius, 2.0f * radius).withCentre(point));
        graphics.setColour(colour);
        graphics.drawEllipse(juce::Rectangle<float>(2.0f * radius, 2.0f * radius).withCentre(point), 1.5f);
    }

    void beginGesture()
    {
        switch (drag_) {
        case Drag::Move:
            beginParameter(processor_, "position_x");
            beginParameter(processor_, "position_y");
            break;
        case Drag::Scale:
            beginParameter(processor_, "scale_x");
            beginParameter(processor_, "scale_y");
            break;
        case Drag::Rotate:
            beginParameter(processor_, "yaw");
            beginParameter(processor_, "pitch");
            break;
        case Drag::Roll:
            beginParameter(processor_, "roll");
            break;
        case Drag::None:
            break;
        }
    }

    void endGesture()
    {
        switch (drag_) {
        case Drag::Move:
            endParameter(processor_, "position_x");
            endParameter(processor_, "position_y");
            break;
        case Drag::Scale:
            endParameter(processor_, "scale_x");
            endParameter(processor_, "scale_y");
            break;
        case Drag::Rotate:
            endParameter(processor_, "yaw");
            endParameter(processor_, "pitch");
            break;
        case Drag::Roll:
            endParameter(processor_, "roll");
            break;
        case Drag::None:
            break;
        }
    }

    HolotonerProcessor& processor_;
    juce::Image image_;
    float aspectX_ = 1.0f, aspectY_ = 1.0f;
    PlaneTransform start_;
    Drag drag_ = Drag::None;
};

class HolotonerEditor::PolarView final : public juce::Component {
public:
    explicit PolarView(HolotonerProcessor& processor) : processor_(processor)
    {
        setInterceptsMouseClicks(false, false);
    }

    void paint(juce::Graphics& graphics) override
    {
        const auto well = getLocalBounds().toFloat();
        style::paintWell(graphics, well);
        const auto area = well.reduced(16.0f).withTrimmedTop(22.0f).withTrimmedBottom(24.0f);
        const juce::Point<float> origin {area.getCentreX(), area.getBottom() - 2.0f};
        const auto radius = std::max(10.0f, std::min(0.5f * area.getWidth() - 4.0f, area.getHeight() - 8.0f));
        graphics.saveState();
        graphics.reduceClipRegion(area.toNearestInt());
        graphics.setColour(style::grid);
        for (float fraction : {0.25f, 0.5f, 0.75f, 1.0f}) {
            const auto r = radius * fraction;
            graphics.drawEllipse(juce::Rectangle<float>(2.0f * r, 2.0f * r).withCentre(origin), 1.0f);
        }
        for (float degrees : {-45.0f, 0.0f, 45.0f}) {
            const auto angle = degrees * pi / 180.0f;
            graphics.drawLine(origin.x, origin.y, origin.x + radius * std::sin(angle),
                              origin.y - radius * std::cos(angle), 1.0f);
        }
        const auto toScreen = [origin, radius](holotoner::ScopePoint point) {
            return juce::Point<float> {origin.x + radius * point.x, origin.y - radius * point.y};
        };
        const auto count = processor_.scopeSamples(samples_.data(), static_cast<int>(samples_.size()));
        // A fixed, labelled +14 dB display gain, applied identically to both
        // measured input and output. This is NOT a fabricated image trace.
        for (int layer = 0; layer < 2; ++layer) {
            graphics.setColour(layer == 0 ? style::textDim.withAlpha(0.18f) : style::signal.withAlpha(0.38f));
            for (int i = 0; i < count; ++i) {
                const auto& sample = samples_[static_cast<std::size_t>(i)];
                const auto left = layer == 0 ? sample.inputLeft : sample.outputLeft;
                const auto right = layer == 0 ? sample.inputRight : sample.outputRight;
                if (std::abs(left) + std::abs(right) < 1.0e-6f)
                    continue;
                const auto point = toScreen(holotoner::stereoToScopePoint(left * 5.0f, right * 5.0f));
                graphics.fillRect(juce::Rectangle<float>(1.35f, 1.35f).withCentre(point));
            }
        }
        graphics.restoreState();
        graphics.setColour(style::text);
        graphics.setFont(juce::FontOptions(11.0f, juce::Font::bold));
        graphics.drawText("POLAR SAMPLE / +14 dB", 12, 7, 190, 18, juce::Justification::centredLeft);
        graphics.setColour(style::textDim);
        graphics.setFont(juce::FontOptions(10.5f));
        graphics.drawText("input", getWidth() - 180, 7, 100, 18, juce::Justification::centredRight);
        graphics.setColour(style::signal);
        graphics.drawText("output", getWidth() - 74, 7, 62, 18, juce::Justification::centredRight);
        const auto correlation = clamp(processor_.meterState().outputCorrelation, -1.0f, 1.0f);
        const auto bar = juce::Rectangle<float>(area.getX(), well.getBottom() - 21.0f, area.getWidth(), 5.0f);
        graphics.setColour(style::grid);
        graphics.fillRoundedRectangle(bar, 2.5f);
        graphics.setColour(correlation >= 0.0f ? style::linkLight : style::signal);
        const auto centreX = bar.getCentreX();
        const auto valueX = centreX + 0.5f * bar.getWidth() * correlation;
        graphics.fillRoundedRectangle(juce::Rectangle<float>::leftTopRightBottom(
            std::min(centreX, valueX), bar.getY(), std::max(centreX, valueX), bar.getBottom()), 2.5f);
    }

private:
    HolotonerProcessor& processor_;
    std::array<HolotonerProcessor::ScopeSample, 16384> samples_ {};
};

HolotonerEditor::HolotonerEditor(HolotonerProcessor& owner)
    : AudioProcessorEditor(owner),processor_(owner),
      planeView_(std::make_unique<PlaneView>(owner)),polarView_(std::make_unique<PolarView>(owner)),
      textPanel_(std::make_unique<TextPanel>())
{
    setLookAndFeel(&look_);
    setOpaque(true); setWantsKeyboardFocus(true);
    title_.setText("HOLOTONER",juce::dontSendNotification);
    title_.setFont(juce::FontOptions(15.0f,juce::Font::bold));
    addAndMakeVisible(title_);
    for (auto* c : std::array<juce::Component*,12> {&source_,&loadAudio_,&restart_,&play_,
        &loadImage_,&reset_,&textButton_,&channel_,&projection_,&shape_,&textLayout_,&status_}) addAndMakeVisible(c);
    source_.addItemList({"Input","File","Clicks"},1);
    source_.onChange = [this] {
        const auto kind = static_cast<SourceKind>(source_.getSelectedId()-1);
        if (kind == SourceKind::File && processor_.sourceName().isEmpty()) chooseAudioFile();
        else processor_.setSourceKind(kind);
    };
    loadAudio_.onClick = [this] { chooseAudioFile(); };
    loadImage_.onClick = [this] { chooseImageFile(); };
    restart_.onClick = [this] { processor_.restartSource(); };
    play_.onClick = [this] { processor_.toggleSourcePlaying(); };
    reset_.onClick = [this] { resetTransform(); };
    const auto combo = [this](juce::ComboBox& control,const char* id,juce::StringArray choices) {
        control.addItemList(choices,1);
        comboAttachments_.push_back(std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(processor_.state(),id,control));
    };
    combo(channel_,"image_channel",{"Off","Luma","Alpha","Red","Green","Blue"});
    combo(projection_,"image_projection",{"XY rectangle","Polar warp"});
    combo(shape_,"motion_shape",{"Swing","Spin"});
    combo(textLayout_,"text_layout",{"Whole","Characters","Words"});
    const auto toggle = [this](juce::ToggleButton& control,const char* id) {
        addAndMakeVisible(control);
        buttonAttachments_.push_back(std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(processor_.state(),id,control));
    };
    toggle(normalize_,"image_normalize"); toggle(invert_,"image_invert");
    toggle(binary_,"image_binary"); toggle(textPlay_,"text_play");
    const auto knob = [this](const juce::String& key,const juce::String& label,bool sync = false) {
        auto cell = std::make_unique<KnobCell>(processor_,key,label,sync);
        addAndMakeVisible(*cell);
        return cell;
    };
    constexpr std::array<const char*,7> labels {"Rotate X","Rotate Y","Rotate Z","Move X","Move Y","Scale X","Scale Y"};
    for (std::size_t i = 0; i < geometry_.size(); ++i) {
        geometry_[i] = knob(HolotonerProcessor::kMotionAxes[i],labels[i]);
        axisRates_[i] = knob(juce::String(HolotonerProcessor::kMotionAxes[i])+"_rate","Rate",true);
    }
    gain_ = knob("image_gain","Gain"); fill_ = knob("image_fill","Fill");
    threshold_ = knob("image_threshold","Threshold"); depth_ = knob("motion_depth","Depth");
    frame_ = knob("text_frame","Frame"); textRate_ = knob("text_rate","Text rate",true);
    wet_ = knob("wet","Wet"); amount_ = knob("image_amount","Amount"); tempo_ = knob("tempo","Tempo");
    tempo_->setVisible(processor_.wrapperType == juce::AudioProcessor::wrapperType_Standalone);
    addAndMakeVisible(*planeView_); addAndMakeVisible(*polarView_);
    addChildComponent(*textPanel_);
    textPanel_->addAndMakeVisible(textInput_);
    textPanel_->addAndMakeVisible(applyText_); textPanel_->addAndMakeVisible(closeText_);
    textInput_.setMultiLine(false);
    textInput_.setText(processor_.text(),false); lastText_ = processor_.text();
    applyText_.onClick = [this] { processor_.setText(textInput_.getText()); };
    textInput_.onReturnKey = [this] { processor_.setText(textInput_.getText()); };
    textButton_.setClickingTogglesState(true);
    textButton_.onClick = [this] {
        textPanel_->setVisible(textButton_.getToggleState());
        if (textPanel_->isVisible()) textPanel_->toFront(false);
    };
    closeText_.onClick = [this] {
        textButton_.setToggleState(false,juce::dontSendNotification); textPanel_->setVisible(false);
    };
    status_.setFont(juce::FontOptions(10.5f)); status_.setColour(juce::Label::textColourId,style::textDim);
    setResizable(true,true); setResizeLimits(900,500,1600,1000);
    int width = 1100,height = 700;
    if (const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()) {
        width = std::min(width,std::max(900,juce::roundToInt(display->userBounds.getWidth())-32));
        const auto chrome = processor_.wrapperType == juce::AudioProcessor::wrapperType_Standalone ? 96 : 48;
        height = std::min(height,std::max(500,juce::roundToInt(display->userBounds.getHeight())-chrome));
    }
    setSize(width,height); startTimerHz(30); refresh();
}

HolotonerEditor::~HolotonerEditor()
{
    stopTimer();
    setLookAndFeel(nullptr);
}

void HolotonerEditor::paint(juce::Graphics& graphics)
{
    graphics.fillAll(style::background);
    if (dragOver_) {
        graphics.setColour(style::linkLight.withAlpha(0.65f));
        graphics.drawRoundedRectangle(getLocalBounds().toFloat().reduced(4.0f), 7.0f, 2.0f);
    }
}

void HolotonerEditor::resized()
{
    auto area = getLocalBounds().reduced(10);
    status_.setBounds(area.removeFromBottom(22));
    const auto half = (area.getWidth()-8)/2;
    auto left = area.removeFromLeft(half);
    area.removeFromLeft(8);
    auto right = area;
    auto imageBar = left.removeFromTop(34);
    title_.setBounds(imageBar.removeFromLeft(110));
    loadImage_.setBounds(imageBar.removeFromLeft(70)); imageBar.removeFromLeft(4);
    textButton_.setBounds(imageBar.removeFromLeft(54)); imageBar.removeFromLeft(4);
    reset_.setBounds(imageBar.removeFromLeft(82));
    auto soundBar = right.removeFromTop(34);
    source_.setBounds(soundBar.removeFromLeft(82)); soundBar.removeFromLeft(4);
    loadAudio_.setBounds(soundBar.removeFromLeft(70)); soundBar.removeFromLeft(4);
    restart_.setBounds(soundBar.removeFromLeft(64)); soundBar.removeFromLeft(4);
    play_.setBounds(soundBar.removeFromLeft(54));
    left.removeFromTop(8); right.removeFromTop(8);
    auto imageControls = left.removeFromBottom(214);
    auto soundControls = right.removeFromBottom(214);
    auto axes = imageControls.removeFromTop(104);
    for (std::size_t i = 0; i < geometry_.size(); ++i) {
        const auto x = axes.getX()+static_cast<int>(i)*axes.getWidth()/7;
        const auto width = axes.getX()+static_cast<int>(i+1)*axes.getWidth()/7-x;
        geometry_[i]->setBounds(x,axes.getY(),width,52);
        axisRates_[i]->setBounds(x,axes.getY()+52,width,52);
    }
    auto pixels = imageControls.removeFromTop(54);
    const std::array<KnobCell*,6> cells {gain_.get(),fill_.get(),threshold_.get(),depth_.get(),frame_.get(),textRate_.get()};
    for (std::size_t i = 0; i < cells.size(); ++i) {
        const auto x = pixels.getX()+static_cast<int>(i)*pixels.getWidth()/6;
        const auto width = pixels.getX()+static_cast<int>(i+1)*pixels.getWidth()/6-x;
        cells[i]->setBounds(x,pixels.getY(),width,52);
    }
    auto options = imageControls.removeFromTop(28);
    channel_.setBounds(options.removeFromLeft(76).reduced(1,3));
    projection_.setBounds(options.removeFromLeft(112).reduced(1,3));
    shape_.setBounds(options.removeFromLeft(70).reduced(1,3));
    textLayout_.setBounds(options.removeFromLeft(90).reduced(1,3));
    textPlay_.setBounds(options.removeFromLeft(64));
    normalize_.setBounds(imageControls.removeFromLeft(90));
    invert_.setBounds(imageControls.removeFromLeft(76));
    binary_.setBounds(imageControls.removeFromLeft(64));
    auto mix = soundControls.removeFromTop(56);
    wet_->setBounds(mix.removeFromLeft(80));
    amount_->setBounds(mix.removeFromLeft(80));
    tempo_->setBounds(mix.removeFromLeft(94));
    planeView_->setBounds(left); polarView_->setBounds(right);
    // Text overlays the image well. Opening it never changes window/panel geometry.
    textPanel_->setBounds(left.getX()+8,left.getY()+28,left.getWidth()-16,48);
    auto input = textPanel_->getLocalBounds().reduced(8);
    closeText_.setBounds(input.removeFromRight(52)); input.removeFromRight(4);
    applyText_.setBounds(input.removeFromRight(72)); input.removeFromRight(8);
    textInput_.setBounds(input);
}

bool HolotonerEditor::keyPressed(const juce::KeyPress& key)
{
    if (textInput_.hasKeyboardFocus(true)) return false;
    if (key == juce::KeyPress::spaceKey && processor_.wrapperType == juce::AudioProcessor::wrapperType_Standalone) {
        processor_.toggleSourcePlaying();
        return true;
    }
    return false;
}

bool HolotonerEditor::isInterestedInFileDrag(const juce::StringArray& files)
{
    if (files.size() != 1)
        return false;
    const juce::File file(files[0]);
    return HolotonerProcessor::isSupportedAudioFile(file) || HolotonerProcessor::isSupportedImageFile(file);
}

void HolotonerEditor::filesDropped(const juce::StringArray& files, int, int)
{
    dragOver_ = false;
    if (isInterestedInFileDrag(files)) {
        const juce::File file(files[0]);
        if (HolotonerProcessor::isSupportedImageFile(file))
            processor_.loadImageFile(file);
        else
            processor_.loadSourceFile(file);
    }
    repaint();
}

void HolotonerEditor::fileDragEnter(const juce::StringArray&, int, int)
{
    dragOver_ = true;
    repaint();
}

void HolotonerEditor::fileDragExit(const juce::StringArray&)
{
    dragOver_ = false;
    repaint();
}

void HolotonerEditor::timerCallback()
{
    refresh();
    planeView_->repaint();
    polarView_->repaint();
}

void HolotonerEditor::chooseAudioFile()
{
    chooser_ = std::make_unique<juce::FileChooser>("Load audio into Holotoner", juce::File {},
                                                   "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.m4a;*.caf");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this](const juce::FileChooser& chooser) {
                              const auto file = chooser.getResult();
                              if (file.existsAsFile())
                                  processor_.loadSourceFile(file);
                              refresh();
                          });
}

void HolotonerEditor::chooseImageFile()
{
    chooser_ = std::make_unique<juce::FileChooser>("Load an image into Holotoner", juce::File {},
                                                   "*.png;*.jpg;*.jpeg;*.gif");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this](const juce::FileChooser& chooser) {
                              const auto file = chooser.getResult();
                              if (file.existsAsFile())
                                  processor_.loadImageFile(file);
                              refresh();
                          });
}

void HolotonerEditor::resetTransform()
{
    constexpr std::array<const char*, 7> ids {"position_x", "position_y", "scale_x", "scale_y", "pitch", "yaw", "roll"};
    constexpr std::array<float, 7> values {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f};
    for (const auto* id : ids)
        beginParameter(processor_, id);
    for (std::size_t i = 0; i < ids.size(); ++i)
        setParameter(processor_, ids[i], values[i]);
    for (const auto* id : ids)
        endParameter(processor_, id);
}

void HolotonerEditor::refresh()
{
    const auto value = processor_.text();
    if (value != lastText_) { lastText_ = value; textInput_.setText(value,false); }
    for (auto& cell : geometry_) cell->refresh();
    for (auto& cell : axisRates_) cell->refresh();
    for (auto* cell : std::array<KnobCell*,9> {gain_.get(),fill_.get(),threshold_.get(),depth_.get(),
        frame_.get(),textRate_.get(),wet_.get(),amount_.get(),tempo_.get()}) cell->refresh();
    const auto frames = processor_.textFrameCount();
    frame_->setFrameCount(frames);
    frame_->setEnabled(frames>1 && !textPlay_.getToggleState());
    textPlay_.setEnabled(frames>1); textRate_->setEnabled(frames>1 && textPlay_.getToggleState());
    const auto kind = processor_.sourceKind();
    source_.setSelectedId(sourceItem(kind), juce::dontSendNotification);
    const bool local = processor_.wrapperType == juce::AudioProcessor::wrapperType_Standalone;
    restart_.setEnabled(local || kind != SourceKind::Input);
    play_.setEnabled(local || kind != SourceKind::Input);
    play_.setButtonText(processor_.isSourcePlaying() ? "Stop" : "Play");
    play_.setToggleState((local || kind != SourceKind::Input) && processor_.isSourcePlaying()
                             && !processor_.isWaitingForHost(),
                         juce::dontSendNotification);

    const auto image = processor_.imageDocument();
    const bool imageChanged = image != currentImage_;
    if (image != currentImage_) {
        currentImage_ = image;
    }
    const auto mask = processor_.imageMask();
    if (mask != currentMask_ || imageChanged) {
        currentMask_ = mask;
        planeView_->setMask(image != nullptr && mask != nullptr ? mask.get() : nullptr);
    }

    juce::String message;
    if (processor_.isLoadingImage())
        message = "Loading image...";
    else if (processor_.lastImageMessage().isNotEmpty())
        message = processor_.lastImageMessage();
    else if (processor_.isLoadingSource())
        message = "Loading audio...";
    else if (processor_.isWaitingForHost())
        message = "Source waits for host transport";
    else if (processor_.lastSourceMessage().isNotEmpty())
        message = processor_.lastSourceMessage();
    else if (kind == SourceKind::File)
        message = processor_.sourceName();
    else
        message = kind == SourceKind::Clicks ? "Test clicks"
                                             : "Host input";
    if (image != nullptr)
        message += "    Image: " + image->name;
    status_.setText(message, juce::dontSendNotification);
}
