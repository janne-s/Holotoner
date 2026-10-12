#pragma once

#include "core/geometry/PlaneProjection.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace holotoner {

// Same quarter-note beat units and choice order as Spectral Space.
inline constexpr std::array<const char*, 24> kSyncNames {
    "off", "1/256", "1/128", "1/64T", "1/64", "1/32T", "1/32", "1/32D",
    "1/16T", "1/16", "1/16D", "1/8T", "1/8", "1/8D", "1/4T", "1/4",
    "1/4D", "1/2T", "1/2", "1/2D", "1/1", "2/1", "4/1", "8/1"};
inline constexpr std::array<double, 24> kSyncBeats {
    0, 1.0/64, 1.0/32, 1.0/24, 1.0/16, 1.0/12, 1.0/8, 3.0/16,
    1.0/6, 1.0/4, 3.0/8, 1.0/3, 1.0/2, 3.0/4, 2.0/3, 1, 1.5,
    4.0/3, 2, 3, 4, 8, 16, 32};

inline double cycleFraction(double cycles) { return cycles - std::floor(cycles); }
inline float wrappedDegrees(float degrees) { return std::remainder(degrees, 360.0f); }

struct MotionSettings {
    // Off, Rotation X/Y/Z, Position X/Y, Scale X/Y.
    int target = 0;
    bool spin = false;
    float depth = 0.5f;
    bool operator==(const MotionSettings& other) const
    { return target == other.target && spin == other.spin && std::abs(depth-other.depth) < 1.0e-6f; }
};

inline PlaneTransform motionPlane(PlaneTransform plane, MotionSettings motion, double cycles)
{
    const auto swing = static_cast<float>(std::sin(2.0 * std::numbers::pi * cycleFraction(cycles)));
    const auto rotation = motion.spin ? static_cast<float>(360.0 * cycleFraction(cycles))
                                     : 180.0f * motion.depth * swing;
    switch (motion.target) {
    case 1: plane.pitchDegrees = wrappedDegrees(plane.pitchDegrees + rotation); break;
    case 2: plane.yawDegrees = wrappedDegrees(plane.yawDegrees + rotation); break;
    case 3: plane.rollDegrees = wrappedDegrees(plane.rollDegrees + rotation); break;
    case 4: plane.positionX = std::clamp(plane.positionX + motion.depth*swing, -1.0f, 1.0f); break;
    case 5: plane.positionY = std::clamp(plane.positionY + motion.depth*swing, -1.0f, 1.0f); break;
    case 6:
        plane.scaleX = std::clamp(plane.scaleX * std::exp2(motion.depth*swing), 0.05f, 4.0f);
        break;
    case 7:
        plane.scaleY = std::clamp(plane.scaleY * std::exp2(motion.depth*swing), 0.05f, 4.0f);
        break;
    default: break;
    }
    return plane;
}

/** Audio-owned accumulator. Synced cycles are derived from PPQ, not accumulated Hz.
    Free cycles keep their phase on rate changes. Transport pause holds both clocks. */
struct CycleClock {
    double freeCycles = 0;
    double beats = 0;
    void reset() { *this = CycleClock {}; }
    double cycles(int division, double ppq) const
    {
        const auto period = kSyncBeats[static_cast<std::size_t>(std::clamp(division, 0, 23))];
        return period > 0 ? ppq / period : freeCycles;
    }
    void advance(int samples, double sampleRate, double hz, double bpm, bool playing)
    {
        if (!playing) return;
        // Integer sample epochs avoid accumulating different rounding errors for
        // block 1 versus block 512. Preserve accumulated phase when rate/tempo changes.
        if (std::abs(hz-lastHz_)>1.0e-12 || std::abs(sampleRate-lastSampleRate_)>1.0e-12) {
            freeStart_ = freeCycles; freeSamples_ = 0; lastHz_ = hz;
        }
        if (std::abs(bpm-lastBpm_)>1.0e-12 || std::abs(sampleRate-lastSampleRate_)>1.0e-12) {
            beatStart_ = beats; beatSamples_ = 0; lastBpm_ = bpm;
        }
        lastSampleRate_ = sampleRate;
        freeSamples_ += static_cast<std::uint64_t>(std::max(0,samples));
        beatSamples_ += static_cast<std::uint64_t>(std::max(0,samples));
        freeCycles = freeStart_ + static_cast<double>(freeSamples_)/std::max(1.0,sampleRate)*hz;
        beats = beatStart_ + static_cast<double>(beatSamples_)/std::max(1.0,sampleRate)*bpm/60.0;
    }
private:
    double freeStart_ = 0,beatStart_ = 0,lastHz_ = -1,lastBpm_ = -1,lastSampleRate_ = 0;
    std::uint64_t freeSamples_ = 0,beatSamples_ = 0;
};

} // namespace holotoner
