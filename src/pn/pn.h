// ACZPropNav - proportional navigation guidance core (pure math, no OS deps).
//
// The game's vanilla missiles fly a "pure pursuit" / tail-chase profile: every
// frame the missile turns toward the target's *current* position, limited by a
// turn rate and a seeker cone. Real IR/SARH missiles use Proportional Navigation
// (PN): they steer to null the line-of-sight (LOS) rotation rate, which means
// they aim at a *lead* point (where the target will be), giving much better
// intercept geometry.
//
// We expose both, plus a blend knob (HomingForesightAmount, 0 = vanilla pursuit,
// 1 = full proportional lead) so the mod can interpolate between the two exactly
// like the AC8 mod's parameter of the same name.
#pragma once

#include "common/acz_common.h"

namespace acz {

struct GuidanceParams {
    // 0..1. Blend between pure pursuit (pursuitDir) and pure PN (pnDir).
    float homingForesightAmount = 0.0f;
    // Seeker field of view, degrees. The target must be inside this cone of the
    // missile's current heading or guidance is lost for this frame.
    float maxHomingAngleDeg = 180.0f;
    // Maximum turn rate, degrees/second.
    float maxRotationAngleDegPerSec = 180.0f;
    // Seconds of flight; used by the simulator and by the lifetime patcher.
    float lifeTimeSec = 0.0f;
};

struct GuidanceInput {
    Vec3 missilePos;
    Vec3 missileVel;   // world units/second
    Vec3 targetPos;
    Vec3 targetVel;    // world units/second
    float dt = 1.0f / 120.0f;
};

enum class GuidanceMode { Pursuit, ProportionalNavigation, Blended };

struct GuidanceResult {
    Vec3 desiredDir;          // unit vector the seeker wants to fly
    float losAngleDeg = 0.f;  // angle target<->heading before turning
    float turnDeg = 0.f;      // turn applied this frame (after rate limiting)
    bool inFov = false;       // target inside seeker cone
    bool valid = false;       // input geometry usable
    float timeToGo = 0.f;     // closing-time estimate used by the PN term
    GuidanceMode mode = GuidanceMode::Pursuit;
};

// Line-of-sight rotation rate (rad/s) and closing velocity (units/s).
// losRate is the magnitude of the LOS angular velocity vector.
void ComputeLos(const GuidanceInput& in, Vec3& losUnit, float& losRateRadPerSec, float& closingSpeed);

// Pure pursuit: fly straight at the target's current position.
Vec3 PursuitDirection(const GuidanceInput& in);

// Proportional navigation: steer at the *intercept* point instead of the target's
// current position, i.e. at
//     leadPoint = targetPos + targetVel * tGo * lead
// with tGo = range / closingSpeed (clamped). lead = 0 gives pure pursuit,
// lead = 1 gives the full collision-course point — this is the
// HomingForesightAmount blend, exactly the AC8 mod's meaning.
// Returns a unit vector. Falls back to pursuit for degenerate geometry.
Vec3 ProportionalNavigationDirection(const GuidanceInput& in, float lead, float& outTimeToGo);

// Classic PN acceleration law, exposed for the simulator / documentation:
// commanded heading rate = N * losRate. Returns radians/second (>0 turns toward
// the LOS rotation direction, <0 away). N defaults to kPnNavConstant.
float PnRateCommand(const GuidanceInput& in, float navConstant = 4.0f);

// Full pipeline used by the injector and the simulator:
//   1. LOS / FOV check against the current heading,
//   2. blend pursuit <-> PN by homingForesightAmount,
//   3. clamp the turn to maxRotationAngleDegPerSec * dt.
GuidanceResult ComputeGuidance(const GuidanceInput& in, const GuidanceParams& p);

// Effective navigation constant at lead == 1.0 (classic PN N = 3..5; 4 is the
// usual sweet spot for a constant-speed missile with a slow-rotating LOS).
constexpr float kPnNavConstant = 4.0f;

} // namespace acz
