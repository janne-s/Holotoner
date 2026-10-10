juce_add_plugin(Holotoner
    PRODUCT_NAME "Holotoner"
    COMPANY_NAME "Holotoner"
    BUNDLE_ID "audio.holotoner.holotoner"
    PLUGIN_MANUFACTURER_CODE Hltn
    PLUGIN_CODE Holo
    FORMATS Standalone VST3 AU
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES Fx Spatial
    AU_MAIN_TYPE kAudioUnitType_Effect
    # A normal public build must not write into a user's plug-in directories.
    # Installation for DAW listening is a separate, explicit workflow.
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(Holotoner PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginProcessor.h)

target_compile_definitions(Holotoner PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_MODAL_LOOPS_PERMITTED=0)

target_link_libraries(Holotoner
    PRIVATE
        holotoner_core
        juce::juce_audio_utils
        juce::juce_audio_processors
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
