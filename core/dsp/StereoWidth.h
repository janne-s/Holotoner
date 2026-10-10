#pragma once

#include "core/geometry/PlaneProjection.h"

#include <cstdint>

namespace holotoner {

/**
 * Allocation-free signed-width baseline.
 *
 * This stage establishes the orientation invariant used by the later spectral
 * engine. It preserves the input mid channel and scales its side channel by
 * the canonical plane's signed projected width. Horizontal position balances
 * the processed result left or right. At full Wet, a centred plane at 90
 * degrees yaw is mono and 180 degrees reverses left and right. The final
 * image-driven engine will add a bounded mono-to-stereo path before release.
 */
class StereoWidth {
public:
    struct Parameters {
        PlaneTransform plane;
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
    Ramp positionX_;
    Ramp wet_;
    Parameters parameters_;
    std::uint32_t safetyResets_ = 0;
    bool prepared_ = false;
};

} // namespace holotoner
