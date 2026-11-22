// ACZPropNav - patcher implementation.
//
// Design notes
//  * We never trust a memory address we have not verified; RequireVerifiedOffsets
//    defaults to 1 and a missing verification degrades to "log only".
//  * Every write is read-back verified and counted.
//  * The guidance hook is deliberately fixed-ABI (this-pointer, optional float dt,
//    MSVC x64 fastcall): Probe mode is used first to confirm the candidate.
#include "core/patcher.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <initializer_list>

#include "core/log.h"
#include "core/vanilla_records.h"
#include "hook/hook.h"
#include "mem/mem.h"
#include "pn/msl_pn.h"
#include "pn/pn.h"

#include <psapi.h>


namespace acz {

namespace {

// ---- built-in vanilla signature table -------------------------------------
// Used when the INI has no [vanilla] section. The numbers are the ACZ weapon
// record values documented in docs/VANILLA.md (data/mis/mpl/*.wpt schema +
// shipping *.wpb), mapped onto the AC8 property names our INI uses:
//     HomingForesightAmount -> lead      (frames of lead, 60 Hz)
//     MaxHomingAngle        -> startAng  (seeker start angle, degrees)
//     LifeTime              -> range     (missile range, metres)
//     LockonRange           -> scope     (lock-on scope, metres)
//     MaxRotationAngle      -> (no field in the data records)
// A game update can be re-calibrated without rebuilding via [vanilla].
struct BuiltinModel {
    const char* key;
    float leadFrames;
    float startAngDeg;
    float rangeM;
    float scopeM;
};

const BuiltinModel kBuiltin[] = {
    // player standard missile (a.arc: data/mis/mpl/mis.wpb, referenced by every
    // playable aircraft); see docs/VANILLA.md for the full 10-record table.
    {"msl", 40.f, 90.f, 3000.f, 1600.f},
    // wingman missile: same struct, different lead/startAng/range. Its only job
    // here is to make a layout hypothesis prove itself against two models.
    {"misf", 80.f, 60.f, 2000.f, 1600.f},
    // second wingman variant (lead 28) - extra corroboration when present.
    {"misf2", 28.f, 30.f, 2000.f, 1600.f},
};

// Records whose key marks them as NPC/wingman ordnance. Those take the [enemy]
// settings; ACZ ships wingman missiles as separate records (misf/misf2).
bool IsNpcKey(const std::string& key) {
    const std::string k = LowerAscii(key);
    if (k == "misf" || k == "misf2" || k == "gunf" || k == "gunf2") return true;
    return k.find("npc") != std::string::npos || k.find("wing") != std::string::npos ||
           k.find("enemy") != std::string::npos;
}

// Map a record key onto the key users write in the INI: ACZ keeps wingman
// missiles in their own records, but players think of them as "the MSL".
std::string ConfigKeyFor(const std::string& recordKey) {
    const std::string k = LowerAscii(recordKey);
    if (k == "misf" || k == "misf2") return "msl";
    if (k == "gunf" || k == "gunf2") return "gun";
    return k;
}

double Now() {
    LARGE_INTEGER f{}, c{};
    static double freq = 0.0;
    if (freq == 0.0) {
        ::QueryPerformanceFrequency(&f);
        freq = (f.QuadPart > 0) ? static_cast<double>(f.QuadPart) : 1.0;
    }
    ::QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / freq;
}

void RelTol(float base, float pct, float minAbs, game::KnownValue& kv) {
    kv.value = base;
    kv.tolerance = std::max(base * pct, minAbs);
}

// Turn a FieldSet (vanilla values) into a ModelSignature. Requires at least two
// of the record-backed properties; lead + range/scope is the strongest pair.
bool BuildSignature(const std::string& key, const FieldSet& f, game::ModelSignature& out) {
    out.modelKey = LowerAscii(key);
    out.values.clear();
    if (f.homingForesightAmount) {
        game::KnownValue kv;
        kv.prop = game::Property::HomingForesightAmount;      // ACZ `lead`
        RelTol(*f.homingForesightAmount, 0.02f, 1.0f, kv);
        out.values.push_back(kv);
    }
    if (f.maxHomingAngle) {
        game::KnownValue kv;
        kv.prop = game::Property::MaxHomingAngle;             // ACZ `startAng`
        RelTol(*f.maxHomingAngle, 0.02f, 0.5f, kv);
        out.values.push_back(kv);
    }
    if (f.lifeTime) {
        game::KnownValue kv;
        kv.prop = game::Property::LifeTime;                   // ACZ `range`
        RelTol(*f.lifeTime, 0.02f, 5.0f, kv);
        out.values.push_back(kv);
    }
    if (f.lockonRange) {
        game::KnownValue kv;
        kv.prop = game::Property::LockonRange;                // ACZ `scope`
        RelTol(*f.lockonRange, 0.02f, 1.0f, kv);
        out.values.push_back(kv);
    }
    // MaxRotationAngle has no field in the ACZ records: it is intentionally not
    // part of any signature (it is the PN override's rate limit instead).
    return out.values.size() >= 2;
}

Patcher* g_patcher = nullptr;

// ---------------------------------------------------------------------------
// Our own image. The mod's static tables (kBuiltin, vanilla_records) contain the
// very float patterns the discovery engine searches for, so its own module must
// be excluded from every scan - otherwise we would "find" and patch ourselves.
// ---------------------------------------------------------------------------
struct OwnModule {
    uintptr_t base = 0;
    size_t size = 0;
};

OwnModule QueryOwnModule() {
    OwnModule m;
    HMODULE self = nullptr;
    if (!::GetModuleHandleExW(0x00000004u | 0x00000002u,
                              reinterpret_cast<LPCWSTR>(&QueryOwnModule), &self) ||
        !self) {
        return m;
    }
    MODULEINFO mi{};
    if (::GetModuleInformation(::GetCurrentProcess(), self, &mi, sizeof(mi))) {
        m.base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
        m.size = mi.SizeOfImage;
    }
    return m;
}

const OwnModule& Own() {
    static const OwnModule module = QueryOwnModule();
    return module;
}

bool InsideOwnModule(uintptr_t addr) {
    const OwnModule& m = Own();
    return m.base != 0 && addr >= m.base && addr < m.base + m.size;
}

// The detour signature we support by default: MSVC x64 this-call with an optional
// float dt in xmm1. HookArgCount=1 means "this only".
typedef void (*GuidanceFn)(void* self, float dt);
GuidanceFn g_trampoline = nullptr;
void* g_pnTrampoline = nullptr;  // trampoline of the PN integrator hook

int g_hookArgCount = 2;

void __cdecl GuidanceDetour(void* self, float dt) {
    if (g_trampoline) {
        if (g_hookArgCount >= 2) {
            g_trampoline(self, dt);
        } else {
            reinterpret_cast<void (*)(void*)>(g_trampoline)(self);
        }
    }
    if (g_patcher) g_patcher->OnGuidanceHookHit(reinterpret_cast<uintptr_t>(self));
}

} // namespace

Patcher::Patcher() { g_patcher = this; }

Patcher::~Patcher() {
    Shutdown();
    if (g_patcher == this) g_patcher = nullptr;
}

bool Patcher::Start(const Config& cfg, const std::wstring& gameDir) {
    cfg_ = cfg;
    gameDir_ = gameDir;
    startStamp_ = Now();

    mem::SetProcess(::GetCurrentProcess());
    mem::ModuleInfo mi;
    if (!mem::MainModule(mi)) {
        L().Error("无法获取主模块 / cannot query the main module");
        return false;
    }
    L().Info("主模块 / main module: base=%s size=0x%llX", mem::Hex(mi.base).c_str(),
             static_cast<unsigned long long>(mi.size));
    L().Info("  path: %s", WideToUtf8(mi.path).c_str());

    // "Run A" read-only live probe (docs/ARCHITECTURE.md).  It owns its own Tick
    // from here on and does the two observation hooks + sampling itself.
    if (cfg_.runtime.probe) {
        L().Info("Probe=1：启动 Run A 只读探针 / starting the Run A read-only live probe");
        probe_.Start(mi.base);
    }
    // "Run B" hardware read-watch probe (docs/ARCHITECTURE.md).  Not meant to run
    // together with Run A (both want the allocator hook), so Run A takes precedence.
    if (cfg_.runtime.hwWatch) {
        L().Info("HwWatch=1：启动 Run B 硬件观察探针（Dr0..Dr3 只读数据断点）/ starting the Run B "
                 "hardware read-watch probe");
        if (cfg_.runtime.probe) {
            L().Warn("HwWatch=1 与 Probe=1 同时打开：Run B 跳过自己的分配钩子 / both probes enabled, "
                     "Run B skips the allocator hook");
        }
        watch_.Start(mi.base, cfg_.runtime.hwWatchSeconds, /*installModelHook=*/!cfg_.runtime.probe);
    }

    // The static-parameter path is opt-in: it only runs when the config declares
    // [vanilla] values. Reverse engineering proved the shipping build re-serialises
    // the .wpb records into engine structs (docs/MSL_PN.md), so scanning
    // for the on-disk layout is a guaranteed - and expensive - miss.
    staticPatch_ = !cfg_.vanilla.empty();
    if (staticPatch_) {
        RunDiscovery(true);
        ApplyStaticParams();
    } else {
        L().Info("静态参数路径未启用（INI 没有 [vanilla] 段）：该版本游戏把 .wpb 解析成运行时结构，"
                 "按磁盘布局扫描不会命中，因此跳过全内存扫描。");
        L().Info("Static-parameter scan skipped (no [vanilla] section): this build re-serialises the "
                 ".wpb records, so the on-disk layout is not searchable. Nothing is scanned.");
    }

    // Diagnostic survey: which code feeds the missile trail ring? (plain-MSL hunt)
    ringSurveyWanted_ = cfg_.runtime.ringSurvey != 0;
    if (ringSurveyWanted_) {
        L().Info("RingSurvey=1: 只读观察 EFF_MIS_POSDIR 尾迹环的写入点，用来定位普通 MSL 的每帧"
                 "推进函数 / read-only observation of the missile trail-ring producers");
    }

    if (cfg_.runtime.guidanceOverride && !cfg_.runtime.probe) {
        // Primary path: hook the shared steering integrator and apply PN. This is the
        // fully reverse-engineered route (docs/ARCHITECTURE.md), no calibration.
        if (!cfg_.runtime.hookRva && cfg_.runtime.hookSignature.empty()) {
            // acz.exe is SteamStub-wrapped: our DLL is already resident while the
            // imports are resolved, i.e. BEFORE the stub decrypts .text. Installing
            // the hook now would either fail the byte check or patch ciphertext, so
            // it is deferred to Tick(), which retries until the real bytes appear.
            const FieldSet* fs = cfg_.Resolve(Side::Player, "msl");
            const float lead = (fs && fs->homingForesightAmount) ? *fs->homingForesightAmount : 0.f;
            pendingHook_ = true;
            lastHookTry_ = 0.0;
            hookTries_ = 0;
            L().Info("PN 准备就绪，等待 .text 解密后安装钩子（每 200 ms 重试，最多 60 s）"
                     " / PN armed; waiting for the SteamStub-decrypted .text before hooking");
            if (cfg_.runtime.dryRun) {
                L().Info("DryRun=1：钩子装好后只记录不写 / dry-run: hook is read-only");
            } else if (lead > 0.001f) {
                L().Info("HomingForesightAmount=%.2f（[player] msl）", lead);
            } else {
                L().Warn("HomingForesightAmount 为 0（[player] msl 留空？），PN 不会改写导弹轨迹。");
                L().Warn("HomingForesightAmount is 0 (blank in [player] msl?) - PN will not steer.");
            }
        } else if (!InstallGuidanceHook()) {
            L().Warn("自定义 HookRva/HookSignature 未生效 / custom hook target not usable");
        }
    } else if (!cfg_.runtime.guidanceOverride) {
        L().Info("GuidanceOverride=0：不接管制导 / guidance override disabled");
    }

    // Plain-MSL proportional navigation (Run B result, docs/MSL_PN.md): a post-update
    // hook on FUN_14013ECA0. Additive to the WeaponEx integrator hook above (the
    // plain MSL never reaches 0x90510), and never armed in probe runs.
    if (cfg_.runtime.guidanceOverride && !cfg_.runtime.probe && !cfg_.runtime.hwWatch) {
        if (msl_pn::Wanted(cfg_)) {
            msl_pn::Configure(cfg_, mi.base);
        } else {
            L().Info("普通 MSL 比例引导未启用（[player]/[enemy] 的 msl 没有 HomingForesightAmount）"
                     " / plain-MSL PN not armed (no HomingForesightAmount for msl on either side)");
        }
    }
    return true;
}

void Patcher::RunDiscovery(bool force) {
    if (!force && !rows_.empty()) return;
    lastDiscovery_ = Now();
    rows_.clear();
    discoveryDiag_.clear();

    // Fast path: the shipping records are a fixed 96-byte struct loaded verbatim
    // from data/mis/mpl/*.wpb - find them by byte pattern (docs/VANILLA.md).
    if (ScanVanillaRecords()) {
        discoveryBackoff_ = 3.0;
        LogRows();
        return;
    }
    if (lightScanOnly_) {
        L().Info("已连续 %d 次未命中，只做轻量扫描（每 120 s）/ %d misses, light scan only", scanAttempts_,
                 scanAttempts_);
        return;
    }

    game::DiscoveryOptions opt;
    opt.maxAnchorHits = 2048;  // bound the worst-case cost on a multi-GB heap
    opt.maxRows = 128;
    // Prefer [vanilla] from the INI; otherwise the built-in table.
    if (!cfg_.vanilla.empty()) {
        for (const auto& m : cfg_.vanilla) {
            game::ModelSignature sig;
            if (BuildSignature(m.key, m.player, sig)) opt.models.push_back(sig);
        }
        L().Info("扫描签名来自 [vanilla] / signatures from the [vanilla] section: %d",
                 static_cast<int>(opt.models.size()));
    }
    if (opt.models.empty()) {
        for (const auto& b : kBuiltin) {
            FieldSet f;
            f.homingForesightAmount = b.leadFrames;
            f.maxHomingAngle = b.startAngDeg;
            f.lifeTime = b.rangeM;
            f.lockonRange = b.scopeM;
            game::ModelSignature sig;
            if (BuildSignature(b.key, f, sig)) opt.models.push_back(sig);
        }
        L().Info("扫描签名使用内置表 / signatures from the built-in table (docs/VANILLA.md): %d",
                 static_cast<int>(opt.models.size()));
    }

    game::ParamTableResult result;
    discoveryDiag_.clear();
    const bool ok = game::LocateParameterTables(opt, result, discoveryDiag_);
    for (const auto& d : discoveryDiag_) L().Key("%s", d.c_str());

    rows_.clear();
    stats_.hypotheses = static_cast<int>(result.hypotheses.size());
    stats_.rowsFound = static_cast<int>(result.rows.size());
    int ownRows = 0;
    for (const auto& r : result.rows) {
        if (InsideOwnModule(r.rowAddr)) { ++ownRows; continue; }  // our own tables
        if (r.verified || !cfg_.runtime.requireVerifiedOffsets) rows_.push_back(r);
    }
    if (ownRows > 0) {
        L().Key("  忽略 %d 行位于本模组自身镜像内的假阳性 / dropped %d self-image false positives",
                ownRows, ownRows);
    }
    stats_.rowsVerified = static_cast<int>(rows_.size());

    L().Info("参数表搜索：%s，行 %d（已验证 %d），基准 %s，步长 0x%X",
             ok ? "完成" : "未命中", stats_.rowsFound, stats_.rowsVerified,
             result.tableBase ? mem::Hex(result.tableBase).c_str() : "0", result.stride);
    LogRows();
    if (rows_.empty()) {
        L().Warn("没有找到可信的导弹参数表 / no verified missile parameter table found");
        L().Warn("  · 如果游戏刚更新过，请在 ACZPropNav.ini 的 [vanilla] 段填入正确原版值");
        L().Warn("  · If the game was updated, fill the real vanilla values into [vanilla]");
    } else {
        discoveryBackoff_ = 3.0;
    }
}

bool Patcher::ScanVanillaRecords() {
    using namespace acz::vanilla;
    // Needle: explPers (1.0) immediately followed by scope (1600) - both are
    // stable across the shipped missile records, so one scan finds all of them.
    uint8_t needle[8];
    std::memcpy(needle, kRecords[0].bytes + kExplPers, sizeof(needle));
    const uint32_t needleOff = kExplPers;

    std::vector<uintptr_t> hits;
    // Search EVERY readable region, not only writable ones: the game may keep the
    // weapon data in a read-only mapped view of the .arc or in a read-only heap
    // block, and the first live run proved writable-only was not enough.
    std::vector<mem::Region> regions;
    mem::EnumRegions(regions, 0x1000);
    size_t scanned = 0;
    for (const auto& r : regions) {
        if (!r.readable) continue;
        if (InsideOwnModule(r.base)) continue;  // never scan our own image
        scanned += r.size;
        mem::ScanBytes(r, needle, "xxxxxxxx", sizeof(needle), hits, 8192);
        if (hits.size() >= 8192) {
            L().Warn("记录扫描命中过多（%d），已截断 / too many pattern hits, truncated",
                     static_cast<int>(hits.size()));
            break;
        }
    }
    L().Info("记录字节扫描：扫描 %.1f MB，候选 %d 个（data/mis/mpl 指纹）",
             static_cast<double>(scanned) / (1024.0 * 1024.0), static_cast<int>(hits.size()));

    std::vector<game::ParamRow> found;
    int rejected = 0;
    for (uintptr_t hit : hits) {
        if (hit < needleOff) continue;
        const uintptr_t base = hit - needleOff;
        if (InsideOwnModule(base)) { ++rejected; continue; }
        uint8_t buf[kRecordSize];
        if (!mem::ReadBytes(base, buf, sizeof(buf))) { ++rejected; continue; }
        const uint32_t dataOff = *reinterpret_cast<const uint32_t*>(buf + kDataOff);
        if (dataOff != 0x10) { ++rejected; continue; }
        if (buf[kType] != 2) { ++rejected; continue; }  // 2 = missile

        auto asF32 = [&](uint32_t off) {
            float v = 0.f;
            std::memcpy(&v, buf + off, 4);
            return v;
        };
        const float memScope = asF32(kScope);
        const float memLead = asF32(kLead);
        const float memRange = asF32(kRange);
        const float memTimeWait = asF32(kTimeWait);
        const uint16_t memSpeed = *reinterpret_cast<const uint16_t*>(buf + kSpeed);
        const uint8_t memStartAng = buf[kStartAng];
        if (!std::isfinite(memScope) || !std::isfinite(memLead) || !std::isfinite(memRange)) {
            ++rejected;
            continue;
        }
        if (memScope < 100.f || memScope > 20000.f) { ++rejected; continue; }
        if (memLead < 0.f || memLead > 2000.f) { ++rejected; continue; }
        if (memSpeed == 0 || memSpeed > 5000) { ++rejected; continue; }

        // Classify against the shipped records. Required: scope, lead, startAng,
        // speed must match; range/timeWait are allowed to differ (other data mods
        // legitimately change them).
        int best = -1, bestScore = -1;
        for (int i = 0; i < kRecordCount; ++i) {
            const auto& rec = kRecords[i];
            auto recF32 = [&](uint32_t off) {
                float v = 0.f;
                std::memcpy(&v, rec.bytes + off, 4);
                return v;
            };
            int score = 0;
            if (std::fabs(recF32(kScope) - memScope) <= std::max(recF32(kScope) * 0.02f, 1.f)) ++score;
            if (std::fabs(recF32(kLead) - memLead) <= std::max(recF32(kLead) * 0.02f, 0.5f)) ++score;
            if (rec.bytes[kStartAng] == memStartAng) ++score;
            if (*reinterpret_cast<const uint16_t*>(rec.bytes + kSpeed) == memSpeed) ++score;
            if (std::fabs(recF32(kRange) - memRange) <= std::max(recF32(kRange) * 0.02f, 1.f)) ++score;
            if (std::fabs(recF32(kTimeWait) - memTimeWait) <= 0.5f) ++score;
            if (score > bestScore) { bestScore = score; best = i; }
        }
        if (best < 0 || bestScore < 4) { ++rejected; continue; }

        game::ParamRow row;
        row.rowAddr = base;
        row.modelKey = kRecords[best].key;
        row.score = bestScore;
        row.verified = true;
        row.slots.push_back({game::Property::HomingForesightAmount, kLead, memLead});
        row.slots.push_back({game::Property::MaxHomingAngle, kStartAng, static_cast<float>(memStartAng)});
        row.slots.push_back({game::Property::LifeTime, kRange, memRange});
        row.slots.push_back({game::Property::LockonRange, kScope, memScope});
        found.push_back(row);

        if (found.size() <= 24) {
            L().Key("  记录 / record %s @%s  lead=%.1f startAng=%.0f range=%.0f scope=%.0f "
                    "speed=%u  (match %d/6)",
                    row.modelKey.c_str(), mem::Hex(base).c_str(), memLead,
                    static_cast<double>(memStartAng), memRange, memScope, memSpeed, bestScore);
        }
        if (found.size() >= 1024) break;
    }

    if (found.empty()) {
        L().Warn("记录字节扫描没有命中（候选 %d，排除 %d）——游戏数据可能被其它 mod 改过，"
                 "改用数值相关算法 / pattern scan found nothing (hits %d, rejected %d), "
                 "falling back to the value correlator",
                 static_cast<int>(hits.size()), rejected, static_cast<int>(hits.size()), rejected);
        return false;
    }

    // Group by model so logging is readable.
    for (const auto& r : found) rows_.push_back(r);
    int perKey[kRecordCount] = {};
    for (const auto& r : rows_) {
        for (int i = 0; i < kRecordCount; ++i) {
            if (r.modelKey == kRecords[i].key) ++perKey[i];
        }
    }
    std::string summary;
    for (int i = 0; i < kRecordCount; ++i) {
        if (perKey[i] == 0) continue;
        if (!summary.empty()) summary += ", ";
        summary += kRecords[i].key;
        summary += " x" + std::to_string(perKey[i]);
    }
    L().Info("记录字节扫描成功：%s（排除 %d 个假阳性）", summary.c_str(), rejected);
    stats_.rowsFound = static_cast<int>(rows_.size());
    stats_.rowsVerified = static_cast<int>(rows_.size());
    return true;
}

// Dump the neighbourhood of rare vanilla values. If the engine converts the
// .wpb record into its own struct, the values survive (possibly rescaled) and
// this dump shows the runtime layout around them. Cheap, decisive, diagnostic.
// Parse "48 8B 05 ?? ?? ??" into bytes + mask ('x' / '?'). Returns false on syntax
// errors. A leading run of wildcards is allowed (the community addons start every
// patch signature with 5 wildcards so it survives its own hook).
bool ParseSignature(const std::string& text, std::vector<uint8_t>& bytes, std::string& mask) {
    bytes.clear();
    mask.clear();
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
        if (i >= text.size()) break;
        if (i + 1 < text.size() && text[i] == '?' && text[i + 1] == '?') {
            bytes.push_back(0);
            mask.push_back('?');
            i += 2;
            continue;
        }
        auto hexVal = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        if (i + 1 >= text.size()) return false;
        const int hi = hexVal(text[i]), lo = hexVal(text[i + 1]);
        if (hi < 0 || lo < 0) return false;
        bytes.push_back(static_cast<uint8_t>((hi << 4) | lo));
        mask.push_back('x');
        i += 2;
    }
    return bytes.size() >= 5 && bytes.size() <= 512;
}

uintptr_t Patcher::ResolveSignature(const std::string& pattern) {
    std::vector<uint8_t> bytes;
    std::string mask;
    if (!ParseSignature(pattern, bytes, mask)) {
        L().Warn("HookSignature 语法错误 / malformed HookSignature: %s", pattern.c_str());
        return 0;
    }
    mem::ModuleInfo mi;
    if (!mem::MainModule(mi)) return 0;
    std::vector<uintptr_t> hits;
    for (const auto& r : mem::ExecutableRegions(0x1000)) {
        if (r.base < mi.base || r.base >= mi.base + mi.size) continue;  // main module only
        mem::ScanBytes(r, bytes.data(), mask.c_str(), bytes.size(), hits, 8);
        if (!hits.empty()) break;
    }
    if (hits.size() != 1) {
        L().Warn("HookSignature 命中 %d 次（应为 1）/ %d matches (expected exactly 1)",
                 static_cast<int>(hits.size()));
        if (hits.size() > 1) return 0;
    }
    return hits.empty() ? 0 : hits[0];
}

void Patcher::ProbeMissileSlots() {
    mem::ModuleInfo mi;
    if (!mem::MainModule(mi)) return;
    const uintptr_t weaponModelVtable = mi.base + 0x2AE790;  // build 3802B0DC...

    std::vector<mem::Region> regions;
    mem::EnumRegions(regions, 0x1000);
    std::vector<uintptr_t> models;
    for (const auto& r : regions) {
        if (!r.readable || InsideOwnModule(r.base)) continue;
        mem::ScanBytes(r, reinterpret_cast<const uint8_t*>(&weaponModelVtable), "xxxxxxxx",
                       sizeof(weaponModelVtable), models, 64);
        if (models.size() >= 64) break;
    }
    L().Key("---- missile slot probe (WeaponModel vtable %s) ----",
            mem::Hex(weaponModelVtable).c_str());
    L().Key("  WeaponModel instances: %d", static_cast<int>(models.size()));

    int ctrlDumps = 0;
    int slotDumps = 0;
    for (uintptr_t model : models) {
        const uintptr_t manager = model - 0x38;
        for (int i = 0; i < 16 && ctrlDumps < 12; ++i) {
            uintptr_t ctrl = 0;
            if (!mem::Read<uintptr_t>(manager + 0x1F540 + static_cast<uintptr_t>(i) * 8, ctrl) ||
                !ctrl) {
                continue;
            }
            uintptr_t vt = 0;
            if (!mem::Read<uintptr_t>(ctrl, vt)) continue;
            // A plausible vtable lies inside the main module and its COL resolves
            // to a class name that starts with ".?AV".
            if (vt < mi.base || vt >= mi.base + mi.size) continue;
            uintptr_t col = 0;
            uint32_t tdRva = 0;
            if (!mem::Read<uintptr_t>(vt - 8, col) || !col) continue;
            if (!mem::Read<uint32_t>(col + 0x0C, tdRva)) continue;
            std::string cls;
            for (int c = 0; c < 64; ++c) {
                char ch = 0;
                if (!mem::Read<char>(mi.base + tdRva + 0x10 + c, ch) || ch == 0) break;
                cls.push_back(ch);
            }
            uint32_t mask = 0;
            if (!mem::Read<uint32_t>(ctrl + 0x5C, mask)) continue;
            L().Key("  ctrl %s class=%s vtable=%s mask=0x%04X", mem::Hex(ctrl).c_str(), cls.c_str(),
                    mem::Hex(vt).c_str(), mask);
            ++ctrlDumps;
            if (mask == 0) continue;
            for (int k = 0; k < 16; ++k) {
                if ((mask & (1u << k)) == 0) continue;
                const uintptr_t slot = ctrl + 0x80 + static_cast<uintptr_t>(k) * 0x68;
                uint8_t alive = 0;
                mem::Read<uint8_t>(slot + 0x50, alive);
                L().Key("    slot %d @%s alive=0x%02X", k, mem::Hex(slot).c_str(), alive);
                for (int off = 0; off < 0x68; off += 0x10) {
                    float f0 = 0, f1 = 0, f2 = 0, f3 = 0;
                    if (!mem::ReadFloat(slot + off, f0) || !mem::ReadFloat(slot + off + 4, f1) ||
                        !mem::ReadFloat(slot + off + 8, f2) ||
                        !mem::ReadFloat(slot + off + 12, f3)) {
                        continue;
                    }
                    if (!std::isfinite(f0) && !std::isfinite(f1)) continue;
                    L().Debug("      +0x%02X: %12.4f %12.4f %12.4f %12.4f", off, f0, f1, f2, f3);
                }
                if (++slotDumps >= 6) return;
            }
        }
    }
    L().Key("---- /missile slot probe ----");
}

void Patcher::ProbeViaDispatcher() {
    // RVAs proven for acz.exe 4,046,008 B / SHA-256 3802B0DC... (docs/ARCHITECTURE.md).
    // We address the dispatcher by RVA and verify its prologue bytes first; the
    // byte signature is only a fallback (it also matches look-alike code).
    static const uintptr_t kDispatcherRva = 0x9DBB0;
    static const uintptr_t kUpdateRva = 0x91DC0;
    static const uint8_t kExpect[16] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
                                        0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57};

    if (hookInstalled_) {
        L().Key("probe: a hook is already installed, skipping the dispatcher probe");
        return;
    }
    mem::ModuleInfo mi;
    if (!mem::MainModule(mi)) return;

    auto dump = [&](const char* tag, uintptr_t addr, size_t n) {
        uint8_t buf[32] = {};
        if (n > sizeof(buf)) n = sizeof(buf);
        if (!mem::ReadBytes(addr, buf, n)) {
            L().Key("  %s @%s: unreadable", tag, mem::Hex(addr).c_str());
            return;
        }
        std::string hex;
        for (size_t i = 0; i < n; ++i) {
            char b[4];
            std::snprintf(b, sizeof(b), "%02X ", buf[i]);
            hex += b;
        }
        L().Key("  %s @%s = exe+0x%llX: %s", tag, mem::Hex(addr).c_str(),
                static_cast<unsigned long long>(addr - mi.base), hex.c_str());
    };
    L().Key("probe: live code bytes (validates that the unpacked-image RVAs match this process)");
    dump("dispatcher", mi.base + kDispatcherRva, 32);
    dump("per-missile update", mi.base + kUpdateRva, 32);

    uintptr_t target = 0;
    uint8_t pro[16] = {};
    if (mem::ReadBytes(mi.base + kDispatcherRva, pro, sizeof(pro)) &&
        std::memcmp(pro, kExpect, sizeof(kExpect)) == 0) {
        target = mi.base + kDispatcherRva;
        L().Key("probe: dispatcher RVA verified (prologue matches), target exe+0x%llX",
                static_cast<unsigned long long>(kDispatcherRva));
    } else {
        L().Key("probe: RVA 0x%llX prologue mismatch, falling back to the byte signature",
                static_cast<unsigned long long>(kDispatcherRva));
        static const char* kSig =
            "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 40 C5 F8 29 74 24 30 "
            "C5 FA 10 35 ?? ?? ?? ?? 48 8B E9 48 8D B1 80 00 00 00";
        target = ResolveSignature(kSig);
        if (!target) {
            L().Key("probe: dispatcher not found - this acz.exe build differs (nothing hooked)");
            return;
        }
    }
    L().Key("probe: dispatcher @%s = exe+0x%llX", mem::Hex(target).c_str(),
            static_cast<unsigned long long>(target - mi.base));

    g_hookArgCount = 1;  // this-only; the detour forwards one argument
    hook::InstallResult ir;
    std::string why;
    void* tramp = nullptr;
    if (!hook::Install(reinterpret_cast<void*>(target), reinterpret_cast<void*>(&GuidanceDetour),
                       &tramp, ir, &why)) {
        L().Key("probe: hook refused: %s", why.c_str());
        return;
    }
    g_trampoline = reinterpret_cast<GuidanceFn>(tramp);
    hookInstalled_ = true;
    probeHookMode_ = true;
    probeHookCalls_ = 0;
    L().Key("probe: dispatcher hooked; will dump the first 20 missile launches then unhook "
            "(read-only, no writes)");
}

// ---------------------------------------------------------------------------
// Plain-MSL projectile pool probe (docs/MSL_PN.md, zero-hook, read-only)
//
//   singleton  S = exeBase + 0x3219A0
//   live list  head at S+0x20 (intrusive circular list, links at node+0x00/+0x08)
//   node size  0x70
//   node map   +0x20/+0x30/+0x3C float triples, +0x50..0x53 target bytes,
//              +0x64 weapon slot, +0x66/+0x67 shooter indices, +0x6B state
// ---------------------------------------------------------------------------
void Patcher::StartPoolProbe() {
    if (poolProbeActive_) return;
    poolProbeActive_ = true;
    poolSamples_ = 0;
    poolLastSample_ = 0.0;
    poolNodes_.clear();
    L().Key("==== 弹丸池探针 / projectile-pool probe started (read-only) ====");
}

void Patcher::ProbeProjectilePool() {
    if (!poolProbeActive_) return;
    const double now = Now();
    if ((now - poolLastSample_) < 0.05) return;  // 20 Hz
    poolLastSample_ = now;
    if (++poolSamples_ > 900) {  // ~45 s per cycle
        ++poolCycles_;
        poolSamples_ = 0;
        poolNodes_.clear();
        if (poolCycles_ >= 8) {
            poolProbeActive_ = false;
            L().Key("==== projectile-pool probe finished (%d cycles) ====", poolCycles_);
            return;
        }
    }

    mem::ModuleInfo mi;
    if (!mem::MainModule(mi)) return;
    const uintptr_t S = mi.base + 0x3219A0;
    static const uintptr_t kListOffsets[3] = {0x20, 0x98, 0x290};

    // Read the whole node once per sample (one guarded copy, cheap).
    float f[32] = {};
    for (uintptr_t listOff : kListOffsets) {
        const uintptr_t head = S + listOff;
        uintptr_t first = 0;
        if (!mem::Read<uintptr_t>(head, first)) continue;
        if (first == 0 || first == head) continue;
        uintptr_t node = first;
        int walked = 0;
        int reported = 0;
        while (node && node != head && walked < 2048 && reported < 6) {
            uintptr_t next = 0;
            if (!mem::Read<uintptr_t>(node, next)) break;
            if (!mem::ReadBytes(node, f, sizeof(f))) { node = next; ++walked; continue; }

            // Only live missile nodes matter (state 2 = just launched, 6 = flying).
            // Everything else in these lists is pool bookkeeping and would just fill
            // the tracking table with static nodes.
            uint8_t nstate = 0;
            if (!mem::Read<uint8_t>(node + 0x6B, nstate)) { node = next; ++walked; continue; }
            if (nstate != 2 && nstate != 6) { node = next; ++walked; continue; }

            PoolNodeSample* slot = nullptr;
            for (auto& s : poolNodes_) {
                if (s.node == node) { slot = &s; break; }
            }
            if (!slot) {
                if (poolNodes_.size() < 512) {
                    PoolNodeSample s;
                    s.node = node;
                    s.state = nstate;
                    memcpy(s.f, f, sizeof(f));
                    s.samples = 1;
                    poolNodes_.push_back(s);
                    if (poolNodes_.size() <= 24) {
                        L().Key("MSL node 0x%llX state=%u  pos=%.1f,%.1f,%.1f  dir=%.2f,%.2f,%.2f  "
                                "scalar=%.2f",
                                static_cast<unsigned long long>(node), nstate, f[13], f[14], f[15],
                                f[16], f[17], f[18], f[19]);
                        L().Key("   CE watchpoints: pos 0x%llX   dir 0x%llX   (find what writes here)",
                                static_cast<unsigned long long>(node + 0x34),
                                static_cast<unsigned long long>(node + 0x40));
                    }
                }
                node = next;
                ++walked;
                continue;
            }
            // Compare every float; report the indices that changed (a live missile
            // changes several floats every frame: position and heading/direction).
            char changes[512] = {};
            unsigned cn = 0;
            int changed = 0;
            for (int i = 0; i < 24; ++i) {
                const float d = f[i] - slot->f[i];
                if (fabsf(d) > 1e-3f) {
                    ++changed;
                    if (cn + 40 < sizeof(changes)) {
                        cn += std::snprintf(changes + cn, sizeof(changes) - cn, " f%d:%.2f>%.2f", i,
                                            slot->f[i], f[i]);
                    }
                }
            }
            if (changed > 0 && slot->samples <= 30) {
                L().Key("MOVE node %s list+0x%X state=%u slot=%.0f changed=%d |%s",
                        mem::Hex(node).c_str(), static_cast<unsigned>(listOff), slot->state,
                        static_cast<double>(slot->f[0x64 / 4]), changed, changes);
                ++reported;
            }
            ++slot->samples;
            memcpy(slot->f, f, sizeof(f));
            node = next;
            ++walked;
        }
        if ((poolSamples_ % 120) == 0) {
            L().Key("  list +0x%X: walked %d node(s)", static_cast<unsigned>(listOff), walked);
        }
    }
}

// Defined further down in this translation unit (same namespace).
extern "C" void __cdecl AczDispatcherObserver(void*, void*, void*, void*);
extern void* g_dispTrampoline;

void Patcher::ProbeDispatcher() {
    if (dispProbe_) return;
    static const uint8_t kExpect[16] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
                                        0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57};
    mem::ModuleInfo mi;
    if (!mem::MainModule(mi)) return;
    const uintptr_t target = mi.base + 0x9DBB0;
    uint8_t pro[16] = {};
    if (!mem::ReadBytes(target, pro, sizeof(pro)) || std::memcmp(pro, kExpect, sizeof(kExpect)) != 0) {
        L().Key("probe: dispatcher bytes mismatch, observer not installed");
        return;
    }
    hook::InstallResult ir;
    std::string why;
    void* tramp = nullptr;
    if (!hook::Install(reinterpret_cast<void*>(target), reinterpret_cast<void*>(&AczDispatcherObserver),
                       &tramp, ir, &why)) {
        L().Key("probe: dispatcher observer refused: %s", why.c_str());
        return;
    }
    g_dispTrampoline = tramp;
    dispProbe_ = true;
    dispCalls_ = 0;
    L().Key("probe: dispatcher observer installed (args forwarded unchanged; read-only)");
}

void Patcher::OnDispatcherObserved(uintptr_t self, uintptr_t a2, uintptr_t a3, uintptr_t a4) {
    if (!dispProbe_ || !self) return;
    ++dispCalls_;
    if (dispCalls_ <= 5) {
        L().Key("disp call %d: ctrl=%s a2=%s a3=%s a4=%s", dispCalls_, mem::Hex(self).c_str(),
                mem::Hex(a2).c_str(), mem::Hex(a3).c_str(), mem::Hex(a4).c_str());
    }
    std::string cls;
    IsMissileSlot(self + 0x80, &cls);
    uint32_t mask = 0;
    mem::Read<uint32_t>(self + 0x5C, mask);
    static int logged = 0;
    if ((!cls.empty() || mask != 0) && logged < 32) {
        ++logged;
        L().Key("disp: ctrl=%s class=%s mask=0x%04X", mem::Hex(self).c_str(),
                cls.empty() ? "<no-weapon-class>" : cls.c_str(), mask);
    }
    static double lastSlotDump = 0.0;
    const double nowT = Now();
    if (mask != 0 && (nowT - lastSlotDump) > 1.0) {
        lastSlotDump = nowT;
        int shown = 0;
        for (int i = 0; i < 16; ++i) {
            if ((mask & (1u << i)) == 0) continue;
            const uintptr_t slot = self + 0x80 + static_cast<uintptr_t>(i) * 0x68;
            L().Key("disp: LIVE slot %d @%s", i, mem::Hex(slot).c_str());
            for (int off = 0; off < 0x68; off += 0x10) {
                float f0 = 0, f1 = 0, f2 = 0, f3 = 0;
                if (!mem::ReadFloat(slot + off, f0) || !mem::ReadFloat(slot + off + 4, f1) ||
                    !mem::ReadFloat(slot + off + 8, f2) || !mem::ReadFloat(slot + off + 12, f3)) {
                    continue;
                }
                L().Key("    +0x%02X: %12.3f %12.3f %12.3f %12.3f", off, f0, f1, f2, f3);
            }
            if (++shown >= 4) break;
        }
    }
}

// ---------------------------------------------------------------------------
// Route A probe: locate the EFF_MIS_POSDIR ring and sample it (read-only).
//
//   RTTI: .?AV?$EffRingBuffer@UEFF_MIS_POSDIR@@$0IAA@@@  vtable rva 0x2ACC68
//   The ring holds {pos vec4, dir vec4} per missile (2048 entries, 0x20 bytes each)
//   and is written every frame for EVERY visible missile, plain MSL included.
//   We do not know the exact head/entry layout yet, so the probe dumps a window of
//   the object across frames and reports which floats change - the changing 4-float
//   groups are the live missile entries.
// ---------------------------------------------------------------------------
void Patcher::StartRingProbe() {
    if (ringProbeActive_) return;
    ringProbeActive_ = true;
    ringSamples_ = 0;
    ringCycles_ = 0;
    ringLastSample_ = 0.0;
    ringObjects_.clear();
    ringPrev_.clear();

    mem::ModuleInfo mi;
    if (!mem::MainModule(mi)) return;
    const uintptr_t vt = mi.base + 0x2ACC68;  // EffRingBuffer<UEFF_MIS_POSDIR> vtable
    std::vector<mem::Region> regions;
    mem::EnumRegions(regions, 0x1000);
    for (const auto& r : regions) {
        if (!r.readable || !r.writable || InsideOwnModule(r.base)) continue;
        mem::ScanBytes(r, reinterpret_cast<const uint8_t*>(&vt), "xxxxxxxx", sizeof(vt),
                       ringObjects_, 8);
        if (ringObjects_.size() >= 4) break;
    }
    L().Key("==== ring probe: %d EffRingBuffer<UEFF_MIS_POSDIR> object(s) found ====",
            static_cast<int>(ringObjects_.size()));
    for (uintptr_t obj : ringObjects_) {
        L().Key("  ring object 0x%llX", static_cast<unsigned long long>(obj));
        ringPrev_.emplace_back();
    }
}

void Patcher::ProbeRingSample() {
    if (!ringProbeActive_ || ringObjects_.empty()) return;
    const double now = Now();
    if ((now - ringLastSample_) < 0.1) return;  // 10 Hz
    ringLastSample_ = now;
    if (++ringSamples_ > 300) {  // 30 s per cycle
        ++ringCycles_;
        ringSamples_ = 0;
        if (ringCycles_ >= 6) {
            ringProbeActive_ = false;
            L().Key("==== ring probe finished (%d cycles) ====", ringCycles_);
            return;
        }
    }

    // The ring object stores a vtable at +0 and probably head/count + a pointer to
    // (or inline) entry storage. We sample (a) the object itself, and (b) up to 6
    // plausible heap pointers found in its first 0x100 bytes (entry storage may be
    // allocated separately, and 2048 * 0x20 = 64 KiB is bigger than one window).
    constexpr int kFloats = 4096;  // 16 KiB per window
    for (size_t oi = 0; oi < ringObjects_.size(); ++oi) {
        const uintptr_t obj = ringObjects_[oi];
        uintptr_t windows[8] = {obj};
        int nWin = 1;
        uint64_t ptrs[16] = {};
        if (mem::ReadBytes(obj, ptrs, sizeof(ptrs))) {
            for (int k = 0; k < 16 && nWin < 8; ++k) {
                const uintptr_t p = static_cast<uintptr_t>(ptrs[k]);
                if (p < 0x10000 || (p & 0xF) != 0) continue;
                if (InsideOwnModule(p)) continue;
                bool ok = false;
                for (const auto& r : mem::WritableRegions(0x1000)) {
                    if (p >= r.base && p + 0x1000 <= r.base + r.size) { ok = true; break; }
                }
                if (!ok) continue;
                bool dup = false;
                for (int w = 0; w < nWin; ++w) if (windows[w] == p) dup = true;
                if (!dup) windows[nWin++] = p;
            }
        }
        // grow the previous-sample table
        while (ringPrev_.size() <= oi * 8 + 8) ringPrev_.emplace_back();

        for (int w = 0; w < nWin; ++w) {
            const uintptr_t base = windows[w];
            auto& prev = ringPrev_[oi * 8 + static_cast<size_t>(w)];
            std::vector<float> cur(static_cast<size_t>(kFloats), 0.f);
            if (!mem::ReadBytes(base, cur.data(), cur.size() * sizeof(float))) continue;
            if (prev.size() != cur.size()) { prev = cur; continue; }
            char line[700] = {};
            unsigned n = 0;
            int reported = 0;
            for (size_t i = 0; i + 3 < cur.size() && reported < 6; i += 4) {
                const float a0 = cur[i], a1 = cur[i + 1], a2 = cur[i + 2], a3 = cur[i + 3];
                const float b0 = prev[i], b1 = prev[i + 1], b2 = prev[i + 2];
                if (!(fabsf(a0 - b0) > 1e-3f || fabsf(a1 - b1) > 1e-3f || fabsf(a2 - b2) > 1e-3f))
                    continue;
                if (!(std::isfinite(a0) && std::isfinite(a1) && std::isfinite(a2))) continue;
                if (fabsf(a0) > 300000.f || fabsf(a1) > 300000.f || fabsf(a2) > 300000.f) continue;
                ++reported;
                n += std::snprintf(line + n, sizeof(line) - n, " [f%zu] %.1f,%.1f,%.1f,%.1f", i, a0,
                                   a1, a2, a3);
                if (n > sizeof(line) - 90) break;
            }
            if (reported > 0) {
                L().Key("RING win%d base=0x%llX sample %d changed=%d:%s", w,
                        static_cast<unsigned long long>(base), ringSamples_, reported, line);
                // NOTE: the PAGE_GUARD write-watch was disabled after it crashed the
                // game (2026-10-26 00:32:28, exception 0x80000001 in ntdll - an
                // unhandled guard-page violation escaping our VEH). Probe mode is
                // read-only again; do not re-enable without a safer mechanism.
            }
            prev = cur;
        }
    }
}

void Patcher::ProbeLockIndicator() {
    mem::ModuleInfo mi;
    if (!mem::MainModule(mi)) return;
    const uintptr_t vt = mi.base + 0x2ADDD0;  // CIndicatorLockonInfo vtable
    std::vector<uintptr_t> objs;
    std::vector<mem::Region> regions;
    mem::EnumRegions(regions, 0x1000);
    for (const auto& r : regions) {
        if (!r.readable || !r.writable || InsideOwnModule(r.base)) continue;
        mem::ScanBytes(r, reinterpret_cast<const uint8_t*>(&vt), "xxxxxxxx", sizeof(vt), objs, 8);
        if (objs.size() >= 8) break;
    }
    L().Key("==== lock indicator probe: %d CIndicatorLockonInfo instance(s) ====",
            static_cast<int>(objs.size()));

    // One level of pointer inspection only, and only into known writable regions.
    std::vector<mem::Region> writable = mem::WritableRegions(0x1000);
    auto inWritable = [&writable](uintptr_t p) {
        for (const auto& r : writable) {
            if (p >= r.base && p + 0x100 <= r.base + r.size) return true;
        }
        return false;
    };

    int dumped = 0;
    for (uintptr_t obj : objs) {
        if (dumped++ >= 4) break;
        L().Key("  lock-obj 0x%llX:", static_cast<unsigned long long>(obj));
        float f[32] = {};
        if (mem::ReadBytes(obj, f, sizeof(f))) {
            for (int off = 0; off < 0x80; off += 0x10) {
                L().Key("    +0x%02X: %12.3f %12.3f %12.3f %12.3f", off, f[off / 4],
                        f[off / 4 + 1], f[off / 4 + 2], f[off / 4 + 3]);
            }
        }
        uint64_t ptrs[16] = {};
        if (mem::ReadBytes(obj, ptrs, sizeof(ptrs))) {
            int followed = 0;
            for (int k = 0; k < 16 && followed < 3; ++k) {
                const uintptr_t p = static_cast<uintptr_t>(ptrs[k]);
                if (p < 0x10000 || (p & 0x7) != 0 || InsideOwnModule(p)) continue;
                if (!inWritable(p)) continue;
                float tf[16] = {};
                if (!mem::ReadBytes(p, tf, sizeof(tf))) continue;
                ++followed;
                L().Key("    ptr[%d] -> 0x%llX  floats: %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f",
                        k, static_cast<unsigned long long>(p), tf[0], tf[1], tf[2], tf[3], tf[4],
                        tf[5], tf[6], tf[7]);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Page-guard write watch: identifies the instruction that advances a live missile
// node (the MSL "advance" function we could not pin statically).
//   * arm PAGE_GUARD on the page holding [addr, addr+len)
//   * the first access faults; our VEH records the faulting RIP (writer!) and clears
//     the guard so the instruction can execute; Tick() re-arms for the next capture.
//   * faults on the same page but outside [addr,addr+len) are simply cleared, never
//     logged, so the game keeps running normally.
// ---------------------------------------------------------------------------
namespace {
struct GuardState {
    Patcher* owner = nullptr;
    volatile LONG armed = 0;
    volatile LONG pending = 0;
    uintptr_t page = 0;
    size_t pageSize = 0;
    uint32_t oldProtect = 0;
    uintptr_t watchAddr = 0;
    size_t watchLen = 0;
    volatile LONG hits = 0;
    uintptr_t rip = 0;
    uintptr_t fault = 0;
    volatile LONG ready = 0;
};
GuardState g_guard;
} // namespace

static LONG CALLBACK AczGuardVeh(EXCEPTION_POINTERS* ep) {
    if (!g_guard.armed) return EXCEPTION_CONTINUE_SEARCH;
    EXCEPTION_RECORD* rec = ep->ExceptionRecord;
    if (!rec || rec->ExceptionCode != 0x80000001u /*STATUS_GUARD_PAGE_VIOLATION*/) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const uintptr_t fault = static_cast<uintptr_t>(rec->ExceptionInformation[1]);
    if (fault < g_guard.page || fault >= g_guard.page + g_guard.pageSize) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // Clear the guard so the faulting instruction can execute, then let Tick re-arm.
    DWORD op = 0;
    ::VirtualProtect(reinterpret_cast<LPVOID>(g_guard.page), g_guard.pageSize, g_guard.oldProtect, &op);
    ::InterlockedExchange(&g_guard.armed, 0);

    // Only writes inside the watched range are interesting.
    const bool isWrite = rec->ExceptionInformation[0] != 0;
    if (isWrite && fault >= g_guard.watchAddr && fault < g_guard.watchAddr + g_guard.watchLen) {
        g_guard.rip = static_cast<uintptr_t>(ep->ContextRecord->Rip);
        g_guard.fault = fault;
        ::InterlockedExchange(&g_guard.pending, 1);
        ::InterlockedIncrement(&g_guard.hits);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

void Patcher::ArmPageGuard(uintptr_t addr, size_t len) {
    if (guardArmed_ || addr == 0 || len == 0 || len > 0x1000) return;
    SYSTEM_INFO si{};
    ::GetSystemInfo(&si);
    const size_t pageSize = si.dwPageSize ? si.dwPageSize : 0x1000;
    const uintptr_t page = addr & ~(static_cast<uintptr_t>(pageSize) - 1);
    DWORD old = 0;
    if (!::VirtualProtect(reinterpret_cast<LPVOID>(page), pageSize, PAGE_READWRITE | PAGE_GUARD, &old)) {
        return;
    }
    if (!guardHandler_) {
        guardHandler_ = ::AddVectoredExceptionHandler(1, &AczGuardVeh);
        if (!guardHandler_) {
            ::VirtualProtect(reinterpret_cast<LPVOID>(page), pageSize, old, &old);
            return;
        }
    }
    g_guard.owner = this;
    g_guard.page = page;
    g_guard.pageSize = pageSize;
    g_guard.oldProtect = old;
    g_guard.watchAddr = addr;
    g_guard.watchLen = len;
    ::InterlockedExchange(&g_guard.armed, 1);
    guardArmed_ = true;
    guardPage_ = page;
    guardPageSize_ = pageSize;
    guardOldProtect_ = old;
    guardWatchAddr_ = addr;
    guardWatchLen_ = len;
    L().Key("guard: PAGE_GUARD armed on 0x%llX (watch 0x%llX..0x%llX)",
            static_cast<unsigned long long>(page), static_cast<unsigned long long>(addr),
            static_cast<unsigned long long>(addr + len));
}

void Patcher::TickPageGuard() {
    if (!guardArmed_) return;
    if (::InterlockedCompareExchange(&g_guard.pending, 0, 1) == 1) {
        mem::ModuleInfo mi;
        const uintptr_t rip = g_guard.rip;
        const bool inExe = mem::MainModule(mi) && rip >= mi.base && rip < mi.base + mi.size;
        ++guardHits_;
        if (inExe) {
            L().Key("guard HIT %d: writer RIP = exe+0x%llX (fault addr 0x%llX)  <-- the MSL advance function",
                    guardHits_, static_cast<unsigned long long>(rip - mi.base),
                    static_cast<unsigned long long>(g_guard.fault));
        } else {
            L().Key("guard HIT %d: writer RIP = 0x%llX (outside exe; fault 0x%llX)", guardHits_,
                    static_cast<unsigned long long>(rip),
                    static_cast<unsigned long long>(g_guard.fault));
        }
        if (guardHits_ >= 12) {
            DisarmPageGuard();
            return;
        }
    }
    if (!guardArmed_ && guardHits_ < 12 && ::InterlockedCompareExchange(&g_guard.armed, 0, 0) == 0) {
        // Re-arm the same page for the next capture.
        DWORD old = 0;
        if (::VirtualProtect(reinterpret_cast<LPVOID>(guardPage_), guardPageSize_,
                             guardOldProtect_ | PAGE_GUARD, &old)) {
            g_guard.oldProtect = old;
            ::InterlockedExchange(&g_guard.armed, 1);
            guardArmed_ = true;
        }
    }
}

void Patcher::DisarmPageGuard() {
    if (guardPage_) {
        DWORD old = 0;
        ::VirtualProtect(reinterpret_cast<LPVOID>(guardPage_), guardPageSize_, guardOldProtect_, &old);
    }
    guardArmed_ = false;
    ::InterlockedExchange(&g_guard.armed, 0);
    L().Key("guard: disarmed after %d hit(s)", guardHits_);
}

void Patcher::ProbeValueNeighborhoods() {
    struct ProbeValue {
        float value;
        const char* tag;
    };
    static const ProbeValue kValues[] = {
        {1600.f, "scope(1600)"},
        {3000.f, "range(3000)"},
        {2000.f, "range(2000)"},
        {40.f, "lead(40)"},
        {80.f, "lead(80)"},
    };

    std::vector<mem::Region> regions;
    mem::EnumRegions(regions, 0x1000);
    for (const auto& pv : kValues) {
        uint32_t bits = 0;
        std::memcpy(&bits, &pv.value, 4);
        uint8_t needle[4];
        std::memcpy(needle, &bits, 4);
        std::vector<uintptr_t> hits;
        for (const auto& r : regions) {
            if (!r.readable || InsideOwnModule(r.base)) continue;
            mem::ScanBytes(r, needle, "xxxx", 4, hits, 64);
            if (hits.size() >= 64) break;
        }
        L().Key("  %s: %d hits", pv.tag, static_cast<int>(hits.size()));
        int shown = 0;
        for (uintptr_t hit : hits) {
            if (shown++ >= 6) break;
            // Which module/region is this in? Helps distinguish exe data from heap.
            mem::ModuleInfo owner;
            const bool inModule = mem::ModuleForAddress(hit, owner);
            L().Key("    @%s (%s +0x%llX)%s", mem::Hex(hit).c_str(),
                    inModule ? WideToUtf8(owner.path).c_str() : "heap/anon",
                    inModule ? static_cast<unsigned long long>(hit - owner.base) : 0ULL,
                    hit >= 0x40 ? "" : " (low)");
            for (int off = -0x30; off <= 0x30; off += 0x10) {
                const uintptr_t base = hit + static_cast<uintptr_t>(static_cast<intptr_t>(off)) -
                                       (hit % 4);
                float f0 = 0, f1 = 0, f2 = 0, f3 = 0;
                if (!mem::ReadFloat(base, f0) || !mem::ReadFloat(base + 4, f1) ||
                    !mem::ReadFloat(base + 8, f2) || !mem::ReadFloat(base + 12, f3)) {
                    continue;
                }
                L().Key("      %+04d: %12.4f %12.4f %12.4f %12.4f  (this value at col %d)", off, f0, f1,
                        f2, f3, (-off) / 4);
            }
        }
    }
}

void Patcher::ProbeRttiClasses() {
    mem::ModuleInfo mi;
    if (!mem::MainModule(mi)) return;

    struct KnownClass {
        uint32_t vtableRva;
        const char* name;
    };
    // RVAs proven for acz.exe 4,046,008 B / SHA-256 3802B0DC... (docs/ARCHITECTURE.md).
    // They are only used as *probe hints*: every hit is validated by reading the
    // RTTI type descriptor name back at runtime.
    static const KnownClass kClasses[] = {
        {0x2AE790u, "WeaponModel"},
        {0x2AD4D0u, "ObjPlane"},
        {0x2AF740u, "WeaponExQaamCtrl"},
        {0x2AFAE8u, "WeaponExSaamCtrl"},
        {0x2AF560u, "WeaponExXlaaCtrl"},
        {0x2AF980u, "WeaponExXagmCtrl"},
    };

    L().Key("---- RTTI probe (exe base %s) ----", mem::Hex(mi.base).c_str());
    for (const auto& kc : kClasses) {
        const uintptr_t vtable = mi.base + kc.vtableRva;
        uintptr_t col = 0;
        if (!mem::Read<uintptr_t>(vtable - 8, col) || !col) {
            L().Key("  %-20s vtable %s: no COL -> not readable", kc.name,
                    mem::Hex(vtable).c_str());
            continue;
        }
        uint32_t sig = 0, tdRva = 0;
        if (!mem::Read<uint32_t>(col, sig) || !mem::Read<uint32_t>(col + 0x0C, tdRva)) {
            L().Key("  %-20s vtable %s: COL %s unreadable", kc.name, mem::Hex(vtable).c_str(),
                    mem::Hex(col).c_str());
            continue;
        }
        // TypeDescriptor: vfptr(8) spare(8) name[..]  -> name at +0x10
        std::string name;
        const uintptr_t td = mi.base + tdRva;
        for (int i = 0; i < 96; ++i) {
            char ch = 0;
            if (!mem::Read<char>(td + 0x10 + i, ch) || ch == 0) break;
            name.push_back(ch);
        }
        L().Key("  %-20s vtable %s COL sig=0x%X type=\"%s\"", kc.name, mem::Hex(vtable).c_str(), sig,
                name.c_str());

        // Find the live objects: first qword == vtable. Scan every readable
        // region (objects can sit in read-only pool blocks too).
        std::vector<uintptr_t> hits;
        std::vector<mem::Region> allRegions;
        mem::EnumRegions(allRegions, 0x1000);
        for (const auto& r : allRegions) {
            if (!r.readable) continue;
            if (InsideOwnModule(r.base)) continue;
            mem::ScanBytes(r, reinterpret_cast<const uint8_t*>(&vtable), "xxxxxxxx",
                           sizeof(vtable), hits, 256);
            if (hits.size() >= 256) break;
        }
        L().Key("    instances: %d", static_cast<int>(hits.size()));
        int shown = 0;
        for (uintptr_t obj : hits) {
            if (shown++ >= 4) break;
            L().Key("    obj %s:", mem::Hex(obj).c_str());
            // Raw dump: floats + matching vanilla values (this is what tells us
            // where lead/scope/range/startAng live in the runtime object).
            for (int off = 0; off < 0x100; off += 0x10) {
                float f0 = 0, f1 = 0, f2 = 0, f3 = 0;
                if (!mem::ReadFloat(obj + off, f0) || !mem::ReadFloat(obj + off + 4, f1) ||
                    !mem::ReadFloat(obj + off + 8, f2) || !mem::ReadFloat(obj + off + 12, f3)) {
                    break;
                }
                if (!std::isfinite(f0) || !std::isfinite(f1) || !std::isfinite(f2) ||
                    !std::isfinite(f3)) {
                    continue;
                }
                char flags[64] = {};
                unsigned fl = 0;
                auto mark = [&](float v, float want, const char* tag) {
                    if (std::fabs(v - want) <= want * 0.01f && fl + 16 < sizeof(flags)) {
                        std::snprintf(flags + fl, sizeof(flags) - fl, "%s ", tag);
                        fl += static_cast<unsigned>(std::strlen(tag)) + 1;
                    }
                };
                mark(f0, 1600.f, "scope?");
                mark(f0, 3000.f, "range?");
                mark(f0, 40.f, "lead?");
                mark(f0, 90.f, "startAng?");
                mark(f1, 1600.f, "scope?");
                mark(f1, 3000.f, "range?");
                mark(f1, 40.f, "lead?");
                mark(f1, 90.f, "startAng?");
                mark(f2, 1600.f, "scope?");
                mark(f2, 3000.f, "range?");
                mark(f2, 40.f, "lead?");
                mark(f2, 90.f, "startAng?");
                mark(f3, 1600.f, "scope?");
                mark(f3, 3000.f, "range?");
                mark(f3, 40.f, "lead?");
                mark(f3, 90.f, "startAng?");
                L().Key("      +0x%02X: %12.4f %12.4f %12.4f %12.4f  %s", off, f0, f1, f2, f3, flags);
            }
        }
    }
    L().Key("---- /RTTI probe ----");
}

bool Patcher::ResolveRowSettings(const game::ParamRow& row, Side side, const FieldSet*& out) const {
    // 1) the record's own key, 2) the name players actually write (misf -> msl).
    out = cfg_.Resolve(side, row.modelKey);
    if (out) return true;
    const std::string alias = ConfigKeyFor(row.modelKey);
    if (alias != LowerAscii(row.modelKey)) {
        out = cfg_.Resolve(side, alias);
        if (out) return true;
    }
    return false;
}

float Patcher::LastWritten(uintptr_t addr) const {
    for (const auto& kv : lastWrites_) {
        if (kv.first == addr) return kv.second;
    }
    return 0.f;
}

void Patcher::RememberWrite(uintptr_t addr, float value) {
    for (auto& kv : lastWrites_) {
        if (kv.first == addr) { kv.second = value; return; }
    }
    if (lastWrites_.size() < 4096) lastWrites_.push_back({addr, value});
}

void Patcher::ApplyStaticParams() {
    lastParamPatch_ = Now();
    if (rows_.empty()) return;
    if (cfg_.runtime.dryRun) {
        L().Info("DryRun=1：只检测不写入 / detection only, nothing is written");
    }

    // Count how many distinct tables (row bases) we have per model: a second
    // table of the same model is treated as the NPC variant, which is how the
    // [enemy] section can take effect even though ACZ stores one row per weapon.
    std::vector<std::pair<std::string, int>> perModel;
    for (auto& kv : perModel) kv.second = 0;
    for (const auto& row : rows_) {
        bool found = false;
        for (auto& kv : perModel) {
            if (kv.first == row.modelKey) { kv.second++; found = true; break; }
        }
        if (!found) perModel.push_back({row.modelKey, 1});
    }
    bool enemySeparate = false;
    for (const auto& kv : perModel) enemySeparate = enemySeparate || kv.second > 1;

    std::vector<std::string> seen;
    std::vector<std::string> matchedKeys;
    for (auto& row : rows_) {
        int ordinal = 0;
        for (const auto& s : seen) if (s == row.modelKey) ordinal++;
        seen.push_back(row.modelKey);

        // NPC/wingman records (misf/misf2/gunf) take the [enemy] settings; a
        // second table for the same model is also treated as the NPC variant.
        Side side = (IsNpcKey(row.modelKey) || (enemySeparate && ordinal > 0)) ? Side::Enemy
                                                                               : Side::Player;

        const FieldSet* fs = nullptr;
        if (!ResolveRowSettings(row, side, fs) || fs == nullptr) {
            // Also try the other side before giving up.
            if (!ResolveRowSettings(row, side == Side::Player ? Side::Enemy : Side::Player, fs) ||
                fs == nullptr) {
                continue;
            }
        }
        if (!fs->Any()) continue;
        matchedKeys.push_back(LowerAscii(row.modelKey));
        matchedKeys.push_back(ConfigKeyFor(row.modelKey));

        bool wroteAny = false;
        for (const auto& slot : row.slots) {
            float value = 0.f;
            bool want = false;
            switch (slot.prop) {
                case game::Property::HomingForesightAmount:      // ACZ `lead`
                    if (fs->homingForesightAmount) {
                        // HomingForesightAmount is 0..1; the engine value is a
                        // frame count. See RuntimeSection::leadMode.
                        const float amount = Clampf(*fs->homingForesightAmount, 0.f, 1.f);
                        const float vanilla = slot.foundValue;
                        const float scale = cfg_.runtime.leadScale;
                        if (cfg_.runtime.leadMode == "absolute") {
                            value = amount * scale;
                        } else if (cfg_.runtime.leadMode == "blend") {
                            value = vanilla * (1.f - amount) + scale * amount;
                        } else {  // "add"
                            value = vanilla + amount * scale;
                        }
                        want = true;
                    }
                    break;
                case game::Property::MaxHomingAngle:             // ACZ `startAng`
                    if (fs->maxHomingAngle) { value = *fs->maxHomingAngle; want = true; }
                    break;
                case game::Property::MaxRotationAngle:
                    // No field in the ACZ data records; deliberately not written.
                    break;
                case game::Property::LifeTime:                   // ACZ `range`
                    if (fs->lifeTime) { value = *fs->lifeTime; want = true; }
                    break;
                case game::Property::LockonRange:                // ACZ `scope`
                    if (fs->lockonRange) { value = *fs->lockonRange; want = true; }
                    break;
                default:
                    break;
            }
            if (!want) continue;
            const uintptr_t addr = row.rowAddr + slot.offset;
            if (!cfg_.runtime.dryRun) {
                float have = 0.f;
                if (!mem::ReadFloat(addr, have)) {
                    L().Warn("  读取失败 / read failed at %s (%s)", mem::Hex(addr).c_str(),
                             game::PropertyName(slot.prop));
                    continue;
                }
                // Staleness guard: the row must still hold either the value the
                // discovery saw, or a value we wrote earlier. Anything else means
                // the game reloaded its data and our row pointer is stale.
                const float last = LastWritten(addr);
                const bool ours = std::fabs(have - last) < 1e-2f;
                const bool untouched = std::fabs(have - slot.foundValue) <
                                       std::max(std::fabs(slot.foundValue) * 0.02f, 0.05f);
                if (!ours && !untouched) {
                    ++staleRows_;
                    continue;
                }
                if (std::fabs(have - value) < 1e-4f) { RememberWrite(addr, have); continue; }
                if (!mem::Write<float>(addr, value)) {
                    L().Warn("  写入失败 / write failed at %s (%s)", mem::Hex(addr).c_str(),
                             game::PropertyName(slot.prop));
                    continue;
                }
                float check = 0.f;
                if (!mem::ReadFloat(addr, check) || std::fabs(check - value) > 1e-3f) {
                    L().Warn("  写入校验失败 / verify failed at %s", mem::Hex(addr).c_str());
                    continue;
                }
                RememberWrite(addr, value);
                wroteAny = true;
                ++stats_.fieldsWritten;
            } else {
                wroteAny = true;
            }
            L().Debug("  [%s] %s %s +0x%X = %.3f (vanilla %.3f)", SideName(side),
                      row.modelKey.c_str(), game::PropertyName(slot.prop), slot.offset, value,
                      slot.foundValue);
        }
        if (wroteAny) ++stats_.rowsWritten;
    }
    if (!enemySeparate) {
        L().Info("[enemy] 说明：ACZ 的敌我导弹共用一个记录；僚机弹（misf/misf2）是独立记录，"
                 "已按 [enemy] 处理。");
        L().Info("[enemy] note: ACZ shares one record for player/enemy MSL; the separate "
                 "wingman records (misf/misf2) are handled as [enemy].");
    }

    // Warn about configured models that no discovered record could serve - in
    // ACZ the special weapons (QAAM/LAAM/HVAA/...) have no data record at all.
    std::vector<std::string> unmatched;
    for (const auto& m : cfg_.models) {
        if (!(m.player.Any() || m.enemy.Any())) continue;
        bool hit = false;
        for (const auto& k : matchedKeys) {
            if (k == LowerAscii(m.key)) { hit = true; break; }
        }
        if (!hit) unmatched.push_back(m.key);
    }
    if (!unmatched.empty()) {
        std::string list;
        for (const auto& k : unmatched) { if (!list.empty()) list += ", "; list += k; }
        L().Warn("这些型号没有对应的数据记录，未写入：%s", list.c_str());
        L().Warn("no data record for: %s", list.c_str());
        L().Warn("ACZ 把特殊武器（QAAM/LAAM/HVAA/4AAM...）做在代码里、数据里没有记录；"
                 "要改它们的引导只能走 PN 接管（docs/CALIBRATION.md 第 5 节）。");
        L().Warn("special-weapon guidance is code-side in ACZ; use the PN override for those.");
    }
    L().Info("静态参数补丁：写 %d 行 / %d 个字段（找到 %d 行，已验证 %d 行）",
             stats_.rowsWritten, stats_.fieldsWritten, stats_.rowsFound, stats_.rowsVerified);
}

void Patcher::LogRows() const {
    int shown = 0;
    for (const auto& row : rows_) {
        if (shown++ >= 16) {
            L().Debug("  ... 其余 %d 行省略 / %d more rows", static_cast<int>(rows_.size()) - 16,
                      static_cast<int>(rows_.size()) - 16);
            break;
        }
        std::string slots;
        for (const auto& s : row.slots) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%s+0x%X=%.2f ", game::PropertyName(s.prop), s.offset,
                          s.foundValue);
            slots += buf;
        }
        L().Key("  行 / row %s [%s] score=%d verified=%d  %s", mem::Hex(row.rowAddr).c_str(),
                row.modelKey.c_str(), row.score, row.verified ? 1 : 0, slots.c_str());
    }
}

void Patcher::DiscoverEntities() {
    lastEntityScan_ = Now();
    if (rows_.empty()) return;
    std::vector<std::string> diag;
    entities_.clear();
    game::FindEntities(rows_, entities_, 512, diag);
    for (const auto& d : diag) L().Key("%s", d.c_str());
    stats_.entitiesFound = static_cast<int>(entities_.size());
    stats_.entitiesVerified = 0;
    for (const auto& e : entities_) {
        game::EntityLayout layout;
        if (game::CorrelateMotion(e.addr, 1.0f / 60.0f, layout) && layout.verified) {
            layout_ = layout;
            layoutVerified_ = true;
            ++stats_.entitiesVerified;
            break;
        }
    }
    L().Info("实体搜索：候选 %d，已验证 %d", stats_.entitiesFound, stats_.entitiesVerified);
}

bool Patcher::InstallGuidanceHook() {
    if (!cfg_.runtime.hookRva && cfg_.runtime.hookSignature.empty()) {
        stats_.guidanceDetail = "no HookRva/HookSignature configured";
        return false;
    }
    uintptr_t target = 0;
    mem::ModuleInfo mi;
    if (!mem::MainModule(mi)) return false;
    if (cfg_.runtime.hookRva) {
        target = mi.base + *cfg_.runtime.hookRva;
        L().Info("制导钩子目标 / hook target: exe+0x%X = %s", *cfg_.runtime.hookRva,
                 mem::Hex(target).c_str());
    } else {
        target = ResolveSignature(cfg_.runtime.hookSignature);
        if (!target) {
            stats_.guidanceDetail = "HookSignature not found (game build differs?)";
            L().Warn("HookSignature 没有唯一命中，未安装钩子（游戏版本可能不同）/ HookSignature not "
                     "found, hook not installed");
            return false;
        }
        L().Info("制导钩子目标 / hook target: %s = exe+0x%llX (signature hit)",
                 mem::Hex(target).c_str(),
                 static_cast<unsigned long long>(target - mi.base));
    }
    if (!target) {
        stats_.guidanceDetail = "no hook target";
        return false;
    }
    g_hookArgCount = cfg_.runtime.hookArgIndex <= 1 ? 1 : 2;

    hook::InstallResult ir;
    std::string why;
    g_trampoline = nullptr;
    void* tramp = nullptr;
    if (!hook::Install(reinterpret_cast<void*>(target), reinterpret_cast<void*>(&GuidanceDetour),
                       &tramp, ir, &why)) {
        stats_.guidanceDetail = "hook install failed: " + why;
        L().Warn("制导钩子安装失败 / hook install failed: %s", why.c_str());
        return false;
    }
    if (cfg_.runtime.hookArgIndex > 2) {
        L().Warn("HookArgIndex>2 未支持，按 2 参数处理 / unsupported arg index, using 2 args");
    }
    g_trampoline = reinterpret_cast<GuidanceFn>(tramp);
    hookInstalled_ = true;
    stats_.guidanceActive = true;
    stats_.guidanceDetail = "hooked";
    L().Info("制导钩子已安装 / guidance hook installed (stole %d bytes)",
             static_cast<int>(ir.stolenBytes));
    return true;
}

void Patcher::TearDownGuidance() {
    // Restore the patched bytes. Without this the jump stays behind while the
    // trampoline pointer is cleared, i.e. the game's own function would be skipped.
    if (hookInstalled_ && hookTarget_) {
        if (!hook::Remove(hookTarget_, hookResult_)) {
            L().Warn("钩子卸载失败 / hook remove failed at %s",
                     mem::Hex(reinterpret_cast<uintptr_t>(hookTarget_)).c_str());
        }
        hookTarget_ = nullptr;
    }
    hookInstalled_ = false;
    probeHookMode_ = false;
    pnHooked_ = false;
    stats_.guidanceActive = false;
    g_trampoline = nullptr;
    g_pnTrampoline = nullptr;
}

// ---------------------------------------------------------------------------
// Plain-MSL survey: who feeds EFF_MIS_POSDIR?  (docs/MSL_PN.md)
//
// Everything that flies -- special weapons AND the plain standard missile -- pushes
// {position, direction} into the EFF_MIS_POSDIR trail ring every frame. The ring
// lives inside the mission-control object (constructed at exe+0x16D980: ring at
// mc+0x240, vtable rva 0x2ACC68, 2048 x 0x20 entries of {pos vec4, dir vec4}).
//
// Its two "Add" methods are vtable slots 2 and 3 (rva 0x3B300 / 0x3B370). They are
// only ever reached through the vtable, so a static cross-reference cannot find the
// producer -- but a detour on the method itself catches every call regardless of how
// it was made, and the return address identifies the game function that computed
// this frame's missile motion. That is the missing plain-MSL per-frame advance.
//
// The detour is read-only and order-preserving: run the original body first through
// the trampoline (so the trail is emitted exactly as vanilla), then log.
// ---------------------------------------------------------------------------
static void* g_ringTrampA = nullptr;  // exe+0x3B300
static void* g_ringTrampB = nullptr;  // exe+0x3B370
typedef void (*RingPushFn)(void*, void*, void*, void*);

extern "C" void __cdecl AczRingPushObserver(void* ring, void* idx, void* src, uintptr_t caller) {
    if (g_patcher) g_patcher->OnRingPushed(reinterpret_cast<uintptr_t>(ring),
                                           reinterpret_cast<uintptr_t>(idx),
                                           reinterpret_cast<uintptr_t>(src), caller);
}

// Hand-built x64 stub:  <save rcx/rdx/r8/r9>  call handler(ring, idx, src, caller)
//                      <restore rcx/rdx/r8/r9> call trampoline  ret
// The original arguments must survive our C call, and the caller's return address
// sits at [rsp] on entry, i.e. at [rsp+0x48] after the frame (docs/ARCHITECTURE.md
// uses the same trick for the integrator).
static void BuildRingThunk(uint8_t* mem, void* handler, void* tramp) {
    uint8_t* p = mem;
    auto emit = [&p](std::initializer_list<uint8_t> b) {
        for (uint8_t v : b) *p++ = v;
    };
    emit({0x48, 0x83, 0xEC, 0x48});                          // sub  rsp, 0x48
    emit({0x48, 0x89, 0x4C, 0x24, 0x20});                    // mov  [rsp+0x20], rcx
    emit({0x48, 0x89, 0x54, 0x24, 0x28});                    // mov  [rsp+0x28], rdx
    emit({0x4C, 0x89, 0x44, 0x24, 0x30});                    // mov  [rsp+0x30], r8
    emit({0x4C, 0x89, 0x4C, 0x24, 0x38});                    // mov  [rsp+0x38], r9
    emit({0x48, 0x8B, 0x4C, 0x24, 0x20});                    // mov  rcx, [rsp+0x20]
    emit({0x48, 0x8B, 0x54, 0x24, 0x28});                    // mov  rdx, [rsp+0x28]
    emit({0x4C, 0x8B, 0x44, 0x24, 0x30});                    // mov  r8,  [rsp+0x30]
    emit({0x4C, 0x8B, 0x4C, 0x24, 0x48});                    // mov  r9,  [rsp+0x48] (= caller)
    emit({0x48, 0xB8});
    const uintptr_t h = reinterpret_cast<uintptr_t>(handler);
    std::memcpy(p, &h, sizeof(h));
    p += 8;                                                  // mov  rax, handler
    emit({0xFF, 0xD0});                                      // call rax
    emit({0x48, 0x8B, 0x4C, 0x24, 0x20});                    // mov  rcx, [rsp+0x20]
    emit({0x48, 0x8B, 0x54, 0x24, 0x28});                    // mov  rdx, [rsp+0x28]
    emit({0x4C, 0x8B, 0x44, 0x24, 0x30});                    // mov  r8,  [rsp+0x30]
    emit({0x4C, 0x8B, 0x4C, 0x24, 0x38});                    // mov  r9,  [rsp+0x38]
    emit({0x48, 0xB8});
    const uintptr_t t = reinterpret_cast<uintptr_t>(tramp);
    std::memcpy(p, &t, sizeof(t));
    p += 8;                                                  // mov  rax, trampoline
    emit({0xFF, 0xD0});                                      // call rax
    emit({0x48, 0x83, 0xC4, 0x48});                          // add  rsp, 0x48
    emit({0xC3});                                            // ret
}

bool Patcher::InstallRingSurvey() {
    // MIS_POSDIR ring "Add" entry bytes (this build; verified against .pdata-less
    // leaf functions, so no symbol can be used as an anchor).
    static const uint8_t kAddA[12] = {0x8B, 0x42, 0x08, 0x4C, 0x8B, 0xD2,
                                      0x41, 0x03, 0xC1, 0x99, 0x41, 0xF7};
    static const uint8_t kAddB[12] = {0x48, 0x63, 0x02, 0x4C, 0x63, 0x4A,
                                      0x0C, 0x4C, 0x03, 0xC8, 0x41, 0x8B};
    struct Site {
        uintptr_t rva;
        const uint8_t* expect;
        void** tramp;
        const char* name;
    };
    Site sites[2] = {{0x3B300, kAddA, &g_ringTrampA, "exe+0x3B300"},
                     {0x3B370, kAddB, &g_ringTrampB, "exe+0x3B370"}};
    mem::ModuleInfo mi;
    if (!mem::MainModule(mi)) return false;

    int installed = 0;
    for (Site& s : sites) {
        const uintptr_t target = mi.base + s.rva;
        uint8_t pro[12] = {};
        if (!mem::ReadBytes(target, pro, sizeof(pro)) ||
            std::memcmp(pro, s.expect, sizeof(pro)) != 0) {
            if (ringSurveyTries_ == 1) {
                L().Key("RING: %s 字节不匹配（.text 可能尚未解密）/ bytes mismatch, will retry",
                        s.name);
            }
            continue;
        }
        auto* page = static_cast<uint8_t*>(
            ::VirtualAlloc(nullptr, 0x100, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!page) continue;
        // The thunk needs the trampoline address, which only exists after Install.
        BuildRingThunk(page, reinterpret_cast<void*>(&AczRingPushObserver), nullptr);
        hook::InstallResult ir;
        std::string why;
        void* tramp = nullptr;
        if (!hook::Install(reinterpret_cast<void*>(target), page, &tramp, ir, &why)) {
            L().Warn("RING: %s 挂钩失败 / hook refused: %s", s.name, why.c_str());
            ::VirtualFree(page, 0, MEM_RELEASE);
            continue;
        }
        *s.tramp = tramp;
        BuildRingThunk(page, reinterpret_cast<void*>(&AczRingPushObserver), tramp);
        ::FlushInstructionCache(::GetCurrentProcess(), page, 0x100);
        ++installed;
        L().Key("RING: %s 观察钩子已安装 / observer installed (stole %d bytes)", s.name,
                static_cast<int>(ir.stolenBytes));
    }
    if (installed == 0) return false;
    ringSurveyInstalled_ = true;
    L().Info("RING 调查已启用 / ring caller survey active (%d site(s)) - 打几发普通导弹 "
             "MSL，然后回来看 ACZPropNav.key.log",
             installed);
    return true;
}

void Patcher::RingSurveyTick(double now) {
    if (!ringSurveyWanted_ || ringSurveyInstalled_) {
        if (ringSurveyInstalled_ && (now - lastRingSummary_) > 30.0) {
            lastRingSummary_ = now;
            L().Info("RING 心跳 / heartbeat: pushes=%lld distinctCallers=%d distinctSrc=%d",
                     ringPushCount_, static_cast<int>(ringCallerCount_),
                     static_cast<int>(ringSrcCount_));
            mem::ModuleInfo mi;
            const bool haveMi = mem::MainModule(mi);
            for (int i = 0; i < kRingCallerSlots; ++i) {
                const long long c = ringCallers_[i].caller;
                if (!c) continue;
                L().Info("RING   caller exe+0x%llX count=%lld srcs=%d",
                         static_cast<unsigned long long>(haveMi && static_cast<uintptr_t>(c) >= mi.base
                                                             ? static_cast<uintptr_t>(c) - mi.base
                                                             : static_cast<uintptr_t>(c)),
                         ringCallers_[i].count, static_cast<int>(ringCallers_[i].uniqueSrc));
            }
        }
        return;
    }
    if ((now - ringSurveyLastTry_) < 0.5) return;
    ringSurveyLastTry_ = now;
    ++ringSurveyTries_;
    if (InstallRingSurvey()) return;
    if (ringSurveyTries_ == 1) {
        L().Key("RING: 等待 .text 解密后安装观察钩子（每 500 ms 重试）/ waiting for decrypted .text");
    } else if (ringSurveyTries_ == 240) {  // 120 s
        L().Warn("RING: 120 s 内无法安装观察钩子，放弃 / giving up on the ring survey");
        ringSurveyWanted_ = false;
    }
}

// Runs on every trail-ring push (a few thousand per second at most). Everything here
// is read-only and allocation-free: fixed arrays + Interlocked, so a faulting read
// can never leave shared state half-updated.
void Patcher::OnRingPushed(uintptr_t ring, uintptr_t idx, uintptr_t src, uintptr_t caller) {
    ::InterlockedIncrement64(&ringPushCount_);
    // Cached: MainModule() is a PSAPI call and must not happen per push.
    static uintptr_t cachedBase = 0;
    if (!cachedBase) {
        mem::ModuleInfo mi;
        if (mem::MainModule(mi)) cachedBase = mi.base;
    }
    const uintptr_t rva = (cachedBase && caller >= cachedBase) ? (caller - cachedBase) : 0;

    RingCallerInfo* info = nullptr;
    for (int i = 0; i < kRingCallerSlots; ++i) {
        if (static_cast<uintptr_t>(ringCallers_[i].caller) == caller) {
            info = &ringCallers_[i];
            break;
        }
    }
    if (!info) {
        const long slot = ::InterlockedIncrement(&ringCallerCount_) - 1;
        if (slot >= kRingCallerSlots) return;  // bounded; still counted globally
        info = &ringCallers_[slot];
        ::InterlockedExchange64(&info->caller, static_cast<long long>(caller));
        // The WeaponEx controllers live in 0x90000..0xA1FFF; anything else is the
        // interesting case (plain MSL / other projectile kinds).
        const bool exFamily = (rva >= 0x90000 && rva < 0xA2000);
        L().Key("RING: === new caller %s (exe+0x%llX) %s ===", mem::Hex(caller).c_str(),
                static_cast<unsigned long long>(rva),
                exFamily ? "[WeaponEx family]" : "[NOT WeaponEx -> plain MSL candidate]");
    }
    ::InterlockedIncrement64(&info->count);
    if (src && static_cast<uintptr_t>(info->lastSrc) != src) {
        ::InterlockedExchange64(&info->lastSrc, static_cast<long long>(src));
        ::InterlockedIncrement(&info->uniqueSrc);
        const long s = ::InterlockedIncrement(&ringSrcCount_) - 1;
        if (s < kRingSrcSlots) ::InterlockedExchange64(&ringSrcSeen_[s], static_cast<long long>(src));
    }

    // Per-caller detail: the first few pushes verbatim, then only a rare refresh.
    // Every entry is {pos vec4, dir vec4}; a moving missile shows pos advancing along dir.
    const long long n = info->count;
    const bool detail = (n <= 6) || (n % 2000) == 0;
    if (!detail) return;
    const long loggedBefore = ::InterlockedIncrement(&info->logged);
    float f[8] = {};
    for (int i = 0; i < 8; ++i) {
        float v = 0.f;
        if (mem::ReadFloat(src + static_cast<uintptr_t>(i) * 4, v)) f[i] = v;
    }
    L().Key("RING  #%lld caller=exe+0x%llX ring=%s idx=%s src=%s", n,
            static_cast<unsigned long long>(rva), mem::Hex(ring).c_str(), mem::Hex(idx).c_str(),
            mem::Hex(src).c_str());
    L().Key("RING      pos=(%8.2f %8.2f %8.2f %.3f) dir=(%8.4f %8.4f %8.4f %.3f)", f[0], f[1],
            f[2], f[3], f[4], f[5], f[6], f[7]);
    // Once per caller, dump the neighbourhood: if `src` points inside the missile
    // object, this reveals the object's own position/velocity/target fields, which is
    // what a proportional-navigation rewrite needs.
    if (loggedBefore <= 1 && src) {
        for (int row = 0; row < 3; ++row) {
            char line[512];
            size_t len = 0;
            for (int i = 0; i < 8 && len + 40 < sizeof(line); ++i) {
                const int off = -0x20 + row * 0x20 + i * 4;
                float v = 0.f;
                if (!mem::ReadFloat(static_cast<uintptr_t>(static_cast<intptr_t>(src) + off), v))
                    v = 0.f;
                len += static_cast<size_t>(std::snprintf(line + len, sizeof(line) - len,
                                                         "%+10.3f ",
                                                         static_cast<double>(v)));
            }
            line[len] = 0;
            L().Key("RING      src%+04X: %s", -0x20 + row * 0x20, line);
        }
    }
}

// ---------------------------------------------------------------------------
// Proportional-navigation integrator hook (docs/ARCHITECTURE.md)
//
//   exe+0x90510 is the shared steering/integrator: rcx don't-care, rdx = MissileSlot*,
//   xmm2/xmm3 + [rsp+0x28] tuning floats. It is the single place where a missile's
//   velocity (slot+0x20..0x2C, metres/frame) is committed, so we let the game
//   compute and apply it, then overwrite the velocity with the PN command.
//
//   A hand-built thunk preserves the original argument registers (including the
//   5th stack argument that the relocated prologue spills), calls the trampoline,
//   then calls our C handler with the slot pointer.
// ---------------------------------------------------------------------------
void* g_dispTrampoline = nullptr;
typedef void (*DispatcherFn)(void*, void*, void*, void*);

extern "C" void __cdecl AczDispatcherObserver(void* self, void* a2, void* a3, void* a4) {
    DispatcherFn tramp = reinterpret_cast<DispatcherFn>(g_dispTrampoline);
    if (tramp) tramp(self, a2, a3, a4);   // exact original argument registers
    if (g_patcher && self) {
        g_patcher->OnDispatcherObserved(reinterpret_cast<uintptr_t>(self),
                                        reinterpret_cast<uintptr_t>(a2),
                                        reinterpret_cast<uintptr_t>(a3),
                                        reinterpret_cast<uintptr_t>(a4));
    }
}

extern "C" void __cdecl AczPnSlotHandler(void* slot, void* retSlot) {
    if (!g_patcher || !slot) return;
    uintptr_t caller = 0;
    if (retSlot) mem::Read<uintptr_t>(reinterpret_cast<uintptr_t>(retSlot), caller);
    g_patcher->OnMissileSlotUpdated(reinterpret_cast<uintptr_t>(slot), caller);
}

bool Patcher::InstallIntegratorHook(bool quiet) {
    static const uintptr_t kIntegratorRva = 0x90510;
    static const uint8_t kExpect[8] = {0x48, 0x8B, 0xC4, 0xC5, 0xFA, 0x11, 0x58, 0x20};
    // Unique 12-byte form (the 8-byte one also matches 0x91DC0 and 0x1B4A20).
    static const uint8_t kPat12[12] = {0x48, 0x8B, 0xC4, 0xC5, 0xFA, 0x11,
                                       0x58, 0x20, 0xC5, 0xFA, 0x11, 0x50};

    mem::ModuleInfo mi;
    if (!mem::MainModule(mi)) return false;
    uintptr_t target = mi.base + kIntegratorRva;

    uint8_t pro[16] = {};
    const bool readOk = mem::ReadBytes(target, pro, sizeof(pro));
    const bool rvaOk = readOk && std::memcmp(pro, kExpect, sizeof(kExpect)) == 0;
    if (!rvaOk) {
        if (!quiet) {
            std::string hex;
            for (size_t i = 0; i < sizeof(pro); ++i) {
                char b[4];
                std::snprintf(b, sizeof(b), "%02X ", pro[i]);
                hex += b;
            }
            L().Warn("PN: 积分器字节校验失败 / integrator bytes mismatch at %s (=exe+0x%llX): read=%s [%s]",
                     mem::Hex(target).c_str(), static_cast<unsigned long long>(kIntegratorRva),
                     readOk ? "ok" : "FAILED", hex.c_str());
        }
        // Fallback: locate the function by its unique 12-byte pattern anywhere in
        // the module's executable sections (also survives a rebuild that shifts .text).
        std::vector<uintptr_t> hits;
        for (const auto& r : mem::ExecutableRegions(0x1000)) {
            if (r.base < mi.base || r.base >= mi.base + mi.size) continue;
            mem::ScanBytes(r, kPat12, "xxxxxxxxxxxx", sizeof(kPat12), hits, 8);
            if (hits.size() >= 2) break;
        }
        if (hits.size() == 1) {
            target = hits[0];
            if (!quiet) {
                L().Info("PN: 改用签名定位积分器 / located integrator by signature at %s (exe+0x%llX)",
                         mem::Hex(target).c_str(),
                         static_cast<unsigned long long>(target - mi.base));
            }
        } else {
            if (!quiet) {
                L().Warn("PN: 签名命中 %d 次，无法安全定位；本模组保持只读 / signature hits=%d, "
                         "staying read-only",
                         static_cast<int>(hits.size()), static_cast<int>(hits.size()));
            }
            return false;
        }
    } else if (!quiet) {
        L().Info("PN: 积分器字节校验通过 / integrator bytes verified at exe+0x%llX",
                 static_cast<unsigned long long>(kIntegratorRva));
    }

    auto* mem = static_cast<uint8_t*>(
        ::VirtualAlloc(nullptr, 0x100, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!mem) return false;
    uint8_t* p = mem;
    auto emit = [&p](const uint8_t* b, size_t n) { std::memcpy(p, b, n); p += n; };
    static const uint8_t kSub[] = {0x48, 0x83, 0xEC, 0x58};
    static const uint8_t kSaveRdx[] = {0x48, 0x89, 0x54, 0x24, 0x30};
    static const uint8_t kArg5In[] = {0xF3, 0x0F, 0x10, 0x84, 0x24, 0x80, 0x00, 0x00, 0x00};
    static const uint8_t kArg5Out[] = {0xF3, 0x0F, 0x11, 0x44, 0x24, 0x20};
    static const uint8_t kMovRax[] = {0x48, 0xB8};
    static const uint8_t kCallRax[] = {0xFF, 0xD0};
    static const uint8_t kRestoreRcx[] = {0x48, 0x8B, 0x4C, 0x24, 0x30};
    static const uint8_t kAdd[] = {0x48, 0x83, 0xC4, 0x58};
    static const uint8_t kRet[] = {0xC3};

    emit(kSub, sizeof(kSub));
    emit(kSaveRdx, sizeof(kSaveRdx));
    emit(kArg5In, sizeof(kArg5In));
    emit(kArg5Out, sizeof(kArg5Out));
    emit(kMovRax, sizeof(kMovRax));
    const size_t atTramp = static_cast<size_t>(p - mem);
    p += 8;
    emit(kCallRax, sizeof(kCallRax));
    emit(kRestoreRcx, sizeof(kRestoreRcx));
    // lea rdx, [rsp+0x58]  -> address of the original return address (identifies
    // the game call site that reached the integrator)
    static const uint8_t kLeaRdx[] = {0x48, 0x8D, 0x54, 0x24, 0x58};
    emit(kLeaRdx, sizeof(kLeaRdx));
    emit(kMovRax, sizeof(kMovRax));
    const size_t atHandler = static_cast<size_t>(p - mem);
    p += 8;
    emit(kCallRax, sizeof(kCallRax));
    emit(kAdd, sizeof(kAdd));
    emit(kRet, sizeof(kRet));

    hook::InstallResult ir;
    std::string why;
    void* tramp = nullptr;
    if (!hook::Install(reinterpret_cast<void*>(target), mem, &tramp, ir, &why)) {
        L().Warn("PN 钩子安装失败 / PN hook install failed: %s", why.c_str());
        ::VirtualFree(mem, 0, MEM_RELEASE);
        return false;
    }
    const uintptr_t trampAddr = reinterpret_cast<uintptr_t>(tramp);
    const uintptr_t handlerAddr = reinterpret_cast<uintptr_t>(&AczPnSlotHandler);
    std::memcpy(mem + atTramp, &trampAddr, sizeof(trampAddr));
    std::memcpy(mem + atHandler, &handlerAddr, sizeof(handlerAddr));
    ::FlushInstructionCache(::GetCurrentProcess(), mem, 0x100);

    g_pnTrampoline = tramp;
    pnThunk_ = mem;
    pnHooked_ = true;
    hookInstalled_ = true;
    hookTarget_ = reinterpret_cast<void*>(target);
    hookResult_ = ir;
    stats_.guidanceActive = true;
    stats_.guidanceDetail = "integrator hooked";
    L().Info("PN 积分器钩子已安装 / PN integrator hooked at exe+0x%llX (stole %d bytes, thunk %s)",
             static_cast<unsigned long long>(kIntegratorRva), static_cast<int>(ir.stolenBytes),
             mem::Hex(reinterpret_cast<uintptr_t>(mem)).c_str());
    return true;
}

// Runs with the stack aligned, once per live missile per frame, after the game has
// written the velocity. Slot layout: docs/ARCHITECTURE.md.
// The shared integrator is reached for more than missiles (docs/ARCHITECTURE.md:
// 7 call sites, only part of them are homing weapons). Writing the velocity of a
// non-missile object corrupts the game, so every slot must prove that it sits inside
// a weapon controller's 16-entry missile array: ctrl = slot - (0x80 + i*0x68), and
// the object at ctrl must be an RTTI class whose name looks like a weapon controller.
bool Patcher::IsMissileSlot(uintptr_t slot, std::string* className) {
    if (className) className->clear();
    if (!slot) return false;
    for (const auto& kv : slotClassCache_) {
        if (kv.first == slot) return kv.second != 0;
    }
    mem::ModuleInfo mi;
    if (!mem::MainModule(mi)) return false;

    bool isMissile = false;
    std::string found;
    for (int i = 0; i < 16 && !isMissile; ++i) {
        const uintptr_t off = 0x80 + static_cast<uintptr_t>(i) * 0x68;
        if (slot < off) continue;
        const uintptr_t ctrl = slot - off;
        uintptr_t vt = 0;
        if (!mem::Read<uintptr_t>(ctrl, vt)) continue;
        if (vt < mi.base || vt >= mi.base + mi.size) continue;
        uintptr_t col = 0;
        uint32_t tdRva = 0;
        if (!mem::Read<uintptr_t>(vt - 8, col) || !col) continue;
        if (!mem::Read<uint32_t>(col + 0x0C, tdRva)) continue;
        std::string name;
        for (int c = 0; c < 96; ++c) {
            char ch = 0;
            if (!mem::Read<char>(mi.base + tdRva + 0x10 + c, ch) || ch == 0) break;
            name.push_back(ch);
        }
        if (name.find("Weapon") != std::string::npos && name.find("Ctrl") != std::string::npos) {
            // Strong confirmation: the controller's live mask (+0x5C) must have this
            // slot's bit set. Without it, an unrelated object could be mistaken for a
            // controller because its first qword happened to look like a vtable.
            uint32_t mask = 0;
            if (mem::Read<uint32_t>(ctrl + 0x5C, mask) && (mask & (1u << i)) != 0) {
                isMissile = true;
                found = name;
            }
        }
    }
    if (slotClassCache_.size() < 512) slotClassCache_.push_back({slot, isMissile ? 1 : 0});
    if (className) *className = found;
    return isMissile;
}

void Patcher::OnMissileSlotUpdated(uintptr_t slot, uintptr_t caller) {
    if (!cfg_.runtime.guidanceOverride || !slot) return;
    ++pnIntegratorCalls_;   // heartbeat evidence: the hook is actually being reached

    // Track which game code paths reach the integrator, and dump the object the
    // first time a new call site shows up. That dump is what identifies the layout
    // used by a weapon family we have not seen yet (e.g. the plain MSL).
    if (caller) {
        bool seen = false;
        for (auto& kv : callersSeen_) {
            if (kv.first == caller) { ++kv.second; seen = true; break; }
        }
        if (!seen) {
            if (callersSeen_.size() < 64) callersSeen_.push_back({caller, 1});
            mem::ModuleInfo mi;
            const bool inExe =
                mem::MainModule(mi) && caller >= mi.base && caller < mi.base + mi.size;
            L().Debug("PN: 新的积分器调用点 / new integrator call site %s (caller %s%s)",
                    inExe ? "exe" : "other", mem::Hex(caller).c_str(),
                    inExe ? "" : " <outside main module>");
            if (inExe) {
                L().Debug("PN:   caller RVA = exe+0x%llX",
                        static_cast<unsigned long long>(caller - mi.base));
            }
            // Read-only dump of the object passed in (valid by construction: the game
            // just handed us this pointer).
            L().Debug("PN:   object dump %s:", mem::Hex(slot).c_str());
            for (int off = 0; off < 0x68; off += 0x10) {
                float f0 = 0, f1 = 0, f2 = 0, f3 = 0;
                if (!mem::ReadFloat(slot + off, f0) || !mem::ReadFloat(slot + off + 4, f1) ||
                    !mem::ReadFloat(slot + off + 8, f2) || !mem::ReadFloat(slot + off + 12, f3)) {
                    continue;
                }
                L().Debug("      +0x%02X: %12.4f %12.4f %12.4f %12.4f", off, f0, f1, f2, f3);
            }
        }
    }

    // Hard safety gate: only objects that provably live in a weapon-controller
    // missile array may be written. Everything else is counted and ignored.
    std::string cls;
    if (!IsMissileSlot(slot, &cls)) {
        ++pnNonMissile_;
        if (pnNonMissile_ <= 3) {
            L().Debug("PN: 跳过非导弹对象 %s (caller %s) - 不是武器控制器槽位 / not a missile slot",
                    mem::Hex(slot).c_str(), mem::Hex(caller).c_str());
        }
        return;
    }
    if (!cls.empty()) {
        L().Debug("PN: 识别到导弹槽 / missile slot %s class=%s caller=exe+0x%llX",
                mem::Hex(slot).c_str(), cls.c_str(),
                static_cast<unsigned long long>(caller > 0 ? caller : 0));
    }

    uint8_t validObj = 0, validPart = 0;
    if (!mem::Read<uint8_t>(slot + 0x52, validObj) ||
        !mem::Read<uint8_t>(slot + 0x53, validPart)) {
        return;
    }
    if (validObj == 0xFF || validPart == 0xFF) { ++stats_.guidanceSkipped; return; }

    Vec3 pos, velFrame, aim;
    if (!mem::ReadVec3(slot + 0x00, pos) || !mem::ReadVec3(slot + 0x20, velFrame) ||
        !mem::ReadVec3(slot + 0x30, aim)) {
        return;
    }
    if (!mem::IsFinite(pos) || !mem::IsFinite(velFrame) || !mem::IsFinite(aim)) return;
    const float speedFrame = Length(velFrame);
    if (!(speedFrame > 0.02f && speedFrame < 60.f)) { ++stats_.guidanceSkipped; return; }
    const float range = Length(aim - pos);
    if (!(range > 30.f && range < 30000.f)) return;

    // The engine only stores the target's position, so target velocity is a finite
    // difference over one *frame*. The frame period comes from the slot's own age
    // field (+0x54, seconds, advanced once per frame by the game). If the integrator
    // is entered more than once per frame for the same slot, the previous frame
    // period (and target velocity) is reused instead of a noisy wall-clock delta.
    const double now = Now();
    float age = 0.f;
    mem::Read<float>(slot + 0x54, age);
    Vec3 tgtVel;
    float dt = 1.f / 60.f;
    bool haveSample = false;
    for (auto& h : slotHistory_) {
        if (h.slot != slot) continue;
        haveSample = true;
        const float ageDelta = age - h.age;
        if (ageDelta > 0.0005f && ageDelta < 0.25f) {
            dt = ageDelta;              // exact engine frame time
            h.dtLast = ageDelta;
            h.tgtVel = (aim - h.aim) * (1.f / dt);
            h.aim = aim;
            h.age = age;
        } else if (ageDelta < -0.001f) {
            // age went backwards: the slot was recycled for a new missile
            ++pnLaunches_;
            h.age = age;
            h.aim = aim;
            h.tgtVel = Vec3();
            dt = h.dtLast;
        } else {
            dt = h.dtLast;              // same frame, second entry: reuse
        }
        tgtVel = h.tgtVel;
        h.stamp = now;
        break;
    }
    if (!haveSample) {
        if (slotHistory_.size() < 64) {
            SlotHistory h;
            h.slot = slot;
            h.aim = aim;
            h.age = age;
            h.dtLast = 1.f / 60.f;
            h.stamp = now;
            slotHistory_.push_back(h);
            ++pnSlotsSeen_;
            ++pnLaunches_;
        }
        return;
    }

    // The engine does not tell us which weapon a slot belongs to, so all homing
    // missiles are driven from the [player] entry of the standard missile.
    const FieldSet* fs = cfg_.Resolve(Side::Player, "msl");
    GuidanceParams p;
    p.homingForesightAmount = (fs && fs->homingForesightAmount) ? *fs->homingForesightAmount : 0.f;
    p.maxRotationAngleDegPerSec = (fs && fs->maxRotationAngle) ? *fs->maxRotationAngle : 45.f;
    p.maxHomingAngleDeg = (fs && fs->maxHomingAngle) ? *fs->maxHomingAngle : 90.f;
    if (p.homingForesightAmount <= 0.001f) return;  // 0 = vanilla, leave it alone

    GuidanceInput in;
    in.missilePos = pos;
    in.missileVel = velFrame * (1.f / dt);
    in.targetPos = aim;
    in.targetVel = tgtVel;
    in.dt = dt;
    const GuidanceResult r = ComputeGuidance(in, p);
    // Reject unreliable geometry: when the closing speed is tiny compared to the
    // missile speed (target crossing/beaming at long range) time-to-go saturates and
    // the lead point becomes meaningless - writing it would fling the missile around.
    if (!r.valid || !r.inFov || r.timeToGo > 8.0f) {
        ++stats_.guidanceSkipped;
        if (r.timeToGo > 8.0f) {
            ++pnBadGeometry_;
            if (pnBadGeometry_ <= 3) {
                L().Debug("PN: 跳过几何不可靠的帧 / skipping unreliable geometry "
                        "(range=%.0f m tGo=%.2f s speed=%.0f m/s)",
                        range, r.timeToGo, speedFrame / dt);
            }
        }
        return;
    }

    const Vec3 newVel = r.desiredDir * speedFrame;
    if (!mem::IsFinite(newVel)) return;
    ++pnFrames_;

    // Logging throttle: the integrator runs once per missile per frame, so logging
    // every call would flood the file. First 20 calls, then every 300th, plus a
    // periodic summary that answers "did it track everything I fired?".
    const float speedMs = speedFrame / dt;
    const bool verbose = (pnFrames_ <= 20) || ((pnFrames_ % 300) == 0);
    if ((now - lastPnSummary_) > 20.0) {
        lastPnSummary_ = now;
        L().Info("PN 运行统计 / PN stats: frames=%lld launches=%d slots=%d callers=%d "
                 "nonMissile=%d lastRange=%.0f m lastSpeed=%.0f m/s frame=%.2f ms %s",
                 pnFrames_, pnLaunches_, pnSlotsSeen_, static_cast<int>(callersSeen_.size()),
                 pnNonMissile_, range, speedMs, dt * 1000.0f,
                 cfg_.runtime.dryRun ? "(dry-run)" : "(writing)");
    }

    if (cfg_.runtime.dryRun) {
        ++stats_.guidanceApplied;
        if (verbose) {
            L().Debug("PN dry-run: frames=%lld range=%.0f m  los=%.1f deg  turn=%.2f deg  speed=%.0f m/s  tGo=%.2f s",
                    pnFrames_, range, r.losAngleDeg, r.turnDeg, speedMs, r.timeToGo);
        }
        return;
    }
    if (mem::WriteVec3(slot + 0x20, newVel)) {
        const float zero = 0.f;
        mem::Write<float>(slot + 0x2C, zero);
        ++stats_.guidanceApplied;
        if (verbose) {
            L().Debug("PN applied: frames=%lld range=%.0f m los=%.1f deg turn=%.2f deg speed=%.0f m/s tGo=%.2f",
                    pnFrames_, range, r.losAngleDeg, r.turnDeg, speedMs, r.timeToGo);
        }
    } else {
        ++stats_.guidanceSkipped;
    }
}

// Called from the detour after the game's own update ran. `self` is the missile
// object pointer (argument 1 of the original function).
void Patcher::OnGuidanceHookHit(uintptr_t self) {
    if (!self) return;

    // Probe mode: observation only. We read exactly the controller object the game
    // itself passed in (valid by construction) and never chase pointers found by
    // scanning - that is what crashed the game on an earlier attempt.
    if (probeHookMode_) {
        ++probeHookCalls_;
        uint32_t mask = 0;
        int32_t cur = -1;
        mem::Read<uint32_t>(self + 0x5C, mask);
        mem::Read<int32_t>(self + 0x60, cur);

        // Dump the controller body once per distinct controller object. The
        // dispatcher is called every frame per weapon, so this also tells us which
        // frame index we are in.
        bool seen = false;
        for (uintptr_t c : probeCtrlDumped_) {
            if (c == self) { seen = true; break; }
        }
        if (!seen && probeCtrlDumped_.size() < 16) {
            probeCtrlDumped_.push_back(self);
            L().Key("probe ctrl %s (first sighting, mask=0x%04X curIdx=%d):", mem::Hex(self).c_str(),
                    mask, cur);
            for (int off = 0; off < 0x80; off += 0x10) {
                float f0 = 0, f1 = 0, f2 = 0, f3 = 0;
                if (!mem::ReadFloat(self + off, f0) || !mem::ReadFloat(self + off + 4, f1) ||
                    !mem::ReadFloat(self + off + 8, f2) || !mem::ReadFloat(self + off + 12, f3)) {
                    continue;
                }
                L().Key("    +0x%02X: %12.4f %12.4f %12.4f %12.4f   (u32: %u %u %u %u)", off, f0, f1,
                        f2, f3, *reinterpret_cast<uint32_t*>(&f0), *reinterpret_cast<uint32_t*>(&f1),
                        *reinterpret_cast<uint32_t*>(&f2), *reinterpret_cast<uint32_t*>(&f3));
            }
        }

        // Only frames with live missiles are interesting. Capture several of them so
        // the per-frame deltas reveal position/velocity/target slots.
        if (mask != 0) {
            ++probeFramesLive_;
            L().Key("probe frame %d: ctrl=%s mask=0x%04X curIdx=%d", probeFramesLive_,
                    mem::Hex(self).c_str(), mask, cur);
            for (int i = 0; i < 16; ++i) {
                if ((mask & (1u << i)) == 0) continue;
                const uintptr_t slot = self + 0x80 + static_cast<uintptr_t>(i) * 0x68;
                uint8_t alive = 0;
                mem::Read<uint8_t>(slot + 0x50, alive);
                L().Key("  slot %d @%s alive=0x%02X", i, mem::Hex(slot).c_str(), alive);
                for (int off = 0; off < 0x68; off += 0x10) {
                    float f0 = 0, f1 = 0, f2 = 0, f3 = 0;
                    if (!mem::ReadFloat(slot + off, f0) || !mem::ReadFloat(slot + off + 4, f1) ||
                        !mem::ReadFloat(slot + off + 8, f2) ||
                        !mem::ReadFloat(slot + off + 12, f3)) {
                        continue;
                    }
                    L().Key("    +0x%02X: %12.4f %12.4f %12.4f %12.4f", off, f0, f1, f2, f3);
                }
            }
        }

        // Stop after a few live frames (or after 3 minutes of nothing useful).
        const double elapsed = Now() - startStamp_;
        if (probeFramesLive_ >= 6 || elapsed > 180.0) {
            L().Key("probe: captured %d live frame(s) in %.0f s, unhooking", probeFramesLive_, elapsed);
            TearDownGuidance();
        }
        return;
    }

    if (!cfg_.runtime.guidanceOverride) return;
    ++stats_.guidanceCalls;

    // First hit(s): correlate this object's layout once, then cache it.
    if (!layoutVerified_) {
        const double now = Now();
        if (pendingEntity_ != self || (now - pendingStamp_) > 0.5) {
            pendingEntity_ = self;
            pendingStamp_ = now;
            return;
        }
        game::EntityLayout layout;
        const float dt = static_cast<float>((now - pendingStamp_) > 0.001 ? (now - pendingStamp_) : 0.016);
        if (!game::CorrelateMotion(self, dt, layout) || !layout.verified) {
            ++stats_.guidanceSkipped;
            if (stats_.guidanceSkipped < 8) {
                L().Key("  实体布局未识别 / entity layout not recognised: %s", layout.diag.c_str());
            }
            return;
        }
        layout_ = layout;
        layoutVerified_ = true;
        L().Key("  导弹实体布局已确认 / entity layout verified: pos+0x%X vel+0x%X tgt+0x%X tgtVel+0x%X speed=%.1f",
                layout.posOff, layout.velOff, layout.targetPosOff, layout.targetVelOff,
                layout.speed);
    }
    ApplyGuidanceToEntity(self, "hook");
}

void Patcher::ApplyGuidanceToEntity(uintptr_t entity, const char* source) {
    if (!layoutVerified_ || !entity) { ++stats_.guidanceSkipped; return; }

    Vec3 pos, vel, tgtPos, tgtVel;
    if (!mem::ReadVec3(entity + layout_.posOff, pos) || !mem::ReadVec3(entity + layout_.velOff, vel) ||
        !mem::ReadVec3(entity + layout_.targetPosOff, tgtPos) ||
        !mem::ReadVec3(entity + layout_.targetVelOff, tgtVel)) {
        ++stats_.guidanceSkipped;
        return;
    }
    if (!mem::IsFinite(pos) || !mem::IsFinite(vel) || !mem::IsFinite(tgtPos) || !mem::IsFinite(tgtVel)) {
        ++stats_.guidanceSkipped;
        return;
    }
    const float speed = Length(vel);
    if (speed < 1.f || speed > 20000.f) { ++stats_.guidanceSkipped; return; }

    // Which settings apply? Prefer the row that this entity points at.
    const FieldSet* fs = nullptr;
    for (const auto& e : entities_) {
        if (e.addr == entity) {
            ResolveRowSettings(game::ParamRow{{}, e.modelKey, {}, 0, true}, Side::Player, fs);
            break;
        }
    }
    GuidanceParams p;
    p.homingForesightAmount = 0.f;
    p.maxRotationAngleDegPerSec = 45.f;
    p.maxHomingAngleDeg = 60.f;
    if (fs) {
        if (fs->homingForesightAmount) p.homingForesightAmount = *fs->homingForesightAmount;
        if (fs->maxRotationAngle) p.maxRotationAngleDegPerSec = *fs->maxRotationAngle;
        if (fs->maxHomingAngle) p.maxHomingAngleDeg = *fs->maxHomingAngle;
    }
    if (p.homingForesightAmount <= 0.f) { ++stats_.guidanceSkipped; return; }

    GuidanceInput in;
    in.missilePos = pos;
    in.missileVel = vel;
    in.targetPos = tgtPos;
    in.targetVel = tgtVel;
    in.dt = 1.0f / 60.0f;
    const GuidanceResult r = ComputeGuidance(in, p);
    if (!r.valid) { ++stats_.guidanceSkipped; return; }
    if (!r.inFov) { ++stats_.guidanceSkipped; return; }

    Vec3 newVel = r.desiredDir * speed;
    if (!mem::IsFinite(newVel)) { ++stats_.guidanceSkipped; return; }
    if (cfg_.runtime.dryRun) {
        ++stats_.guidanceApplied;
        if (stats_.guidanceApplied < 4) {
            L().Key("  [dry] PN %s: los=%.2f° turn=%.3f° -> dir(%.3f, %.3f, %.3f)", source,
                    r.losAngleDeg, r.turnDeg, r.desiredDir.x, r.desiredDir.y, r.desiredDir.z);
        }
        return;
    }
    if (mem::WriteVec3(entity + layout_.velOff, newVel)) {
        ++stats_.guidanceApplied;
        if (stats_.guidanceApplied < 4 || (stats_.guidanceApplied % 600) == 0) {
            L().Key("  PN %s: los=%.2f° turn=%.3f° speed=%.1f tGo=%.2f", source, r.losAngleDeg,
                    r.turnDeg, speed, r.timeToGo);
        }
    } else {
        ++stats_.guidanceSkipped;
    }
}

void Patcher::Tick() {
    const double now = Now();

    // Run A probe (no-op unless [runtime] Probe = 1).  Deliberately first: the probe
    // owns its deferred hook installation, capture drain and sampling, and must keep
    // running even though the static-parameter path is off in probe runs.
    probe_.Tick();
    watch_.Tick();   // Run B hardware read-watch (no-op unless HwWatch = 1)
    msl_pn::Tick();  // plain-MSL PN hook install + summaries (no-op unless armed)

    if (poolProbeActive_) ProbeProjectilePool();
    if (ringProbeActive_) ProbeRingSample();
    TickPageGuard();
    RingSurveyTick(now);

    // Deferred hook installation: retry until SteamStub has decrypted .text (the
    // real bytes appear) or give up after 60 s.
    if (pendingHook_) {
        if ((now - lastHookTry_) > 0.2) {
            lastHookTry_ = now;
            ++hookTries_;
            const bool ok = InstallIntegratorHook(/*quiet=*/true);
            if (ok) {
                pendingHook_ = false;
                stats_.guidanceActive = true;
                L().Info("比例引导已接管 / proportional navigation active%s",
                         cfg_.runtime.dryRun ? " (DryRun: logging only)" : "");
            } else if (hookTries_ >= 300) {  // 300 * 200 ms = 60 s
                pendingHook_ = false;
                L().Warn("PN: 60 s 内未等到解密后的 .text（或字节不匹配），放弃挂钩，保持只读。");
                L().Warn("PN: .text did not become verifiable within 60 s; staying read-only.");
                // One loud, diagnostic attempt so the log records what we saw.
                InstallIntegratorHook(/*quiet=*/false);
            } else if (hookTries_ == 1) {
                L().Key("PN: 首次尝试挂钩时 .text 尚未就绪，继续每 200 ms 重试（这是正常的）");
            }
        }
    }

    // ---- hook watchdog + activity heartbeat ---------------------------------
    // Two things must be visible even when nothing is being steered: whether our
    // jump is still in place, and whether the integrator is being called at all.
    if (pnHooked_) {
        if ((now - lastHookCheck_) > 10.0) {
            lastHookCheck_ = now;
            uint8_t head[8] = {};
            const bool readable =
                mem::ReadBytes(reinterpret_cast<uintptr_t>(hookTarget_), head, sizeof(head));
            static const uint8_t kOrig[8] = {0x48, 0x8B, 0xC4, 0xC5, 0xFA, 0x11, 0x58, 0x20};
            // Our patch is either a 5-byte E9 (rel32) or a 14-byte "jmp [rip+0]"
            // (FF 25 ...) followed by the absolute target; both are ours.
            const bool isOurs = readable && (head[0] == 0xE9 || (head[0] == 0xFF && head[1] == 0x25));
            const bool isOriginal = readable && std::memcmp(head, kOrig, sizeof(kOrig)) == 0;
            if (!isOurs && isOriginal && hookReinstalls_ < 3) {
                ++hookReinstalls_;
                L().Warn("PN: 钩子字节被还原（游戏改回了原始代码），重新安装 / hook bytes restored, "
                         "reinstalling (attempt %d)",
                         hookReinstalls_);
                const bool ok = InstallIntegratorHook(/*quiet=*/true);
                L().Warn(ok ? "PN: 钩子已重新安装 / hook reinstalled"
                            : "PN: 重新安装失败 / reinstall failed");
            } else if (!isOurs && !isOriginal && (now - lastHookAnomaly_) > 60.0) {
                // Reading code bytes inside a Steam-DRM process can transiently show
                // something unexpected; do not spam, do not thrash the hook. The
                // heartbeat below is the authority on whether it still works.
                lastHookAnomaly_ = now;
                L().Key("PN: 看门狗读到异常字节 %02X %02X %02X %02X（心跳为准，不重装）",
                        head[0], head[1], head[2], head[3]);
            }
        }
        if ((now - lastHeartbeat_) > 30.0) {
            lastHeartbeat_ = now;
            L().Info("PN 心跳 / heartbeat: integratorCalls=%lld frames=%lld launches=%d "
                     "callers=%d nonMissile=%d badGeometry=%d applied=%lld %s",
                     pnIntegratorCalls_, pnFrames_, pnLaunches_,
                     static_cast<int>(callersSeen_.size()), pnNonMissile_, pnBadGeometry_,
                     stats_.guidanceApplied,
                     pnIntegratorCalls_ == 0
                         ? "(no homing missile has flown yet - or the hook is not reached)"
                         : "(alive)");
        }
    }

    // The static-parameter path is off unless [vanilla] is configured; in that case
    // there is nothing to rediscover and the game stays untouched (no heavy scans).
    if (!staticPatch_) {
        if (cfg_.runtime.guidanceOverride && !hookInstalled_ && cfg_.runtime.entityOverride) {
            if (entities_.empty() && (now - lastEntityScan_) > 2.0) DiscoverEntities();
            if (layoutVerified_) {
                stats_.guidanceActive = true;
                for (const auto& e : entities_) ApplyGuidanceToEntity(e.addr, "entity");
            }
        }
        // A hooked probe that never fires means no missile was launched yet.
        if (probeHookMode_ && probeHookCalls_ == 0 && !probeIdleLogged_ &&
            (now - startStamp_) > 90.0) {
            probeIdleLogged_ = true;
            L().Key("probe: hook is installed but no missile launch has been seen yet - "
                    "fire a missile (or let wingmen/enemies fire) to capture the slot layout");
            L().Warn("Probe 钩子已装但还没见到发射：请进任务并发射几枚导弹（僚机/敌机发射也算）。");
        }
        return;
    }

    // The game reloaded weapon data under us: drop everything and rediscover.
    if (staleRows_ > 0) {
        L().Warn("参数表疑似被游戏重新加载（%d 个取值不再匹配），重新搜索 / data reloaded, "
                 "re-running discovery", staleRows_);
        staleRows_ = 0;
        rows_.clear();
        lastWrites_.clear();
        RunDiscovery(true);
        ApplyStaticParams();
    }

    // Rediscover if we never found a table (weapon data may load with the level).
    // Back off: an unsuccessful pass can scan well over a gigabyte of memory.
    if (rows_.empty() && (now - lastDiscovery_) > discoveryBackoff_) {
        const double interval = lightScanOnly_ ? 120.0 : discoveryBackoff_;
        if (lightScanOnly_ || (now - lastDiscovery_) > interval) {
            L().Info("重新搜索参数表 / re-running discovery (attempt %d, next retry in %.0f s)",
                     scanAttempts_ + 1, std::min(interval * 2.0, lightScanOnly_ ? 120.0 : 60.0));
            RunDiscovery(true);
            ApplyStaticParams();
            ++scanAttempts_;
            if (rows_.empty()) {
                discoveryBackoff_ = std::min(discoveryBackoff_ * 2.0, 60.0);
                // A full value-correlation pass is expensive (seconds to tens of
                // seconds on a multi-GB heap). After a few misses the game has been
                // fully loaded and the on-disk layout is simply not present, so we
                // drop to the cheap byte scan instead of stuttering the game.
                if (scanAttempts_ >= 3 && !lightScanOnly_) {
                    lightScanOnly_ = true;
                    L().Warn("连续 %d 次未命中：切换到轻量扫描，避免影响游戏性能。"
                             "请用 Probe=1 跑一次并把 ACZPropNav.key.log 发出来。",
                             scanAttempts_);
                    L().Warn("%d discovery misses: switching to light scanning so the game stays "
                             "smooth. Set Probe=1 and send ACZPropNav.key.log.", scanAttempts_);
                }
            }
        }
    }
    // Re-apply static params when the game reloads weapon data.
    if (!rows_.empty() && (now - lastParamPatch_) > 20.0) {
        ApplyStaticParams();
    }

    if (cfg_.runtime.guidanceOverride && !hookInstalled_ && cfg_.runtime.entityOverride) {
        if (entities_.empty() && (now - lastEntityScan_) > 2.0) DiscoverEntities();
        if (layoutVerified_) {
            stats_.guidanceActive = true;
            for (const auto& e : entities_) ApplyGuidanceToEntity(e.addr, "entity");
        }
    }
}

void Patcher::Probe() {
    if (probe_.Active()) {
        // The legacy one-shot probe installs a dispatcher hook and scans the heap again.
        // With Probe=1 that is both redundant and extra risk: Run A is already running.
        L().Info("Probe=1：Run A 探针正在运行，跳过旧的一次性探测 / Run A probe active; legacy "
                 "one-shot probe skipped");
        return;
    }
    if (watch_.Active() || watch_.Finished()) {
        // Same reasoning for Run B: it owns the debug registers; a dispatcher hook and
        // heap scans would only add noise/risk.
        L().Info("HwWatch=1：Run B 探针正在运行，跳过旧的一次性探测 / Run B probe active; legacy "
                 "one-shot probe skipped");
        return;
    }
    L().Key("================ PROBE ================");
    mem::ModuleInfo mi;
    if (mem::MainModule(mi)) {
        L().Key("module %s base=%s size=0x%llX", WideToUtf8(mi.path).c_str(), mem::Hex(mi.base).c_str(),
                static_cast<unsigned long long>(mi.size));
    }
    auto regions = mem::WritableRegions(0x10000);
    size_t total = 0;
    for (const auto& r : regions) total += r.size;
    L().Key("writable regions: %d, total 0x%llX bytes", static_cast<int>(regions.size()),
            static_cast<unsigned long long>(total));

    for (const auto& d : discoveryDiag_) L().Key("discovery: %s", d.c_str());
    L().Key("rows found=%d verified=%d", stats_.rowsFound, stats_.rowsVerified);
    LogRows();

    // Probe mode is deliberately HOOK-FREE: the projectile-pool scan is pure
    // read-only sampling from Tick(). No code is patched, so probe mode cannot
    // influence the game at all (earlier versions also hooked the weapon
    // dispatcher here, which is unnecessary for this probe).
    L().Key("probe: read-only mode (dispatcher observer + pool sampling)");
    ProbeDispatcher();
    StartPoolProbe();
    StartRingProbe();
    ProbeLockIndicator();

    if (!rows_.empty()) {
        std::vector<game::CodeRefSite> refs;
        const uintptr_t lo = rows_.front().rowAddr;
        uintptr_t hi = lo;
        for (const auto& r : rows_) hi = std::max(hi, r.rowAddr + 0x80);
        game::FindCodeReferences(lo, hi, refs, 64);
        L().Key("code references into [%s, %s): %d", mem::Hex(lo).c_str(), mem::Hex(hi).c_str(),
                static_cast<int>(refs.size()));
        for (const auto& r : refs) {
            L().Key("  ref insn %s -> %s", mem::Hex(r.insnAddr).c_str(), mem::Hex(r.target).c_str());
        }
    }

    DiscoverEntities();
    for (const auto& e : entities_) {
        L().Key("entity candidate %s model=%s rowPtr=%s", mem::Hex(e.addr).c_str(),
                e.modelKey.c_str(), mem::Hex(e.rowPtrAddr).c_str());
        game::EntityLayout layout;
        if (game::CorrelateMotion(e.addr, 1.0f / 60.0f, layout)) {
            L().Key("  layout verified=%d pos+0x%X vel+0x%X tgt+0x%X tgtVel+0x%X speed=%.1f :: %s",
                    layout.verified ? 1 : 0, layout.posOff, layout.velOff, layout.targetPosOff,
                    layout.targetVelOff, layout.speed, layout.diag.c_str());
        }
        // Raw float dump around the object helps manual calibration.
        char line[256];
        size_t n = 0;
        for (int off = 0; off < 0x100 && n < sizeof(line) - 16; off += 4) {
            float f = 0.f;
            if (!mem::ReadFloat(e.addr + off, f)) continue;
            if (!std::isfinite(f)) continue;
            int written = std::snprintf(line + n, sizeof(line) - n, "%+08.3f ", f);
            if (written <= 0) break;
            n += static_cast<size_t>(written);
        }
        line[n] = '\0';
        L().Key("  floats: %s", line);
    }
    L().Key("================ /PROBE ================");
}

void Patcher::Shutdown() {
    probe_.Shutdown();
    watch_.Shutdown();
    msl_pn::Shutdown();
    TearDownGuidance();
    g_trampoline = nullptr;
}

} // namespace acz
