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

juce::Point<float> polarPoint(juce::Point<float> centre, float maximumRadius,
                              const holotoner::ProjectedPoint& point)
{
    const auto stereo = clamp(point.stereo, -2.0f, 2.0f) * 0.5f;
    const auto frequency = 0.25f + 0.7f * ((clamp(point.frequency, -2.0f, 2.0f) + 2.0f) * 0.25f);
    const auto angle = (-90.0f + 60.0f * stereo) * pi / 180.0f;
    const auto radius = maximumRadius * frequency;
    return {centre.x + radius * std::cos(angle), centre.y + radius * std::sin(angle)};
}

} // namespace

class HolotonerEditor::PlaneView final : public juce::Component {
public:
    explicit PlaneView(HolotonerProcessor& processor) : processor_(processor)
    {
        setMouseCursor(juce::MouseCursor::DraggingHandCursor);
    }

    void setImage(juce::Image image)
    {
        image_ = std::move(image);
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

        const auto projected = holotoner::projectPlane(processor_.planeTransform());
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
        graphics.drawText("IMAGE PLANE", 12, 7, 150, 18, juce::Justification::centredLeft);
        graphics.setColour(style::textDim);
        graphics.setFont(juce::FontOptions(10.5f));
        graphics.drawText("drag centre / white rotate / pink scale / top roll", 118, 7,
                          getWidth() - 130, 18, juce::Justification::centredRight, true);
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
            setParameter(processor_, "scale_x", clamp(start_.scaleX + delta.x / unit, 0.05f, 2.0f));
            setParameter(processor_, "scale_y", clamp(start_.scaleY - delta.y / unit, 0.05f, 2.0f));
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
        return std::max(1.0f, std::min(area.getWidth(), area.getHeight()) / 5.0f);
    }

    std::array<juce::Point<float>, 4> screenPoints(const ProjectedPlane& projected) const
    {
        std::array<juce::Point<float>, 4> points;
        const auto centre = drawingArea().getCentre();
        const auto unit = viewUnit();
        for (std::size_t i = 0; i < points.size(); ++i)
            points[i] = {centre.x + projected.corners[i].stereo * unit,
                         centre.y - projected.corners[i].frequency * unit};
        return points;
    }

    static Handles handlePoints(const std::array<juce::Point<float>, 4>& points)
    {
        const auto centre = 0.25f * (points[0] + points[1] + points[2] + points[3]);
        const auto right = 0.5f * (points[1] + points[2]);
        const auto top = 0.5f * (points[2] + points[3]);
        auto direction = top - centre;
        const auto length = std::max(1.0f, direction.getDistanceFromOrigin());
        direction *= 22.0f / length;
        return {centre, points[2], right, top, top + direction};
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
    PlaneTransform start_;
    Drag drag_ = Drag::None;
};

class HolotonerEditor::PolarView final : public juce::Component {
public:
    explicit PolarView(HolotonerProcessor& processor) : processor_(processor)
    {
        setInterceptsMouseClicks(false, false);
    }

    void setMask(const holotoner::ImageMask* mask)
    {
        hasMask_ = mask != nullptr;
        if (mask != nullptr)
            mask_ = *mask;
        repaint();
    }

    void paint(juce::Graphics& graphics) override
    {
        const auto well = getLocalBounds().toFloat();
        style::paintWell(graphics, well);
        const auto area = well.reduced(16.0f).withTrimmedTop(22.0f).withTrimmedBottom(24.0f);
        const juce::Point<float> centre {area.getCentreX(), area.getBottom() - 2.0f};
        const auto radius = std::max(10.0f, std::min(0.5f * area.getWidth() - 10.0f, area.getHeight() - 8.0f));

        graphics.saveState();
        graphics.reduceClipRegion(area.toNearestInt());
        graphics.setColour(style::grid);
        for (float fraction : {0.25f, 0.5f, 0.75f, 1.0f}) {
            const auto r = radius * fraction;
            graphics.drawEllipse(juce::Rectangle<float>(2.0f * r, 2.0f * r).withCentre(centre), 1.0f);
        }
        for (float degrees : {-60.0f, -30.0f, 0.0f, 30.0f, 60.0f}) {
            const auto angle = (-90.0f + degrees) * pi / 180.0f;
            graphics.drawLine(centre.x, centre.y, centre.x + radius * std::cos(angle),
                              centre.y + radius * std::sin(angle), 1.0f);
        }

        const auto projected = holotoner::projectPlane(processor_.planeTransform());
        if (hasMask_)
            drawMask(graphics, centre, radius, projected);
        juce::Path plane;
        for (std::size_t i = 0; i < projected.corners.size(); ++i) {
            const auto point = polarPoint(centre, radius, projected.corners[i]);
            if (i == 0)
                plane.startNewSubPath(point);
            else
                plane.lineTo(point);
        }
        plane.closeSubPath();
        graphics.setColour(style::signal.withAlpha(0.08f));
        graphics.fillPath(plane);
        graphics.setColour(style::signal.withAlpha(0.45f));
        graphics.strokePath(plane, juce::PathStrokeType(1.2f));

        const auto meters = processor_.meterState();
        drawMeter(graphics, centre, radius, meters.inputLeft, meters.inputRight,
                  style::textDim.withAlpha(0.65f), 3.0f);
        drawMeter(graphics, centre, radius, meters.outputLeft, meters.outputRight,
                  style::signal, 4.5f);
        graphics.restoreState();

        graphics.setColour(style::text);
        graphics.setFont(juce::FontOptions(11.0f, juce::Font::bold));
        graphics.drawText("POLAR / OUTPUT", 12, 7, 150, 18, juce::Justification::centredLeft);
        graphics.setColour(style::textDim);
        graphics.setFont(juce::FontOptions(10.5f));
        graphics.drawText("input", getWidth() - 120, 7, 42, 18, juce::Justification::centredRight);
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
                                          std::min(centreX, valueX), bar.getY(), std::max(centreX, valueX), bar.getBottom()),
                                      2.5f);
    }

private:
    void drawMask(juce::Graphics& graphics, juce::Point<float> centre, float radius,
                  const ProjectedPlane& projected) const
    {
        constexpr int stride = 4;
        for (int y = 0; y < holotoner::kImageMaskHeight; y += stride)
            for (int x = 0; x < holotoner::kImageMaskWidth; x += stride) {
                const auto value = mask_.at(x, y);
                if (value <= 0.01f)
                    continue;
                const auto u = static_cast<float>(x) / static_cast<float>(holotoner::kImageMaskWidth - 1);
                const auto topToBottom = static_cast<float>(y) / static_cast<float>(holotoner::kImageMaskHeight - 1);
                const auto vertical = 1.0f - topToBottom;
                const auto interpolate = [](const holotoner::ProjectedPoint& a,
                                            const holotoner::ProjectedPoint& b, float amount) {
                    return holotoner::ProjectedPoint {a.stereo + amount * (b.stereo - a.stereo),
                                                       a.frequency + amount * (b.frequency - a.frequency),
                                                       a.depth + amount * (b.depth - a.depth)};
                };
                const auto bottom = interpolate(projected.corners[0], projected.corners[1], u);
                const auto top = interpolate(projected.corners[3], projected.corners[2], u);
                const auto sample = interpolate(bottom, top, vertical);
                const auto point = polarPoint(centre, radius, sample);
                graphics.setColour(style::text.withAlpha(0.04f + 0.13f * value));
                graphics.fillEllipse(juce::Rectangle<float>(5.0f, 5.0f).withCentre(point));
            }
    }

    static void drawMeter(juce::Graphics& graphics, juce::Point<float> centre, float maximumRadius,
                          float left, float right, juce::Colour colour, float width)
    {
        const auto total = left + right;
        const auto balance = total > 1.0e-9f ? clamp((right - left) / total, -1.0f, 1.0f) : 0.0f;
        const auto level = clamp(std::sqrt(std::max(left, right)), 0.0f, 1.0f);
        const auto angle = (-90.0f + 60.0f * balance) * pi / 180.0f;
        const auto end = juce::Point<float> {centre.x + maximumRadius * level * std::cos(angle),
                                             centre.y + maximumRadius * level * std::sin(angle)};
        graphics.setColour(colour.withAlpha(0.12f));
        graphics.drawLine(centre.x, centre.y, end.x, end.y, width + 8.0f);
        graphics.setColour(colour);
        graphics.drawLine(centre.x, centre.y, end.x, end.y, width);
    }

    HolotonerProcessor& processor_;
    holotoner::ImageMask mask_;
    bool hasMask_ = false;
};

HolotonerEditor::HolotonerEditor(HolotonerProcessor& owner)
    : AudioProcessorEditor(owner), processor_(owner),
      planeView_(std::make_unique<PlaneView>(owner)), polarView_(std::make_unique<PolarView>(owner))
{
    setLookAndFeel(&look_);
    setOpaque(true);
    setWantsKeyboardFocus(true);

    title_.setText("HOLOTONER", juce::dontSendNotification);
    title_.setFont(juce::FontOptions(15.0f, juce::Font::bold));
    title_.setColour(juce::Label::textColourId, style::text);
    addAndMakeVisible(title_);

    source_.addItem("Input", sourceItem(SourceKind::Input));
    source_.addItem("File", sourceItem(SourceKind::File));
    source_.addItem("Clicks", sourceItem(SourceKind::Clicks));
    source_.onChange = [this] {
        const auto kind = static_cast<SourceKind>(source_.getSelectedId() - 1);
        if (kind == SourceKind::File && processor_.sourceName().isEmpty())
            chooseAudioFile();
        else
            processor_.setSourceKind(kind);
    };
    loadAudio_.onClick = [this] { chooseAudioFile(); };
    restart_.onClick = [this] { processor_.restartSource(); };
    play_.onClick = [this] { processor_.toggleSourcePlaying(); };
    loadImage_.onClick = [this] { chooseImageFile(); };
    reset_.onClick = [this] { resetTransform(); };
    const std::array<juce::Component*, 6> barComponents {&source_, &loadAudio_, &restart_, &play_, &loadImage_, &reset_};
    for (auto* component : barComponents)
        addAndMakeVisible(component);

    wetLabel_.setText("WET", juce::dontSendNotification);
    wetLabel_.setFont(juce::FontOptions(10.5f, juce::Font::bold));
    wetLabel_.setColour(juce::Label::textColourId, style::textDim);
    wetLabel_.setJustificationType(juce::Justification::centredRight);
    wet_.setSliderStyle(juce::Slider::LinearHorizontal);
    wet_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 52, 24);
    wet_.textFromValueFunction = [](double value) { return juce::String(100.0 * value, 0) + " %"; };
    wet_.valueFromTextFunction = [](const juce::String& text) { return 0.01 * text.getDoubleValue(); };
    wetAttachment_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(processor_.state(), "wet", wet_);
    addAndMakeVisible(wetLabel_);
    addAndMakeVisible(wet_);

    status_.setFont(juce::FontOptions(10.5f));
    status_.setColour(juce::Label::textColourId, style::textDim);
    addAndMakeVisible(status_);
    addAndMakeVisible(*planeView_);
    addAndMakeVisible(*polarView_);

    setResizable(true, true);
    setResizeLimits(760, 480, 1440, 920);
    setSize(1040, 650);
    startTimerHz(30);
    refresh();
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
    auto bar = area.removeFromTop(34);
    title_.setBounds(bar.removeFromLeft(112));
    source_.setBounds(bar.removeFromLeft(82));
    bar.removeFromLeft(4);
    loadAudio_.setBounds(bar.removeFromLeft(68));
    bar.removeFromLeft(4);
    restart_.setBounds(bar.removeFromLeft(62));
    bar.removeFromLeft(4);
    play_.setBounds(bar.removeFromLeft(52));
    bar.removeFromLeft(12);
    loadImage_.setBounds(bar.removeFromLeft(68));
    bar.removeFromLeft(4);
    reset_.setBounds(bar.removeFromLeft(82));
    wet_.setBounds(bar.removeFromRight(190));
    wetLabel_.setBounds(bar.removeFromRight(38));

    area.removeFromTop(8);
    status_.setBounds(area.removeFromBottom(24));
    area.removeFromBottom(4);
    const auto gap = 8;
    const auto leftWidth = (area.getWidth() - gap) / 2;
    planeView_->setBounds(area.removeFromLeft(leftWidth));
    area.removeFromLeft(gap);
    polarView_->setBounds(area);
}

bool HolotonerEditor::keyPressed(const juce::KeyPress& key)
{
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
    const auto kind = processor_.sourceKind();
    source_.setSelectedId(sourceItem(kind), juce::dontSendNotification);
    restart_.setEnabled(kind != SourceKind::Input);
    play_.setEnabled(kind != SourceKind::Input);
    play_.setButtonText(processor_.isSourcePlaying() ? "Stop" : "Play");
    play_.setToggleState(kind != SourceKind::Input && processor_.isSourcePlaying()
                             && !processor_.isWaitingForHost(),
                         juce::dontSendNotification);

    const auto image = processor_.imageDocument();
    if (image != currentImage_) {
        currentImage_ = image;
        planeView_->setImage(image != nullptr ? image->image : juce::Image {});
        polarView_->setMask(image != nullptr ? &image->lumaMask : nullptr);
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
        message = kind == SourceKind::Clicks ? "Test clicks - Space toggles playback in Standalone"
                                             : "Host input";
    if (image != nullptr)
        message += "    Image: " + image->name;
    status_.setText(message + "    Plane gestures are automatable; polar is read-only.", juce::dontSendNotification);
}
