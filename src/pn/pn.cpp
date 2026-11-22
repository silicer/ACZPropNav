// ACZPropNav - proportional navigation guidance core.
//
// Pure math on floats: no OS calls, no allocation, no exceptions, no RTTI.
// That keeps it (a) unit-testable offline in tools/pnsim and (b) cheap enough
// to evaluate once per missile per frame inside the injector tick.
//
// Geometry convention (matches pn.h / docs/PN.md):
//   r   = targetPos - missilePos             (LOS vector, missile -> target)
//   v   = targetVel - missileVel             (relative velocity of target)
//   rHat= r / |r|                            (unit LOS)
//   w   = (r x v) / |r|^2                    (LOS angular velocity vector)
//   lambda_dot = |w|                         (LOS rotation rate, rad/s)
//   Vc  = dot(missileVel - targetVel, rHat)  (>0 while the range shrinks)
#include "pn/pn.h"

#include <cmath>

namespace acz {
namespace {

constexpr float kEps = 1e-6f;         // degenerate-length threshold (metres)
constexpr float kMaxTimeToGo = 20.0f; // sanity clamp on tGo (seconds)

inline bool Finite(float v) { return std::isfinite(v); }
inline bool Finite(const Vec3& v) { return Finite(v.x) && Finite(v.y) && Finite(v.z); }

// All four state vectors plus dt must be finite for the geometry to mean
// anything. NaN/inf from a torn game read must never reach the math.
inline bool InputFinite(const GuidanceInput& in) {
    return Finite(in.missilePos) && Finite(in.missileVel) &&
           Finite(in.targetPos) && Finite(in.targetVel) && Finite(in.dt);
}

// Great-circle (slerp) interpolation of direction a toward direction b.
//
//   w <= 0    -> a  bit-exactly, so HomingForesightAmount = 0 is *exactly* the
//                    vanilla pure-pursuit direction (no re-normalisation blur);
//   w >= 1    -> b  bit-exactly (full lead point);
//   0 < w < 1 -> rotate a toward b along their common great circle by
//                w * angle(a, b).
//
// Inputs are expected to be unit vectors. Degenerate inputs are tolerated:
// whichever endpoint is usable is returned.
Vec3 SlerpDir(const Vec3& a, const Vec3& b, float w) {
    if (!(w > 0.0f)) return a;     // also catches NaN: fall back to pursuit
    if (w >= 1.0f) return b;
    Vec3 ua, ub;
    const bool okA = Normalize(a, ua, kEps);
    const bool okB = Normalize(b, ub, kEps);
    if (!okA) return okB ? ub : Vec3();
    if (!okB) return ua;
    const float ang = AngleBetween(ua, ub);
    if (ang < 1e-6f) return ua;    // same direction, nothing to blend
    return RotateTowards(ua, ub, w * ang);
}

} // namespace

// ---------------------------------------------------------------------------
// LOS kinematics
// ---------------------------------------------------------------------------
void ComputeLos(const GuidanceInput& in, Vec3& losUnit, float& losRateRadPerSec,
                float& closingSpeed) {
    losUnit = Vec3();
    losRateRadPerSec = 0.0f;
    closingSpeed = 0.0f;

    if (!InputFinite(in)) return;

    const Vec3 r = in.targetPos - in.missilePos;      // missile -> target
    const float rangeSq = LengthSq(r);
    if (!Finite(rangeSq) || rangeSq < kEps * kEps) return; // no LOS at all

    const Vec3 vRel = in.targetVel - in.missileVel;   // relative velocity

    losUnit = r * (1.0f / std::sqrt(rangeSq));

    // LOS angular velocity: w = (r x v_rel) / |r|^2, rate = |w|.
    const Vec3 omega = Cross(r, vRel) * (1.0f / rangeSq);
    const float rate = Length(omega);
    if (Finite(rate)) losRateRadPerSec = rate;

    // Closing speed: positive while the range is shrinking.
    const float closing = Dot(in.missileVel - in.targetVel, losUnit);
    if (Finite(closing)) closingSpeed = closing;
}

// ---------------------------------------------------------------------------
// Pure pursuit
// ---------------------------------------------------------------------------
Vec3 PursuitDirection(const GuidanceInput& in) {
    if (!InputFinite(in)) return Vec3();
    Vec3 dir;
    if (!Normalize(in.targetPos - in.missilePos, dir, kEps)) return Vec3();
    return dir;
}

// ---------------------------------------------------------------------------
// Lead / intercept solution
// ---------------------------------------------------------------------------
Vec3 ProportionalNavigationDirection(const GuidanceInput& in, float lead,
                                     float& outTimeToGo) {
    outTimeToGo = 0.0f;
    if (!InputFinite(in)) return Vec3();

    const Vec3 r = in.targetPos - in.missilePos;
    const float range = Length(r);
    if (!Finite(range) || range < kEps) return Vec3();

    Vec3 losUnit;
    float losRate = 0.0f;
    float closing = 0.0f;
    ComputeLos(in, losUnit, losRate, closing);

    // No useful closing solution (receding, beam-on with zero closing, or a
    // garbage sample): fall back to vanilla pursuit, i.e. lead = 0 behaviour.
    if (!Finite(closing) || closing <= 1.0f) {
        // timeToGo stays 0: the caller must not trust a tGo we did not use.
        return PursuitDirection(in);
    }

    float tGo = range / closing;
    if (!Finite(tGo)) tGo = kMaxTimeToGo;
    tGo = Clampf(tGo, 0.0f, kMaxTimeToGo);
    outTimeToGo = tGo;

    if (!Finite(lead)) lead = 0.0f;
    const float w = Clampf(lead, 0.0f, 1.0f);

    // Collision-course point: where the target will be after (tGo * lead).
    const Vec3 leadPoint = in.targetPos + in.targetVel * (tGo * w);

    Vec3 dir;
    if (!Normalize(leadPoint - in.missilePos, dir, kEps)) {
        return PursuitDirection(in); // degenerate lead point: use pursuit
    }
    return dir;
}

// ---------------------------------------------------------------------------
// Classic PN rate law, N * lambda_dot
// ---------------------------------------------------------------------------
float PnRateCommand(const GuidanceInput& in, float navConstant) {
    if (!Finite(navConstant) || !InputFinite(in)) return 0.0f;

    const Vec3 r = in.targetPos - in.missilePos;
    const float rangeSq = LengthSq(r);
    if (!Finite(rangeSq) || rangeSq < kEps * kEps) return 0.0f;

    Vec3 heading;
    if (!Normalize(in.missileVel, heading, kEps)) return 0.0f; // no heading

    Vec3 losUnit;
    float losRate = 0.0f;
    float closing = 0.0f;
    ComputeLos(in, losUnit, losRate, closing);
    if (!(losRate > 0.0f)) return 0.0f;

    // Rotation direction needed to swing the heading onto the LOS is the axis
    // heading x losUnit. The commanded rate is positive (turn "up" around that
    // axis) when the LOS itself is rotating the same way.
    Vec3 axis;
    if (!Normalize(Cross(heading, losUnit), axis, kEps)) return 0.0f; // aligned
    const Vec3 omega = Cross(r, in.targetVel - in.missileVel) * (1.0f / rangeSq);
    const float sense = Dot(omega, axis);

    const float magnitude = navConstant * losRate;
    if (!Finite(magnitude)) return 0.0f;
    if (sense > 0.0f) return magnitude;
    if (sense < 0.0f) return -magnitude;
    return 0.0f;
}

// ---------------------------------------------------------------------------
// Full guidance pipeline
// ---------------------------------------------------------------------------
GuidanceResult ComputeGuidance(const GuidanceInput& in, const GuidanceParams& p) {
    GuidanceResult out;
    out.desiredDir = Vec3();
    out.valid = false;
    out.inFov = false;
    out.turnDeg = 0.0f;
    out.losAngleDeg = 0.0f;
    out.timeToGo = 0.0f;
    out.mode = GuidanceMode::Pursuit;

    // ---- 1. reject garbage up front (NaN/inf, negative dt) ----------------
    if (!InputFinite(in) || !(in.dt >= 0.0f) ||
        !Finite(p.homingForesightAmount) || !Finite(p.maxHomingAngleDeg) ||
        !Finite(p.maxRotationAngleDegPerSec) || !Finite(p.lifeTimeSec)) {
        return out; // valid = false, desiredDir = zero (no usable heading)
    }

    // ---- 2. current heading (cannot guide without one) ---------------------
    Vec3 heading;
    if (!Normalize(in.missileVel, heading, kEps)) return out; // zero velocity

    const Vec3 losVec = in.targetPos - in.missilePos;
    const float range = Length(losVec);
    if (!Finite(range) || range < kEps) return out; // target on top of us

    Vec3 losUnit;
    float losRate = 0.0f;
    float closing = 0.0f;
    ComputeLos(in, losUnit, losRate, closing);
    if (!Finite(losUnit) || LengthSq(losUnit) < 0.5f) return out; // paranoid

    // ---- 3. blend pursuit <-> intercept point ------------------------------
    const float lead = Clampf(p.homingForesightAmount, 0.0f, 1.0f);
    out.mode = (lead <= 0.0f) ? GuidanceMode::Pursuit
               : (lead >= 1.0f) ? GuidanceMode::ProportionalNavigation
                                : GuidanceMode::Blended;

    const Vec3 pursuitDir = PursuitDirection(in);                  // lead = 0
    const Vec3 leadDir = ProportionalNavigationDirection(in, 1.0f, out.timeToGo);
    const Vec3 idealDir = SlerpDir(pursuitDir, leadDir, lead);
    if (!Finite(idealDir)) return out; // cannot happen, but stay safe

    // ---- 4. seeker cone ----------------------------------------------------
    out.losAngleDeg = RadToDeg(AngleBetween(in.missileVel, losUnit));
    const bool alwaysInFov = p.maxHomingAngleDeg >= 359.9f;
    out.inFov = alwaysInFov || (out.losAngleDeg <= p.maxHomingAngleDeg);
    out.valid = true;

    // Guidance lost: the missile keeps flying ballistic (valid stays true so
    // the caller can keep the entity alive and log the loss, but we must not
    // rotate it).
    if (!out.inFov) {
        out.desiredDir = heading;
        out.turnDeg = 0.0f;
        return out;
    }

    // ---- 5. rate-limited turn toward the ideal direction -------------------
    const float angToIdeal = AngleBetween(heading, idealDir); // rad
    float maxTurnRad = 0.0f;
    if (p.maxRotationAngleDegPerSec > 0.0f) {
        maxTurnRad = DegToRad(p.maxRotationAngleDegPerSec) * in.dt;
    }
    const float turnRad = (maxTurnRad < angToIdeal) ? maxTurnRad : angToIdeal;
    out.turnDeg = RadToDeg(turnRad);

    // "No turn needed" is returned as the ideal direction bit-exactly; that
    // keeps lead = 0 identical to PursuitDirection() step for step.
    out.desiredDir = (turnRad >= angToIdeal) ? idealDir
                                             : RotateTowards(heading, idealDir, turnRad);

    if (!Finite(out.desiredDir) || LengthSq(out.desiredDir) < 0.5f) {
        out.desiredDir = heading; // last-resort: do not hand out a broken dir
        out.turnDeg = 0.0f;
    }
    return out;
}

} // namespace acz
