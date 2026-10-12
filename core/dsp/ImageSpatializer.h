#pragma once

#include "core/dsp/Fft.h"
#include "core/dsp/PhaseMap.h"
#include "core/geometry/PlaneProjection.h"
#include "core/image/ImageMask.h"

#include <array>
#include <complex>
#include <cstdint>
#include <vector>

namespace holotoner {

/**
 * Fixed-latency image spatializer.
 *
 * Phase combines bounded spectral phase drift with a projected image steering field.
 * Uses a 2048-sample, four-times-overlapped spectral stage plus image steering.
 * Wet and Dry have the same fixed latency. No allocation after prepare.
 */
class ImageSpatializer {
public:
    static constexpr std::size_t kFftSize = 2048;
    static constexpr std::size_t kHopSize = kFftSize / 4;
    struct Parameters {
        PlaneTransform plane;
        PhaseProjection phaseProjection = PhaseProjection::Cartesian;
        float imageAmount = 1.0f;
        float wet = 1.0f;
    };

    void prepare(double sampleRate);
    void reset();
    void setParameters(Parameters parameters);
    void process(float* left, float* right, int sampleCount, const ImageMask* mask);

    int latencySamples() const { return static_cast<int>(kFftSize - 1); }
    PlaneTransform currentPlane() const { return smoothedParameters_.plane; } // audio thread only
    std::uint32_t takeSafetyResets();

private:
    void renderFrame(const ImageMask* mask);
    void smoothFrameState(const ImageMask* mask);
    static void spatialize(std::complex<double> left, std::complex<double> right,
                           double signedWidth, double basePosition,
                           std::complex<double>& outputLeft, std::complex<double>& outputRight);

    double sampleRate_ = 48000.0;
    Fft fft_;
    std::array<double, kFftSize> window_ {};
    std::array<float, kFftSize> inputLeft_ {}, inputRight_ {};
    std::array<float, kFftSize> dryLeft_ {}, dryRight_ {};
    std::array<double, 2 * kFftSize> outputLeft_ {}, outputRight_ {};
    std::array<std::complex<double>, kFftSize> spectrum_ {};
    std::size_t inputAt_ = 0;
    std::size_t dryAt_ = 0;
    std::size_t outputAt_ = 0;
    std::size_t filled_ = 0;
    std::size_t sinceHop_ = 0;
    Parameters parameters_;
    Parameters smoothedParameters_;
    ImageMask targetMask_;
    const ImageMask* targetMaskIdentity_ = nullptr;
    double maskPresence_ = 0.0;
    double targetMaskPresence_ = 0.0;
    struct PhaseControls { double angle = 0.0, relative = 0.0, common = 0.0; };
    static constexpr std::size_t kPhaseAnchors = 33;
    std::array<PhaseControls, kPhaseAnchors> phaseControls_ {};
    bool phaseInitialized_ = false;
    double fieldTime_ = 0.0;
    float geometryBlend_ = 1.0f;
    std::array<ImageMask::PhasePoint,kImageMaskWidth*kImageMaskHeight> nearest_ {},previousNearest_ {};
    std::array<float,kImageMaskWidth*kImageMaskHeight> supportValues_ {};
    bool fieldHasSupport_ = false;
    float fieldAspectX_ = 1,fieldAspectY_ = 1;
    ProjectedPlane imagePlane_,previousImagePlane_;
    void constrainField(double& left, double& right);
    void buildPhaseControls(const ProjectedPlane&);
    void applyPhase(std::size_t bin, std::complex<double> left, std::complex<double> right,
                    double signedWidth, std::complex<double>& outLeft, std::complex<double>& outRight) const;
    double frameSmoothing_ = 1.0;
    float wetCurrent_ = 1.0f;
    float wetRate_ = 1.0f;
    std::uint32_t safetyResets_ = 0;
};

} // namespace holotoner
