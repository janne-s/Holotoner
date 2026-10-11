#pragma once

#include "core/dsp/Fft.h"
#include "core/geometry/PlaneProjection.h"
#include "core/image/ImageMask.h"

#include <array>
#include <complex>
#include <cstdint>
#include <vector>

namespace holotoner {

/** Frequency-dependent destination for the selected image channel. */
enum class ImageTarget { Off, Place, Level };

/**
 * Fixed-latency image spatializer.
 *
 * A 2048-sample, four-times-overlapped STFT maps logarithmic frequency to the
 * projected image. Processing is allocation-free after prepare(). Dry is
 * delayed by the same fixed latency, so Dry/Wet remains phase-aligned.
 */
class ImageSpatializer {
public:
    static constexpr std::size_t kFftSize = 2048;
    static constexpr std::size_t kHopSize = kFftSize / 4;
    static constexpr std::size_t kAnalysisBins = 64;

    struct Analysis {
        std::array<float, kAnalysisBins> level {};
        std::array<float, kAnalysisBins> balance {};
        std::array<float, kAnalysisBins> correlation {};
        std::uint64_t revision = 0;
    };

    struct Parameters {
        PlaneTransform plane;
        ImageTarget target = ImageTarget::Place;
        float imageAmount = 0.7f;
        float wet = 1.0f;
    };

    void prepare(double sampleRate);
    void reset();
    void setParameters(Parameters parameters);
    void process(float* left, float* right, int sampleCount, const ImageMask* mask);

    int latencySamples() const { return static_cast<int>(kFftSize - 1); }
    std::uint32_t takeSafetyResets();
    const Analysis& analysis() const { return analysis_; }

private:
    struct RowTarget {
        double frequency = 0.0;
        double lateral = 0.0;
        double signedSpread = 0.0;
        double level = 0.0;
        double presence = 0.0;
    };

    void renderFrame(const ImageMask* mask);
    void smoothFrameState(const ImageMask* mask);
    void buildRows(const ProjectedPlane& projection);
    RowTarget rowForFrequency(double normalizedFrequency) const;
    static void spatialize(std::complex<double> left, std::complex<double> right, double signedWidth,
                           double basePosition, const RowTarget& row,
                           const std::array<double, 3>& targetWeights, double amount,
                           std::complex<double>& outputLeft, std::complex<double>& outputRight);

    double sampleRate_ = 48000.0;
    Fft fft_;
    std::array<double, kFftSize> window_ {};
    std::array<float, kFftSize> inputLeft_ {}, inputRight_ {};
    std::array<float, kFftSize> dryLeft_ {}, dryRight_ {};
    std::array<double, 2 * kFftSize> outputLeft_ {}, outputRight_ {};
    std::array<std::complex<double>, kFftSize> spectrum_ {};
    std::array<RowTarget, kImageMaskHeight> rows_ {};
    std::size_t inputAt_ = 0;
    std::size_t dryAt_ = 0;
    std::size_t outputAt_ = 0;
    std::size_t filled_ = 0;
    std::size_t sinceHop_ = 0;
    Parameters parameters_;
    Parameters smoothedParameters_;
    ImageMask smoothedMask_;
    ImageMask targetMask_;
    const ImageMask* targetMaskIdentity_ = nullptr;
    double maskPresence_ = 0.0;
    double targetMaskPresence_ = 0.0;
    std::array<double, 3> targetWeights_ {0.0, 1.0, 0.0};
    double frameSmoothing_ = 1.0;
    Analysis analysis_;
    float wetCurrent_ = 1.0f;
    float wetRate_ = 1.0f;
    std::uint32_t safetyResets_ = 0;
};

} // namespace holotoner
