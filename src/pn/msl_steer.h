// ACZPropNav - plain-MSL proportional-navigation steer math (pure, header-only).
//
// This is the *only* new guidance math for the plain MSL.  It is deliberately
// free of OS / game / config dependencies so tools/pnsim can unit-test it
// directly (it links only src/pn/pn.cpp).
//
// Units: the game's plain-MSL slot stores its motion as a PER-FRAME DISPLACEMENT
// (slot+0x20..0x28), not a velocity -- and that displacement is in the game's own
// position units, NOT metres.  1 metre == 5.12 position units: the engine's range
// kill compares the travelled distance against `P+0x14 * 5.12` (the constant is
// 0x1402D46B0 == 5.1199999), and a live run logs |v| ~ 60.4 units/frame, which is
// ~11.8 m/frame ~ 707 m/s at 60 fps.
//
// Everything here therefore works in per-frame units and never needs a metres/second
// conversion.  The per-frame turn budget uses the game's OWN frame-rate constant
// (`*(u32*)(ctrl+0x6EC)`, 60 in this build), because that is precisely what the
// vanilla clamp does:
//     vanilla maxTurn = lead_deg * pi/180 / fps
// Using 1/fps sidesteps the position-unit conversion entirely.  (Deriving
// `dt = |v| / speedMps` was wrong: it mixed units and inflated the turn budget 3x.)
#pragma once

#include "common/acz_common.h"

namespace acz {
namespace msl_steer {

struct Input {
    Vec3 p;             // missile position (m)
    Vec3 v;             // missile per-frame displacement (m/frame)
    Vec3 target;        // target position (m)
    Vec3 targetVel;     // target per-frame velocity (m/frame), 0 if unknown
    float speedMps = 1000.f;              // nominal missile speed (u16 at P+0x44)
    float amount = 1.f;                   // HomingForesightAmount 0..1
    float maxRotationDegPerSec = 180.f;   // MaxRotationAngle; <=0 means 180
};

struct Output {
    Vec3 vNew;          // new per-frame displacement, |vNew| == |v|
    float range = 0.f;  // |target - p| (m)
    float closing = 0.f;  // closing speed (m/frame, after the clamp)
    float tGo = 0.f;    // frames to intercept (clamped to [0, 600])
    float theta = 0.f;     // rad, angle between the current and the desired direction
    float turnRad = 0.f;   // rad, turn actually applied (<= theta)
    float thetaMax = 0.f;  // rad, per-frame turn limit
    float dtGame = 0.f;    // s, recovered game frame time
    bool valid = false;
};

// Proportional navigation on a per-frame displacement vector:
//   1. closing speed vc = |v| - dot(uR, Vt), clamped to >= 0.2|v|,
//   2. tGo = range / vc clamped to [0, 600] frames,
//   3. aim point = T + Vt * tGo * amount (amount = 0 -> pure pursuit),
//   4. rotate the flight direction toward the aim direction, limited by
//      (MaxRotationAngle degrees/second) * dt_game, where dt_game = 1/gameFps
//      clamped to [1/240, 1/20] s,
//   5. keep the speed: vNew = |v| * rotated unit direction.
// Returns false (and leaves `out` zeroed except the measured fields) for
// degenerate geometry: |v| too small, target on top of the missile, non-finite
// inputs.
inline bool Compute(const Input& in, Output& out) {
    out = Output();
    const Vec3 R = in.target - in.p;
    const float range = Length(R);
    const float speed = Length(in.v);
    out.range = range;
    if (!(range > 1e-3f) || !(speed > 0.01f)) return false;
    if (!std::isfinite(in.p.x) || !std::isfinite(in.p.y) || !std::isfinite(in.p.z)) return false;
    if (!std::isfinite(in.v.x) || !std::isfinite(in.v.y) || !std::isfinite(in.v.z)) return false;
    if (!std::isfinite(in.target.x) || !std::isfinite(in.target.y) ||
        !std::isfinite(in.target.z)) {
        return false;
    }
    if (!std::isfinite(in.targetVel.x) || !std::isfinite(in.targetVel.y) ||
        !std::isfinite(in.targetVel.z)) {
        return false;
    }

    Vec3 uR;
    if (!Normalize(R, uR)) return false;

    // Closing speed per frame. A receding target must not produce a negative or
    // exploding tGo, hence the floor at 20 % of the missile's per-frame speed.
    float vc = speed - Dot(uR, in.targetVel);
    const float vcFloor = 0.2f * speed;
    if (vc < vcFloor) vc = vcFloor;
    out.closing = vc;

    out.tGo = Clampf(range / vc, 0.f, 600.f);

    const float amount = Clampf(in.amount, 0.f, 1.f);
    const Vec3 aim = in.target + in.targetVel * (out.tGo * amount);
    Vec3 d;
    if (!Normalize(aim - in.p, d)) return false;

    Vec3 f;
    if (!Normalize(in.v, f)) return false;
    float cosTheta = Dot(f, d);
    cosTheta = Clampf(cosTheta, -1.f, 1.f);
    out.theta = std::acos(cosTheta);

    // Recover the game frame time from the speed in m/frame and m/s, then limit
    // the turn to MaxRotationAngle * dt (same convention as the WeaponEx path).
    const float speedMps = (in.speedMps > 1.f) ? in.speedMps : 1.f;
    out.dtGame = Clampf(speed / speedMps, 1.f / 240.f, 1.f / 20.f);
    const float rotDegPerSec = (in.maxRotationDegPerSec > 0.1f) ? in.maxRotationDegPerSec : 180.f;
    out.thetaMax = rotDegPerSec * out.dtGame * 0.017453292519943295f;
    out.turnRad = (out.theta < out.thetaMax) ? out.theta : out.thetaMax;

    const Vec3 dir = RotateTowards(f, d, out.turnRad);
    out.vNew = dir * speed;
    if (!std::isfinite(out.vNew.x) || !std::isfinite(out.vNew.y) ||
        !std::isfinite(out.vNew.z)) {
        return false;
    }
    out.valid = true;
    return true;
}

// ---------------------------------------------------------------------------
// Step 1B orientation helpers (pure math, no game/Euler-convention assumption).
//
// The game's slot+0x10 is Euler angles; FUN_140202FA0(euler) builds the matrix and
// FUN_1402036D0(euler, rotvec) rotates it.  The matrix layout and the rotation
// frame are discovered at runtime by calibration, so these helpers only provide
// convention-agnostic primitives.
// ---------------------------------------------------------------------------

// Axis-angle vector rotating unit `from` toward unit `to`, magnitude clamped to
// [0, maxAngleRad].  Returns (0,0,0) for identical or antiparallel vectors (for
// antiparallel there is no unique axis) and for degenerate input.
inline Vec3 RotVecFromTo(const Vec3& from, const Vec3& to, float maxAngleRad) {
    Vec3 f, t;
    if (!Normalize(from, f) || !Normalize(to, t)) return Vec3();
    const Vec3 axisRaw = Cross(f, t);
    const float s = Length(axisRaw);
    const float c = Clampf(Dot(f, t), -1.f, 1.f);
    float angle = std::atan2(s, c);
    if (!(angle > 1e-6f)) return Vec3();
    if (maxAngleRad > 0.f && angle > maxAngleRad) angle = maxAngleRad;
    Vec3 axis;
    if (!Normalize(axisRaw, axis)) return Vec3();  // antiparallel
    return axis * angle;
}

// Direction candidate of a 4x4 float matrix: 0..2 = columns of the row-major
// interpretation (m[c], m[4+c], m[8+c]), 3..5 = its rows.  The calibration picks
// whichever candidate matches the missile heading; this covers both storage orders.
inline Vec3 MatrixAxis(const float m[16], int candidate) {
    if (candidate < 0 || candidate > 5) return Vec3();
    if (candidate < 3) return Vec3(m[candidate], m[4 + candidate], m[8 + candidate]);
    const int r = candidate - 3;
    return Vec3(m[3 * r], m[3 * r + 1], m[3 * r + 2]);
}

// Best forward-axis candidate for a heading (unit-vector dot product).  Returns the
// candidate index (0..5) or -1; `outScore` receives the best dot product.
inline int PickForwardAxis(const float m[16], const Vec3& heading, float* outScore) {
    if (outScore != nullptr) *outScore = -2.f;
    Vec3 h;
    if (!Normalize(heading, h)) return -1;
    int best = -1;
    float bestS = -2.f;
    for (int c = 0; c < 6; ++c) {
        Vec3 u;
        if (!Normalize(MatrixAxis(m, c), u)) continue;
        const float sc = Dot(u, h);
        if (sc > bestS) {
            bestS = sc;
            best = c;
        }
    }
    if (outScore != nullptr) *outScore = bestS;
    return best;
}

// World -> local: transpose of the 3x3 part (row-major assumption).  Used only for
// the local-frame rotation-vector variants; calibration rejects it if wrong.
inline Vec3 WorldToLocal(const float m[16], const Vec3& v) {
    return Vec3(m[0] * v.x + m[4] * v.y + m[8] * v.z,
                m[1] * v.x + m[5] * v.y + m[9] * v.z,
                m[2] * v.x + m[6] * v.y + m[10] * v.z);
}

// Rodrigues rotation of v by a rotation vector (axis * angle).  Reference
// implementation used by the offline tests.
inline Vec3 RotateByRotVec(const Vec3& v, const Vec3& rotvec) {
    const float ang = Length(rotvec);
    if (!(ang > 1e-6f)) return v;
    Vec3 axis;
    if (!Normalize(rotvec, axis)) return v;
    const float c = std::cos(ang), sn = std::sin(ang);
    return v * c + Cross(axis, v) * sn + axis * (Dot(axis, v) * (1.f - c));
}

// ---------------------------------------------------------------------------
// STEP 2: true proportional navigation (LOS-rate / lambda-dot), blended with
// one-frame pure pursuit by `amount` (the INI HomingForesightAmount):
//   amount = 0 -> pure pursuit (vanilla-like chase)
//   amount = 1 -> full PN with navigation constant N = 4
// Everything is in per-frame units (v and targetVel are per-frame displacements).
// ---------------------------------------------------------------------------

struct PnInput {
    Vec3 p;                 // missile position (m)
    Vec3 v;                 // missile per-frame displacement (m/frame)
    Vec3 target;            // target position (m)
    Vec3 targetVel;         // target per-frame velocity (m/frame)
    // The game's own frame rate, i.e. *(u32*)(ctrl+0x6EC) (a constant 60 in this build).
    // The vanilla guidance divides its deg/s turn clamp by exactly this number, so
    // using it makes our per-frame budget identical to the game's own convention.
    float gameFps = 60.f;
    float amount = 1.f;                  // 0 = pursuit, 1 = PN (clamped to [0,1])
    float maxRotationDegPerSec = 180.f;  // turn limit; <=0 means 180
    float navConstant = 4.0f;            // PN N (exposed for the monotonicity tests)
};

struct PnOutput {
    Vec3 vNew;          // new per-frame displacement, |vNew| == |v|
    float range = 0.f;
    float theta = 0.f;      // angle between the flight direction and the LOS
    float turnRad = 0.f;    // applied turn
    float thetaMax = 0.f;   // per-frame turn limit
    float dtGame = 0.f;     // recovered game frame time (s)
    float lambdaDot = 0.f;  // |Cross(R,Vr)| / r^2  (rad/frame)
    float omegaPn = 0.f;    // |omegaPN| = N * lambdaDot (rad/frame)
    float omegaPursuit = 0.f;  // |omegaP| (rad/frame)
    float blend = 1.f;         // clamped amount actually used
    bool valid = false;
};

// Rotate a unit vector about a unit axis by `angle` rad (Rodrigues).
inline Vec3 RotateAboutAxis(const Vec3& v, const Vec3& axisUnit, float angle) {
    if (!(std::fabs(angle) > 1e-9f)) return v;
    const float c = std::cos(angle), s = std::sin(angle);
    return v * c + Cross(axisUnit, v) * s + axisUnit * (Dot(axisUnit, v) * (1.f - c));
}

inline bool ComputePn(const PnInput& in, PnOutput& out) {
    out = PnOutput();
    const Vec3 R = in.target - in.p;
    const float r = Length(R);
    const float speed = Length(in.v);
    out.range = r;
    if (!(r > 0.05f) || !(speed > 1e-3f)) return false;
    if (!std::isfinite(r) || !std::isfinite(speed)) return false;
    if (!std::isfinite(in.p.x) || !std::isfinite(in.p.y) || !std::isfinite(in.p.z) ||
        !std::isfinite(in.v.x) || !std::isfinite(in.v.y) || !std::isfinite(in.v.z) ||
        !std::isfinite(in.target.x) || !std::isfinite(in.target.y) ||
        !std::isfinite(in.target.z) || !std::isfinite(in.targetVel.x) ||
        !std::isfinite(in.targetVel.y) || !std::isfinite(in.targetVel.z)) {
        return false;
    }

    const Vec3 uV = in.v * (1.f / speed);
    const Vec3 Vr = in.targetVel - in.v;
    const Vec3 uR = R * (1.f / r);

    // Soft singularity guard: below rmin the LOS rate would explode; clamping r^2
    // replaces the old 1200 m / 250 m terminal hardcodes from Step 1.
    const float rmin = (2.0f * speed > 25.0f) ? (2.0f * speed) : 25.0f;
    const float r2 = (r * r > rmin * rmin) ? (r * r) : (rmin * rmin);

    const Vec3 lambdaVec = Cross(R, Vr) * (1.f / r2);  // rad/frame vector
    out.lambdaDot = Length(lambdaVec);

    const float N = (in.navConstant > 0.f) ? in.navConstant : 4.0f;
    const Vec3 omegaPN = lambdaVec * N;
    out.omegaPn = Length(omegaPN);

    // One-frame pure-pursuit rotation: rotate the heading onto the LOS.
    const float thetaP = AngleBetween(uV, uR);  // [0, pi]
    out.theta = thetaP;
    Vec3 omegaP;
    {
        const Vec3 axisRaw = Cross(uV, uR);
        Vec3 axis;
        if (Normalize(axisRaw, axis)) omegaP = axis * thetaP;
    }
    out.omegaPursuit = Length(omegaP);

    // Blend pure pursuit <-> proportional navigation by interpolating the two
    // RESULTING DIRECTIONS, not the two rotation-rate vectors.
    //
    // Two earlier formulations in this file were wrong:
    //   (1) `omegaCmd = omegaP*(1-a) + omegaPN*a` - a linear average of two rotation
    //       vectors. The pursuit axis (uV x uR) and the PN axis (R x Vr) are usually
    //       NOT collinear (median angle ~45 deg over a 44k-geometry sweep, >120 deg in
    //       ~5% of cases), so a weighted sum of opposing vectors nearly cancels: at
    //       bearing 5 deg / target heading 285 deg / R=500 m / Vt=750 m/s it commanded
    //       0.012 deg/frame at amount=0.5 versus 3.0 deg at BOTH endpoints - the
    //       missile flew straight at exactly the value the shipped INI uses (0.7).
    //   (2) composing the two partial rotations in sequence. That cures the
    //       instantaneous sum but not the underlying disagreement: when the laws pull
    //       in opposite directions the missile rotates out and then straight back,
    //       so the NET swept angle is still ~0 (verified: amount=0.5 still gave 0).
    //
    // Interpolating the directions fixes both, because a great-circle interpolation
    // between two unit vectors always lands *between* them:
    //   a = 0 -> exactly the pursuit direction   (vanilla-style chase, mod's turn cap)
    //   a = 1 -> exactly the PN direction        (N * lambda-dot, N = PnGain)
    //   in between -> a genuine intermediate heading, monotone in `a`.
    // Each endpoint is individually clamped to the per-frame turn budget thetaMax, so
    // their mutual angle is <= 2*thetaMax and any point between them is a legal frame.
    const float a = Clampf(in.amount, 0.f, 1.f);
    out.blend = a;

    // Frame time comes straight from the game's frame-rate constant (1/fps), which is
    // exactly what the vanilla clamp does: maxTurn = lead_deg*pi/180 / *(u32*)(ctrl+0x6EC).
    //
    // Do NOT derive it as |v| / speedMps from the displacement. The slot positions are
    // stored in the game's own position units, NOT metres: the range kill proves
    // 1 m == 5.12 units (P+0x14 * 5.12 compared against the travelled distance, and
    // 0x1402D46B0 == 5.1199999). A live log shows |v| ~ 60.4 units/frame, i.e.
    // ~11.8 m/frame ~ 707 m/s at 60 fps, so the naive division gave 60.4/1000 = 0.0604 s,
    // saturated the [1/240, 1/20] clamp at 0.05 s and inflated the per-frame turn budget
    // by 3x (a 90 deg/s setting behaved like 270 deg/s: the log printed limit=4.50deg
    // where the correct value is 1.50deg).
    const float fps = (in.gameFps > 1.f && in.gameFps < 1000.f) ? in.gameFps : 60.f;
    out.dtGame = 1.f / fps;
    const float rotDeg = (in.maxRotationDegPerSec > 0.1f) ? in.maxRotationDegPerSec : 180.f;
    out.thetaMax = rotDeg * out.dtGame * 0.017453292519943295f;

    // Endpoint A: pure pursuit, clamped to the turn budget.
    Vec3 dirPursuit = uV;
    if (out.omegaPursuit > 1e-9f) {
        Vec3 axisP;
        if (Normalize(omegaP, axisP)) {
            const float t = (out.omegaPursuit < out.thetaMax) ? out.omegaPursuit : out.thetaMax;
            dirPursuit = RotateAboutAxis(uV, axisP, t);
        }
    }

    // Endpoint B: proportional navigation (turn about the LOS-rate axis), clamped.
    Vec3 dirPn = uV;
    if (out.omegaPn > 1e-9f) {
        Vec3 axisPn;
        if (Normalize(omegaPN, axisPn)) {
            const float t = (out.omegaPn < out.thetaMax) ? out.omegaPn : out.thetaMax;
            dirPn = RotateAboutAxis(uV, axisPn, t);
        }
    }

    // Blend along the great circle between the two endpoints.
    Vec3 dir = dirPursuit;
    if (a > 1e-6f) {
        const float span = AngleBetween(dirPursuit, dirPn);
        dir = (span < 1e-9f) ? dirPn : RotateTowards(dirPursuit, dirPn, a * span);
    }
    out.turnRad = AngleBetween(uV, dir);   // net swept angle this frame

    // Keep the direction numerically orthonormal after two composed rotations.
    Vec3 dirN;
    if (Normalize(dir, dirN)) dir = dirN;
    out.vNew = dir * speed;
    out.valid = std::isfinite(out.vNew.x) && std::isfinite(out.vNew.y) &&
                std::isfinite(out.vNew.z);
    return out.valid;
}


} // namespace msl_steer
} // namespace acz
