// ACZPropNav - offline engagement simulator and self test for src/pn/pn.cpp.
//
//   pnsim.exe --selftest     assertions, PASS/FAIL lines, non-zero exit on failure
//   pnsim.exe --report       sweep of engagement geometries (this is the default)
//   pnsim.exe --report -v    ... plus one line per simulated case
//
// The simulated world is deliberately simple and planar (x/y): the target flies
// a straight line or a constant-rate turn, the missile keeps a constant speed
// and steers only with ComputeGuidance() - the exact same code the DLL injects.
// Everything is deterministic (no rand(), fixed dt = 1/120 s).
#include "pn/msl_steer.h"
#include "pn/pn.h"
#include "common/acz_common.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

using namespace acz;

// ---------------------------------------------------------------------------
// Simulated world constants (ACZ-plausible numbers; see docs/PN.md)
// ---------------------------------------------------------------------------
constexpr float kMissileSpeed = 680.0f;         // m/s, ~Mach 2 cruise
constexpr float kSimDt = 1.0f / 120.0f;         // s, matches the injector tick
constexpr float kHitRadius = 10.0f;             // m
constexpr float kMaxSimTime = 60.0f;            // s, hard ceiling
constexpr float kVanillaFovDeg = 75.0f;         // ACZ MSL seeker cone
constexpr float kVanillaTurnDegPerSec = 40.0f;  // ACZ MSL turn rate
constexpr float kVanillaLifeTimeSec = 9.0f;     // ACZ MSL lifetime (~9.7 km range)
constexpr int kMaxSteps = static_cast<int>(kMaxSimTime / kSimDt) + 1;

struct SimConfig {
    float fovDeg = kVanillaFovDeg;
    float turnDegPerSec = kVanillaTurnDegPerSec;
    float lifeTimeSec = kVanillaLifeTimeSec;
};

// HomingForesightAmount -> guidance setting used by the sweep.
struct GuidanceSetting {
    const char* label;
    float lead;
};
const GuidanceSetting kModes[3] = {
    {"pure pursuit (lead 0.0)", 0.0f},
    {"blend (lead 0.7)", 0.7f},
    {"full PN (lead 1.0)", 1.0f},
};

// ---------------------------------------------------------------------------
// Scenario / outcome
// ---------------------------------------------------------------------------
struct Scenario {
    int familyId = 0;
    float range0 = 2000.0f;           // m, initial range
    float bearingDeg = 0.0f;          // target direction from missile, off boresight (+x)
    float targetHeadingDeg = 0.0f;    // world heading of target velocity
    float targetSpeed = 300.0f;       // m/s
    float targetTurnDegPerSec = 0.0f; // deg/s, + = left (CCW)
};

struct Outcome {
    bool hit = false;
    float closest = 0.0f;      // closest approach, m
    float flightTime = 0.0f;   // s (to hit, or total flown on a miss)
    int steps = 0;
    int fovLostFrames = 0;
    bool guidanceInvalid = false; // ComputeGuidance ever reported valid == false
};

// Rotate a planar vector about +Z.
Vec3 RotateZ(const Vec3& v, float rad) {
    const float c = std::cos(rad), s = std::sin(rad);
    return Vec3(v.x * c - v.y * s, v.x * s + v.y * c, v.z);
}

// ---------------------------------------------------------------------------
// One engagement
// ---------------------------------------------------------------------------
Outcome RunScenario(const Scenario& sc, float lead, const SimConfig& cfg) {
    GuidanceInput in;
    in.missilePos = Vec3(0.0f, 0.0f, 0.0f);
    in.missileVel = Vec3(kMissileSpeed, 0.0f, 0.0f); // starts on +x (boresight)
    const float bearing = DegToRad(sc.bearingDeg);
    const float heading = DegToRad(sc.targetHeadingDeg);
    in.targetPos = Vec3(std::cos(bearing), std::sin(bearing), 0.0f) * sc.range0;
    in.targetVel = Vec3(std::cos(heading), std::sin(heading), 0.0f) * sc.targetSpeed;
    in.dt = kSimDt;

    GuidanceParams gp;
    gp.homingForesightAmount = lead;
    gp.maxHomingAngleDeg = cfg.fovDeg;
    gp.maxRotationAngleDegPerSec = cfg.turnDegPerSec;
    gp.lifeTimeSec = cfg.lifeTimeSec;

    const float life = (cfg.lifeTimeSec > 0.0f)
                           ? (cfg.lifeTimeSec < kMaxSimTime ? cfg.lifeTimeSec : kMaxSimTime)
                           : kMaxSimTime;

    Outcome out;
    out.closest = Length(in.targetPos - in.missilePos);

    float t = 0.0f;
    while (t < life && out.steps < kMaxSteps) {
        const GuidanceResult g = ComputeGuidance(in, gp);
        if (!g.valid) out.guidanceInvalid = true;
        if (!g.inFov) ++out.fovLostFrames;

        // The missile flies at constant speed along whatever the seeker wants;
        // if the guidance sample was unusable it simply keeps its heading.
        const Vec3 missileVel = (g.valid && LengthSq(g.desiredDir) > 0.5f)
                                    ? g.desiredDir * kMissileSpeed
                                    : in.missileVel;

        // Exact closest approach over this step: both velocities are constant
        // here, so the relative motion is a straight line in relative space.
        const Vec3 relPos = in.targetPos - in.missilePos;
        const Vec3 relVel = in.targetVel - missileVel;
        const float vv = LengthSq(relVel);
        float tau = 0.0f;
        if (vv > 1e-6f) tau = Clampf(-Dot(relPos, relVel) / vv, 0.0f, in.dt);
        const float dStar = Length(relPos + relVel * tau);
        if (dStar < out.closest) out.closest = dStar;
        if (dStar <= kHitRadius) {
            out.hit = true;
            out.flightTime = t + tau;
            return out;
        }

        in.missilePos += missileVel * in.dt;
        in.missilePos = Vec3(in.missilePos.x, in.missilePos.y, 0.0f); // keep it planar
        in.targetPos += in.targetVel * in.dt;
        in.targetPos = Vec3(in.targetPos.x, in.targetPos.y, 0.0f);
        in.targetVel = RotateZ(in.targetVel, DegToRad(sc.targetTurnDegPerSec) * in.dt);
        in.missileVel = missileVel;
        t += in.dt;
        ++out.steps;
    }

    out.flightTime = t;
    return out;
}

// ---------------------------------------------------------------------------
// Report aggregation
// ---------------------------------------------------------------------------
struct Agg {
    int cases = 0;
    int hits = 0;
    int misses = 0;
    int fovLostCases = 0;
    int invalidCases = 0;
    double sumClosest = 0.0;     // closest approach over every case
    double sumMissClosest = 0.0; // closest approach over misses only
    double sumHitTime = 0.0;
    std::vector<float> closestList;
};

void AddToAgg(Agg& a, const Outcome& o) {
    ++a.cases;
    if (o.hit) {
        ++a.hits;
        a.sumHitTime += o.flightTime;
    } else {
        ++a.misses;
        a.sumMissClosest += o.closest;
    }
    if (o.fovLostFrames > 0) ++a.fovLostCases;
    if (o.guidanceInvalid) ++a.invalidCases;
    a.sumClosest += o.closest;
    a.closestList.push_back(o.closest);
}

float Median(std::vector<float> v) {
    if (v.empty()) return 0.0f;
    const size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid), v.end());
    return v[mid];
}

void AddFamily(std::vector<Scenario>& out, std::vector<std::string>& families,
               const char* name, float bearingDeg, float headingDeg,
               std::initializer_list<float> ranges, std::initializer_list<float> speeds,
               std::initializer_list<float> turns) {
    const int id = static_cast<int>(families.size());
    families.push_back(name);
    for (float r : ranges) {
        for (float v : speeds) {
            for (float w : turns) {
                Scenario s;
                s.familyId = id;
                s.range0 = r;
                s.bearingDeg = bearingDeg;
                s.targetHeadingDeg = headingDeg;
                s.targetSpeed = v;
                s.targetTurnDegPerSec = w;
                out.push_back(s);
            }
        }
    }
}

// Scenario grid. Angles in degrees; the missile starts at the origin on +x.
void BuildScenarios(std::vector<Scenario>& scenarios, std::vector<std::string>& families) {
    //                                                bearing heading  ranges               speeds           turn rates
    AddFamily(scenarios, families, "head-on",          0.f,   180.f, {1200, 1800, 2600}, {200, 300, 450}, {0, 6, 15});
    AddFamily(scenarios, families, "tail-chase slow",  0.f,     0.f, {1200, 1800, 2600}, {250, 350},      {0, 6, 15});
    AddFamily(scenarios, families, "tail-chase equal", 0.f,     0.f, {1200, 1800, 2600}, {680},           {0, 6, 15});
    AddFamily(scenarios, families, "tail-chase fast",  0.f,     0.f, {1200, 1800, 2600}, {900},           {0, 6, 15});
    AddFamily(scenarios, families, "crossing 90",     35.f,   125.f, {1000, 1600, 2400}, {200, 300, 450}, {0, 6, 15});
    AddFamily(scenarios, families, "beam",            45.f,    90.f, {1000, 1600, 2400}, {200, 300, 450}, {0, 6, 15});
    AddFamily(scenarios, families, "high offset 60",  60.f,   180.f, {1000, 1600, 2400}, {200, 300, 450}, {0, 6, 15});
    AddFamily(scenarios, families, "crossing 90 tight", 40.f, 130.f, {700, 1000, 1400},  {350, 450, 550}, {0, 6, 15});
    AddFamily(scenarios, families, "beam hard",       50.f,   140.f, {800, 1200, 1700},  {400, 500, 600}, {0, 6, 15});
}

bool IsCrossingFamily(const std::string& name) {
    return name.find("crossing") != std::string::npos || name.find("beam") != std::string::npos;
}

void PrintTableRow(const char* family, float lead, const Agg& a) {
    const float hitPct =
        a.cases ? 100.0f * static_cast<float>(a.hits) / static_cast<float>(a.cases) : 0.0f;
    const float medClosest = Median(a.closestList);
    const float meanT = a.hits ? static_cast<float>(a.sumHitTime / a.hits) : 0.0f;
    const float fovPct =
        a.cases ? 100.0f * static_cast<float>(a.fovLostCases) / static_cast<float>(a.cases) : 0.0f;

    char meanMiss[24];
    if (a.misses > 0) {
        std::snprintf(meanMiss, sizeof(meanMiss), "%.1f",
                      static_cast<float>(a.sumMissClosest / a.misses));
    } else {
        std::snprintf(meanMiss, sizeof(meanMiss), "--");
    }
    std::printf("%-24s %4.2f %6d %5.1f %9s %10.1f %6.1f %6.1f\n", family, lead, a.cases, hitPct,
                meanMiss, medClosest, meanT, fovPct);
}

void PrintTableHead() {
    std::printf("%-24s %4s %6s %5s %9s %10s %6s %6s\n", "family", "lead", "cases", "hit%",
                "meanMiss", "medClosest", "meanT", "fov%");
    std::printf("------------------------ ----- ------ ----- --------- ---------- ------ ------\n");
}

// Run the whole scenario grid for one seeker configuration and print the tables.
void RunSweep(const std::vector<Scenario>& scenarios, const std::vector<std::string>& families,
              const SimConfig& cfg, const char* title, bool verbose) {
    std::printf("---- %s ----\n", title);
    std::printf("     seeker: FOV %.0f deg, turn %.0f deg/s, lifetime %.0f s\n", cfg.fovDeg,
                cfg.turnDegPerSec, cfg.lifeTimeSec);
    PrintTableHead();

    Agg total[3];
    Agg crossing[3];

    for (size_t f = 0; f < families.size(); ++f) {
        Agg perMode[3];
        for (int m = 0; m < 3; ++m) {
            for (const Scenario& sc : scenarios) {
                if (sc.familyId != static_cast<int>(f)) continue;
                const Outcome o = RunScenario(sc, kModes[m].lead, cfg);
                AddToAgg(perMode[m], o);
                AddToAgg(total[m], o);
                if (IsCrossingFamily(families[f])) AddToAgg(crossing[m], o);
                if (verbose) {
                    std::printf("  case %-17s R0=%4.0f bearing=%2.0f Vt=%3.0f turn=%2.0f  %-23s "
                                "hit=%d closest=%7.1f t=%5.2f fovLost=%d\n",
                                families[f].c_str(), sc.range0, sc.bearingDeg, sc.targetSpeed,
                                sc.targetTurnDegPerSec, kModes[m].label, o.hit ? 1 : 0,
                                o.closest, o.flightTime, o.fovLostFrames);
                }
            }
        }
        for (int m = 0; m < 3; ++m) PrintTableRow(families[f].c_str(), kModes[m].lead, perMode[m]);
    }

    std::printf("------------------------ ----- ------ ----- --------- ---------- ------ ------\n");
    for (int m = 0; m < 3; ++m) PrintTableRow("ALL FAMILIES", kModes[m].lead, total[m]);
    std::printf("-------------------------------------------------------------------------------\n");
    for (int m = 0; m < 3; ++m) PrintTableRow("  of which crossing/beam", kModes[m].lead, crossing[m]);
    std::printf("\n");

    // Human-readable summary of the same numbers.
    std::printf("%s summary (all %d scenarios):\n", title, total[0].cases);
    const float pHit = total[0].cases ? 100.0f * total[0].hits / total[0].cases : 0.0f;
    for (int m = 0; m < 3; ++m) {
        const Agg& a = total[m];
        const float hitPct = a.cases ? 100.0f * static_cast<float>(a.hits) / static_cast<float>(a.cases) : 0.0f;
        const float meanMiss = a.misses ? static_cast<float>(a.sumMissClosest / a.misses) : 0.0f;
        std::printf("  %-23s  hit %5.1f%%  (%+5.1f pp vs pursuit)  mean miss %7.1f m  %d/%d killed\n",
                    kModes[m].label, hitPct, hitPct - pHit, meanMiss, a.hits, a.cases);
    }
    const float pCross = crossing[0].cases ? 100.0f * crossing[0].hits / crossing[0].cases : 0.0f;
    std::printf("  crossing/beam subset:%s\n", "");
    for (int m = 0; m < 3; ++m) {
        const Agg& a = crossing[m];
        const float hitPct = a.cases ? 100.0f * static_cast<float>(a.hits) / static_cast<float>(a.cases) : 0.0f;
        const float meanMiss = a.misses ? static_cast<float>(a.sumMissClosest / a.misses) : 0.0f;
        std::printf("  %-23s  hit %5.1f%%  (%+5.1f pp vs pursuit)  mean miss %7.1f m  %d/%d killed\n",
                    kModes[m].label, hitPct, hitPct - pCross, meanMiss, a.hits, a.cases);
    }
    std::printf("\n");
}

int RunReport(bool verbose) {
    std::vector<Scenario> scenarios;
    std::vector<std::string> families;
    BuildScenarios(scenarios, families);

    std::printf("============================================================================================\n");
    std::printf(" ACZPropNav - proportional navigation engagement sweep (offline, planar 2-D)\n");
    std::printf(" missile: constant %.0f m/s   |   dt = 1/120 s   |   hit radius = %.0f m\n",
                kMissileSpeed, kHitRadius);
    std::printf(" target: straight line or constant-rate turn (0 / 6 / 15 deg/s); random-free, deterministic\n");
    std::printf("============================================================================================\n");
    std::printf("grid: %d scenarios = 9 geometries x ranges {700..2600} m x target speeds {200..900} m/s\n",
                static_cast<int>(scenarios.size()));
    std::printf("      x target turn rates, each flown with HomingForesightAmount = 0.0 / 0.7 / 1.0\n");
    std::printf("columns: hit%% = kill rate, meanMiss = mean closest approach over misses (m),\n");
    std::printf("         medClosest = median closest approach over all cases (m), meanT = mean time to kill,\n");
    std::printf("         fov%% = share of cases that lost the seeker cone at least once\n\n");

    SimConfig vanilla;
    RunSweep(scenarios, families, vanilla, "A: vanilla ACZ MSL seeker (FOV 75 deg)", verbose);

    SimConfig wideFov;
    wideFov.fovDeg = 360.0f;
    RunSweep(scenarios, families, wideFov,
             "B: FOV 360 deg (guidance quality with the cone out of the way)", false);

    std::printf("--------------------------------------------------------------------------------\n");
    std::printf("Reading the tables:\n");
    std::printf(" * meanMiss is conditioned on the misses, so it can look *worse* for better guidance:\n");
    std::printf("   PN converts the near misses into kills, leaving only the hopeless geometries in its\n");
    std::printf("   miss set. medClosest (over all cases) and hit%% are the unbiased comparisons.\n");
    std::printf(" * Scenarios where the missile cannot win even in theory (target at 900 m/s outrunning a\n");
    std::printf("   680 m/s missile, or a 9 s lifetime expiring mid-flight) score 0%% for every setting\n");
    std::printf("   and are kept as controls.\n");
    std::printf(" * Sweep B sets FOV = 360 deg: no seeker-cone losses, so it isolates the guidance law.\n");
    std::printf("   Sweep A is the vanilla 75 deg cone, i.e. the real missile.\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Self test helpers
// ---------------------------------------------------------------------------
int g_checks = 0;
int g_failures = 0;

std::string Fmt(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return std::string(buf);
}

void Check(bool ok, const char* name, const std::string& detail) {
    ++g_checks;
    if (ok) {
        std::printf("PASS  %s\n", name);
    } else {
        std::printf("FAIL  %s  [%s]\n", name, detail.c_str());
        ++g_failures;
    }
}

bool AllFinite(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool AllFinite(const GuidanceResult& r) {
    return AllFinite(r.desiredDir) && std::isfinite(r.losAngleDeg) && std::isfinite(r.turnDeg) &&
           std::isfinite(r.timeToGo);
}

float MaxCompDiff(const Vec3& a, const Vec3& b) {
    const float dx = std::fabs(a.x - b.x);
    const float dy = std::fabs(a.y - b.y);
    const float dz = std::fabs(a.z - b.z);
    return dx > dy ? (dx > dz ? dx : dz) : (dy > dz ? dy : dz);
}

GuidanceInput MakeTailChaseInput(float lateralOffset, float range0, float targetSpeed) {
    GuidanceInput in;
    in.missilePos = Vec3(0.0f, 0.0f, 0.0f);
    in.missileVel = Vec3(kMissileSpeed, 0.0f, 0.0f); // on +x boresight
    in.targetPos = Vec3(range0, lateralOffset, 0.0f);
    in.targetVel = Vec3(targetSpeed, 0.0f, 0.0f);
    in.dt = kSimDt;
    return in;
}

GuidanceInput MakeCrossingInput(float bearingDeg, float headingDeg, float range0,
                                float targetSpeed) {
    GuidanceInput in;
    in.missilePos = Vec3(0.0f, 0.0f, 0.0f);
    in.missileVel = Vec3(kMissileSpeed, 0.0f, 0.0f);
    const float b = DegToRad(bearingDeg), h = DegToRad(headingDeg);
    in.targetPos = Vec3(std::cos(b), std::sin(b), 0.0f) * range0;
    in.targetVel = Vec3(std::cos(h), std::sin(h), 0.0f) * targetSpeed;
    in.dt = kSimDt;
    return in;
}

GuidanceParams ParamsFor(float lead, float fovDeg = kVanillaFovDeg,
                         float turnDegPerSec = kVanillaTurnDegPerSec) {
    GuidanceParams gp;
    gp.homingForesightAmount = lead;
    gp.maxHomingAngleDeg = fovDeg;
    gp.maxRotationAngleDegPerSec = turnDegPerSec;
    gp.lifeTimeSec = kVanillaLifeTimeSec;
    return gp;
}

// (a) lead = 0 must be bit-exactly pure pursuit at every step of a tail chase.
void TestLeadZeroEqualsPursuit() {
    GuidanceInput in = MakeTailChaseInput(0.0f, 2000.0f, 300.0f);
    const GuidanceParams gp = ParamsFor(0.0f);
    int steps = 0;
    float maxDiff = 0.0f;
    bool allValid = true;
    bool allInFov = true;
    for (; steps < 1000; ++steps) {
        const GuidanceResult g = ComputeGuidance(in, gp);
        const Vec3 pursuit = PursuitDirection(in);
        const float diff = MaxCompDiff(g.desiredDir, pursuit);
        if (diff > maxDiff) maxDiff = diff;
        allValid = allValid && g.valid;
        allInFov = allInFov && g.inFov;
        if (!g.valid || !g.inFov) break;
        in.missileVel = g.desiredDir * kMissileSpeed;
        in.missilePos += in.missileVel * in.dt;
        in.targetPos += in.targetVel * in.dt;
        if (Length(in.targetPos - in.missilePos) <= kHitRadius) break;
    }
    Check(steps > 100 && allValid && allInFov && maxDiff == 0.0f,
          "(a) lead=0 pipeline == PursuitDirection exactly, every step of a tail chase",
          Fmt("steps=%d maxDiff=%.3g valid=%d inFov=%d", steps, maxDiff, allValid ? 1 : 0,
              allInFov ? 1 : 0));

    // Same invariant on an offset tail chase, but only on frames where the turn
    // is not rate limited (then desiredDir must be exactly pursuit).
    GuidanceInput off = MakeTailChaseInput(120.0f, 2000.0f, 300.0f);
    const float maxTurnDeg = gp.maxRotationAngleDegPerSec * off.dt;
    int offSteps = 0;
    float offMax = 0.0f;
    for (; offSteps < 1000; ++offSteps) {
        const GuidanceResult g = ComputeGuidance(off, gp);
        const Vec3 pursuit = PursuitDirection(off);
        if (g.turnDeg < maxTurnDeg - 1e-3f) {
            const float diff = MaxCompDiff(g.desiredDir, pursuit);
            if (diff > offMax) offMax = diff;
        }
        if (!g.valid || !g.inFov) break;
        off.missileVel = g.desiredDir * kMissileSpeed;
        off.missilePos += off.missileVel * off.dt;
        off.targetPos += off.targetVel * off.dt;
        if (Length(off.targetPos - off.missilePos) <= kHitRadius) break;
    }
    Check(offSteps > 100 && offMax <= 1e-6f,
          "(a2) lead=0 offset tail chase: desiredDir == pursuit whenever not rate limited",
          Fmt("steps=%d maxDiff=%.3g", offSteps, offMax));
}

// (b) at least one crossing/beam geometry where pursuit misses and full PN hits.
// The witness must be a *real* geometric miss (pursuit closest approach > 100 m),
// not a pursuit run that was 13 m away when its lifetime expired.
void TestPursuitMissesPnHits() {
    std::vector<Scenario> scenarios;
    std::vector<std::string> families;
    BuildScenarios(scenarios, families);
    const SimConfig cfg;

    bool found = false;
    int pnBetter = 0;
    int pursuitBetter = 0;
    Scenario witness{};
    Outcome witnessPursuit{};
    Outcome witnessPn{};
    for (const Scenario& sc : scenarios) {
        if (!IsCrossingFamily(families[static_cast<size_t>(sc.familyId)])) continue;
        const Outcome o0 = RunScenario(sc, 0.0f, cfg);
        const Outcome o1 = RunScenario(sc, 1.0f, cfg);
        if (!o0.hit && o1.hit) {
            ++pnBetter;
            if (!found && o0.closest > 100.0f) {
                found = true;
                witness = sc;
                witnessPursuit = o0;
                witnessPn = o1;
            }
        } else if (o0.hit && !o1.hit) {
            ++pursuitBetter;
        }
    }
    Check(found && pnBetter > pursuitBetter,
          "(b) crossing geometry where pure pursuit misses by >100 m but full PN hits",
          Fmt("strict witnesses=%d PN-only kills=%d pursuit-only kills=%d", found ? 1 : 0,
              pnBetter, pursuitBetter));
    if (found) {
        std::printf("      witness: %s  R0=%.0f m  bearing=%.0f deg  target heading=%.0f deg  "
                    "Vt=%.0f m/s  target turn=%.0f deg/s\n",
                    families[static_cast<size_t>(witness.familyId)].c_str(), witness.range0,
                    witness.bearingDeg, witness.targetHeadingDeg, witness.targetSpeed,
                    witness.targetTurnDegPerSec);
        std::printf("               pursuit: miss, closest %.1f m, flew %.2f s  |  PN: hit at %.2f s\n",
                    witnessPursuit.closest, witnessPursuit.flightTime, witnessPn.flightTime);
    }
    std::printf("      over crossing/beam scenarios: PN-only kills = %d, pursuit-only kills = %d\n",
                pnBetter, pursuitBetter);
}

// (c) the commanded turn is never larger than maxRotationAngleDegPerSec * dt.
void TestTurnRateLimit() {
    float worstTurnDeg = 0.0f;
    float worstMeasuredDeg = 0.0f;
    float limitDeg = 0.0f;
    bool allFinite = true;
    int steps = 0;

    const Scenario cases[] = {
        {0, 700.0f, 40.0f, 130.0f, 450.0f, 6.0f},
        {1, 800.0f, 50.0f, 140.0f, 500.0f, 15.0f},
        {2, 1000.0f, 35.0f, 125.0f, 300.0f, 0.0f},
        {3, 1200.0f, 60.0f, 180.0f, 200.0f, 6.0f},
    };
    for (int m = 0; m < 3; ++m) {
        for (const Scenario& sc : cases) {
            GuidanceInput in;
            in.missilePos = Vec3(0.0f, 0.0f, 0.0f);
            in.missileVel = Vec3(kMissileSpeed, 0.0f, 0.0f);
            const float b = DegToRad(sc.bearingDeg), h = DegToRad(sc.targetHeadingDeg);
            in.targetPos = Vec3(std::cos(b), std::sin(b), 0.0f) * sc.range0;
            in.targetVel = Vec3(std::cos(h), std::sin(h), 0.0f) * sc.targetSpeed;
            in.dt = kSimDt;
            const GuidanceParams gp = ParamsFor(kModes[m].lead);
            const float frameLimitDeg = gp.maxRotationAngleDegPerSec * in.dt;
            if (frameLimitDeg > limitDeg) limitDeg = frameLimitDeg;
            for (int s = 0; s < 2000; ++s) {
                const GuidanceResult g = ComputeGuidance(in, gp);
                if (!AllFinite(g)) allFinite = false;
                if (g.turnDeg > worstTurnDeg) worstTurnDeg = g.turnDeg;
                const Vec3 oldDir = NormalizedOr(in.missileVel, Vec3(1, 0, 0));
                const Vec3 newDir = NormalizedOr(g.desiredDir, oldDir);
                const float measuredDeg = RadToDeg(AngleBetween(oldDir, newDir));
                if (measuredDeg > worstMeasuredDeg) worstMeasuredDeg = measuredDeg;
                if (!g.valid || !g.inFov) break;
                in.missileVel = g.desiredDir * kMissileSpeed;
                in.missilePos += in.missileVel * in.dt;
                in.targetPos += in.targetVel * in.dt;
                in.targetVel = RotateZ(in.targetVel, DegToRad(sc.targetTurnDegPerSec) * in.dt);
                if (Length(in.targetPos - in.missilePos) <= kHitRadius) break;
                ++steps;
            }
        }
    }
    const float tol = 1e-3f;
    Check(allFinite && steps > 1000 && worstTurnDeg <= limitDeg + tol &&
              worstMeasuredDeg <= limitDeg + tol,
          "(c) turn per frame <= MaxRotationAngle * dt (commanded and measured)",
          Fmt("limit=%.4f deg worstCommanded=%.4f worstMeasured=%.4f steps=%d", limitDeg,
              worstTurnDeg, worstMeasuredDeg, steps));
}

// (d) target outside the seeker cone: no turn at all, heading unchanged.
void TestFovLoss() {
    GuidanceInput in = MakeCrossingInput(90.0f, 0.0f, 1500.0f, 0.0f); // target abeam
    const GuidanceParams gp = ParamsFor(1.0f, 30.0f);                  // narrow cone
    const GuidanceResult g = ComputeGuidance(in, gp);
    const Vec3 heading = NormalizedOr(in.missileVel, Vec3(1, 0, 0));
    const bool headingUnchanged = MaxCompDiff(g.desiredDir, heading) == 0.0f;
    Check(g.valid && !g.inFov && g.turnDeg == 0.0f && headingUnchanged && AllFinite(g),
          "(d) target outside MaxHomingAngle => inFov=false, turn=0, heading unchanged",
          Fmt("valid=%d inFov=%d turn=%.4f diff=%.3g losAngle=%.2f", g.valid ? 1 : 0,
              g.inFov ? 1 : 0, g.turnDeg, MaxCompDiff(g.desiredDir, heading), g.losAngleDeg));

    // A full 360 degree seeker must never lose the cone, even abeam.
    const GuidanceParams wide = ParamsFor(1.0f, 360.0f);
    const GuidanceResult gWide = ComputeGuidance(in, wide);
    Check(gWide.valid && gWide.inFov && gWide.turnDeg > 0.0f,
          "(d2) MaxHomingAngle >= 360 deg means always in FOV",
          Fmt("valid=%d inFov=%d turn=%.4f", gWide.valid ? 1 : 0, gWide.inFov ? 1 : 0,
              gWide.turnDeg));
}

// (e) degenerate geometry: valid=false, never NaN, no crash.
void TestDegenerate() {
    const GuidanceParams gp = ParamsFor(1.0f);

    GuidanceInput zeroVel = MakeTailChaseInput(0.0f, 1500.0f, 300.0f);
    zeroVel.missileVel = Vec3(0, 0, 0);

    GuidanceInput nanPos = MakeTailChaseInput(0.0f, 1500.0f, 300.0f);
    nanPos.targetPos.y = std::nanf("");

    GuidanceInput infVel = MakeTailChaseInput(0.0f, 1500.0f, 300.0f);
    infVel.missileVel.x = std::numeric_limits<float>::infinity();

    GuidanceInput coincident;
    coincident.missilePos = Vec3(10, 20, 30);
    coincident.targetPos = Vec3(10, 20, 30);

    GuidanceInput huge = MakeTailChaseInput(0.0f, 1500.0f, 300.0f);
    huge.targetPos = Vec3(3.0e38f, 3.0e38f, 0.0f); // overflows |r|^2

    GuidanceParams nanLead = gp;
    nanLead.homingForesightAmount = std::nanf("");
    GuidanceParams infTurn = gp;
    infTurn.maxRotationAngleDegPerSec = std::numeric_limits<float>::infinity();

    GuidanceInput negDt = MakeTailChaseInput(0.0f, 1500.0f, 300.0f);
    negDt.dt = -kSimDt;

    struct Case {
        const char* name;
        GuidanceInput in;
        GuidanceParams p;
    };
    const Case cases[] = {
        {"zero missile velocity", zeroVel, gp},
        {"NaN target position", nanPos, gp},
        {"inf missile velocity", infVel, gp},
        {"coincident positions", coincident, gp},
        {"overflowing range", huge, gp},
        {"NaN HomingForesightAmount", MakeTailChaseInput(0.f, 1500.f, 300.f), nanLead},
        {"inf MaxRotationAngle", MakeTailChaseInput(0.f, 1500.f, 300.f), infTurn},
        {"negative dt", negDt, gp},
    };

    int bad = 0;
    std::string detail;
    for (const Case& c : cases) {
        const GuidanceResult g = ComputeGuidance(c.in, c.p);
        const bool ok = !g.valid && AllFinite(g) && LengthSq(g.desiredDir) < 1.5f;
        if (!ok) {
            ++bad;
            detail += Fmt("%s(valid=%d finite=%d) ", c.name, g.valid ? 1 : 0,
                          AllFinite(g) ? 1 : 0);
        }
    }
    Check(bad == 0, "(e) degenerate / non-finite input => valid=false and no NaN",
          detail.empty() ? std::string("all 8 cases") : detail);

    // ComputeLos must also stay finite on coincident positions.
    Vec3 losUnit(9, 9, 9);
    float losRate = 9.0f, closing = 9.0f;
    ComputeLos(coincident, losUnit, losRate, closing);
    Check(AllFinite(losUnit) && std::isfinite(losRate) && std::isfinite(closing) &&
              LengthSq(losUnit) == 0.0f && losRate == 0.0f && closing == 0.0f,
          "(e2) ComputeLos on coincident positions => zero LOS, zero rates, finite",
          Fmt("rate=%.3g closing=%.3g |los|=%.3g", losRate, closing, Length(losUnit)));
}

// (f) a lead strictly between 0 and 1 lands strictly between the two endpoints.
void TestBlendBetween() {
    // Crossing geometry: pursuit and the full lead point differ clearly.
    GuidanceInput in = MakeCrossingInput(35.0f, 125.0f, 1600.0f, 300.0f);
    const Vec3 pursuit = PursuitDirection(in);
    float tGo = 0.0f;
    const Vec3 leadDir = ProportionalNavigationDirection(in, 1.0f, tGo);
    const float angEndpoints = AngleBetween(pursuit, leadDir);

    bool ok = angEndpoints > DegToRad(1.0f); // endpoints must really differ
    std::string detail = Fmt("endpoints=%.3f rad tGo=%.2f", angEndpoints, tGo);

    const float weights[2] = {0.25f, 0.7f};
    for (float w : weights) {
        GuidanceParams gp = ParamsFor(w, kVanillaFovDeg, 1.0e9f); // no rate limiting
        const GuidanceResult g = ComputeGuidance(in, gp);
        const float angPursuitToBlend = AngleBetween(pursuit, g.desiredDir);
        const float angBlendToLead = AngleBetween(g.desiredDir, leadDir);
        const bool between = angPursuitToBlend > 1e-5f && angBlendToLead > 1e-5f &&
                             std::fabs(angPursuitToBlend - w * angEndpoints) < 1e-4f &&
                             std::fabs(angBlendToLead - (1.0f - w) * angEndpoints) < 1e-4f;
        ok = ok && between && g.mode == GuidanceMode::Blended && g.turnDeg > 0.0f;
        detail += Fmt(" | w=%.2f p->b=%.4f b->l=%.4f", w, angPursuitToBlend, angBlendToLead);
    }
    Check(ok, "(f) lead in (0,1) is angularly strictly between pursuit and full PN", detail);
}

// (g) LOS rate, closing speed and the N * lambda_dot rate law.
void TestLosAndRateLaw() {
    GuidanceInput crossing = MakeCrossingInput(0.0f, 90.0f, 1000.0f, 300.0f);
    crossing.targetPos = Vec3(1000.0f, 100.0f, 0.0f); // not exactly collinear
    crossing.targetVel = Vec3(0.0f, 300.0f, 0.0f);    // clean beam-on crossing

    Vec3 losUnit;
    float losRate = 0.0f, closing = 0.0f;
    ComputeLos(crossing, losUnit, losRate, closing);

    // Closed form: r=(1000,100), v_rel = v_t - v_m = (-680,300)
    //   |r x v| = 1000*300 - 100*(-680) = 368000,  |r|^2 = 1010000
    //   lambda_dot = 368000 / 1010000
    //   closing = dot(v_m - v_t, rHat) = (680*1000 - 300*100) / |r|
    const float expectedRate = 368000.0f / 1010000.0f;
    const float rlen = std::sqrt(1010000.0f);
    const float expectedClosing = (680.0f * 1000.0f - 300.0f * 100.0f) / rlen;
    const bool losOk = std::fabs(losRate - expectedRate) < 1e-6f &&
                       std::fabs(closing - expectedClosing) < 1e-3f && AllFinite(losUnit) &&
                       std::fabs(Length(losUnit) - 1.0f) < 1e-5f;
    Check(losOk, "(g) ComputeLos rate/closing match the closed-form values",
          Fmt("rate=%.7f exp=%.7f closing=%.4f exp=%.4f", losRate, expectedRate, closing,
              expectedClosing));

    // N = 4 rate law: magnitude 4 * losRate, sign toward the LOS rotation.
    // Here the target crosses to +y, so the LOS rotates CCW (+z) and the missile
    // must turn CCW => the signed command is positive.
    const float cmd = PnRateCommand(crossing, kPnNavConstant);
    Check(cmd > 0.0f && std::fabs(cmd - 4.0f * expectedRate) < 1e-5f,
          "(g2) PnRateCommand = +N * lambda_dot (positive = toward the rotating LOS)",
          Fmt("cmd=%.7f exp=%.7f", cmd, 4.0f * expectedRate));

    // Head-on: closing speed positive (range shrinking); receding: negative.
    GuidanceInput headOn = MakeCrossingInput(0.0f, 180.0f, 1500.0f, 300.0f);
    float headClosing = 0.0f;
    ComputeLos(headOn, losUnit, losRate, headClosing);
    GuidanceInput away = MakeCrossingInput(0.0f, 0.0f, 1500.0f, 900.0f); // outruns the missile
    float awayClosing = 0.0f;
    ComputeLos(away, losUnit, losRate, awayClosing);
    Check(headClosing > 0.0f && awayClosing < 0.0f,
          "(g3) closingSpeed > 0 head-on, < 0 when the target outruns the missile",
          Fmt("head-on=%.1f receding=%.1f", headClosing, awayClosing));

    // Receding target: no closing solution => tGo = 0 and the lead direction
    // must collapse to pure pursuit.
    float tGo = 1234.0f;
    const Vec3 leadDir = ProportionalNavigationDirection(away, 1.0f, tGo);
    const Vec3 pursuit = PursuitDirection(away);
    Check(tGo == 0.0f && MaxCompDiff(leadDir, pursuit) == 0.0f,
          "(g4) receding target => tGo = 0 and lead direction == pursuit direction",
          Fmt("tGo=%.1f diff=%.3g", tGo, MaxCompDiff(leadDir, pursuit)));

    // In a perfect tail chase the LOS does not rotate at all.
    GuidanceInput pureTail = MakeTailChaseInput(0.0f, 1500.0f, 300.0f);
    ComputeLos(pureTail, losUnit, losRate, closing);
    Check(losRate < 1e-6f, "(g5) collinear tail chase => LOS rotation rate ~ 0",
          Fmt("rate=%.3g", losRate));
}

// (h) determinism: the same case flown twice must give identical numbers.
void TestDeterminism() {
    const Scenario sc{0, 1400.0f, 40.0f, 130.0f, 450.0f, 6.0f};
    const SimConfig cfg;
    const Outcome a = RunScenario(sc, 1.0f, cfg);
    const Outcome b = RunScenario(sc, 1.0f, cfg);
    Check(a.hit == b.hit && a.closest == b.closest && a.flightTime == b.flightTime &&
              a.steps == b.steps && a.fovLostFrames == b.fovLostFrames,
          "(h) simulator is deterministic (same case twice -> identical outcome)",
          Fmt("closest %.6f/%.6f t %.6f/%.6f", a.closest, b.closest, a.flightTime, b.flightTime));
}

// (i) plain-MSL PN steering math (src/pn/msl_steer.h, used by the exe+0x13ECA0 hook).
// The plain-MSL slot stores a PER-FRAME displacement, so this checks the frame-time
// recovery, the turn-rate clamp, the lead blend and the degenerate cases.
void TestMslSteer() {
    using msl_steer::Compute;
    using msl_steer::Input;
    using msl_steer::Output;

    // Target slightly off-axis: turn toward it, keep the speed, clamp the turn.
    {
        Input in;
        in.p = Vec3(0.0f, 0.0f, 0.0f);
        in.v = Vec3(1.0f, 0.0f, 0.0f);  // 1 m/frame
        in.target = Vec3(100.0f, 10.0f, 0.0f);
        in.targetVel = Vec3();
        in.speedMps = 60.0f;  // => dt_game = 1/60 s
        in.amount = 1.0f;
        in.maxRotationDegPerSec = 180.0f;  // => thetaMax = 3 deg
        Output out;
        const bool ok = Compute(in, out);
        const float speedOut = Length(out.vNew);
        Check(ok, "(i1) msl_steer::Compute valid for a simple pursuit geometry",
              Fmt("ok=%d", ok ? 1 : 0));
        Check(std::fabs(speedOut - 1.0f) < 1e-4f, "(i2) PN keeps the per-frame speed",
              Fmt("|vNew|=%.6f", speedOut));
        const float turned = AngleBetween(in.v, out.vNew);
        Check(std::fabs(turned - out.thetaMax) < 1e-4f && out.theta > out.thetaMax,
              "(i3) turn clamped to MaxRotationAngle * dt_game",
              Fmt("turn=%.5f limit=%.5f theta=%.5f", turned, out.thetaMax, out.theta));
        const Vec3 toward = NormalizedOr(in.target - in.p, Vec3(1.0f, 0.0f, 0.0f));
        Check(Dot(out.vNew, toward) > Dot(in.v, toward),
              "(i4) new direction is closer to the target", "");
        Check(std::fabs(out.dtGame - 1.0f / 60.0f) < 1e-6f,
              "(i5) dt_game recovered as |v| / speedMps", Fmt("dt=%.6f", out.dtGame));
    }

    // Lead: with a crossing target and no rate limit, amount=1 must turn further
    // toward the intercept than amount=0 (pure pursuit).
    {
        Input base;
        base.p = Vec3();
        base.v = Vec3(10.0f, 0.0f, 0.0f);
        base.target = Vec3(1000.0f, 10.0f, 0.0f);
        base.targetVel = Vec3(0.0f, 5.0f, 0.0f);  // 300 m/s lateral at 60 Hz
        base.speedMps = 600.0f;
        base.maxRotationDegPerSec = 1.0e6f;  // effectively unlimited
        Input withLead = base;
        withLead.amount = 1.0f;
        Input pure = base;
        pure.amount = 0.0f;
        Output ol;
        Output op;
        const bool okl = Compute(withLead, ol);
        const bool okp = Compute(pure, op);
        Check(okl && okp, "(i6) both amount variants compute", "");
        const float delta = AngleBetween(ol.vNew, op.vNew);
        Check(delta > 1e-3f, "(i7) lead (amount 1) changes the commanded direction",
              Fmt("delta=%.5f rad", delta));
        Check(ol.tGo > 1.0f && ol.tGo < 600.0f, "(i8) tGo is a sane positive frame count",
              Fmt("tGo=%.2f", ol.tGo));
        const Vec3 dirPursuit = NormalizedOr(base.target - base.p, Vec3(1.0f, 0.0f, 0.0f));
        Check(AngleBetween(op.vNew, dirPursuit) < 1e-3f,
              "(i9) amount=0 => pure pursuit at the target's current position", "");
        Check(std::fabs(ol.tGo - op.tGo) < 1e-6f,
              "(i10) tGo is shared by both variants (lead only moves the aim point)", "");
    }

    // Receding target: closing floored at 0.2|v|, tGo bounded.
    {
        Input in;
        in.p = Vec3();
        in.v = Vec3(10.0f, 0.0f, 0.0f);
        in.target = Vec3(1000.0f, 0.0f, 0.0f);
        in.targetVel = Vec3(100.0f, 0.0f, 0.0f);  // outruns the missile
        in.speedMps = 600.0f;
        in.amount = 1.0f;
        in.maxRotationDegPerSec = 180.0f;
        Output out;
        const bool ok = Compute(in, out);
        Check(ok && std::fabs(out.closing - 2.0f) < 1e-4f,
              "(i11) receding target: closing floored at 0.2|v|",
              Fmt("closing=%.5f", out.closing));
        Check(ok && out.tGo > 400.0f && out.tGo <= 600.0f,
              "(i12) receding target: tGo clamped to <= 600 frames", Fmt("tGo=%.2f", out.tGo));
    }

    // dt_game clamps + degenerate inputs.
    {
        Input fast;
        fast.p = Vec3();
        fast.v = Vec3(0.5f, 0.0f, 0.0f);
        fast.target = Vec3(100.0f, 1.0f, 0.0f);
        fast.speedMps = 1000.0f;  // 0.0005 s -> clamp to 1/240
        Output out;
        Compute(fast, out);
        Check(std::fabs(out.dtGame - 1.0f / 240.0f) < 1e-6f,
              "(i13) dt_game lower clamp is 1/240 s", Fmt("dt=%.7f", out.dtGame));

        Input zero;
        zero.p = Vec3();
        zero.v = Vec3();
        zero.target = Vec3(100.0f, 0.0f, 0.0f);
        Output o2;
        Check(!Compute(zero, o2), "(i14) zero velocity is rejected", "");

        Input same;
        same.p = Vec3();
        same.v = Vec3(1.0f, 0.0f, 0.0f);
        same.target = Vec3();
        Output o3;
        Check(!Compute(same, o3), "(i15) target on top of the missile is rejected", "");

        Input nan = same;
        nan.target = Vec3(std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f);
        Output o4;
        Check(!Compute(nan, o4), "(i16) non-finite target is rejected", "");
    }
}

// (j) Step 1B orientation helpers: axis-angle from two unit vectors, matrix axis
// identification (the runtime calibration uses exactly these), world->local, and a
// quaternion-matrix reference for the rotation itself.
void TestStep1B() {
    using msl_steer::MatrixAxis;
    using msl_steer::PickForwardAxis;
    using msl_steer::RotateByRotVec;
    using msl_steer::RotVecFromTo;
    using msl_steer::WorldToLocal;

    // rotvec from +z to +x is +y * pi/2 and rotating +z by it lands on +x.
    {
        const Vec3 from(0.0f, 0.0f, 1.0f);
        const Vec3 to(1.0f, 0.0f, 0.0f);
        const Vec3 rv = RotVecFromTo(from, to, 3.14159265f);
        Check(std::fabs(Length(rv) - 1.5707963268f) < 1e-5f,
              "(j1) RotVecFromTo(z, x) has magnitude pi/2", Fmt("|rv|=%.6f", Length(rv)));
        Check(MaxCompDiff(RotateByRotVec(from, rv), to) < 1e-5f,
              "(j2) rotating +z by that rotvec gives +x", "");
    }

    // Degenerate + clamp cases.
    {
        Check(Length(RotVecFromTo(Vec3(0, 0, 1), Vec3(0, 0, 1), 3.14f)) < 1e-6f,
              "(j3) identical directions -> zero rotvec", "");
        Check(Length(RotVecFromTo(Vec3(0, 0, 1), Vec3(0, 0, -1), 3.14f)) < 1e-6f,
              "(j4) antiparallel directions -> zero rotvec (no unique axis)", "");
        const Vec3 near(0.0f, std::sin(0.2f), std::cos(0.2f));  // 0.2 rad from +z
        const Vec3 rv = RotVecFromTo(Vec3(0, 0, 1), near, 0.05f);
        Check(std::fabs(Length(rv) - 0.05f) < 1e-5f,
              "(j5) rotvec magnitude is clamped to maxAngle", Fmt("|rv|=%.6f", Length(rv)));
    }

    // Matrix axis identification on a known y-rotation (row-major Ry(theta)):
    // forward = column 2 = (sin, 0, cos).
    const float th = 0.6f;
    const float c = std::cos(th), sn = std::sin(th);
    float m[16] = {c, 0.f, sn, 0.f, 0.f, 1.f, 0.f, 0.f, -sn, 0.f, c, 0.f, 0.f, 0.f, 0.f, 1.f};
    {
        const Vec3 heading(sn, 0.0f, c);
        float score = 0.0f;
        const int axis = PickForwardAxis(m, heading, &score);
        Check(axis == 2 && std::fabs(score - 1.0f) < 1e-5f,
              "(j6) PickForwardAxis finds column 2 of a y-rotation",
              Fmt("axis=%d score=%.6f", axis, static_cast<double>(score)));
        Check(MaxCompDiff(MatrixAxis(m, 2), heading) < 1e-6f,
              "(j7) MatrixAxis(2) returns that column", "");
        // REGRESSION: the row candidates used a stride of 3 instead of 4, so
        // candidate 4 read (m[3],m[4],m[5]) and candidate 5 read (m[6],m[7],m[8]) -
        // mixing in the w column and missing the real rows entirely. Nobody noticed
        // because the calibration usually picks a column candidate; this pins both.
        {
            // A matrix built with a distinct value per element, so a wrong stride is
            // guaranteed to change the result.
            float mm[16];
            for (int i = 0; i < 16; ++i) mm[i] = static_cast<float>(i + 1);
            const Vec3 row0 = MatrixAxis(mm, 3);
            const Vec3 row1 = MatrixAxis(mm, 4);
            const Vec3 row2 = MatrixAxis(mm, 5);
            Check(MaxCompDiff(row0, Vec3(1, 2, 3)) < 1e-6f &&
                      MaxCompDiff(row1, Vec3(5, 6, 7)) < 1e-6f &&
                      MaxCompDiff(row2, Vec3(9, 10, 11)) < 1e-6f,
                  "(j7b) MatrixAxis rows use stride 4 (not 3)",
                  Fmt("row1=(%g %g %g) row2=(%g %g %g)", static_cast<double>(row1.x),
                      static_cast<double>(row1.y), static_cast<double>(row1.z),
                      static_cast<double>(row2.x), static_cast<double>(row2.y),
                      static_cast<double>(row2.z)));
            // And the transpose relationship must hold for every axis candidate:
            // matrix rows are identity-image under WorldToLocal only when consistent.
            Check(MaxCompDiff(MatrixAxis(mm, 3), Vec3(mm[0], mm[1], mm[2])) < 1e-6f,
                  "(j7c) MatrixAxis row0 == the first four floats' xyz", "");
        }
        Check(MaxCompDiff(WorldToLocal(m, heading), Vec3(0, 0, 1)) < 1e-5f,
              "(j8) WorldToLocal maps the world heading back to +z", "");
        Check(MaxCompDiff(WorldToLocal(m, MatrixAxis(m, 0)), Vec3(1, 0, 0)) < 1e-5f &&
                  MaxCompDiff(WorldToLocal(m, MatrixAxis(m, 1)), Vec3(0, 1, 0)) < 1e-5f &&
                  MaxCompDiff(WorldToLocal(m, MatrixAxis(m, 2)), Vec3(0, 0, 1)) < 1e-5f,
              "(j9) WorldToLocal is the inverse of the row-major 3x3 part", "");
    }

    // Rotation vs an independent quaternion->matrix reference.
    {
        const Vec3 from = NormalizedOr(Vec3(0.2f, 0.9f, -0.3f), Vec3(0, 0, 1));
        const Vec3 to = NormalizedOr(Vec3(-0.5f, 0.3f, 0.8f), Vec3(0, 0, 1));
        const Vec3 rv = RotVecFromTo(from, to, 3.14159265f);
        const Vec3 rod = RotateByRotVec(from, rv);
        Check(MaxCompDiff(rod, to) < 1e-4f, "(j10) rotvec maps the source heading to the target",
              Fmt("diff=%.3g", MaxCompDiff(rod, to)));
        const float ang = Length(rv);
        Vec3 axis;
        if (Normalize(rv, axis)) {
            const float w = std::cos(ang * 0.5f), s2 = std::sin(ang * 0.5f);
            const float x = axis.x * s2, y = axis.y * s2, z = axis.z * s2;
            float R[9] = {1.f - 2.f * (y * y + z * z), 2.f * (x * y - w * z), 2.f * (x * z + w * y),
                          2.f * (x * y + w * z), 1.f - 2.f * (x * x + z * z), 2.f * (y * z - w * x),
                          2.f * (x * z - w * y), 2.f * (y * z + w * x), 1.f - 2.f * (x * x + y * y)};
            const Vec3 ref(R[0] * from.x + R[1] * from.y + R[2] * from.z,
                           R[3] * from.x + R[4] * from.y + R[5] * from.z,
                           R[6] * from.x + R[7] * from.y + R[8] * from.z);
            Check(MaxCompDiff(rod, ref) < 2e-5f,
                  "(j11) Rodrigues rotation equals the quaternion-matrix reference",
                  Fmt("diff=%.3g", MaxCompDiff(rod, ref)));
        } else {
            Check(false, "(j11) Rodrigues rotation equals the quaternion-matrix reference",
                  "empty rotvec");
        }
    }
}

// (k) STEP 2: true lambda-dot PN blended with one-frame pure pursuit.
void TestPn() {
    using msl_steer::ComputePn;
    using msl_steer::PnInput;
    using msl_steer::PnOutput;
    const float kSpeed = 10.0f;  // m/frame

    // amount=0 -> pure pursuit: the command points at the current target.
    {
        PnInput in;
        in.p = Vec3();
        in.v = Vec3(kSpeed, 0, 0);
        in.target = Vec3(500, 120, 0);
        in.targetVel = Vec3(0, 4, 0);
        in.gameFps = 60.f;
        in.amount = 0.f;
        in.maxRotationDegPerSec = 1e6f;
        PnOutput out;
        const bool ok = ComputePn(in, out);
        Vec3 uR;
        Normalize(in.target - in.p, uR);
        Vec3 dirV;
        Normalize(out.vNew, dirV);
        Check(ok && Dot(dirV, uR) > 0.9999f,
              "(k1) amount=0 commands pure pursuit at the target",
              Fmt("dot=%.6f", static_cast<double>(Dot(dirV, uR))));
        Check(ok && std::fabs(Length(out.vNew) - kSpeed) < 1e-4f,
              "(k2) |vNew| == |v| (speed preserved)",
              Fmt("|vNew|=%.6f", static_cast<double>(Length(out.vNew))));
    }

    // Rate limit: turn == thetaMax and never more.
    {
        PnInput in;
        in.p = Vec3();
        in.v = Vec3(1.f, 0, 0);
        in.target = Vec3(100, 10, 0);
        in.gameFps = 60.f;
        in.amount = 0.f;
        in.maxRotationDegPerSec = 40.f;
        PnOutput out;
        const bool ok = ComputePn(in, out);
        // dtGame is now exactly 1/gameFps (the engine's own constant), so the
        // per-frame budget is rot*pi/180/fps - the same convention the vanilla
        // clamp uses (lead*pi/180/60), with no position-unit conversion involved.
        const float expected = 40.f * (1.f / 60.f) * 0.017453292519943295f;
        // turnRad is re-measured with acos() after composing the rotation, so allow a
        // few ulp of slack between it and the analytic budget.
        Check(ok && std::fabs(out.turnRad - expected) < 1e-4f &&
                  std::fabs(out.thetaMax - expected) < 1e-6f,
              "(k3) turn rate-limited to MaxRotationAngle / gameFps",
              Fmt("turn=%.6f limit=%.6f", static_cast<double>(out.turnRad),
                  static_cast<double>(out.thetaMax)));
    }

    // Collision course, amount=1: Vr parallel to R -> lambdaDot 0 -> no turn.
    {
        PnInput in;
        in.p = Vec3();
        const Vec3 v(10.f * std::cos(0.35f), 10.f * std::sin(0.35f), 0.f);
        in.v = v;
        in.target = Vec3(1000, 0, 0);
        in.targetVel = v + Vec3(9.f, 0, 0);
        in.gameFps = 60.f;
        in.amount = 1.f;
        in.maxRotationDegPerSec = 1e6f;
        PnOutput out;
        const bool ok = ComputePn(in, out);
        Check(ok && out.lambdaDot < 1e-7f, "(k4) collision course -> |lambdaDot| ~ 0",
              Fmt("|lambda|=%.3g", static_cast<double>(out.lambdaDot)));
        Check(ok && out.turnRad < 1e-7f && out.omegaPursuit > 1e-3f,
              "(k5) ... so amount=1 commands no turn although the heading is off-LOS",
              Fmt("turn=%.3g pursuit=%.4f", static_cast<double>(out.turnRad),
                  static_cast<double>(out.omegaPursuit)));
    }

    // Crossing target: N monotonic + turn axis follows Cross(R,Vr).
    {
        PnInput base;
        base.p = Vec3();
        base.v = Vec3(kSpeed, 0, 0);
        base.target = Vec3(500, 400, 0);
        base.targetVel = Vec3(0, -5, 0);
        base.gameFps = 60.f;
        base.amount = 1.f;
        base.maxRotationDegPerSec = 1e6f;
        float turns[4] = {0, 0, 0, 0};
        bool ok = true;
        for (int n = 1; n <= 4; ++n) {
            PnInput in = base;
            in.navConstant = static_cast<float>(n);
            PnOutput out;
            ok = ok && ComputePn(in, out);
            turns[n - 1] = out.turnRad;
            if (n == 4) {
                const Vec3 R = base.target - base.p;
                const Vec3 Vr = base.targetVel - base.v;
                const float rmin = (2.f * kSpeed > 25.f) ? 2.f * kSpeed : 25.f;
                const float rr = LengthSq(R);
                const float r2 = (rr > rmin * rmin) ? rr : rmin * rmin;
                const Vec3 lam = Cross(R, Vr) * (1.f / r2);
                Vec3 uV, d, rotAxis, lamAxis;
                Normalize(base.v, uV);
                Normalize(out.vNew, d);
                Normalize(Cross(uV, d), rotAxis);
                Normalize(lam, lamAxis);
                Check(Dot(rotAxis, lamAxis) > 0.9f,
                      "(k6) turn axis follows Cross(R,Vr) (LOS-rate direction)",
                      Fmt("dot=%.4f", static_cast<double>(Dot(rotAxis, lamAxis))));
            }
        }
        Check(ok, "(k7) PN computes for every N", "");
        Check(turns[0] > 0.f && turns[1] > turns[0] && turns[2] > turns[1] &&
                  turns[3] > turns[2],
              "(k8) commanded turn grows monotonically with N",
              Fmt("1..4: %.5f %.5f %.5f %.5f", static_cast<double>(turns[0]),
                  static_cast<double>(turns[1]), static_cast<double>(turns[2]),
                  static_cast<double>(turns[3])));
    }

    // Blend: with a PN-dominated geometry amount=1 turns more than amount=0.
    // (A large fast crossing with a small heading error: the LOS rate demand dominates
    // the one-frame pursuit demand, so the blend direction is visible.)
    {
        PnInput base;
        base.p = Vec3();
        base.v = Vec3(kSpeed, 0, 0);
        base.target = Vec3(3000, 30, 0);
        base.targetVel = Vec3(0, 50, 0);
        base.gameFps = 60.f;
        base.maxRotationDegPerSec = 1e6f;
        PnInput p0 = base;
        p0.amount = 0.f;
        PnInput p1 = base;
        p1.amount = 1.f;
        PnOutput o0, o1;
        Check(ComputePn(p0, o0) && ComputePn(p1, o1) && o1.turnRad > o0.turnRad,
              "(k9) amount blends pursuit(0) -> PN(1)",
              Fmt("turn0=%.5f turn1=%.5f", static_cast<double>(o0.turnRad),
                  static_cast<double>(o1.turnRad)));
    }

    // Close range: the rmin guard keeps lambdaDot/vNew finite.
    {
        PnInput in;
        in.p = Vec3();
        in.v = Vec3(1.f, 0, 0);
        in.target = Vec3(10, 0, 0);  // r = 10 < rmin = 25
        in.targetVel = Vec3(0, 3, 0);
        in.gameFps = 60.f;
        in.amount = 1.f;
        in.maxRotationDegPerSec = 180.f;
        PnOutput out;
        const bool ok = ComputePn(in, out);
        Check(ok && std::isfinite(out.lambdaDot) && std::isfinite(out.vNew.x) &&
                  std::fabs(Length(out.vNew) - Length(in.v)) < 1e-4f,
              "(k10) r < rmin: guard keeps lambdaDot finite and preserves speed",
              Fmt("|lambda|=%.4f |vNew|=%.4f", static_cast<double>(out.lambdaDot),
                  static_cast<double>(Length(out.vNew))));
    }

    // Degenerate inputs rejected.
    {
        PnInput z;
        z.p = Vec3();
        z.v = Vec3();
        z.target = Vec3(100, 0, 0);
        PnOutput o;
        Check(!ComputePn(z, o), "(k11) zero missile speed is rejected", "");
        PnInput c;
        c.p = Vec3();
        c.v = Vec3(1.f, 0, 0);
        c.target = Vec3(0.01f, 0, 0);
        PnOutput o2;
        Check(!ComputePn(c, o2), "(k12) r <= 0.05 m is rejected", "");
    }

    // (k11a) REGRESSION: the per-frame turn budget must NOT be derived from the
    // missile's displacement. The slot positions are in the game's own position units
    // (1 m == 5.12 units, proven by the engine's range kill at 0x1402D46B0), so
    // `dt = |v| / speedMps` mixed two different unit systems. A live run logged
    // |v| = 60.37 units/frame: 60.37/1000 = 0.060 s, clamped to the 1/20 ceiling,
    // i.e. a budget 3x larger than the real 1/60 s frame. The visible symptom was
    // `limit=4.50deg` for a 90 deg/s setting (correct: 1.50 deg). The frame time now
    // comes from the engine's own fps constant, so this assertion pins the mapping.
    {
        PnInput in;
        in.p = Vec3();
        in.v = Vec3(60.37f, 0, 0);       // exactly the live-log displacement
        in.target = Vec3(1000, 200, 0);
        in.targetVel = Vec3(0, 2, 0);
        in.amount = 1.f;
        in.maxRotationDegPerSec = 90.f;
        in.gameFps = 60.f;
        PnOutput out;
        const bool ok = ComputePn(in, out);
        Check(ok && std::fabs(out.dtGame - 1.f / 60.f) < 1e-6f,
              "(k11a1) frame time is 1/gameFps, independent of the displacement units",
              Fmt("dt=%.6f (want %.6f)", static_cast<double>(out.dtGame),
                  static_cast<double>(1.0 / 60.0)));
        const float wantLimitDeg = 90.f / 60.f;   // 1.50 deg, NOT 4.50
        Check(ok && std::fabs(out.thetaMax * 57.29577951308232f - wantLimitDeg) < 1e-3f,
              "(k11a2) 90 deg/s at 60 fps is a 1.50 deg/frame budget (was 3x too big)",
              Fmt("limit=%.4f deg (want %.4f)", static_cast<double>(out.thetaMax) * 57.29577951308232,
                  static_cast<double>(wantLimitDeg)));
    }

    // (k11b) REGRESSION: the blend must interpolate, not cancel.
    //
    // Two wrong formulations lived here before. (1) averaging the pursuit and PN
    // *rotation vectors*: they are usually not collinear (median angle ~45 deg), so
    // the weighted sum of opposing vectors collapses - in this geometry it commanded
    // 0.012 deg/frame at amount=0.5 while both endpoints commanded 3.0 deg/frame.
    // (2) composing the two rotations in sequence: the instantaneous sum is fine but
    // the laws still disagree, so the net swept angle came out ~0 again.
    // The shipped formulation interpolates the two resulting DIRECTIONS along the
    // great circle between them, which is well defined for any pair of endpoints.
    //
    // Invariants asserted here:
    //   * amount=0 reproduces the pure-pursuit direction exactly,
    //   * amount=1 reproduces the PN direction exactly,
    //   * the blended heading sits a*span of the way from pursuit to PN (monotone),
    //   * everything stays rate limited (<= thetaMax from the current heading).
    {
        const float step = 1000.f / 60.f;
        const float br = 5.f * 0.017453292519943295f;
        const float th = 285.f * 0.017453292519943295f;
        auto make = [&](float amount) {
            msl_steer::PnInput in;
            in.p = Vec3();
            in.v = Vec3(step, 0, 0);
            in.target = Vec3(std::cos(br) * 500.f, std::sin(br) * 500.f, 0.f);
            in.targetVel = Vec3(std::cos(th), std::sin(th), 0.f) * (750.f / 60.f);
            in.gameFps = 60.f;
            in.amount = amount;
            in.maxRotationDegPerSec = 180.f;
            in.navConstant = 4.f;
            return in;
        };
        PnOutput o0, o1;
        const bool okEndpoints = ComputePn(make(0.f), o0) && ComputePn(make(1.f), o1);
        Vec3 d0, d1;
        Normalize(o0.vNew, d0);
        Normalize(o1.vNew, d1);
        const float span = AngleBetween(d0, d1);
        Check(okEndpoints && span > 0.01f,
              "(k11b1) blend regression geometry has distinguishable endpoints",
              Fmt("span=%.4f deg", static_cast<double>(span) * 57.29577951308232));

        bool monotone = true;
        float worstFracErr = 0.f;
        float maxTurn = 0.f;
        for (float a : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
            PnOutput o;
            if (!ComputePn(make(a), o)) { monotone = false; continue; }
            Vec3 d;
            Normalize(o.vNew, d);
            const float fromPursuit = AngleBetween(d0, d);
            const float expect = a * span;
            worstFracErr = (std::fabs(fromPursuit - expect) > worstFracErr)
                               ? std::fabs(fromPursuit - expect)
                               : worstFracErr;
            if (o.turnRad > maxTurn) maxTurn = o.turnRad;
        }
        Check(monotone && worstFracErr < 1e-4f,
              "(k11b2) blended heading sits exactly a*span from pursuit toward PN",
              Fmt("worst |err|=%.6f rad", static_cast<double>(worstFracErr)));
        // Rate limit must hold for every blend value in this (opposing) geometry too.
        const float limit = make(0.f).maxRotationDegPerSec > 0.1f
                                ? DegToRad(180.f) * (step / 1000.f)
                                : 0.f;
        Check(maxTurn <= limit + 1e-6f,
              "(k11b3) every blend value stays inside the per-frame turn budget",
              Fmt("maxTurn=%.5f limit=%.5f", static_cast<double>(maxTurn),
                  static_cast<double>(limit)));
    }

    // (k11c) The blended direction must never exceed the per-frame turn budget.
    // Each endpoint is clamped to thetaMax, but a great-circle interpolation between
    // two clamped directions could in principle bulge outside it, which would silently
    // bypass MaxRotationAngle. Swept over ~100k geometries (all amounts, both laws,
    // 10..360 deg/s, ranges 50..4000 m, target speeds 0..700 m/s) there are zero
    // violations; this assertion keeps it that way.
    {
        int violations = 0;
        float worstOver = 0.f;
        for (float a = 0.f; a <= 1.0001f; a += 0.25f) {
            for (float rot : {10.f, 60.f, 180.f, 360.f}) {
                for (float br = -180.f; br <= 180.f; br += 30.f) {
                    for (float th = -180.f; th <= 180.f; th += 45.f) {
                        for (float r = 100.f; r <= 3000.f; r += 700.f) {
                            for (float ts = 0.f; ts <= 600.f; ts += 200.f) {
                                PnInput in;
                                in.p = Vec3();
                                in.v = Vec3(1000.f / 60.f, 0, 0);
                                const float brr = br * 0.017453292519943295f;
                                const float thh = th * 0.017453292519943295f;
                                in.target = Vec3(std::cos(brr) * r, std::sin(brr) * r, 0.f);
                                in.targetVel =
                                    Vec3(std::cos(thh), std::sin(thh), 0.f) * (ts / 60.f);
                                in.gameFps = 60.f;
                                in.amount = a;
                                in.maxRotationDegPerSec = rot;
                                PnOutput o;
                                if (!ComputePn(in, o)) continue;
                                const float over = o.turnRad - o.thetaMax;
                                if (over > 1e-5f) {
                                    ++violations;
                                    if (over > worstOver) worstOver = over;
                                }
                            }
                        }
                    }
                }
            }
        }
        Check(violations == 0,
              "(k11c) blended heading never exceeds the per-frame turn budget",
              Fmt("violations=%d worst=%.7f rad", violations,
                  static_cast<double>(worstOver)));
    }

    // (k14) Exactly anti-parallel heading/LOS must still produce a turn.
    // A missile flying directly away from its target (theta = pi) makes
    // Cross(uV,uR) vanish, so the pursuit demand used to be exactly zero and the
    // missile flew straight forever. Any perpendicular axis is a valid choice here;
    // what matters is that the command is non-zero and is rate limited.
    {
        float turns[4] = {0, 0, 0, 0};
        const float amounts[4] = {0.f, 0.5f, 1.f, 0.f};
        bool allValid = true;
        for (int k = 0; k < 3; ++k) {
            PnInput in;
            in.p = Vec3();
            in.v = Vec3(10.f, 0, 0);
            in.target = Vec3(-500.f, 0, 0);          // exactly behind
            in.targetVel = Vec3();
            in.gameFps = 60.f;
            in.amount = amounts[k];
            in.maxRotationDegPerSec = 180.f;
            PnOutput out;
            allValid = allValid && ComputePn(in, out);
            turns[k] = out.turnRad;
        }
        Check(allValid, "(k14a) anti-parallel geometry still computes", "");
        Check(std::fabs(turns[0] - DegToRad(180.f) / 60.f) < 1e-4f,
              "(k14b) amount=0 with the target dead astern turns by the full budget",
              Fmt("turn=%.6f rad", static_cast<double>(turns[0])));
        Check(turns[0] > 1e-4f && turns[0] <= DegToRad(180.f) / 60.f + 1e-4f,
              "(k14c) that turn is non-zero and rate limited", "");
    }

    // (k15) REGRESSION: the target-step sanity bound must be expressed in POSITION
    // UNITS, and it must reject a teleport rather than clamp-and-keep its direction.
    //
    // The live game stores positions in units where 1 m == 5.12 units (proven by the
    // engine's range kill at 0x1402D46B0, and cross-checked against a real log: the
    // distance between two logged position vectors was 6704.8 while the same frame
    // printed |R| = 6705, i.e. no 5.12 factor is applied anywhere in this path). The
    // old bound was `20.f` commented as "20 m/frame == 1200 m/s"; but 20 units/frame
    // is only ~234 m/s, so every real target exceeded it EVERY frame, had its speed
    // under-estimated, and had its EMA history reset each time.
    {
        using msl_steer::SanitizeTargetStep;
        const float kUnitsPerMetre = 5.12f;
        const float kMaxSpeedMps = 1200.f;
        const float bound = kMaxSpeedMps / 60.f * kUnitsPerMetre;   // ~102.4 units/frame

        // A 600 m/s target moves 51.2 units/frame: comfortably inside the bound, yet
        // it was over the OLD 20-unit bound. This is the assertion that pins the bug.
        const float realStep = 600.f / 60.f * kUnitsPerMetre;       // 51.2
        Vec3 out;
        const bool kept = SanitizeTargetStep(Vec3(realStep, 0, 0), bound, out);
        Check(kept && std::fabs(Length(out) - realStep) < 1e-4f,
              "(k15a) a 600 m/s target's step is kept, not clamped away",
              Fmt("len=%.3f", static_cast<double>(Length(out))));
        Check(realStep > 20.f,
              "(k15b) ...and it IS above the old hard-coded 20-unit bound (the bug)",
              Fmt("step=%.2f units/frame", static_cast<double>(realStep)));

        // 1200 m/s exactly at the bound must survive; slighly beyond must not.
        const bool atBound = SanitizeTargetStep(Vec3(bound, 0, 0), bound, out);
        Check(atBound, "(k15c) a step exactly at the bound is accepted", "");
        const bool beyond = SanitizeTargetStep(Vec3(bound * 1.5f, 0, 0), bound, out);
        Check(!beyond && Length(out) == 0.f,
              "(k15d) a teleport is rejected and yields a ZERO vector, not a clamped one",
              Fmt("kept=%d len=%.3f", beyond ? 1 : 0, static_cast<double>(Length(out))));

        // Non-finite samples must never reach the guidance maths.
        const float inf = std::numeric_limits<float>::infinity();
        Check(!SanitizeTargetStep(Vec3(inf, 0, 0), bound, out) && Length(out) == 0.f,
              "(k15e) an infinite step is rejected as unknown", "");
        const float nan = std::numeric_limits<float>::quiet_NaN();
        Check(!SanitizeTargetStep(Vec3(nan, nan, nan), bound, out) && Length(out) == 0.f,
              "(k15f) a NaN step is rejected as unknown", "");
    }

    // (k13) The rmin guard must bound lambda-dot through the whole terminal phase.
    // This is the invariant that makes the old 30 m "terminal hand-back" unnecessary:
    // a real ACZ MSL steps ~16.7 m/frame, so rmin = max(2*|v|, 25) = 33.3 m and the
    // guard engages slightly BEFORE the range at which the hand-back used to fire.
    // Sweep the range from far to point-blank on a deliberate near miss and check
    // that the commanded turn never runs away while the speed is preserved.
    {
        const float step = 1000.f / 60.f;      // 16.667 m/frame (vanilla MSL)
        const float rmin = (2.f * step > 25.f) ? 2.f * step : 25.f;
        float maxLambdaInGuard = 0.f;
        float worstTurn = 0.f;
        bool allFinite = true;
        bool allSpeedKept = true;
        for (float r = 400.f; r > 0.5f; r *= 0.9f) {
            PnInput in;
            in.p = Vec3();
            in.v = Vec3(step, 0, 0);
            in.target = Vec3(r, 25.f, 0);       // 25 m lateral offset -> never a clean hit
            in.targetVel = Vec3(0, 400.f / 60.f, 0);
            in.gameFps = 60.f;
            in.amount = 1.f;
            in.maxRotationDegPerSec = 180.f;
            PnOutput out;
            if (!ComputePn(in, out)) continue;
            if (!std::isfinite(out.lambdaDot) || !std::isfinite(out.vNew.x) ||
                !std::isfinite(out.vNew.y) || !std::isfinite(out.vNew.z)) {
                allFinite = false;
            }
            if (std::fabs(Length(out.vNew) - step) > 1e-3f) allSpeedKept = false;
            if (r < rmin && out.lambdaDot > maxLambdaInGuard) maxLambdaInGuard = out.lambdaDot;
            if (out.turnRad > worstTurn) worstTurn = out.turnRad;
        }
        Check(allFinite, "(k13a) terminal sweep: lambdaDot/vNew stay finite down to r<1 m", "");
        Check(allSpeedKept, "(k13b) terminal sweep: |vNew| == |v| at every range", "");
        // Inside the guard the LOS rate is bounded by |Vr| / rmin, not by 1/r^2.
        const float bound = (400.f / 60.f + step) / rmin;
        Check(maxLambdaInGuard <= bound * 1.05f,
              "(k13c) r<rmin: |lambdaDot| bounded by |Vr|/rmin (no 1/r^2 blow-up)",
              Fmt("max=%.5f bound=%.5f", static_cast<double>(maxLambdaInGuard),
                  static_cast<double>(bound)));
        Check(worstTurn <= 1e-6f || worstTurn < 1.5708f,
              "(k13d) terminal sweep: commanded turn stays below a quarter turn",
              Fmt("worst=%.4f rad", static_cast<double>(worstTurn)));
    }
}

} // namespace

int main(int argc, char** argv) {
    bool selftest = false;
    bool report = false;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--selftest") == 0) selftest = true;
        else if (std::strcmp(a, "--report") == 0) report = true;
        else if (std::strcmp(a, "-v") == 0 || std::strcmp(a, "--verbose") == 0) verbose = true;
        else if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0) {
            std::printf("usage: pnsim [--selftest] [--report] [-v]\n"
                        "  --selftest  run assertions, exit non-zero on failure\n"
                        "  --report    sweep of engagement geometries (default)\n"
                        "  -v          per-case lines with the report\n");
            return 0;
        } else {
            std::printf("pnsim: unknown argument '%s' (try --help)\n", a);
            return 2;
        }
    }
    if (!selftest && !report) report = true;

    int selftestCode = 0;
    if (selftest) {
        std::printf("=== pnsim self test: pn.cpp guidance core + plain-MSL steer math ===\n");
        TestLeadZeroEqualsPursuit();
        TestPursuitMissesPnHits();
        TestTurnRateLimit();
        TestFovLoss();
        TestDegenerate();
        TestBlendBetween();
        TestLosAndRateLaw();
        TestDeterminism();
        TestMslSteer();
        TestStep1B();
        TestPn();
        std::printf("--- %d checks, %d failure(s) ---\n\n", g_checks, g_failures);
        selftestCode = g_failures == 0 ? 0 : 1;
    }

    if (report) {
        RunReport(verbose);
    }
    return selftestCode;
}
