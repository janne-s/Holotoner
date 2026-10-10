#pragma once

#include <cstdint>

namespace holotoner {

/**
 * Allocation-free signed-width baseline.
 *
 * This stage establishes the orientation invariant used by the later spectral
 * engine. It preserves the input mid channel and scales its side channel by
 * width * cos(yaw). At full Wet, 90 degrees is mono and 180 degrees reverses
 * left and right. The final image-driven engine will add a bounded
 * mono-to-stereo path before the first release.
 */
class StereoWidth {
public:
    struct Parameters {
        float yawDegrees = 0.0f;
        float width = 1.0f;
        float wet = 1.0f;
    };

    void prepare(double sampleRate, double smoothingMilliseconds = 20.0);
    void reset();
    void setParameters(Parameters parameters);
    void process(float* left, float* right, int sampleCount);
    std::uint32_t takeSafetyResets();

private:
    class Ramp {
    public:
        void prepare(int samples);
        void reset(float value);
        void set(float target);
        float next();

    private:
        float current_ = 0.0f;
        float target_ = 0.0f;
        float step_ = 0.0f;
        int remaining_ = 0;
        int length_ = 0;
    };

    Ramp signedWidth_;
    Ramp wet_;
    Parameters parameters_;
    std::uint32_t safetyResets_ = 0;
    bool prepared_ = false;
};

} // namespace holotoner
