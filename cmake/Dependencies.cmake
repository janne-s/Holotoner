include(FetchContent)

# JUCE 9 is dual-licensed under AGPLv3 and the commercial JUCE licence. The
# public Holotoner repository uses the AGPLv3 path. Keep the version pinned so a
# build cannot silently change its framework or licence inventory.
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 9.0.2
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)
