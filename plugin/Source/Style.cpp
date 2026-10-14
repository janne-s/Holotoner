#include "Style.h"
#include <algorithm>
#include <cmath>

namespace holotoner_style {

void paintWell(juce::Graphics& graphics, juce::Rectangle<float> area)
{
    graphics.setColour(well);
    graphics.fillRoundedRectangle(area, corner);
    const juce::ColourGradient glow(signal.withAlpha(0.04f), area.getCentreX(), area.getBottom(),
                                    juce::Colours::transparentBlack, area.getCentreX(), area.getCentreY(), false);
    graphics.setGradientFill(glow);
    graphics.fillRoundedRectangle(area, corner);
}

} // namespace holotoner_style

HolotonerLook::HolotonerLook()
{
    using namespace holotoner_style;
    setColour(juce::ResizableWindow::backgroundColourId, background);
    setColour(juce::TextButton::buttonColourId, raised);
    setColour(juce::TextButton::buttonOnColourId, raised);
    setColour(juce::TextButton::textColourOffId, text);
    setColour(juce::TextButton::textColourOnId, linkLight);
    setColour(juce::ComboBox::backgroundColourId, raised);
    setColour(juce::ComboBox::textColourId, text);
    setColour(juce::ComboBox::arrowColourId, textDim);
    setColour(juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
    setColour(juce::PopupMenu::backgroundColourId, card);
    setColour(juce::PopupMenu::textColourId, text);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, raised);
    setColour(juce::PopupMenu::highlightedTextColourId, linkLight);
    setColour(juce::Label::textColourId, text);
    setColour(juce::Slider::trackColourId, grid);
    setColour(juce::Slider::thumbColourId, holotoner_style::signal);
    setColour(juce::Slider::textBoxTextColourId, text);
    setColour(juce::Slider::textBoxBackgroundColourId, well);
    setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
}

void HolotonerLook::drawButtonBackground(juce::Graphics& graphics, juce::Button& button,
                                         const juce::Colour& background, bool highlighted, bool down)
{
    auto colour = background;
    if (down)
        colour = colour.brighter(0.12f);
    else if (highlighted)
        colour = colour.brighter(0.06f);
    const auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
    graphics.setColour(colour);
    graphics.fillRoundedRectangle(bounds, holotoner_style::corner);
    if (button.getToggleState()) {
        graphics.setColour(holotoner_style::link.withAlpha(0.8f));
        graphics.drawRoundedRectangle(bounds, holotoner_style::corner, 1.0f);
    }
}

void HolotonerLook::drawRotarySlider(juce::Graphics& g,int x,int y,int width,int height,
                                   float position,float start,float end,juce::Slider& slider)
{
    // Spectral Space's small bipolar arc and indicator, not JUCE's default filled knob.
    constexpr float stroke = 2.5f;
    const auto bounds = juce::Rectangle<int>(x,y,width,height).toFloat();
    const auto centre = bounds.getCentre();
    const auto radius = std::min(bounds.getWidth(),bounds.getHeight())*0.5f-stroke;
    juce::Path track;
    track.addCentredArc(centre.x,centre.y,radius,radius,0,start,end,true);
    g.setColour(holotoner_style::grid);
    g.strokePath(track,juce::PathStrokeType(stroke,juce::PathStrokeType::curved,juce::PathStrokeType::rounded));
    const bool bipolar = slider.getMinimum()<0 && slider.getMaximum()>0;
    const auto from = start + (bipolar ? static_cast<float>(slider.valueToProportionOfLength(0)) : 0)*(end-start);
    const auto angle = start+position*(end-start);
    if (std::abs(angle-from)>0.001f) {
        juce::Path value;
        value.addCentredArc(centre.x,centre.y,radius,radius,0,std::min(from,angle),std::max(from,angle),true);
        g.setColour(slider.isEnabled() ? holotoner_style::signal : holotoner_style::textFaint);
        g.strokePath(value,juce::PathStrokeType(stroke,juce::PathStrokeType::curved,juce::PathStrokeType::rounded));
    }
    g.setColour(slider.isMouseOverOrDragging() ? holotoner_style::linkLight
        : holotoner_style::text.withAlpha(slider.isEnabled() ? 0.85f : 0.35f));
    g.drawLine({centre.getPointOnCircumference(radius*0.35f,angle),centre.getPointOnCircumference(radius-3,angle)},1.6f);
}

int HolotonerLook::getSliderThumbRadius(juce::Slider&) { return 4; }

void HolotonerLook::drawLinearSlider(juce::Graphics& g,int x,int y,int width,int height,float position,float minimum,
                                     float maximum,juce::Slider::SliderStyle sliderStyle,juce::Slider& slider)
{
    if (sliderStyle != juce::Slider::LinearHorizontal) {
        LookAndFeel_V4::drawLinearSlider(g,x,y,width,height,position,minimum,maximum,sliderStyle,slider);
        return;
    }
    // A thin track filled from the parameter's default (0 for bipolar axes, 1 for scale).
    const auto centre = static_cast<float>(y)+0.5f*static_cast<float>(height);
    const auto left = static_cast<float>(x),right = static_cast<float>(x+width);
    g.setColour(holotoner_style::grid);
    g.fillRoundedRectangle(juce::Rectangle<float>::leftTopRightBottom(left,centre-1.5f,right,centre+1.5f),1.5f);
    const auto origin = std::clamp(slider.getDoubleClickReturnValue(),slider.getMinimum(),slider.getMaximum());
    const auto from = static_cast<float>(slider.getPositionOfValue(origin));
    g.setColour(slider.isEnabled() ? holotoner_style::signal : holotoner_style::textFaint);
    g.fillRoundedRectangle(juce::Rectangle<float>::leftTopRightBottom(std::min(from,position),centre-1.5f,
                                                                      std::max(from,position),centre+1.5f),1.5f);
    g.setColour(slider.isMouseOverOrDragging() ? holotoner_style::linkLight : holotoner_style::text);
    g.fillRoundedRectangle(juce::Rectangle<float>(4.0f,12.0f).withCentre({position,centre}),2.0f);
}

void HolotonerLook::drawComboBox(juce::Graphics& graphics, int width, int height, bool, int, int, int, int,
                                 juce::ComboBox& box)
{
    const auto bounds = juce::Rectangle<int>(0, 0, width, height).toFloat().reduced(0.5f);
    graphics.setColour(box.isMouseOver(true) ? holotoner_style::raised.brighter(0.06f) : holotoner_style::raised);
    graphics.fillRoundedRectangle(bounds, holotoner_style::corner);
    graphics.setColour(box.isMouseOver(true) ? holotoner_style::linkLight : holotoner_style::textDim);
    graphics.drawVerticalLine(width - 8, 0.5f * static_cast<float>(height) - 3.0f,
                              0.5f * static_cast<float>(height) + 3.0f);
}

void HolotonerLook::positionComboBoxText(juce::ComboBox& box, juce::Label& label)
{
    label.setBounds(4, 1, box.getWidth() - 14, box.getHeight() - 2);
    label.setFont(getComboBoxFont(box));
    label.setJustificationType(juce::Justification::centred);
}

juce::Font HolotonerLook::getComboBoxFont(juce::ComboBox&) { return juce::FontOptions(holotoner_style::kTextBody); }

juce::Font HolotonerLook::getTextButtonFont(juce::TextButton&, int) { return juce::FontOptions(holotoner_style::kTextBody); }

juce::Font HolotonerLook::getPopupMenuFont() { return juce::FontOptions(holotoner_style::kTextTitle); }

void HolotonerLook::drawToggleButton(juce::Graphics& g,juce::ToggleButton& button,bool highlighted,bool)
{
    using namespace holotoner_style;
    auto area = button.getLocalBounds();
    const auto box = area.removeFromLeft(18).toFloat().withSizeKeepingCentre(13.0f,13.0f);
    g.setColour(raised.brighter(highlighted ? 0.06f : 0.0f));
    g.fillRoundedRectangle(box,3.0f);
    if (button.getToggleState()) {
        juce::Path tick;
        tick.startNewSubPath(box.getX()+3.0f,box.getCentreY());
        tick.lineTo(box.getX()+5.5f,box.getBottom()-3.5f);
        tick.lineTo(box.getRight()-3.0f,box.getY()+3.5f);
        g.setColour(button.isEnabled() ? linkLight : textFaint);
        g.strokePath(tick,juce::PathStrokeType(1.6f,juce::PathStrokeType::curved,juce::PathStrokeType::rounded));
    }
    g.setColour(button.isEnabled() ? text : textFaint);
    g.setFont(juce::FontOptions(kTextBody));
    g.drawText(button.getButtonText(),area.withTrimmedLeft(4),juce::Justification::centredLeft);
}
