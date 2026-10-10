#pragma once

#include <cstdint>
#include <vector>

namespace holotoner {

enum class SourceKind { Input, File, Clicks };

struct SourceAudio {
    std::vector<float> left;
    std::vector<float> right;
    double sampleRate = 48000.0;
};

inline constexpr double kMaxSourceSeconds = 120.0;

/** Loops an immutable audio file or generates a test click. Real-time safe. */
class SourcePlayer {
public:
    static constexpr double kClickIntervalSeconds = 0.75;
    static constexpr double kLoopFadeSeconds = 0.005;

    void prepare(double sampleRate);
    void restart();

    /** Input leaves the buffers untouched; File and Clicks overwrite them. */
    void render(SourceKind kind, const SourceAudio* file, float* left, float* right, int samples);

private:
    double sampleRate_ = 48000.0;
    std::int64_t counter_ = 0;
    double filePosition_ = 0.0;
    const SourceAudio* file_ = nullptr;
};

} // namespace holotoner
