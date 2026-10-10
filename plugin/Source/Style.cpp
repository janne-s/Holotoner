#include "Style.h"

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

juce::Font HolotonerLook::getComboBoxFont(juce::ComboBox&) { return juce::FontOptions(11.0f); }

juce::Font HolotonerLook::getTextButtonFont(juce::TextButton&, int) { return juce::FontOptions(11.0f); }
