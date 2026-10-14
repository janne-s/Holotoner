#include "PluginEditor.h"

#include "core/dsp/SpaceCues.h"
#include "core/dsp/ImageSpatializer.h"
#include "core/geometry/PlaneProjection.h"
#include "core/image/ShapeGenerator.h"

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

juce::String formatValue(const juce::RangedAudioParameter& parameter, double number)
{
    const auto unit = parameter.getLabel();
    const auto digits = parameter.isDiscrete() || std::abs(number)>=100 ? 0 : std::abs(number)>=10 ? 1 : 2;
    // juce::String(double, 0) means full precision, so whole percents are rounded explicitly.
    auto text = unit == "%" ? juce::String(juce::roundToInt(number*100))+"%" : juce::String(std::abs(number)<0.005 ? 0.0 : number,digits);
    if (unit == "Hz") text = number<=0 ? "off" : text+" Hz";
    else if (unit == "BPM") text += " BPM";
    else if (unit == "deg") text += juce::String::fromUTF8("°");
    else if (unit == "ms") text = (parameter.getParameterID() == "image_smooth"
        ? juce::String(number*1000,2) : juce::String(juce::roundToInt(number*1000)))+" ms";
    return text;
}

} // namespace

class HolotonerEditor::KnobCell final : public juce::Component {
public:
    KnobCell(HolotonerProcessor& owner,const juce::String& key,const juce::String& name,bool synced = false)
        : state_(owner.state()),parameter_(*state_.getParameter(key)),syncKey_(key+"_sync")
    {
        name_.setText(name,juce::dontSendNotification);
        name_.setFont(juce::FontOptions(style::kTextSmall));
        name_.setColour(juce::Label::textColourId,style::textDim);
        name_.setJustificationType(juce::Justification::centred);
        value_.setFont(juce::FontOptions(style::kTextSmall));
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
    static constexpr int kSyncWidth = 16, kNotesWidth = 46;
    // Row mode: one line, knob then value (then ♪), so it aligns with a slider row.
    void setRow(bool row)
    {
        row_ = row;
        value_.setJustificationType(row ? juce::Justification::centredLeft : juce::Justification::centred);
        name_.setJustificationType(row ? juce::Justification::centredRight : juce::Justification::centred);
        name_.setVisible(!row || name_.getText().isNotEmpty());
        resized();
    }
    int rowNameWidth() const { return name_.getText().isEmpty() ? 0 : 6+juce::roundToInt(juce::GlyphArrangement::getStringWidth(name_.getFont(),name_.getText())); }
    void refresh()
    {
        if (!value_.isBeingEdited()) value_.setText(formatValue(parameter_,knob_.getValue()),juce::dontSendNotification);
        if (sync_ != nullptr) {
            const auto division = juce::roundToInt(state_.getRawParameterValue(syncKey_)->load());
            if (division > 0) lastDivision_ = division;
            const bool layoutChanged = notes_->isVisible() != (division > 0);
            knob_.setVisible(division == 0); value_.setVisible(division == 0);
            notes_->setVisible(division > 0);
            if (layoutChanged) resized();
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
        if (row_) {
            if (name_.isVisible()) name_.setBounds(area.removeFromLeft(rowNameWidth()));
            if (sync_ != nullptr) {
                sync_->setBounds(area.removeFromRight(kSyncWidth).withSizeKeepingCentre(kSyncWidth,18));
                area.removeFromRight(2);
                notes_->setBounds(area.withSizeKeepingCentre(area.getWidth(),18)); // up to its ♪, 2 px apart
            }
            knob_.setBounds(area.removeFromLeft(22).withSizeKeepingCentre(20,20));
            area.removeFromLeft(2);
            value_.setBounds(area);
            return;
        }
        if (sync_ != nullptr) {
            // The knob (or note menu) and its ♪ form one group, 2 px apart, centred in the cell;
            // name and value centre on the knob. A note name is at most "1/64T": the menu fits that.
            const auto synced = notes_->isVisible();
            const auto main = synced ? std::min(kNotesWidth,area.getWidth()-kSyncWidth-2) : 28;
            const auto left = area.getCentreX()-(main+2+kSyncWidth)/2;
            const auto middle = area.getY()+12+(area.getHeight()-26)/2;
            knob_.setBounds(left,middle-14,main,28);
            notes_->setBounds(left,middle-10,main,20);
            sync_->setBounds(left+main+2,middle-10,kSyncWidth,20);
            const auto centre = left+main/2;
            const auto half = std::min(centre-area.getX(),area.getRight()-centre);
            name_.setBounds(centre-half,area.getY(),2*half,12);
            value_.setBounds(centre-half,area.getBottom()-14,2*half,14);
            return;
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
    bool row_ = false;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment_;
};

// One geometry axis: base-value slider, then its own Rate + ♪ on the same line.
// A thin marker shows where motion currently has the axis; the slider stays the base.
class HolotonerEditor::AxisRow final : public juce::Component {
public:
    AxisRow(HolotonerProcessor& owner,std::size_t axis,const juce::String& name)
        : processor_(owner),axis_(axis),
          parameter_(*owner.state().getParameter(HolotonerProcessor::kMotionAxes[axis])),
          rate_(owner,juce::String(HolotonerProcessor::kMotionAxes[axis])+"_rate","",true)
    {
        name_.setText(name,juce::dontSendNotification);
        name_.setFont(juce::FontOptions(style::kTextSmall));
        name_.setColour(juce::Label::textColourId,style::textDim);
        value_.setFont(juce::FontOptions(style::kTextSmall));
        value_.setJustificationType(juce::Justification::centredRight);
        value_.setEditable(false,true,false);
        value_.onTextChange = [this] {
            parameter_.beginChangeGesture();
            parameter_.setValueNotifyingHost(parameter_.getValueForText(value_.getText()));
            parameter_.endChangeGesture();
        };
        slider_.setSliderStyle(juce::Slider::LinearHorizontal);
        slider_.setTextBoxStyle(juce::Slider::NoTextBox,false,0,0);
        attachment_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            owner.state(),HolotonerProcessor::kMotionAxes[axis],slider_);
        slider_.setDoubleClickReturnValue(true,parameter_.convertFrom0to1(parameter_.getDefaultValue()));
        rate_.setRow(true);
        addAndMakeVisible(name_); addAndMakeVisible(slider_); addAndMakeVisible(value_); addAndMakeVisible(rate_);
    }
    static constexpr int kRateWidth = 80;
    void refresh()
    {
        if (!value_.isBeingEdited()) value_.setText(formatValue(parameter_,slider_.getValue()),juce::dontSendNotification);
        rate_.refresh();
        const auto t = processor_.displayedPlaneTransform();
        const std::array<float,7> shown {t.pitchDegrees,t.yawDegrees,t.rollDegrees,t.positionX,t.positionY,t.scaleX,t.scaleY};
        if (std::abs(shown[axis_]-shown_) > 1.0e-4f) { shown_ = shown[axis_]; repaint(); }
    }
    void resized() override
    {
        auto area = getLocalBounds();
        name_.setBounds(area.removeFromLeft(58));
        rate_.setBounds(area.removeFromRight(kRateWidth));
        area.removeFromRight(10);
        value_.setBounds(area.removeFromRight(50));
        slider_.setBounds(area);
    }
    void paintOverChildren(juce::Graphics& graphics) override
    {
        const auto base = static_cast<float>(slider_.getValue());
        if (std::abs(shown_-base) <= 1.0e-3f*(static_cast<float>(slider_.getMaximum()-slider_.getMinimum())))
            return;
        const auto value = std::clamp(static_cast<double>(shown_),slider_.getMinimum(),slider_.getMaximum());
        const auto x = static_cast<float>(slider_.getX())+slider_.getPositionOfValue(value);
        const auto centre = static_cast<float>(slider_.getBounds().getCentreY());
        graphics.setColour(style::linkLight);
        graphics.fillRoundedRectangle(juce::Rectangle<float>(2.0f,12.0f).withCentre({x,centre}),1.0f);
    }
private:
    HolotonerProcessor& processor_;
    std::size_t axis_;
    juce::RangedAudioParameter& parameter_;
    juce::Label name_,value_;
    juce::Slider slider_;
    KnobCell rate_;
    float shown_ = 0.0f;
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

        if (controlsHeight_ > 0) {
            const auto y = drawingArea().getBottom() + 6.0f;
            graphics.setColour(style::grid);
            graphics.drawHorizontalLine(juce::roundToInt(y), 12.0f, static_cast<float>(getWidth()) - 12.0f);
        }
        graphics.setColour(style::text);
        graphics.setFont(juce::FontOptions(style::kTextTitle, juce::Font::bold));
        graphics.drawText("Processed image", 12, 7, 150, 18, juce::Justification::centredLeft);
    }

    // The geometry rows sit inside this well, below the drawing area.
    void setControlsHeight(int height)
    {
        controlsHeight_ = height;
        repaint();
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        start_ = processor_.planeTransform();
        const auto points = screenPoints(holotoner::projectPlane(start_));
        const auto handles = handlePoints(points);
        const auto position = event.position;
        if (position.y > drawingArea().getBottom())
            drag_ = Drag::None;
        else if (position.getDistanceFrom(handles.scale) <= 15.0f)
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
            setParameter(processor_, "roll", clamp(start_.rollDegrees - 0.75f * delta.x, -180.0f, 180.0f));
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

    juce::Rectangle<float> drawingArea() const
    {
        return getLocalBounds().toFloat().reduced(12.0f).withTrimmedTop(22.0f).withTrimmedBottom(static_cast<float>(controlsHeight_));
    }

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
    int controlsHeight_ = 0;
    PlaneTransform start_;
    Drag drag_ = Drag::None;
};

// Space, above the sound field: what Interfere, Feedback and Resonate do to each ear's
// spectrum (steady state, 0 dB = unchanged) and Echo's time per frequency, computed by the
// same core functions the engine applies per FFT bin.
class HolotonerEditor::SpaceView final : public juce::Component {
public:
    explicit SpaceView(HolotonerProcessor& processor) : processor_(processor)
    {
        setInterceptsMouseClicks(false, false);
    }

    void paint(juce::Graphics& graphics) override
    {
        const auto well = getLocalBounds().toFloat();
        style::paintWell(graphics, well);
        update();
        graphics.setColour(style::text);
        graphics.setFont(juce::FontOptions(style::kTextTitle, juce::Font::bold));
        graphics.drawText("Space", 12, 7, 60, 18, juce::Justification::centredLeft);
        graphics.setFont(juce::FontOptions(style::kTextSmall));
        // Both ears get the same level from these effects (they differ only in phase), so one curve.
        graphics.setColour(style::signal);
        graphics.drawText("level", getWidth() - 180, 7, 40, 18, juce::Justification::centredRight);
        graphics.setColour(style::text);
        graphics.drawText("echo time 0-2 s", getWidth() - 132, 7, 120, 18, juce::Justification::centredRight);
        paintResponse(graphics, well.reduced(12.0f).withTrimmedTop(20.0f));
    }

private:
    static constexpr double kRangeDb = 18.0;

    void update()
    {
        const auto value = [this](const char* id) {
            return static_cast<double>(processor_.state().getRawParameterValue(id)->load());
        };
        amounts_ = {value("space_interfere"), value("space_feedback"), value("space_echo"), value("space_resonate")};
        auto transform = processor_.displayedPlaneTransform();
        const auto mask = processor_.imageMask();
        if (mask != nullptr) {
            transform.scaleX *= mask->aspectX;
            transform.scaleY *= mask->aspectY;
        }
        if (mask != mask_) {
            mask_ = mask;
            weights_ = mask != nullptr ? holotoner::spaceRowWeights(*mask) : decltype(weights_) {};
        }
        plane_ = holotoner::projectPlane(transform);
        rings_ = amounts_.resonate > 0.0 ? holotoner::resonators(plane_, weights_) : holotoner::Resonators {};
        echoTime_ = processor_.echoTimeSeconds();
    }

    // Steady-state level: Interfere (with Feedback), then Resonate's boost.
    double levelAt(double hz) const
    {
        const auto response = holotoner::interfereResponse(plane_, amounts_, hz);
        return std::abs(response.left) * (1.0 + amounts_.resonate * holotoner::kResonanceBoost * holotoner::resonance(rings_, hz));
    }

    void paintResponse(juce::Graphics& graphics, juce::Rectangle<float> area)
    {
        const auto xOfHz = [&](double hz) {
            return area.getX() + area.getWidth() * static_cast<float>(holotoner::spaceRowPosition(hz));
        };
        const auto hzOfX = [&](float x) {
            return holotoner::kSpaceLowHz * std::pow(holotoner::kSpaceHighHz / holotoner::kSpaceLowHz,
                                                     static_cast<double>((x - area.getX()) / area.getWidth()));
        };
        const auto yOfDb = [&](double db) {
            return area.getCentreY() - area.getHeight() * 0.5f * static_cast<float>(std::clamp(db / kRangeDb, -1.0, 1.0));
        };
        graphics.setFont(juce::FontOptions(style::kTextSmall));
        for (const auto hz : {100.0, 1000.0, 10000.0}) {
            const auto x = xOfHz(hz);
            graphics.setColour(style::grid);
            graphics.drawVerticalLine(juce::roundToInt(x), area.getY(), area.getBottom());
            graphics.setColour(style::textFaint);
            graphics.drawText(hz >= 1000.0 ? juce::String(hz / 1000.0, 0) + " kHz" : juce::String(hz, 0) + " Hz",
                              juce::Rectangle<float>(x + 3.0f, area.getY(), 50.0f, 12.0f), juce::Justification::centredLeft);
        }
        for (const auto db : {-12, 0, 12}) {
            const auto y = yOfDb(db);
            graphics.setColour(db == 0 ? style::textFaint : style::grid);
            graphics.drawHorizontalLine(juce::roundToInt(y), area.getX(), area.getRight());
            if (db != 0 && area.getHeight() < 90.0f)
                continue; // a short chart labels only its 0 dB line
            graphics.setColour(style::textFaint);
            graphics.drawText((db > 0 ? "+" : "") + juce::String(db) + " dB",
                              juce::Rectangle<float>(area.getX() + 2.0f, y - 12.0f, 50.0f, 12.0f), juce::Justification::centredLeft);
        }
        // Several frequencies per pixel: a dense comb is drawn as its range (faint) around
        // its average power (line) instead of aliasing into a false pattern.
        {
            juce::Path curve;
            bool started = false;
            const auto colour = style::signal;
            for (float x = area.getX(); x <= area.getRight(); x += 1.0f) {
                double power = 0.0, lowest = 1.0e9, highest = -1.0e9;
                constexpr int steps = 8;
                for (int i = 0; i < steps; ++i) {
                    const auto magnitude = levelAt(hzOfX(x + static_cast<float>(i) / steps));
                    const auto db = 20.0 * std::log10(std::max(magnitude, 1.0e-6));
                    power += magnitude * magnitude / steps;
                    lowest = std::min(lowest, db);
                    highest = std::max(highest, db);
                }
                if (highest - lowest > 1.0) {
                    graphics.setColour(colour.withAlpha(0.18f));
                    graphics.drawVerticalLine(juce::roundToInt(x), yOfDb(highest), yOfDb(lowest));
                }
                const juce::Point<float> point {x, yOfDb(10.0 * std::log10(std::max(power, 1.0e-12)))};
                if (!started) curve.startNewSubPath(point); else curve.lineTo(point);
                started = true;
            }
            graphics.setColour(colour);
            graphics.strokePath(curve, juce::PathStrokeType(1.5f));
        }
        // Echo time per frequency on its own scale (right), brighter where the image echoes.
        if (amounts_.echo > 0.0) {
            const auto yOfSeconds = [&](double seconds) {
                return area.getBottom() - area.getHeight() * static_cast<float>(seconds / holotoner::kEchoMaxSeconds);
            };
            for (float x = area.getX(); x < area.getRight(); x += 2.0f) {
                const auto hz = hzOfX(x);
                const auto weight = mask_ != nullptr ? 0.15 + 0.85 * holotoner::spaceRowWeight(weights_, hz) : 1.0;
                const auto y = yOfSeconds(holotoner::echoSeconds(plane_, hz, echoTime_));
                graphics.setColour(style::text.withAlpha(static_cast<float>(0.2 + 0.8 * weight * amounts_.echo)));
                graphics.fillRect(juce::Rectangle<float>(x, y - 1.0f, 2.0f, 2.0f));
            }
        }
        if (!holotoner::spaceActive(amounts_)) {
            graphics.setColour(style::textDim);
            graphics.setFont(juce::FontOptions(style::kTextBody));
            graphics.drawText("Space off: turn up Interfere, Echo or Resonate in Field",
                              area.withTrimmedBottom(area.getHeight() * 0.55f), juce::Justification::centred);
        }
    }

    HolotonerProcessor& processor_;
    holotoner::SpaceAmounts amounts_;
    holotoner::ProjectedPlane plane_;
    holotoner::Resonators rings_;
    double echoTime_ = 0.25;
    std::shared_ptr<const holotoner::ImageMask> mask_;
    std::array<double, holotoner::kImageMaskHeight> weights_ {};
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
        const auto area = well.reduced(16.0f).withTrimmedTop(30.0f).withTrimmedBottom(28.0f);
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
        // Units: radius is each sample's level, angle its stereo direction.
        // Radius = 1 + log10(5 · level) / 3, so the rings are 15 dB apart, the outer one at -14 dBFS.
        graphics.setFont(juce::FontOptions(style::kTextSmall));
        graphics.setColour(style::textFaint);
        for (const auto& [fraction, label] : {std::pair {1.0f, "-14 dB"}, std::pair {0.75f, "-29"},
                                              std::pair {0.5f, "-44"}, std::pair {0.25f, "-59"}})
            graphics.drawText(label, juce::Rectangle<float>(origin.x + 4.0f, origin.y - radius * fraction + 1.0f, 50.0f, 12.0f),
                              juce::Justification::centredLeft);
        const auto edge = [&](float degrees, float extra) {
            const auto angle = degrees * pi / 180.0f;
            return juce::Point<float> {origin.x + (radius + extra) * std::sin(angle), origin.y - (radius + extra) * std::cos(angle)};
        };
        graphics.setColour(style::textDim);
        graphics.drawText("mono", juce::Rectangle<float>(60.0f, 12.0f).withCentre(edge(0.0f, 9.0f)), juce::Justification::centred);
        graphics.drawText("L", juce::Rectangle<float>(16.0f, 12.0f).withCentre(edge(-45.0f, 9.0f)), juce::Justification::centred);
        graphics.drawText("R", juce::Rectangle<float>(16.0f, 12.0f).withCentre(edge(45.0f, 9.0f)), juce::Justification::centred);
        graphics.setColour(style::textFaint);
        graphics.drawText("out of phase", juce::Rectangle<float>(origin.x - radius, origin.y - 14.0f, 80.0f, 12.0f),
                          juce::Justification::centredLeft);
        graphics.drawText("out of phase", juce::Rectangle<float>(origin.x + radius - 80.0f, origin.y - 14.0f, 80.0f, 12.0f),
                          juce::Justification::centredRight);
        graphics.setColour(style::text);
        graphics.setFont(juce::FontOptions(style::kTextTitle, juce::Font::bold));
        graphics.drawText("Sound field", 12, 7, 100, 18, juce::Justification::centredLeft);
        graphics.setColour(style::textDim);
        graphics.setFont(juce::FontOptions(style::kTextSmall));
        graphics.drawText("input", getWidth() - 180, 7, 100, 18, juce::Justification::centredRight);
        graphics.setColour(style::signal);
        graphics.drawText("output", getWidth() - 74, 7, 62, 18, juce::Justification::centredRight);
        const auto correlation = clamp(processor_.meterState().outputCorrelation, -1.0f, 1.0f);
        const auto bar = juce::Rectangle<float>(area.getX(), well.getBottom() - 24.0f, area.getWidth(), 5.0f);
        graphics.setColour(style::textFaint);
        graphics.setFont(juce::FontOptions(style::kTextSmall));
        const auto scale = bar.withY(bar.getBottom() + 2.0f).withHeight(12.0f);
        graphics.drawText("-1 opposite", scale, juce::Justification::centredLeft);
        graphics.drawText("correlation", scale, juce::Justification::centred);
        graphics.drawText("same +1", scale, juce::Justification::centredRight);
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
      textPanel_(std::make_unique<TextPanel>()),shapePanel_(std::make_unique<TextPanel>()),
      spaceView_(std::make_unique<SpaceView>(owner))
{
    setLookAndFeel(&look_);
    setOpaque(true); setWantsKeyboardFocus(true);
    title_.setText("Holotoner",juce::dontSendNotification);
    title_.setFont(juce::FontOptions(15.0f,juce::Font::bold));
    addAndMakeVisible(title_);
    for (auto* c : std::array<juce::Component*,12> {&source_,&loadAudio_,&restart_,&play_,
        &loadImage_,&reset_,&textButton_,&channel_,&xyButton_,&textLayout_,&status_}) addAndMakeVisible(c);
    // Projection is how the image is laid into the sound field: two buttons on that view.
    for (auto* button : {&xyButton_,&polarButton_}) {
        addAndMakeVisible(*button);
        button->onClick = [this,button] {
            beginParameter(processor_,"image_projection");
            setParameter(processor_,"image_projection",button == &polarButton_ ? 1.0f : 0.0f);
            endParameter(processor_,"image_projection");
            refresh();
        };
    }
    xyButton_.setTooltip("XY rectangle: the image laid straight into the sound field");
    polarButton_.setTooltip("Polar warp: the image bent around the sound field's arc");
    addAndMakeVisible(shapeButton_);
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
    combo(shape_,"motion_shape",{"Swing","Spin"});
    combo(textLayout_,"text_layout",{"Whole","Characters","Words"});
    juce::StringArray shapeNames;
    for (const auto* name : holotoner::kShapeNames) shapeNames.add(name);
    combo(shapeKind_,"shape_kind",shapeNames);
    const auto toggle = [this](juce::ToggleButton& control,const char* id) {
        addAndMakeVisible(control);
        buttonAttachments_.push_back(std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(processor_.state(),id,control));
    };
    toggle(normalize_,"image_normalize"); toggle(invert_,"image_invert");
    toggle(binary_,"image_binary"); toggle(textPlay_,"text_play"); toggle(delta_,"out_delta");
    delta_.setTooltip("Hear only what Holotoner changes: processed minus dry");
    const auto knob = [this](const juce::String& key,const juce::String& label,bool sync = false) {
        auto cell = std::make_unique<KnobCell>(processor_,key,label,sync);
        addAndMakeVisible(*cell);
        return cell;
    };
    constexpr std::array<const char*,7> labels {"Rotate X","Rotate Y","Rotate Z","Move X","Move Y","Scale X","Scale Y"};
    for (std::size_t i = 0; i < axes_.size(); ++i) {
        axes_[i] = std::make_unique<AxisRow>(processor_,i,labels[i]);
        motionPanel_.addAndMakeVisible(*axes_[i]);
    }
    for (auto* label : {&motionTitle_,&rateTitle_}) {
        label->setFont(juce::FontOptions(style::kTextBody,juce::Font::bold));
        label->setColour(juce::Label::textColourId,style::textDim);
        motionPanel_.addAndMakeVisible(*label);
    }
    motionTitle_.setText("Motion",juce::dontSendNotification);
    rateTitle_.setText("Rate",juce::dontSendNotification);
    gain_ = knob("image_gain","Gain"); fill_ = knob("image_fill","Fill");
    threshold_ = knob("image_threshold","Threshold"); depth_ = knob("motion_depth","Depth");
    frame_ = knob("text_frame","Step"); textRate_ = knob("text_rate","Rate",true);
    wet_ = knob("wet","Wet"); amount_ = knob("image_amount","Amount"); tempo_ = knob("tempo","Tempo");
    interfere_ = knob("space_interfere","Interfere"); echo_ = knob("space_echo","Echo");
    feedback_ = knob("space_feedback","Feedback"); resonate_ = knob("space_resonate","Resonate");
    echoTime_ = knob("space_echo_time","Time",true);
    edge_ = knob("image_edge","Snap"); smooth_ = knob("image_smooth","Smooth"); rest_ = knob("image_rest","Rest");
    shapeOrder_ = knob("shape_order","Order"); shapeOrder_->setRow(true);
    depth_->setRow(true); tempo_->setRow(true);
    motionPanel_.addAndMakeVisible(shape_); motionPanel_.addAndMakeVisible(*depth_);
    motionPanel_.addChildComponent(*tempo_);
    tempo_->setVisible(processor_.wrapperType == juce::AudioProcessor::wrapperType_Standalone);
    addAndMakeVisible(*planeView_); addAndMakeVisible(*polarView_); addAndMakeVisible(*spaceView_);
    xyButton_.toFront(false); polarButton_.toFront(false);
    // Motion opens inside the image well: the drawing area rises above it.
    addChildComponent(motionPanel_);
    // A drawer handle in the well's corner: it rides on the panel's top edge.
    addAndMakeVisible(motionButton_);
    motionButton_.onClick = [this] { processor_.motionPanelOpen = !processor_.motionPanelOpen; refresh(); };
    motionHeight_ = processor_.motionPanelOpen ? static_cast<float>(kMotionHeight) : 0.0f;
    addChildComponent(*textPanel_);
    textPanel_->addAndMakeVisible(textInput_);
    textPanel_->addAndMakeVisible(applyText_); textPanel_->addAndMakeVisible(closeText_);
    // Text playback belongs to the text source: it lives in the Text panel, not in Image.
    for (auto* c : std::array<juce::Component*,4> {&textLayout_,&textPlay_,frame_.get(),textRate_.get()})
        textPanel_->addAndMakeVisible(c);
    frame_->setRow(true); textRate_->setRow(true);
    textInput_.setMultiLine(false);
    textInput_.setText(processor_.text(),false); lastText_ = processor_.text();
    applyText_.onClick = [this] { processor_.setText(textInput_.getText()); };
    textInput_.onReturnKey = [this] { processor_.setText(textInput_.getText()); };
    // Text and Shape share the overlay row over the image well; one is open at a time.
    addChildComponent(*shapePanel_);
    for (auto* c : std::array<juce::Component*,4> {&shapeKind_,shapeOrder_.get(),&useShape_,&closeShape_})
        shapePanel_->addAndMakeVisible(c);
    useShape_.onClick = [this] { processor_.useShape(); };
    const auto openOverlay = [](juce::TextButton& button,TextPanel& panel,juce::TextButton& other,TextPanel& otherPanel) {
        panel.setVisible(button.getToggleState());
        if (panel.isVisible()) {
            other.setToggleState(false,juce::dontSendNotification);
            otherPanel.setVisible(false);
            panel.toFront(false);
        }
    };
    textButton_.setClickingTogglesState(true);
    textButton_.onClick = [this,openOverlay] { openOverlay(textButton_,*textPanel_,shapeButton_,*shapePanel_); };
    shapeButton_.setClickingTogglesState(true);
    shapeButton_.onClick = [this,openOverlay] { openOverlay(shapeButton_,*shapePanel_,textButton_,*textPanel_); };
    closeShape_.onClick = [this] {
        shapeButton_.setToggleState(false,juce::dontSendNotification); shapePanel_->setVisible(false);
    };
    closeText_.onClick = [this] {
        textButton_.setToggleState(false,juce::dontSendNotification); textPanel_->setVisible(false);
    };
    status_.setFont(juce::FontOptions(style::kTextSmall)); status_.setColour(juce::Label::textColourId,style::textDim);
    setResizable(true,true); setResizeLimits(900,620,1600,1000);
    int width = 1100,height = 620; // opens at the smallest height it can be dragged to
    if (const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()) {
        width = std::min(width,std::max(900,juce::roundToInt(display->userBounds.getWidth())-32));
        const auto chrome = processor_.wrapperType == juce::AudioProcessor::wrapperType_Standalone ? 96 : 48;
        height = std::min(height,std::max(620,juce::roundToInt(display->userBounds.getHeight())-chrome));
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
    // Spectral's stages: a card each, titled, with › for the flow.
    const std::array<std::pair<juce::Rectangle<int>,const char*>,3> stages {{{imageCard_,"Image"},{fieldCard_,"Space"},{outCard_,"Out"}}};
    for (std::size_t i = 0; i < stages.size(); ++i) {
        const auto& [card,title] = stages[i];
        graphics.setColour(style::card);
        graphics.fillRoundedRectangle(card.toFloat(),style::corner);
        graphics.setColour(style::text);
        graphics.setFont(juce::FontOptions(style::kTextTitle,juce::Font::bold));
        graphics.drawText(title,card.withHeight(24).withTrimmedLeft(10),juce::Justification::centredLeft);
        if (card == imageCard_ && imageDivider_ > 0) {
            graphics.setColour(style::grid);
            graphics.drawVerticalLine(imageDivider_,static_cast<float>(card.getY()+28),static_cast<float>(card.getY()+28+54));
        }
        if (i+1 < stages.size()) {
            const auto next = stages[i+1].first;
            graphics.setColour(style::textDim);
            graphics.setFont(juce::FontOptions(18.0f,juce::Font::bold));
            graphics.drawText(juce::String::fromUTF8("›"),juce::Rectangle<int>(card.getRight(),card.getY(),next.getX()-card.getRight(),24),
                              juce::Justification::centred);
        }
    }
    if (dragOver_) {
        graphics.setColour(style::linkLight.withAlpha(0.65f));
        graphics.drawRoundedRectangle(getLocalBounds().toFloat().reduced(4.0f), 7.0f, 2.0f);
    }
}

void HolotonerEditor::resized()
{
    constexpr int gap = 16,bandHeight = 112;
    auto area = getLocalBounds().reduced(10);
    status_.setBounds(area.removeFromBottom(22));
    const auto half = (area.getWidth()-gap)/2;
    auto bars = area.removeFromTop(kControlHeight);
    auto imageBar = bars.removeFromLeft(half);
    bars.removeFromLeft(gap);
    title_.setBounds(imageBar.removeFromLeft(104));
    for (auto* button : std::array<juce::Button*,4> {&loadImage_,&textButton_,&shapeButton_,&reset_}) {
        const auto width = button == &reset_ ? 82 : button == &loadImage_ ? 70 : 60;
        button->setBounds(imageBar.removeFromLeft(width)); imageBar.removeFromLeft(4);
    }
    source_.setBounds(bars.removeFromLeft(82)); bars.removeFromLeft(4);
    loadAudio_.setBounds(bars.removeFromLeft(70)); bars.removeFromLeft(4);
    restart_.setBounds(bars.removeFromLeft(64)); bars.removeFromLeft(4);
    play_.setBounds(bars.removeFromLeft(54));
    area.removeFromTop(8);

    // One band of stages under both views keeps the two views equally tall.
    auto band = area.removeFromBottom(bandHeight);
    area.removeFromBottom(8);
    planeWell_ = area.removeFromLeft(half);
    area.removeFromLeft(gap);
    // Space stays visible above the sound field at every window size.
    const auto spaceHeight = juce::jlimit(120,210,area.getHeight()-std::min(area.getHeight()-128,area.getWidth()/2+90));
    spaceView_->setBounds(area.removeFromTop(spaceHeight));
    area.removeFromTop(8);
    polarView_->setBounds(area);
    const auto projection = juce::Rectangle<int>(area.getRight()-150-104,area.getY()+5,104,kControlHeight);
    xyButton_.setBounds(projection.withWidth(52));
    polarButton_.setBounds(projection.withTrimmedLeft(52));
    planeView_->setBounds(planeWell_);
    imageCard_ = band.removeFromLeft(half);
    band.removeFromLeft(gap);
    outCard_ = band.removeFromRight(110);
    band.removeFromRight(gap);
    fieldCard_ = band;

    auto image = imageCard_.reduced(10,0).withTrimmedTop(24).withTrimmedBottom(4);
    // Image: first the picture itself (Gain, Fill, Threshold), then, after a divider, how it
    // holds the sound (Snap, Smooth, Rest, and Amount last, next to Space).
    auto pixels = image.removeFromTop(54);
    const std::array<KnobCell*,7> cells {gain_.get(),fill_.get(),threshold_.get(),edge_.get(),smooth_.get(),rest_.get(),amount_.get()};
    const auto unit = std::min(72,(pixels.getWidth()-kGroupGap)/7);
    for (auto* cell : cells) {
        if (cell == edge_.get()) {
            imageDivider_ = pixels.getX()+kGroupGap/2;
            pixels.removeFromLeft(kGroupGap);
        }
        cell->setBounds(pixels.removeFromLeft(unit).withHeight(54));
    }
    auto options = image.removeFromTop(28);
    channel_.setBounds(options.removeFromLeft(72).withSizeKeepingCentre(70,kControlHeight)); options.removeFromLeft(6);
    normalize_.setBounds(options.removeFromLeft(84));
    invert_.setBounds(options.removeFromLeft(60));
    binary_.setBounds(options.removeFromLeft(48));

    // Space: the effects. Time carries its ♪, so it is half as wide again.
    auto field = fieldCard_.reduced(10,0).withTrimmedTop(24).withTrimmedBottom(4);
    const auto cellWidth = std::min(72,juce::roundToInt(static_cast<float>(field.getWidth())/5.25f));
    for (auto* cell : {interfere_.get(),echo_.get(),echoTime_.get(),feedback_.get(),resonate_.get()}) {
        const auto width = cell == echoTime_.get() ? cellWidth*5/4 : cellWidth;
        cell->setBounds(field.removeFromLeft(width).withHeight(54)); // the same knob row as Image
    }
    // Out: Wet on the shared knob row, Delta on Image's option row.
    auto out = outCard_.reduced(10,0).withTrimmedTop(24).withTrimmedBottom(4);
    wet_->setBounds(out.removeFromTop(54).withSizeKeepingCentre(80,54));
    delta_.setBounds(out.removeFromTop(28).withSizeKeepingCentre(62,kControlHeight));

    // Motion content is laid out at full height; the panel reveals it as it opens.
    constexpr int rowHeight = 20,groupGap = 5,headerHeight = 22;
    auto rows = juce::Rectangle<int>(0,0,planeWell_.getWidth()-24,kMotionHeight).withTrimmedTop(4).withTrimmedBottom(10);
    auto header = rows.removeFromTop(headerHeight);
    rows.removeFromTop(4);
    rateTitle_.setBounds(header.removeFromRight(AxisRow::kRateWidth));
    motionTitle_.setBounds(header.removeFromLeft(58));
    shape_.setBounds(header.removeFromLeft(70).withSizeKeepingCentre(70,kControlHeight)); header.removeFromLeft(10);
    depth_->setBounds(header.removeFromLeft(depth_->rowNameWidth()+64)); header.removeFromLeft(6);
    tempo_->setBounds(header.removeFromLeft(tempo_->rowNameWidth()+90));
    for (std::size_t i = 0; i < axes_.size(); ++i) {
        if (i == 3 || i == 5) rows.removeFromTop(groupGap);
        axes_[i]->setBounds(rows.removeFromTop(rowHeight));
    }
    layoutMotion();

    // Text overlays the image well. Opening it never changes window/panel geometry.
    // Every button and menu is kControlHeight tall, as in the toolbar.
    // Two rows: the text, then how it plays (layout, Cycle, Step, Rate ♪).
    textPanel_->setBounds(planeWell_.getX()+8,planeWell_.getY()+28,planeWell_.getWidth()-16,2*kControlHeight+22);
    auto rowsArea = textPanel_->getLocalBounds().reduced(8);
    auto input = rowsArea.removeFromTop(kControlHeight);
    closeText_.setBounds(input.removeFromRight(52)); input.removeFromRight(4);
    applyText_.setBounds(input.removeFromRight(72)); input.removeFromRight(8);
    textInput_.setBounds(input);
    rowsArea.removeFromTop(6);
    auto play = rowsArea.removeFromTop(kControlHeight);
    textLayout_.setBounds(play.removeFromLeft(96)); play.removeFromLeft(8);
    textPlay_.setBounds(play.removeFromLeft(64)); play.removeFromLeft(10);
    frame_->setBounds(play.removeFromLeft(frame_->rowNameWidth()+64)); play.removeFromLeft(10);
    textRate_->setBounds(play.removeFromLeft(std::min(play.getWidth(),textRate_->rowNameWidth()+112)));
    shapePanel_->setBounds(textPanel_->getBounds().withHeight(kControlHeight+16));
    auto shape = shapePanel_->getLocalBounds().reduced(8);
    closeShape_.setBounds(shape.removeFromRight(52)); shape.removeFromRight(4);
    useShape_.setBounds(shape.removeFromRight(80)); shape.removeFromRight(8);
    shapeKind_.setBounds(shape.removeFromLeft(110)); shape.removeFromLeft(12);
    shapeOrder_->setBounds(shape.removeFromLeft(shapeOrder_->rowNameWidth()+64));
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

void HolotonerEditor::layoutMotion()
{
    const auto height = juce::roundToInt(motionHeight_);
    planeView_->setControlsHeight(height);
    motionPanel_.setBounds(planeWell_.getX()+12,planeWell_.getBottom()-height,planeWell_.getWidth()-24,height);
    motionPanel_.setVisible(height > 0);
    motionButton_.setBounds(planeWell_.getRight()-12-84,planeWell_.getBottom()-std::max(height,12)-kControlHeight-2,84,kControlHeight);
}

void HolotonerEditor::timerCallback()
{
    const auto target = processor_.motionPanelOpen ? static_cast<float>(kMotionHeight) : 0.0f;
    if (std::abs(motionHeight_-target) > 0.0f) {
        motionHeight_ += 0.45f*(target-motionHeight_);
        if (std::abs(target-motionHeight_) < 1.0f) motionHeight_ = target;
        layoutMotion();
    }
    refresh();
    spaceView_->repaint();
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
    const bool polar = processor_.state().getRawParameterValue("image_projection")->load() >= 0.5f;
    xyButton_.setToggleState(!polar,juce::dontSendNotification);
    polarButton_.setToggleState(polar,juce::dontSendNotification);
    const auto value = processor_.text();
    if (value != lastText_) { lastText_ = value; textInput_.setText(value,false); }
    for (auto& row : axes_) row->refresh();
    motionButton_.setButtonText(juce::String::fromUTF8(processor_.motionPanelOpen ? "Motion \u25BE" : "Motion \u25B4"));
    for (auto* cell : std::array<KnobCell*,12> {gain_.get(),fill_.get(),threshold_.get(),depth_.get(),
        frame_.get(),textRate_.get(),wet_.get(),amount_.get(),tempo_.get(),feedback_.get(),echo_.get(),
        resonate_.get()}) cell->refresh();
    interfere_->refresh(); shapeOrder_->refresh(); edge_->refresh(); smooth_->refresh(); rest_->refresh(); echoTime_->refresh();
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
