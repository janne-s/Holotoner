#pragma once
#include "core/geometry/PlaneProjection.h"

namespace holotoner {
enum class PhaseProjection { Cartesian, Polar };
struct ScopePoint { float x = 0.0f; float y = 0.0f; };
struct StereoSample { float left = 0.0f; float right = 0.0f; };

// Cartesian placement preserves straight image edges; points outside the
// semicircle are clipped, never bent to make the rectangle fit.
ScopePoint phaseMapPoint(const ProjectedPlane&, float x, float y, PhaseProjection);
StereoSample scopePointToStereo(ScopePoint);
ScopePoint stereoToScopePoint(float left, float right);
} // namespace holotoner
