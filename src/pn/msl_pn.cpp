// ACZPropNav - plain-MSL PN hook implementation (see pn/msl_pn.h for the design).
//
// ABI: FUN_14013ECA0 takes the world-level weapon manager in rcx and returns a
// value the only caller (0x1415DD) ignores, so a plain `void Detour(void* ctrl)`
// is safe.  The 15-byte prologue is
//   48 8B C4              mov rax, rsp
//   48 89 58 08           mov [rax+0x8], rbx
//   48 89 70 18           mov [rax+0x18], rsi
//   48 89 78 20           mov [rax+0x20], rdi
// = four instructions, no RIP-relative operand, ending exactly at the instruction
// boundary 0x13ECAF (the following `push rbp` is NOT stolen).
//
// Model facts that matter for the steering (verified statically by re-msl, and
// matching the Run B hardware watch): ctrl is the WORLD weapon manager
// (world+0x6A130), called once per frame; slot = ctrl+0x158+i*0x58; the game
// integrates the missile by adding its per-frame displacement (slot+0x20..0x2C) to
// slot+0x00..0x0C and writes that displacement right before our post-update pass.
//
// STEP 1B (this file): we take over the slot ORIENTATION as well.
//   * vGame is read straight from slot+0x20..0x28 (the game's own step for this
//     frame: length = step, direction = post-integration heading).  There is no
//     cross-frame position cache any more.
//   * the only per-slot state is `dir` = the unit heading WE commanded last frame,
//     used as the guidance basis (`dir * |vGame|`), so the PN rotation accumulates
//     instead of resetting to the game's heading every frame.
//   * slot+0x10 is Euler angles (3 floats + 4th), not a quaternion.  We rotate them
//     with the game's own FUN_1402036D0(out, in, rotvec) by the rotvec taking the
//     game's heading to the PN direction, and we discover the matrix forward-axis
//     convention + rotation-vector frame once per run by calibration on scratch
//     copies (FUN_140202FA0 builds the matrix).  If the calibration is ambiguous the
//     orientation writes are disabled for the run (position steering still works).
#include "pn/msl_pn.h"

#include <windows.h>

#include <cmath>
#include <cstring>

#include "core/log.h"
#include "hook/hook.h"
#include "mem/mem.h"
#include "pn/msl_steer.h"

namespace acz {
namespace msl_pn {
namespace {

#if defined(_MSC_VER)
#define ACZ_NOINLINE __declspec(noinline)
#else
#define ACZ_NOINLINE __attribute__((noinline))
#endif

constexpr uint32_t kRvaGuidance = 0x13ECA0;      // FUN_14013ECA0: per-frame plain-MSL guidance
constexpr uint32_t kRvaWorldGlobal = 0x3409F8;   // *(void**) = world / mission control
constexpr uint32_t kRvaPlayerIdx = 0x3409F2;     // u8 player object index

// VERIFIED against work/acz_copy.exe.unpacked.exe (and asserted by hooktest Test8).
const uint8_t kPrologue[15] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48,
                               0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20};

double NowQpc() {
    LARGE_INTEGER f{}, c{};
    static double freq = 0.0;
    if (freq == 0.0) {
        ::QueryPerformanceFrequency(&f);
        freq = (f.QuadPart > 0) ? static_cast<double>(f.QuadPart) : 1.0;
    }
    ::QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / freq;
}

inline LONG FBits(float f) {
    LONG out = 0;
    std::memcpy(&out, &f, 4);
    return out;
}
inline float BitsF(LONG v) {
    float out = 0.f;
    std::memcpy(&out, &v, 4);
    return out;
}

// ---- state shared between Patcher (Tick) and the game thread (detour) --------
struct TargetCacheEntry {
    uint32_t key = 0xFFFFFFFFu;  // (tObj << 8) | tPart
    uint32_t frame = 0;          // frame the cached position was sampled
    uint32_t lastFrame = 0;      // frame this entry was last used (LRU eviction)
    Vec3 pos;
    Vec3 vel;        // per-frame target velocity (raw, this frame)
    Vec3 velSmooth;  // low-pass smoothed target velocity (used for the lead)
    bool velSmoothValid = false;
    bool valid = false;
};

struct SlotCacheEntry {
    // Key = the slot ROW index (0..15), NOT the owner (obj,part): two missiles fired
    // from one aircraft share owner (obj,part) = (playerIdx, rootPart) (the launcher
    // writes slot+0x44/0x45 from part+0x234/0x235 of the fire record's part), so an
    // owner-keyed entry collided between the two live slots: the first slot read the
    // second's `dir` as its guidance basis while the second was permanently
    // non-continuous ("fast circling" of one of two missiles, 2026-10-27 report).
    // The frame-continuity guard handles a row being re-used by a later missile.
    uint32_t key = 0xFFFFFFFFu;  // slot row index i (0..15)
    uint32_t frame = 0;
    Vec3 dir;                  // unit heading WE commanded last frame (guidance basis)
    float amountSmooth = 0.f;  // low-passed effective lead factor
    bool valid = false;
};

constexpr int kTargetCache = 64;
constexpr int kSlotCache = 64;

struct Detail {
    volatile LONG ready = 0;
    volatile LONG logged = 0;
    volatile LONG tObj = 0;
    volatile LONG tPart = 0;
    volatile LONG amountBits = 0;
    volatile LONG rangeBits = 0;
    volatile LONG thetaBits = 0;
    volatile LONG thetaMaxBits = 0;
    volatile LONG speedBits = 0;
    volatile LONG vn0 = 0, vn1 = 0, vn2 = 0;
    volatile LONG lambdaBits = 0;   // |lambdaDot| rad/frame
    volatile LONG omegaPnBits = 0;  // N * |lambdaDot|
    volatile LONG omegaPBits = 0;   // one-frame pure-pursuit demand
    volatile LONG turnBits = 0;     // applied turn (rad)
};
constexpr int kDetails = 16;

// One-shot Step 1B convention-calibration result (written by the detour, logged by Tick).
struct CalibLog {
    volatile LONG ready = 0;
    volatile LONG logged = 0;
    volatile LONG axis = -1;     // best forward-axis candidate, -1 none
    volatile LONG variant = -1;  // winning rotvec variant, -1 none
    volatile LONG ok = 0;
    volatile LONG baseBits = 0;  // dot(heading, d) of the deciding attempt
    volatile LONG s0 = 0, s1 = 0, s2 = 0, s3 = 0;   // variant scores (float bits)
    volatile LONG a0 = 0, a1 = 0, a2 = 0, a3 = 0;   // variant ACHIEVED turn angle (float bits)
    volatile LONG reqBits = 0;                      // commanded turn angle (float bits)
    volatile LONG axisDot[6] = {};                   // six axis candidate dots (float bits)
    volatile LONG attempts = 0;
    volatile LONG tooSmall = 0;   // frames skipped because the commanded turn was tiny
    // raw heading / desired direction of the last attempt, for the failure log
    volatile LONG hx = 0, hy = 0, hz = 0;
    volatile LONG dx = 0, dy = 0, dz = 0;
};

// One-shot diagnostic: the first few resolved player-missile targets, so a run
// that produces no corrections shows exactly what we resolved (target pair, part
// flag byte, and the two positions) instead of leaving it to guesswork.
struct TgtDebug {
    volatile LONG tObj = 0, tPart = 0, partFlags = 0;
    volatile LONG px = 0, py = 0, pz = 0;
    volatile LONG tx = 0, ty = 0, tz = 0;
};
constexpr int kTgtDebug = 8;

struct State {
    bool wanted = false;
    bool hooked = false;
    bool gaveUp = false;
    bool dryRun = false;
    float pnGain = 4.0f;  // navigation constant N for the true-PN law
    float terminalZone = 0.f;  // [runtime] TerminalZone: 0 = PN owns the whole flight
    float gameFps = 60.f;      // *(u32*)(ctrl+0x6EC): the engine's own frame rate
    uintptr_t base = 0;
    const FieldSet* playerFs = nullptr;
    const FieldSet* enemyFs = nullptr;

    void* target = nullptr;
    hook::InstallResult install;

    double lastTry = 0.0;
    int tries = 0;
    double lastSummary = 0.0;

    // One Apply() call == one game frame; target/slot deltas are per frame.
    uint32_t frameId = 0;
    double lastApply = 0.0;

    // counters (detour side, Interlocked)
    volatile LONG64 calls = 0;
    volatile LONG64 slots = 0;
    volatile LONG64 playerSlots = 0;
    volatile LONG64 enemySlots = 0;
    volatile LONG64 computed = 0;
    volatile LONG64 applied = 0;
    volatile LONG64 badTarget = 0;
    volatile LONG64 noTarget = 0;      // slot has 0xFF (no target) - normal, not an error
    volatile LONG64 badTobj = 0;
    volatile LONG64 badObjArray = 0;
    volatile LONG64 badObjPtr = 0;
    volatile LONG64 badParts = 0;
    volatile LONG64 badPartRead = 0;
    volatile LONG objCountSeen = 0;
    volatile LONG64 badGeometry = 0;
    volatile LONG64 writeFails = 0;
    volatile LONG64 slotKeyCollisions = 0;  // same owner (obj,part) seen on 2+ live slots in one Apply()
    volatile LONG64 quietTerminal = 0;      // frames inside TerminalZone (diag; 0 unless opted in)
    volatile LONG64 outOfCone = 0;          // frames where the seeker cone refused the write
    volatile LONG64 staleDirResets = 0;     // cached heading disagreed with the game (row reused)
    volatile LONG64 fastCallbacks = 0;      // Apply() entered faster than the frame heuristic
    volatile LONG fastWarned = 0;
    volatile LONG maxCorrMilliDeg = 0;

    TargetCacheEntry cache[kTargetCache];
    SlotCacheEntry slotCache[kSlotCache];
    Detail details[kDetails];
    volatile LONG detailWritten = 0;
    volatile LONG detailOverflow = 0;   // sticky: set once, never re-armed
    volatile LONG detailOverflowSeen = 0;

    TgtDebug tgtDbg[kTgtDebug];
    volatile LONG tgtDbgWritten = 0;
    volatile LONG tgtDbgLogged = 0;

    // ---- Step 1B: Euler orientation takeover + one-shot convention calibration ----
    bool helpersChecked = false;
    bool helpersOk = false;
    bool calibDone = false;
    bool calibOk = false;
    int calibAxis = -1;     // 0..2 row-major columns, 3..5 rows (see msl_steer::MatrixAxis)
    int calibVariant = -1;  // 0 world, 1 -world, 2 local-frame, 3 -local-frame
    int calibAttempts = 0;
    volatile LONG calibTooSmall = 0;
    double calibStart = 0.0;
    volatile LONG64 orientWrites = 0;
    volatile LONG64 orientFails = 0;
    CalibLog calibLog;

    // Step 2 lambda statistics (micro-rad/frame, for the summary)
    volatile LONG64 lambdaSumMicro = 0;  // sum of |lambdaDot| in micro-rad/frame
    volatile LONG lambdaMaxMicro = 0;

    // guidance-liveness watchdog (Tick side only)
    LONG64 lastSeenCalls = 0;
    double lastProgress = 0.0;
    bool stallLogged = false;
};

State g;

// ---- detour ---------------------------------------------------------------
typedef void (*GuidanceFn)(void* ctrl);
void* volatile g_tramp = nullptr;

void* WaitTramp() {
    void* t = g_tramp;
    for (int i = 0; i < 100 && t == nullptr; ++i) {
        ::SwitchToThread();
        t = g_tramp;
    }
    while (t == nullptr) {
        ::SwitchToThread();
        t = g_tramp;
    }
    return t;
}

void Apply(void* ctrl);  // forward declaration

ACZ_NOINLINE void MslPnDetour(void* ctrl) {
    ::InterlockedIncrement64(&g.calls);
    GuidanceFn f = reinterpret_cast<GuidanceFn>(WaitTramp());
    f(ctrl);      // vanilla integration / lifetime / seeker / target bookkeeping
    Apply(ctrl);  // then the PN override on the live slots
}

// ---- caches ---------------------------------------------------------------
TargetCacheEntry* CacheFind(uint32_t key) {
    TargetCacheEntry* freeEntry = nullptr;
    TargetCacheEntry* lru = nullptr;
    for (int i = 0; i < kTargetCache; ++i) {
        TargetCacheEntry& e = g.cache[i];
        if (e.valid && e.key == key) return &e;
        if (!e.valid) {
            if (freeEntry == nullptr) freeEntry = &e;
        } else if (lru == nullptr || e.lastFrame < lru->lastFrame) {
            // A big mission can easily have more than kTargetCache distinct
            // (tObj,tPart) pairs alive (enemies + ground targets + per-part targets).
            // Returning nullptr once the table filled up silently dropped the target
            // velocity estimate for every newly locked target, degrading it to a
            // no-lead intercept - so evict the least recently used entry instead.
            lru = &e;
        }
    }
    if (freeEntry == nullptr) freeEntry = lru;   // table full: recycle the stalest entry
    if (freeEntry != nullptr) {
        freeEntry->valid = false;  // no previous position yet
        freeEntry->key = key;
        freeEntry->frame = 0;
        freeEntry->lastFrame = g.frameId;
        freeEntry->vel = Vec3();
        freeEntry->velSmooth = Vec3();
        freeEntry->velSmoothValid = false;
        return freeEntry;
    }
    return nullptr;
}

SlotCacheEntry* SlotCacheFind(uint32_t key) {
    SlotCacheEntry* freeEntry = nullptr;
    for (int i = 0; i < kSlotCache; ++i) {
        SlotCacheEntry& e = g.slotCache[i];
        if (e.valid && e.key == key) return &e;
        if (!e.valid && freeEntry == nullptr) freeEntry = &e;
    }
    if (freeEntry != nullptr) {
        freeEntry->valid = false;  // no previous heading yet
        freeEntry->key = key;
        freeEntry->frame = 0;
        freeEntry->dir = Vec3();
        return freeEntry;
    }
    return nullptr;
}

void RecordDetail(int tObj, int tPart, float amount, const msl_steer::PnOutput& out, float speed) {
    const LONG idx = ::InterlockedIncrement(&g.detailWritten) - 1;
    if (idx < 0 || idx >= kDetails) {
        // Sticky one-shot: only the FIRST overflow is recorded. Clearing this from
        // Tick and re-setting it here made the notice repeat every 2 s forever.
        ::InterlockedCompareExchange(&g.detailOverflow, 1, 0);
        return;
    }
    Detail& d = g.details[idx];
    d.tObj = tObj;
    d.tPart = tPart;
    d.amountBits = FBits(amount);
    d.rangeBits = FBits(out.range);
    d.thetaBits = FBits(out.theta);
    d.thetaMaxBits = FBits(out.thetaMax);
    d.speedBits = FBits(speed);
    d.lambdaBits = FBits(out.lambdaDot);
    d.omegaPnBits = FBits(out.omegaPn);
    d.omegaPBits = FBits(out.omegaPursuit);
    d.turnBits = FBits(out.turnRad);
    d.vn0 = FBits(out.vNew.x);
    d.vn1 = FBits(out.vNew.y);
    d.vn2 = FBits(out.vNew.z);
    ::InterlockedExchange(&d.ready, 1);
}

// ---- Step 1B: Euler orientation helpers (called from Apply on the game thread) ----
typedef void (*EulerToMatrixFn)(float* mat16, const float* euler);
typedef void (*RotateEulerFn)(float* outEuler, const float* inEuler, const float* rotvec);

inline void CallEulerToMatrix(float* mat, const float* euler) {
    reinterpret_cast<EulerToMatrixFn>(g.base + kRvaEulerToMatrix)(mat, euler);
}
inline void CallRotateEuler(float* outE, const float* inE, const float* rotvec) {
    reinterpret_cast<RotateEulerFn>(g.base + kRvaRotateEuler)(outE, inE, rotvec);
}

// Byte-check both game helpers once (same idea as the hook prologue check).  A
// mismatch on a different build disables only the orientation path.
bool EnsureHelpers() {
    if (g.helpersChecked) return g.helpersOk;
    g.helpersChecked = true;
    uint8_t mat[sizeof(kEulerToMatrixHead)] = {};
    uint8_t rot[sizeof(kRotateEulerHead)] = {};
    g.helpersOk = mem::ReadBytes(g.base + kRvaEulerToMatrix, mat, sizeof(mat)) &&
                  std::memcmp(mat, kEulerToMatrixHead, sizeof(mat)) == 0 &&
                  mem::ReadBytes(g.base + kRvaRotateEuler, rot, sizeof(rot)) &&
                  std::memcmp(rot, kRotateEulerHead, sizeof(rot)) == 0;
    return g.helpersOk;
}

// Orientation-convention calibration, retried until the commanded turn is large
// enough to be distinguishable.  Runs on scratch Euler copies only (never writes a
// live missile) and publishes the deciding attempt for Tick to log.
//
// Step-1B bug being fixed here: the old code calibrated once, on the first
// correction, which could be a ~0.6 deg turn - all variant scores were then
// indistinguishable, orientation was disabled permanently and nothing retried.
bool CalibAttempt(uintptr_t slot, const Vec3& heading, const Vec3& d) {
    CalibLog& lg = g.calibLog;
    lg.attempts = g.calibAttempts;
    lg.tooSmall = g.calibTooSmall;
    lg.hx = FBits(heading.x);
    lg.hy = FBits(heading.y);
    lg.hz = FBits(heading.z);
    lg.dx = FBits(d.x);
    lg.dy = FBits(d.y);
    lg.dz = FBits(d.z);
    if (!EnsureHelpers()) {
        lg.axis = -1;
        lg.variant = -1;
        lg.ok = 0;
        return false;
    }
    float euler[4] = {};
    if (!mem::ReadBytes(slot + 0x10, euler, sizeof(euler))) return false;
    float mat[16] = {};
    CallEulerToMatrix(mat, euler);

    // Six forward-axis candidates: 0..2 = row-major columns, 3..5 = rows.
    // NOTE: the game integrates position along MINUS the quat-derived axis, so the
    // correct candidate usually has a NEGATIVE dot with the motion direction.  Match
    // on |dot| and carry the sign into the scoring below (v1 calibration bug: the best
    // candidate was missed because only positive dots were considered).
    float axisDots[6] = {-2.f, -2.f, -2.f, -2.f, -2.f, -2.f};
    int axis = -1;
    float bestAbsDot = -2.f;
    float axisSign = 1.f;
    for (int c = 0; c < 6; ++c) {
        Vec3 u;
        if (!Normalize(msl_steer::MatrixAxis(mat, c), u)) continue;
        axisDots[c] = Dot(u, heading);
        const float a = std::fabs(axisDots[c]);
        if (a > bestAbsDot) {
            bestAbsDot = a;
            axis = c;
            axisSign = (axisDots[c] >= 0.f) ? 1.f : -1.f;
        }
    }
    const float base = Dot(heading, d);
    lg.baseBits = FBits(base);
    lg.reqBits = FBits(AngleBetween(heading, d));
    for (int c = 0; c < 6; ++c) lg.axisDot[c] = FBits(axisDots[c]);
    lg.axis = axis;
    if (axis < 0 || bestAbsDot < 0.5f) {
        lg.variant = -1;
        lg.ok = 0;
        return false;  // retry: matrix convention not recognisable at this sample
    }

    const Vec3 world = msl_steer::RotVecFromTo(heading, d, 1.57079632679f);
    float scores[4] = {-2.f, -2.f, -2.f, -2.f};
    float achieved[4] = {0.f, 0.f, 0.f, 0.f};
    for (int v = 0; v < 4; ++v) {
        Vec3 rv = world;
        if (v >= 2) rv = msl_steer::WorldToLocal(mat, rv);
        if (v == 1 || v == 3) rv = rv * -1.f;
        if (!(Length(rv) > 1e-6f)) {
            scores[v] = base;
            continue;
        }
        float outE[4] = {euler[0], euler[1], euler[2], euler[3]};
        const float rvv[3] = {rv.x, rv.y, rv.z};
        CallRotateEuler(outE, euler, rvv);
        float mat2[16] = {};
        CallEulerToMatrix(mat2, outE);
        Vec3 fwd;
        if (!Normalize(msl_steer::MatrixAxis(mat2, axis), fwd)) continue;
        scores[v] = Dot(fwd * axisSign, d);
        // How far did this variant actually rotate the forward axis? If this is much
        // larger than the commanded turn, the rotvec magnitude convention is wrong
        // (e.g. axis-angle vs delta-euler), which is exactly what a live run showed:
        // a 5.7 deg command produced 18-23 deg of change in every variant.
        achieved[v] = AngleBetween(heading, fwd * axisSign);
    }
    int best = 0;
    for (int v = 1; v < 4; ++v) {
        if (scores[v] > scores[best]) best = v;
    }
    lg.variant = best;
    lg.s0 = FBits(scores[0]);
    lg.s1 = FBits(scores[1]);
    lg.s2 = FBits(scores[2]);
    lg.s3 = FBits(scores[3]);
    lg.a0 = FBits(achieved[0]);
    lg.a1 = FBits(achieved[1]);
    lg.a2 = FBits(achieved[2]);
    lg.a3 = FBits(achieved[3]);
    const bool ok = (scores[best] > base + 1e-4f);
    lg.ok = ok ? 1 : 0;
    if (!ok) return false;  // retry: not distinguishable at this commanded angle
    g.calibAxis = axis;
    g.calibVariant = best;
    return true;
}

void PublishCalibration(bool ok) {
    CalibLog& lg = g.calibLog;
    lg.ok = ok ? 1 : 0;
    lg.attempts = g.calibAttempts;
    lg.tooSmall = g.calibTooSmall;
    ::InterlockedExchange(&lg.ready, 1);
}

// Called once per frame until calibrated.  Only attempts when the commanded turn is
// meaningful (> 0.02 rad); gives up (orientation disabled, position steering still
// active) after 60 attempts or 6 s.
void CalibrateTick(uintptr_t slot, const Vec3& heading, const Vec3& d, double now) {
    if (g.calibDone) return;
    if (g.calibStart == 0.0) g.calibStart = now;
    // A distinguishing sample needs a REAL heading change: a live run showed the old
    // 0.02 rad (1.15 deg) gate accepting ~5.7 deg samples and burning all 60 attempts
    // on them. At that magnitude every one of the four rotation conventions scores
    // 0.92..0.95 against the target while simply NOT rotating scores 0.995, so all
    // variants look "worse than doing nothing" and the calibration can never pass
    // (observed: ok=0, attempts=60, tooSmall=52).
    //
    // Snapping the gate up to a big angle is not an option either: the per-frame turn
    // is clamped by MaxRotationAngle/fps (~1.5 deg at 90 deg/s), so no single frame
    // ever produces a large heading change. Instead we accumulate frames: keep testing
    // on every frame but only spend the attempt budget once the commanded turn has
    // converged (consecutive frames agree), which is when the sample is stable and the
    // variants are maximally separated.
    const float dTheta = AngleBetween(heading, d);
    if (dTheta > 0.0005f) {
        ++g.calibAttempts;
        if (CalibAttempt(slot, heading, d)) {
            g.calibDone = true;
            g.calibOk = true;
            g.calibLog.tooSmall = g.calibTooSmall;
            PublishCalibration(true);
            return;
        }
    } else {
        ::InterlockedIncrement(&g.calibTooSmall);
        g.calibLog.tooSmall = g.calibTooSmall;
    }
    // Give up only after real OPPORTUNITIES, not on a wall clock. The old rule
    // ("6 s from the first frame with a live slot") disabled the Euler takeover for
    // the whole session whenever the first launches happened to be head-on (heading
    // error <= 0.02 rad): those frames are counted as tooSmall, so 6 s could elapse
    // with zero usable samples and the calibration would never be retried. Now the
    // attempt budget is what limits us, and a period with no live missiles resets
    // the clock so a later launch gets a fresh chance.
    if (g.calibAttempts >= 60) {
        g.calibDone = true;
        g.calibOk = false;
        g.calibAxis = -1;
        g.calibVariant = -1;
        PublishCalibration(false);
    } else if ((now - g.calibStart) > 6.0) {
        // No usable opportunity yet - restart the window instead of giving up.
        g.calibStart = now;
    }
}

// Rotate the slot's Euler angles so the game's heading follows the PN direction.
void SyncOrientation(uintptr_t slot, const Vec3& heading, const Vec3& d) {
    Vec3 rv = msl_steer::RotVecFromTo(heading, d, 1.57079632679f);
    if (!(Length(rv) > 1e-4f)) return;  // already aligned: nothing to do
    float euler[4] = {};
    if (!mem::ReadBytes(slot + 0x10, euler, sizeof(euler))) return;
    if (g.calibVariant >= 2) {
        float mat[16] = {};
        CallEulerToMatrix(mat, euler);
        rv = msl_steer::WorldToLocal(mat, rv);
    }
    if (g.calibVariant == 1 || g.calibVariant == 3) rv = rv * -1.f;
    if (!(Length(rv) > 1e-6f)) return;
    float outE[4] = {euler[0], euler[1], euler[2], euler[3]};
    const float rvv[3] = {rv.x, rv.y, rv.z};
    CallRotateEuler(outE, euler, rvv);
    if (mem::WriteVec3(slot + 0x10, Vec3(outE[0], outE[1], outE[2]))) {
        ::InterlockedIncrement64(&g.orientWrites);
    } else {
        ::InterlockedIncrement64(&g.orientFails);
    }
}

// ---- the steering pass (game thread, once per frame) ----------------------
void Apply(void* ctrl) {
    const uintptr_t c = reinterpret_cast<uintptr_t>(ctrl);
    if (c < 0x10000 || !g.wanted) return;

    // Frame identification. FUN_14013ECA0 is called once per game frame, so a new
    // call after a >3 ms gap starts a new frame id. Above ~333 calls/s this
    // heuristic under-counts: consecutive calls would share a frameId, the
    // frame-continuity guard would then treat every frame as discontinuous (guidance
    // basis silently reverts to the game's heading) and target velocity would stall.
    // Detect that case and report it once instead of degrading in silence.
    const double now = NowQpc();
    if (g.lastApply != 0.0) {
        const double dtCall = now - g.lastApply;
        if (dtCall > 0.003) {
            ++g.frameId;
        } else if (dtCall > 0.0 && dtCall < 1.0 / 330.0) {
            ::InterlockedIncrement64(&g.fastCallbacks);
        }
    }
    g.lastApply = now;

    uint32_t mask = 0;
    if (!mem::Read<uint32_t>(c + 0x6E8, mask) || mask == 0) return;

    // The game's own frame-rate constant. The vanilla guidance clamps its deg/s turn
    // rate with `lead_deg*pi/180 / *(u32*)(ctrl+0x6EC)` (verified at exe+0x13EFA7),
    // so dividing by this same number reproduces the engine's per-frame convention
    // without any position-unit conversion. It is 60 in the shipping build; if a build
    // ever changes it we follow automatically.
    {
        uint32_t fps = 0;
        if (mem::Read<uint32_t>(c + 0x6EC, fps) && fps >= 1 && fps <= 1000) {
            g.gameFps = static_cast<float>(fps);
        }
    }

    // Object array: the game's own ctrl-relative path first (site 0x13ED70:
    // objArray = *(u64*)(*(u64*)(ctrl+0x08))), world global as fallback.
    uintptr_t objArray = 0;
    uintptr_t ctrlOwner = 0;
    if (mem::Read<uint64_t>(c + 0x08, ctrlOwner) && ctrlOwner != 0) {
        mem::Read<uint64_t>(ctrlOwner, objArray);
    }
    uintptr_t world = 0;
    mem::Read<uint64_t>(g.base + kRvaWorldGlobal, world);
    uintptr_t worldArr = 0;
    if (world != 0) {
        uint8_t c8 = 0;
        if (mem::Read<uint8_t>(world + 0xA87E4, c8)) {
            // NOTE: this byte is NOT the object count (it read 1 while many aircraft
            // existed) - log-only, never a gate.
            g.objCountSeen = c8;
        }
        mem::Read<uint64_t>(world + 0x697B8, worldArr);
    }
    uint8_t playerIdx = 0xFF;
    mem::Read<uint8_t>(g.base + kRvaPlayerIdx, playerIdx);

    // Validate the ctrl-relative array; if it does not yield a plausible player object,
    // use the world's object array instead.
    if (objArray != 0 && playerIdx != 0xFF) {
        uintptr_t probe = 0;
        if (!mem::Read<uint64_t>(objArray + static_cast<uintptr_t>(playerIdx) * 8, probe) ||
            probe < 0x10000) {
            if (worldArr != 0) objArray = worldArr;
        }
    }
    if (objArray == 0) objArray = worldArr;

    // Owner-key collision diagnostic (one frame's worth of owner pairs).
    uint32_t seenOwnerKeys[16] = {};
    int nOwnerKeys = 0;

    for (int i = 0; i < 16; ++i) {
        if ((mask & (1u << i)) == 0) continue;
        ::InterlockedIncrement64(&g.slots);
        const uintptr_t slot = c + 0x158 + static_cast<uintptr_t>(i) * 0x58;

        // Per-aircraft identity is (owner obj, owner part) at slot+0x44/+0x45.
        uint8_t owner = 0xFF, ownerPart = 0xFF;
        if (!mem::Read<uint8_t>(slot + 0x44, owner) || owner == 0xFF) continue;
        mem::Read<uint8_t>(slot + 0x45, ownerPart);
        const bool isPlayer = (owner == playerIdx);
        const FieldSet* fs = isPlayer ? g.playerFs : g.enemyFs;
        // "Blank = vanilla" is handled by the key being absent. An *explicit* 0 is a
        // real setting: pure pursuit, still rate-limited by MaxRotationAngle, which is
        // what the INI documents and what makes an A/B baseline possible
        // (GuidanceOverride=0 is the mod's actual off switch).
        if (fs == nullptr || !fs->homingForesightAmount) continue;
        const float amount = Clampf(*fs->homingForesightAmount, 0.f, 1.f);
        if (isPlayer) {
            ::InterlockedIncrement64(&g.playerSlots);
        } else {
            ::InterlockedIncrement64(&g.enemySlots);
        }

        uint8_t tObj = 0xFF, tPart = 0xFF;
        if (!mem::Read<uint8_t>(slot + 0x42, tObj) || !mem::Read<uint8_t>(slot + 0x43, tPart)) {
            continue;
        }
        // 0xFF means "no target" - a completely normal state (a missile whose target
        // died or that was never given one), NOT an error. Lumping it into badTarget
        // made a clean run report badTarget=283 and look alarming. Only a target
        // INDEX OUT OF RANGE is a real anomaly.
        if (tObj == 0xFF || tPart == 0xFF) {
            ::InterlockedIncrement64(&g.noTarget);
            continue;
        }
        if (tObj >= 64) {
            ::InterlockedIncrement64(&g.badTobj);
            ::InterlockedIncrement64(&g.badTarget);
            continue;
        }
        if (objArray == 0) {
            ::InterlockedIncrement64(&g.badObjArray);
            ::InterlockedIncrement64(&g.badTarget);
            continue;
        }
        uintptr_t obj = 0;
        if (!mem::Read<uint64_t>(objArray + static_cast<uintptr_t>(tObj) * 8, obj) ||
            obj < 0x10000) {
            ::InterlockedIncrement64(&g.badObjPtr);
            ::InterlockedIncrement64(&g.badTarget);
            continue;
        }
        uintptr_t parts = 0;
        if (!mem::Read<uint64_t>(obj + 0x110, parts) || parts < 0x10000) {
            ::InterlockedIncrement64(&g.badParts);
            ::InterlockedIncrement64(&g.badTarget);
            continue;
        }
        const uintptr_t tPartAddr = parts + static_cast<uintptr_t>(tPart) * 0x520 + 0x258;
        Vec3 T;
        if (!mem::IsReadable(tPartAddr, 16) || !mem::ReadVec3(tPartAddr, T)) {
            ::InterlockedIncrement64(&g.badPartRead);
            ::InterlockedIncrement64(&g.badTarget);
            continue;
        }
        // NOTE: part+0x1F8 bit 3 is NOT a target-validity gate.  At 0x13ED8D the game
        // only uses it to run an extra "target within X -> clear lock" proximity check;
        // the guidance block itself is entered for any non-0xFF target pair.  Treating
        // the clear bit as "inactive" rejected every real target (run 2026-10-26 22:37,
        // inactive=5047, applied=0) - do not reinstate.

        // One-shot diagnostic of what was actually resolved (first few player targets).
        {
            const LONG ti = ::InterlockedIncrement(&g.tgtDbgWritten) - 1;
            if (ti >= 0 && ti < kTgtDebug) {
                TgtDebug& dbg = g.tgtDbg[ti];
                uint8_t pf = 0;
                mem::Read<uint8_t>(tPartAddr - 0x258 + 0x1F8, pf);
                Vec3 mp;
                mem::ReadVec3(slot + 0x00, mp);
                dbg.tObj = tObj;
                dbg.tPart = tPart;
                dbg.partFlags = pf;
                dbg.px = FBits(mp.x);
                dbg.py = FBits(mp.y);
                dbg.pz = FBits(mp.z);
                dbg.tx = FBits(T.x);
                dbg.ty = FBits(T.y);
                dbg.tz = FBits(T.z);
            }
        }

        // Target velocity: position difference between two adjacent game frames,
        // then low-pass filtered.  The raw per-frame difference is noisy whenever the
        // target's own update and our hook land on different frames; that noise made
        // the missile twitch.  Implausible values (position discontinuity) are clamped.
        Vec3 Vt;
        const uint32_t tKey = (static_cast<uint32_t>(tObj) << 8) | static_cast<uint32_t>(tPart);
        TargetCacheEntry* te = CacheFind(tKey);
        if (te != nullptr) {
            te->lastFrame = g.frameId;   // LRU bookkeeping (eviction when the table fills)
            if (te->frame == g.frameId) {
                Vt = te->vel;
            } else if (te->valid && te->frame + 1 == g.frameId) {
                Vt = T - te->pos;
                te->vel = Vt;
                te->pos = T;
                te->frame = g.frameId;
            } else {
                // first sighting (or a gap > 1 frame): velocity unknown this frame
                Vt = Vec3();
                te->vel = Vt;
                te->pos = T;
                te->frame = g.frameId;
                te->valid = true;
            }
            // A mission restart or a recycled object slot makes the target position
            // jump discontinuously; a real aircraft cannot. Tell them apart by the
            // step length.
            //
            // UNITS: Vt is a per-frame displacement in the game's POSITION UNITS, the
            // same unit as the slot positions (see the file header of msl_steer.h -
            // 1 m == 5.12 units). The old bound here was `20.f` with a comment calling
            // it "20 m/frame == 1200 m/s", which conflated the two: 20 units/frame is
            // only ~234 m/s, so EVERY target faster than that tripped the clamp every
            // frame, lost its true speed, and had its EMA history reset each time
            // (velSmoothValid was cleared). Bound below is expressed in the honest
            // unit: 1200 m/s at 60 fps == 20 m/frame == 102.4 units/frame.
            constexpr float kPosUnitsPerMetre = 5.12f;
            constexpr float kMaxTargetSpeedMps = 1200.f;   // generous; no aircraft is this fast
            constexpr float kMaxTargetStep =
                kMaxTargetSpeedMps / 60.f * kPosUnitsPerMetre;   // ~102.4 units/frame
            Vec3 step;
            if (!msl_steer::SanitizeTargetStep(Vt, kMaxTargetStep, step)) {
                // Teleport or a non-finite sample: the direction of a jump says nothing
                // about where the target is heading, so treat velocity as UNKNOWN for
                // this frame rather than a clamped (still teleport-pointing) vector.
                // Drop the smoothed history too, or one jump keeps biasing the lead
                // point for the next several frames through the EMA.
                step = Vec3();
                te->velSmoothValid = false;
            }
            Vt = step;
            if (te->velSmoothValid) {
                Vt = te->velSmooth * 0.6f + Vt * 0.4f;
            } else {
                te->velSmoothValid = true;
            }
            te->velSmooth = Vt;
        }

        // .wpb record sanity: data_off at P-0x10 and type 2 (missile). Keeps the
        // hook away from any non-missile object that happens to share the shape.
        uintptr_t P = 0;
        if (!mem::Read<uint64_t>(slot + 0x30, P) || P < 0x10000) continue;
        uint32_t dataOff = 0;
        uint8_t type = 0;
        if (!mem::Read<uint32_t>(P - 0x10, dataOff) || dataOff != 0x10u) continue;
        if (!mem::Read<uint8_t>(P + 0x48, type) || type != 2u) continue;
        // NOTE: P+0x44 (speed, m/s) is deliberately NOT read any more. The per-frame
        // turn budget comes from the engine's fps constant instead (see above), which
        // avoids mixing position units with m/s - the bug that inflated the budget 3x.
        // Seeker cone: P+0x4A is startAng, the game's own per-frame half-angle
        // (verified in FUN_14013ECA0: cos(startAng) against forward . LOS, so 90 means
        // a 180-degree total field of view).  A configured MaxHomingAngle overrides it.
        // Without this gate the mod would steer toward targets the vanilla seeker had
        // already refused, i.e. missiles that can never lose lock.
        float coneHalfDeg = 90.f;
        {
            uint8_t startAng = 0;
            if (mem::Read<uint8_t>(P + 0x4A, startAng) && startAng > 0 && startAng <= 180) {
                coneHalfDeg = static_cast<float>(startAng);
            }
        }
        if (fs->maxHomingAngle) {
            const float configured = *fs->maxHomingAngle;
            if (configured > 0.1f && configured < 359.9f) coneHalfDeg = configured;
        }

        Vec3 p;
        if (!mem::ReadVec3(slot + 0x00, p)) continue;

        // Two per-slot vectors matter:
        //   vGame  = the displacement the game actually applied to the position this
        //            frame (p - position we wrote last frame) -> used for the correction;
        //   vBasis = our own persistent steering direction (the displacement we wrote
        //            last frame), so the PN rotation accumulates instead of resetting
        //            to the game's own heading every frame.
        //
        // Slot-cache key = the slot ROW index.  Keying it by the owner (obj,part) was
        // wrong: the launcher writes slot+0x44/0x45 from part+0x234/0x235 of the fire
        // record's part, i.e. every missile of one aircraft shares the pair
        // (playerIdx, rootPart).  With two live slots in one Apply() the pair collided:
        // the first slot inherited the second's `dir` (wrong guidance basis, growing
        // heading error) while the second was always non-continuous - the "one of two
        // missiles overshoots and circles" bug (reported 2026-10-27).  A row index is
        // unique per live missile; the frame-continuity guard covers row re-use.
        const uint32_t sKey = static_cast<uint32_t>(i);
        SlotCacheEntry* se = SlotCacheFind(sKey);
        const bool continuous = (se != nullptr) && se->valid && se->frame + 1 == g.frameId;

        // Diagnostic: count how often two live slots share one owner (obj,part) pair
        // in this Apply call.  >0 proves the two-missile owner collision existed in
        // practice; with the row-keyed cache it can no longer corrupt the steering.
        // (After the fix it documents the game's launcher attribution, nothing else.)
        {
            const uint32_t oKey =
                (static_cast<uint32_t>(owner) << 8) | static_cast<uint32_t>(ownerPart);
            bool dup = false;
            for (int k = 0; k < nOwnerKeys && nOwnerKeys <= 16; ++k) {
                if (seenOwnerKeys[k] == oKey) {
                    dup = true;
                    break;
                }
            }
            if (dup) {
                ::InterlockedIncrement64(&g.slotKeyCollisions);
            } else if (nOwnerKeys < 16) {
                seenOwnerKeys[nOwnerKeys++] = oKey;
            }
        }

        // Step 1B: the game has just written this frame's displacement to slot+0x20.
        // Read it BEFORE overwriting it: length = the step, direction = post heading.
        Vec3 vGame;
        if (!mem::ReadVec3(slot + 0x20, vGame)) continue;
        const float vLen = Length(vGame);
        if (!(vLen > 1e-3f) || vLen > 1e5f || !std::isfinite(vLen)) {
            if (se != nullptr) se->valid = false;
            ::InterlockedIncrement64(&g.badGeometry);
            continue;
        }
        Vec3 vGameDir;
        if (!Normalize(vGame, vGameDir)) {
            if (se != nullptr) se->valid = false;
            ::InterlockedIncrement64(&g.badGeometry);
            continue;
        }

        // Guidance basis: the heading WE commanded last frame (dir cache), scaled to
        // the game's step. First sample of a slot starts from the game's own heading.
        //
        // The frame-adjacency guard alone is not enough. A slot ROW can be reused one
        // frame after the previous missile died (rows slide 0..15), and if the cached
        // `dir` belonged to that dead missile it can be tens of degrees away from the
        // direction the game is actually flying. The correction below translates the
        // missile by (vNew - vGame), so a stale basis would teleport it by up to
        // 2*|vGame| in one frame. Only trust the cache when it still roughly agrees
        // with the game's own heading; otherwise fall back to vGameDir.
        Vec3 dir = vGameDir;
        if (continuous) {
            Vec3 cached;
            if (Normalize(se->dir, cached) && Dot(cached, vGameDir) > 0.866f) {  // < 30 deg
                dir = cached;
            } else {
                ::InterlockedIncrement64(&g.staleDirResets);
            }
        }
        const Vec3 vBasis = dir * vLen;

        // STEP 2: true lambda-dot proportional navigation, blended with a one-frame
        // pure-pursuit demand by HomingForesightAmount (0 = pure pursuit, 1 = PN with
        // N = 4).  The rmin soft-singularity guard inside ComputePn replaces the old
        // 1200 m / 250 m terminal hardcodes, so there are no range/tGo special cases
        // left here.
        float effAmount = amount;
        if (continuous && se != nullptr) {
            effAmount = se->amountSmooth * 0.7f + amount * 0.3f;  // EMA on the blend
        }
        if (se != nullptr) se->amountSmooth = effAmount;

        msl_steer::PnInput in;
        in.p = p;
        in.v = vBasis;
        in.target = T;
        in.targetVel = Vt;
        in.gameFps = g.gameFps;
        in.amount = effAmount;
        in.maxRotationDegPerSec = fs->maxRotationAngle ? *fs->maxRotationAngle : 180.f;
        in.navConstant = (g.pnGain > 0.5f) ? g.pnGain : 4.0f;

        msl_steer::PnOutput out;
        if (!msl_steer::ComputePn(in, out)) {
            if (se != nullptr) se->valid = false;
            ::InterlockedIncrement64(&g.badGeometry);
            continue;
        }
        ::InterlockedIncrement64(&g.computed);
        // Lambda statistics for the summary: micro-rad/frame sum and max.
        {
            const LONG64 micro = static_cast<LONG64>(out.lambdaDot * 1.0e6f);
            ::InterlockedExchangeAdd64(&g.lambdaSumMicro, micro);
            for (;;) {
                const LONG cur = g.lambdaMaxMicro;
                if (micro <= static_cast<LONG64>(cur)) break;
                if (::InterlockedCompareExchange(&g.lambdaMaxMicro, static_cast<LONG>(micro),
                                                 cur) == cur) {
                    break;
                }
            }
        }

        const float turnDeg = out.turnRad * 57.29577951308232f;
        const LONG turnMilli = static_cast<LONG>(turnDeg * 1000.0f);
        for (;;) {
            const LONG cur = g.maxCorrMilliDeg;
            if (turnMilli <= cur) break;
            if (::InterlockedCompareExchange(&g.maxCorrMilliDeg, turnMilli, cur) == cur) break;
        }
        if (out.turnRad > 0.0005f) RecordDetail(tObj, tPart, effAmount, out, vLen);

        Vec3 dUnit;
        if (!Normalize(out.vNew, dUnit)) {
            if (se != nullptr) se->valid = false;
            ::InterlockedIncrement64(&g.badGeometry);
            continue;
        }
        // Orientation-convention calibration: retried only on meaningful commanded
        // turns (the old one-shot attempt ran on a ~0.6 deg correction and gave up).
        if (!g.calibDone) CalibrateTick(slot, vGameDir, dUnit, now);

        // The steering write: position only, x/y/z (the w lane stays 1.0f). Next frame
        // the game adds its own displacement on top of this position, so each frame's
        // position delta becomes exactly the PN displacement.
        const Vec3 pNew = p + (out.vNew - vGame);

        if (g.dryRun) {
            if (se != nullptr) {
                // Nothing is written, so the missile keeps flying the GAME's heading.
                // Caching dUnit here would make the next frame's guidance basis a
                // direction that was never actually flown.
                se->frame = g.frameId;
                se->dir = vGameDir;
                se->valid = true;
            }
            continue;  // compute + log only
        }

    // Terminal hand-back (opt-in, [runtime] TerminalZone, default 0 = off).
    //
    // When this was introduced (2026-10-27) it was hard-coded to 30 m to stop one
    // of two simultaneously fired missiles from "orbiting the impact point".  That
    // symptom was actually the owner-keyed slot cache collision (fixed separately:
    // the cache is now keyed by the slot ROW index), and its *visible* effect was a
    // wrong guidance basis, not a terminal-phase singularity - see docs/MSL_PN.md.
    //
    // A 972-geometry closed-loop sweep over the shipped ComputePn shows that
    // handing the last 30 m back to the game changes no outcome at all (identical
    // hit rate, identical closest approach), because:
    //   * ComputePn's rmin guard already floors r^2 at max(2 frames of travel, 25 m),
    //     which for a 16.7 m/frame MSL is 33.3 m - i.e. the guard engages slightly
    //     BEFORE the old 30 m hand-back ever did, so lambda-dot is bounded either way;
    //   * the rate limit (MaxRotationAngle x dt) bounds the achievable turn radius to
    //     |v|/omega (~318 m at the 180 deg/s default), so a tight orbit around a
    //     point 30 m away is geometrically impossible.
    // So PN now owns the entire flight.  TerminalZone > 0 restores the old
    // behaviour for A/B comparison; it is a diagnostic knob, not a fix.
    const float terminalZone = g.terminalZone;
    if (terminalZone > 0.f && out.range < terminalZone) {
            if (se != nullptr) {
                // Vanilla guidance owns the heading inside the zone, so remember what
                // the game actually flew rather than our unused PN command.
                se->frame = g.frameId;
                se->dir = vGameDir;
                se->valid = true;
            }
            ::InterlockedIncrement64(&g.quietTerminal);
            continue;
        }

        // Seeker cone gate: if the target has fallen outside the seeker's field of
        // view, do NOT steer this frame - that is the mechanic that lets a fast
        // crossing target actually escape. The slot cache is still updated with the
        // game's own heading so a target drifting back into the cone resumes PN
        // cleanly rather than inheriting a stale commanded direction.
        {
            Vec3 uLos, uFwd;
            bool inCone = true;
            if (Normalize(T - p, uLos) && Normalize(vGame, uFwd)) {
                const float cosCone = std::cos(DegToRad(coneHalfDeg));
                inCone = (Dot(uFwd, uLos) >= cosCone);
            }
            if (!inCone) {
                ::InterlockedIncrement64(&g.outOfCone);
                if (se != nullptr) {
                    se->frame = g.frameId;
                    se->dir = vGameDir;
                    se->valid = true;
                }
                continue;
            }
        }

        if (!mem::WriteVec3(slot + 0x00, pNew)) {
            if (se != nullptr) se->valid = false;
            ::InterlockedIncrement64(&g.writeFails);
            continue;
        }
        // Optional: keep the displacement/w cache consistent within this frame.
        mem::WriteVec3(slot + 0x20, out.vNew);
        mem::Write<float>(slot + 0x2C, 0.f);
        // Step 1B: take over the slot orientation (Euler at slot+0x10..0x18) so the
        // game's next integration and the visual heading follow the PN heading.
        if (g.calibOk) SyncOrientation(slot, vGameDir, dUnit);
        if (se != nullptr) {
            se->frame = g.frameId;
            se->dir = dUnit;
            se->valid = true;
        }
        ::InterlockedIncrement64(&g.applied);
    }
}

// ---- hook installation (deferred) ----------------------------------------
bool InstallHook() {
    const uintptr_t target = g.base + kRvaGuidance;
    uint8_t pro[sizeof(kPrologue)] = {};
    if (!mem::ReadBytes(target, pro, sizeof(pro)) ||
        std::memcmp(pro, kPrologue, sizeof(pro)) != 0) {
        return false;  // .text not decrypted yet (or a different game build)
    }
    hook::InstallResult ir;
    std::string why;
    void* tramp = nullptr;
    if (!hook::Install(reinterpret_cast<void*>(target),
                       reinterpret_cast<void*>(&MslPnDetour), &tramp, ir, &why)) {
        L().Info("MSLPN: hook refused by the engine: %s", why.c_str());
        return false;
    }
    ::InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(&g_tramp), tramp);
    g.install = ir;
    g.target = reinterpret_cast<void*>(target);
    L().Info("MSLPN: HOOK exe+0x%X installed (stole %d byte(s), %s) - pos-based PN on the live "
            "plain-MSL slots",
            kRvaGuidance, static_cast<int>(ir.stolenBytes), ir.reason.c_str());
    return true;
}

void LogSummary() {
    const LONG maxMilli = ::InterlockedExchange(&g.maxCorrMilliDeg, 0);
    const LONG64 lambdaSum = ::InterlockedExchange64(&g.lambdaSumMicro, 0);
    const LONG lambdaMax = ::InterlockedExchange(&g.lambdaMaxMicro, 0);
    const LONG64 pnCount = g.computed;
    const double lambdaMeanMrad =
        (pnCount > 0) ? (static_cast<double>(lambdaSum) / static_cast<double>(pnCount)) / 1000.0
                      : 0.0;
    const double lambdaMaxMrad = static_cast<double>(lambdaMax) / 1000.0;
    L().Key("MSLPN: calls=%lld slots=%lld player=%lld enemy=%lld computed=%lld applied=%lld "
            "badTarget=%lld (tobj=%lld arr=%lld obj=%lld parts=%lld partRd=%lld) "
            "noTarget=%lld "
            "objCountByte=%d "
            "badGeom=%lld writeFail=%lld orient=%lld orientFail=%lld calib=%d/%d "
            "ownCol=%lld terminal=%lld (zone=%.0fm) outOfCone=%lld staleDir=%lld "
            "pn=%lld lambda=%.3f/%.3f mrad/f frame=%u maxCorr=%.1fdeg dryRun=%d",
            static_cast<long long>(g.calls), static_cast<long long>(g.slots),
            static_cast<long long>(g.playerSlots), static_cast<long long>(g.enemySlots),
            static_cast<long long>(g.computed), static_cast<long long>(g.applied),
            static_cast<long long>(g.badTarget), static_cast<long long>(g.badTobj),
            static_cast<long long>(g.badObjArray), static_cast<long long>(g.badObjPtr),
            static_cast<long long>(g.badParts), static_cast<long long>(g.badPartRead),
            static_cast<long long>(g.noTarget),
            static_cast<int>(g.objCountSeen),
            static_cast<long long>(g.badGeometry),
            static_cast<long long>(g.writeFails), static_cast<long long>(g.orientWrites),
            static_cast<long long>(g.orientFails), g.calibVariant, g.calibAxis,
            static_cast<long long>(g.slotKeyCollisions), static_cast<long long>(g.quietTerminal),
            static_cast<double>(g.terminalZone), static_cast<long long>(g.outOfCone),
            static_cast<long long>(g.staleDirResets),
            static_cast<long long>(pnCount), lambdaMeanMrad, lambdaMaxMrad, g.frameId,
            static_cast<double>(maxMilli) / 1000.0, g.dryRun ? 1 : 0);
}

// Step 1B calibration result, logged once from Tick (never from the detour).
void DrainCalib() {
    CalibLog& lg = g.calibLog;
    if (lg.ready == 0 || lg.logged == 1) return;
    lg.logged = 1;
    const bool ok = (lg.ok == 1);
    float dots[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 6; ++i) dots[i] = BitsF(lg.axisDot[i]);
    L().Key("MSLPN: STEP1B orientation calibration: axis=%ld (0..2 row-major columns, 3..5 "
            "rows) variant=%ld (0 world, 1 -world, 2 local, 3 -local) ok=%d attempts=%ld "
            "tooSmall=%ld base=%.9g scores=[%.9g %.9g %.9g %.9g]",
            static_cast<long>(lg.axis), static_cast<long>(lg.variant), ok ? 1 : 0,
            static_cast<long>(lg.attempts), static_cast<long>(lg.tooSmall),
            static_cast<double>(BitsF(lg.baseBits)), static_cast<double>(BitsF(lg.s0)),
            static_cast<double>(BitsF(lg.s1)), static_cast<double>(BitsF(lg.s2)),
            static_cast<double>(BitsF(lg.s3)));
    L().Key("MSLPN:   calibration raw: heading=(%.9g %.9g %.9g) d=(%.9g %.9g %.9g) "
            "axisDots=[%.9g %.9g %.9g %.9g %.9g %.9g]%s",
            static_cast<double>(BitsF(lg.hx)), static_cast<double>(BitsF(lg.hy)),
            static_cast<double>(BitsF(lg.hz)), static_cast<double>(BitsF(lg.dx)),
            static_cast<double>(BitsF(lg.dy)), static_cast<double>(BitsF(lg.dz)),
            static_cast<double>(dots[0]), static_cast<double>(dots[1]),
            static_cast<double>(dots[2]), static_cast<double>(dots[3]),
            static_cast<double>(dots[4]), static_cast<double>(dots[5]),
            ok ? "" : " -> Euler orientation writes DISABLED for this run (position steering "
                      "still active)");
    // The decisive diagnostic when ok=0: compare the commanded turn with what each
    // rotvec convention ACTUALLY produced. A large mismatch means the axis-angle
    // magnitude convention is wrong, not that the sample was too small.
    L().Key("MSLPN:   calibration turn angles: commanded=%.3f deg, variants produced "
            "[%.3f %.3f %.3f %.3f] deg (ratio vs commanded: [%.2f %.2f %.2f %.2f])",
            static_cast<double>(BitsF(lg.reqBits)) * 57.29577951308232,
            static_cast<double>(BitsF(lg.a0)) * 57.29577951308232,
            static_cast<double>(BitsF(lg.a1)) * 57.29577951308232,
            static_cast<double>(BitsF(lg.a2)) * 57.29577951308232,
            static_cast<double>(BitsF(lg.a3)) * 57.29577951308232,
            static_cast<double>(BitsF(lg.reqBits)) > 1e-6
                ? static_cast<double>(BitsF(lg.a0)) / static_cast<double>(BitsF(lg.reqBits))
                : 0.0,
            static_cast<double>(BitsF(lg.reqBits)) > 1e-6
                ? static_cast<double>(BitsF(lg.a1)) / static_cast<double>(BitsF(lg.reqBits))
                : 0.0,
            static_cast<double>(BitsF(lg.reqBits)) > 1e-6
                ? static_cast<double>(BitsF(lg.a2)) / static_cast<double>(BitsF(lg.reqBits))
                : 0.0,
            static_cast<double>(BitsF(lg.reqBits)) > 1e-6
                ? static_cast<double>(BitsF(lg.a3)) / static_cast<double>(BitsF(lg.reqBits))
                : 0.0);
}

void DrainDetails() {
    if (g.detailOverflow != 0 && ::InterlockedExchange(&g.detailOverflowSeen, 1) == 0) {
        L().Key("MSLPN: detail log full (%d corrections logged); further corrections only "
                "counted (this notice is printed once per run)",
                kDetails);
    }
    for (int i = 0; i < kDetails; ++i) {
        Detail& d = g.details[i];
        if (d.ready == 0 || d.logged == 1) continue;
        d.logged = 1;
        const float amount = BitsF(d.amountBits);
        const float range = BitsF(d.rangeBits);
        const float theta = BitsF(d.thetaBits);
        const float thetaMax = BitsF(d.thetaMaxBits);
        const float speed = BitsF(d.speedBits);
        const float x = BitsF(d.vn0);
        const float y = BitsF(d.vn1);
        const float z = BitsF(d.vn2);
        const float lambda = BitsF(d.lambdaBits);
        const float omegaPn = BitsF(d.omegaPnBits);
        const float omegaP = BitsF(d.omegaPBits);
        const float turn = BitsF(d.turnBits);
        // N is configurable ([runtime] PnGain), so print the live value rather than
        // the old hard-coded "N=4" (which lied as soon as someone set PnGain).
        L().Key("MSLPN: steer tObj=%ld tPart=%ld amount=%.2f N=%.2f |R|=%.0fm "
                "|lambda|=%.6f rad/f losErr=%.2fdeg turn=%.2fdeg limit=%.2fdeg pursuit=%.5f pn=%.5f "
                "|v|=%.4f m/frame vNew=(%.4f %.4f %.4f)",
                static_cast<long>(d.tObj), static_cast<long>(d.tPart),
                static_cast<double>(amount), static_cast<double>(g.pnGain),
                static_cast<double>(range),
                static_cast<double>(lambda),
                static_cast<double>(theta * 57.29577951308232f),
                static_cast<double>(turn * 57.29577951308232f),
                static_cast<double>(thetaMax * 57.29577951308232f),
                static_cast<double>(omegaP), static_cast<double>(omegaPn),
                static_cast<double>(speed), static_cast<double>(x), static_cast<double>(y),
                static_cast<double>(z));
    }
}

// One line if the game stops calling the guidance (world gate frozen / paused /
// menu), so a test run is not silently wasted. Tick side only.
void GuidanceWatchdog(double now) {
    const LONG64 callsNow = g.calls;
    if (callsNow != g.lastSeenCalls) {
        g.lastSeenCalls = callsNow;
        g.lastProgress = now;
        if (g.stallLogged) {
            g.stallLogged = false;
            L().Key("MSLPN: guidance is running again (calls=%lld)",
                    static_cast<long long>(callsNow));
        }
        return;
    }
    if (g.lastProgress == 0.0) {
        g.lastProgress = now;
        return;
    }
    if (!g.stallLogged && (now - g.lastProgress) > 5.0) {
        g.stallLogged = true;
        L().Key("MSLPN: FUN_14013ECA0 has not been entered for >5 s (world gate "
                "world+0xA8790 bit 0x10000000, paused, or a loading screen) - no steering is "
                "happening right now");
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool Wanted(const Config& cfg) {
    if (!cfg.runtime.guidanceOverride) return false;
    if (cfg.runtime.probe || cfg.runtime.hwWatch) return false;
    const FieldSet* p = cfg.Resolve(Side::Player, "msl");
    const FieldSet* e = cfg.Resolve(Side::Enemy, "msl");
    // Armed when either side carries an explicit msl HomingForesightAmount. A value
    // of 0 is valid: it means pure pursuit (rate limited by MaxRotationAngle), i.e.
    // the vanilla chase re-implemented by this code - the A/B baseline. Use
    // GuidanceOverride=0 to actually turn the mod off.
    const bool pOn = p != nullptr && p->homingForesightAmount.has_value();
    const bool eOn = e != nullptr && e->homingForesightAmount.has_value();
    return pOn || eOn;
}

void Configure(const Config& cfg, uintptr_t exeBase) {
    g.wanted = true;
    g.base = exeBase;
    g.dryRun = cfg.runtime.dryRun;
    g.pnGain = (cfg.runtime.pnGain > 0.5f) ? cfg.runtime.pnGain : 4.0f;
    g.terminalZone = (cfg.runtime.terminalZone > 0.f) ? cfg.runtime.terminalZone : 0.f;
    g.gameFps = 60.f;
    g.playerFs = cfg.Resolve(Side::Player, "msl");
    g.enemyFs = cfg.Resolve(Side::Enemy, "msl");
    g.lastTry = 0.0;
    g.tries = 0;
    g.gaveUp = false;
    g.lastSummary = 0.0;
    g.frameId = 0;
    g.lastApply = 0.0;
    g.lastSeenCalls = 0;
    g.lastProgress = 0.0;
    g.stallLogged = false;
    g.helpersChecked = false;
    g.helpersOk = false;
    g.calibDone = false;
    g.calibOk = false;
    g.calibAxis = -1;
    g.calibVariant = -1;
    g.orientWrites = 0;
    g.orientFails = 0;
    g.calibAttempts = 0;
    g.calibTooSmall = 0;
    g.calibStart = 0.0;
    g.lambdaSumMicro = 0;
    g.lambdaMaxMicro = 0;
    g.slotKeyCollisions = 0;
    g.quietTerminal = 0;
    g.outOfCone = 0;
    g.noTarget = 0;
    g.staleDirResets = 0;
    g.fastCallbacks = 0;
    g.fastWarned = 0;
    g.calibLog = CalibLog();
    for (auto& e : g.cache) e = TargetCacheEntry();
    for (auto& e : g.slotCache) e = SlotCacheEntry();
    for (auto& d : g.details) d = Detail();
    g.detailWritten = 0;
    g.detailOverflow = 0;
    g.detailOverflowSeen = 0;
    for (auto& d : g.tgtDbg) d = TgtDebug();
    g.tgtDbgWritten = 0;
    g.tgtDbgLogged = 0;

    const float pa = (g.playerFs && g.playerFs->homingForesightAmount)
                         ? *g.playerFs->homingForesightAmount
                         : 0.f;
    const float ea = (g.enemyFs && g.enemyFs->homingForesightAmount)
                         ? *g.enemyFs->homingForesightAmount
                         : 0.f;
    L().Info("MSLPN: plain-MSL PN armed: hook exe+0x%X (FUN_14013ECA0) post-update; "
            "[player] msl amount=%.2f, [enemy] msl amount=%.2f, N=%.2f, DryRun=%d",
            kRvaGuidance, static_cast<double>(pa), static_cast<double>(ea),
            static_cast<double>(g.pnGain), g.dryRun ? 1 : 0);
    if (g.terminalZone > 0.f) {
        L().Info("MSLPN: terminal hand-back ACTIVE at %.0f m ([runtime] TerminalZone): the last "
                "approach is flown by the vanilla guidance (diagnostic A/B mode)",
                static_cast<double>(g.terminalZone));
    } else {
        L().Info("MSLPN: full PN for the whole flight: no terminal hand-back (TerminalZone=0); "
                "close-range lambda-dot is bounded by the rmin guard");
    }
    L().Info("MSLPN: per-frame turn budget uses the engine's own fps constant "
            "(ctrl+0x6EC, 60 in this build) - no position-unit conversion needed");
    L().Info("MSLPN: ctrl = world weapon manager (world+0x6A130), 16 slots ctrl+0x158+i*0x58, "
            "mask ctrl+0x6E8; steering writes slot+0x00..0x08 (position, w stays 1.0) only, "
            "missile velocity measured from our own per-slot position cache");
    if (!g.playerFs || !g.playerFs->homingForesightAmount) {
        L().Info("MSLPN: [player] msl has no HomingForesightAmount - player missiles keep vanilla "
                "guidance");
    }
}

void Tick() {
    if (!g.wanted) return;
    const double now = NowQpc();
    if (!g.hooked && !g.gaveUp) {
        if ((now - g.lastTry) >= 0.2) {
            g.lastTry = now;
            ++g.tries;
            if (InstallHook()) {
                g.hooked = true;
                g.lastProgress = now;
                g.lastSeenCalls = g.calls;
            } else if (g.tries >= 300) {
                g.gaveUp = true;
                L().Info("MSLPN: hook not installed after 60 s (bytes never matched / engine "
                        "refused); plain-MSL PN stays off this run");
            } else if (g.tries == 1) {
                L().Info("MSLPN: waiting for the SteamStub-decrypted .text (retry every 200 ms, "
                        "max 60 s) - this is expected");
            }
        }
    }
    DrainCalib();
    DrainDetails();
    if (g.tgtDbgWritten > 0 && ::InterlockedCompareExchange(&g.tgtDbgLogged, 1, 0) == 0) {
        const LONG n = (g.tgtDbgWritten > kTgtDebug) ? kTgtDebug : g.tgtDbgWritten;
        L().Key("MSLPN: target debug (first %d resolved player targets; flags=byte[part+0x1F8]):",
                static_cast<int>(n));
        for (LONG i = 0; i < n; ++i) {
            const TgtDebug& d = g.tgtDbg[i];
            L().Key("MSLPN:   tObj=%d tPart=%d flags=0x%02X partPos=(%.1f %.1f %.1f) "
                    "targetPos=(%.1f %.1f %.1f)",
                    static_cast<int>(d.tObj), static_cast<int>(d.tPart),
                    static_cast<int>(d.partFlags), static_cast<double>(BitsF(d.px)),
                    static_cast<double>(BitsF(d.py)), static_cast<double>(BitsF(d.pz)),
                    static_cast<double>(BitsF(d.tx)), static_cast<double>(BitsF(d.ty)),
                    static_cast<double>(BitsF(d.tz)));
        }
        if (g.tgtDbgWritten > kTgtDebug) {
            L().Key("MSLPN: target debug overflow (%lld more resolved targets)",
                    static_cast<long long>(g.tgtDbgWritten - kTgtDebug));
        }
    }
    if (g.hooked) GuidanceWatchdog(now);
    // One-shot warning if the game calls the guidance faster than the frame heuristic
    // can distinguish (see the frame-id comment in Apply). Harmless but it degrades
    // steering, so say so instead of leaving a silent mystery.
    if (g.fastCallbacks > 32 && ::InterlockedCompareExchange(&g.fastWarned, 1, 0) == 0) {
        L().Info("MSLPN: WARNING FUN_14013ECA0 is being called more than ~330 times/s "
                "(count=%lld). The per-frame heuristic cannot tell frames apart at that "
                "rate, so the guidance basis and target velocity degrade. Report this log.",
                static_cast<long long>(g.fastCallbacks));
    }
    // Telemetry; Log::Key() drops it unless LogLevel >= 2 (see src/core/log.cpp).
    if (g.hooked && (now - g.lastSummary) >= 2.0) {
        g.lastSummary = now;
        LogSummary();
    }
}

void Shutdown() {
    if (g.hooked && g.target != nullptr) {
        if (!hook::Remove(g.target, g.install)) {
            // The patch is still live, so the detour can still be entered: keep the
            // trampoline pointer valid or the next MslPnDetour would spin forever in
            // WaitTramp() and freeze the game thread. (hook::Remove also refuses to
            // free the trampoline in this case.)
            L().Info("MSLPN: cannot restore the prologue at %s - leaving the hook and its "
                    "trampoline in place so the game cannot jump into freed memory",
                    mem::Hex(reinterpret_cast<uintptr_t>(g.target)).c_str());
            g.wanted = false;   // stop doing work, but stay callable
            return;
        }
        g.target = nullptr;
        g.hooked = false;
    }
    ::InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(&g_tramp), nullptr);
    g.wanted = false;
}

} // namespace msl_pn
} // namespace acz
