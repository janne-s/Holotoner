#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace holotoner_style {

inline const juce::Colour background {0xff101215};
inline const juce::Colour card {0xff181b1f};
inline const juce::Colour well {0xff0d0f12};
inline const juce::Colour raised {0xff22262c};
inline const juce::Colour grid {0xff272b32};
inline const juce::Colour text {0xffd9dce1};
inline const juce::Colour textDim {0xff8b919b};
inline const juce::Colour textFaint {0xff565c66};
inline const juce::Colour signal {0xffff4fae};
inline const juce::Colour link {0xff168a62};
inline const juce::Colour linkLight {0xff3ccf9b};

inline constexpr float corner = 5.0f;
// Spectral's three text sizes: names and values, body (buttons, groups), titles.
inline constexpr float kTextSmall = 10.5f;
inline constexpr float kTextBody = 11.0f;
inline constexpr float kTextTitle = 12.0f; ///< also Spectral's popup-menu size

void paintWell(juce::Graphics& graphics, juce::Rectangle<float> area);

} // namespace holotoner_style

class HolotonerLook final : public juce::LookAndFeel_V4 {
public:
    HolotonerLook();
    void drawRotarySlider(juce::Graphics&,int,int,int,int,float,float,float,juce::Slider&) override;
    void drawLinearSlider(juce::Graphics&,int,int,int,int,float,float,float,juce::Slider::SliderStyle,juce::Slider&) override;
    int getSliderThumbRadius(juce::Slider&) override;
    void drawToggleButton(juce::Graphics&,juce::ToggleButton&,bool highlighted,bool down) override;

    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour&, bool highlighted, bool down) override;
    void drawComboBox(juce::Graphics&, int width, int height, bool down, int buttonX, int buttonY, int buttonWidth,
                      int buttonHeight, juce::ComboBox&) override;
    void positionComboBoxText(juce::ComboBox&, juce::Label&) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override;
    juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;
    juce::Font getPopupMenuFont() override;
};
