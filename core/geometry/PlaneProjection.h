#pragma once

#include <array>

namespace holotoner {

struct PlaneTransform {
    float positionX = 0.0f; ///< normalized stereo centre, -1 left ... 1 right
    float positionY = 0.0f; ///< normalized logarithmic-frequency centre
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    float pitchDegrees = 0.0f; ///< rotation around local X
    float yawDegrees = 0.0f;   ///< rotation around local Y
    float rollDegrees = 0.0f;  ///< rotation around local Z
};

struct ProjectedPoint {
    float stereo = 0.0f;
    float frequency = 0.0f;
    float depth = 0.0f;
};

struct ProjectedPlane {
    // Counter-clockwise from the lower-left corner in local plane space.
    std::array<ProjectedPoint, 4> corners {};
    float signedFacing = 1.0f; ///< cos(yaw): front +1, edge 0, back -1
    float stereoSpan = 0.0f;
    float frequencySpan = 0.0f;
};

/**
 * Project a unit plane with rotation order X (pitch), Y (yaw), Z (roll).
 *
 * Projection is orthographic on purpose: it keeps transform endpoints exact
 * and deterministic for DSP and UI consumers. Perspective is a presentation
 * concern and must not change the canonical audio geometry.
 */
ProjectedPlane projectPlane(const PlaneTransform& transform);

} // namespace holotoner
