// tools/initest - standalone acceptance test for the ACZPropNav INI parser.
//
// Two layers:
//   1. the REAL AC8PropNav.ini shipped with the AC8 mod (when present on this
//      machine): 11 player models + 1 enemy model, the exact
//      HomingForesightAmount values, every other property unset, zero warnings;
//   2. synthetic INIs written to a temporary file at runtime, covering block and
//      legacy forms, blank/absent values, name normalization, comments, CRLF,
//      BOM, unknown property/section diagnostics, duplicate merging,
//      Config::Resolve() longest-key-first, Config::Sanitize() clamps,
//      ParseFloatValue() edges, the [runtime] extension keys, the [vanilla]
//      discovery section, missing/oversized files.
//
// Every check prints PASS or FAIL with a short reason; the final line is
// "PASS n / FAIL n" and the process exits with 1 when anything failed.
// No external dependencies beyond kernel32 (static CRT).

#include "core/config.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <optional>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

// ===========================================================================
//  tiny test harness
// ===========================================================================

int g_pass = 0;
int g_fail = 0;
int g_skip = 0;
unsigned g_tempCounter = 0;

std::string Fmt(const char* format, ...) {
    char buffer[2048];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return std::string(buffer);
}

void Header(const char* name) { std::printf("\n== %s ==\n", name); }

void Check(bool ok, const std::string& what, const std::string& detail = std::string()) {
    if (ok) {
        ++g_pass;
        std::printf("PASS  %s\n", what.c_str());
    } else {
        ++g_fail;
        std::printf("FAIL  %s%s%s\n", what.c_str(), detail.empty() ? "" : "  --  ", detail.c_str());
    }
}

void Skip(const std::string& what, const std::string& why) {
    ++g_skip;
    std::printf("SKIP  %s  --  %s\n", what.c_str(), why.c_str());
}

bool Eq(float a, float b) { return std::fabs(a - b) <= 1e-5f * (1.0f + std::fabs(b)); }

std::string Describe(const std::optional<float>& value) {
    return value ? Fmt("%g", static_cast<double>(*value)) : std::string("unset");
}

bool EqOpt(const std::optional<float>& value, float expected) { return value.has_value() && Eq(*value, expected); }

void CheckFloat(const std::optional<float>& got, float expected, const std::string& what) {
    Check(EqOpt(got, expected), what,
          "got " + Describe(got) + ", expected " + Fmt("%g", static_cast<double>(expected)));
}

// ===========================================================================
//  config inspection helpers
// ===========================================================================

const acz::MissileModel* FindModel(const std::vector<acz::MissileModel>& table, const std::string& key) {
    for (const acz::MissileModel& model : table) {
        if (model.key == key) return &model;
    }
    return nullptr;
}

const acz::FieldSet* FindFields(const acz::Config& cfg, acz::Side side, const std::string& key) {
    const acz::MissileModel* model = FindModel(cfg.models, key);
    if (model == nullptr) return nullptr;
    return (side == acz::Side::Enemy) ? &model->enemy : &model->player;
}

int CountConfigured(const acz::Config& cfg, acz::Side side) {
    int count = 0;
    for (const acz::MissileModel& model : cfg.models) {
        const acz::FieldSet& fields = (side == acz::Side::Enemy) ? model.enemy : model.player;
        if (fields.Any()) ++count;
    }
    return count;
}

void CheckWarningCount(const acz::Config& cfg, size_t expected, const std::string& what) {
    Check(cfg.warnings.size() == expected, what,
          Fmt("got %u warning(s): %s", static_cast<unsigned>(cfg.warnings.size()),
              cfg.WarningSummary().c_str()));
}

bool AnyWarningContains(const acz::Config& cfg, const std::string& needle) {
    for (const std::string& warning : cfg.warnings) {
        if (warning.find(needle) != std::string::npos) return true;
    }
    return false;
}

// ===========================================================================
//  temporary file helpers (GetTempPath / CreateFile, ./build as a fallback)
// ===========================================================================

std::string Narrow(const std::wstring& wide) {
    if (wide.empty()) return std::string();
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string out(static_cast<size_t>(need), '\0');
    const int wrote = ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                                            &out[0], need, nullptr, nullptr);
    if (wrote <= 0) return std::string();
    out.resize(static_cast<size_t>(wrote));
    return out;
}

std::wstring MakeTempPath() {
    wchar_t directory[MAX_PATH + 1] = {};
    const DWORD length = ::GetTempPathW(MAX_PATH, directory);
    if (length > 0 && length < MAX_PATH) {
        wchar_t file[MAX_PATH + 1] = {};
        if (::GetTempFileNameW(directory, L"acz", 0, file) != 0) return std::wstring(file);
    }
    // Fallback: a file inside ./build (created on demand).
    ::CreateDirectoryW(L"build", nullptr);
    wchar_t buffer[160] = {};
    std::swprintf(buffer, 160, L"build\\initest_%lu_%u.ini",
                  static_cast<unsigned long>(::GetCurrentProcessId()), ++g_tempCounter);
    return std::wstring(buffer);
}

// A temp .ini that deletes itself when it goes out of scope.
class TempIni {
public:
    TempIni() = default;
    TempIni(const TempIni&) = delete;
    TempIni& operator=(const TempIni&) = delete;
    ~TempIni() {
        if (!path_.empty()) ::DeleteFileW(path_.c_str());
    }

    bool Write(const std::string& text, bool withBom = false) {
        path_ = MakeTempPath();
        if (path_.empty()) return false;

        std::string data;
        if (withBom) data.assign("\xEF\xBB\xBF", 3);
        data += text;

        const HANDLE handle = ::CreateFileW(path_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            path_.clear();
            return false;
        }
        bool ok = true;
        size_t offset = 0;
        while (offset < data.size()) {
            const size_t remaining = data.size() - offset;
            const DWORD chunk = static_cast<DWORD>(remaining > 65536 ? 65536 : remaining);
            DWORD written = 0;
            if (!::WriteFile(handle, data.data() + offset, chunk, &written, nullptr) || written != chunk) {
                ok = false;
                break;
            }
            offset += written;
        }
        ::CloseHandle(handle);
        if (!ok) {
            ::DeleteFileW(path_.c_str());
            path_.clear();
        }
        return ok;
    }

    const std::wstring& path() const { return path_; }

private:
    std::wstring path_;
};

// Write `text` to a temp .ini, parse it. Returns false only on file IO trouble
// (a parser failure is reported by the caller's assertions).
bool LoadText(const std::string& text, acz::Config& cfg, std::string& error, bool withBom = false) {
    TempIni file;
    if (!file.Write(text, withBom)) {
        error = "cannot create/write the temporary .ini";
        return false;
    }
    if (!cfg.LoadFromFile(file.path())) {
        error = "LoadFromFile returned false (file was written successfully)";
        return false;
    }
    error.clear();
    return true;
}

bool LoadOrFail(const std::string& what, const std::string& text, acz::Config& cfg, bool withBom = false) {
    std::string error;
    if (LoadText(text, cfg, error, withBom)) return true;
    Check(false, what, error);
    return false;
}

bool FileExists(const std::wstring& path) {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::string WideToUtf8(const std::wstring& wide) { return Narrow(wide); }

// The reference config of the AC8 mod; overridable through AC8PROPNAV_INI.
std::wstring ReferenceIniPath() {
    wchar_t fromEnv[2048] = {};
    const DWORD length = ::GetEnvironmentVariableW(L"AC8PROPNAV_INI", fromEnv, 2048);
    if (length > 0 && length < 2048) return std::wstring(fromEnv, length);
    // Fallback for a developer machine; set AC8PROPNAV_INI to point anywhere else.
    // Missing files only skip the reference-INI comparison (see TestReferenceIni).
    return std::wstring(L"AC8PropNav.ini");
}

// ===========================================================================
//  1. the real AC8PropNav.ini
// ===========================================================================

void TestReferenceIni() {
    Header("reference config: real AC8PropNav.ini");

    const std::wstring path = ReferenceIniPath();
    if (!FileExists(path)) {
        Skip("reference ini", "not present at " + WideToUtf8(path));
        return;
    }
    std::printf("      using %s\n", WideToUtf8(path).c_str());

    acz::Config cfg;
    Check(cfg.LoadFromFile(path), "reference: LoadFromFile succeeds");
    Check(cfg.loaded, "reference: loaded flag is set");
    Check(cfg.sourcePath.find("AC8PropNav.ini") != std::string::npos,
          "reference: sourcePath recorded as UTF-8", cfg.sourcePath);

    if (!cfg.warnings.empty()) {
        for (const std::string& warning : cfg.warnings) std::printf("      warning: %s\n", warning.c_str());
    }
    CheckWarningCount(cfg, 0, "reference: zero warnings");

    struct Expectation {
        const char* key;
        float lead;
    };
    static const Expectation kPlayerModels[] = {
        {"msl", 0.7f},  {"qaam", 1.0f}, {"hvaa", 0.8f}, {"laam", 0.85f}, {"hcaa", 0.8f}, {"hpaa", 0.6f},
        {"saam", 0.8f}, {"sasm", 0.8f}, {"4aam", 0.85f}, {"6aam", 0.8f}, {"2aam", 0.9f},
    };
    for (const Expectation& expected : kPlayerModels) {
        const acz::MissileModel* model = FindModel(cfg.models, expected.key);
        Check(model != nullptr, Fmt("reference: player model '%s' present", expected.key));
        if (model == nullptr) continue;
        Check(EqOpt(model->player.homingForesightAmount, expected.lead),
              Fmt("reference: player %s HomingForesightAmount == %g", expected.key,
                  static_cast<double>(expected.lead)),
              "got " + Describe(model->player.homingForesightAmount));
    }

    const acz::MissileModel* enemyMsl = FindModel(cfg.models, "msl");
    Check(enemyMsl != nullptr && EqOpt(enemyMsl->enemy.homingForesightAmount, 0.3f),
          "reference: enemy msl HomingForesightAmount == 0.3",
          enemyMsl ? Describe(enemyMsl->enemy.homingForesightAmount) : std::string("model missing"));

    Check(CountConfigured(cfg, acz::Side::Player) == 11, "reference: 11 configured player models",
          Fmt("got %d", CountConfigured(cfg, acz::Side::Player)));
    Check(CountConfigured(cfg, acz::Side::Enemy) == 1, "reference: 1 configured enemy model",
          Fmt("got %d", CountConfigured(cfg, acz::Side::Enemy)));
    Check(cfg.models.size() == 11, "reference: 11 entries (msl is shared by both sides)",
          Fmt("got %u", static_cast<unsigned>(cfg.models.size())));
    Check(cfg.vanilla.empty(), "reference: no [vanilla] section -> Config::vanilla empty");

    // Only HomingForesightAmount may be set; the other four stay vanilla (nullopt).
    for (const acz::MissileModel& model : cfg.models) {
        for (int index = 0; index < 2; ++index) {
            const acz::Side side = (index == 0) ? acz::Side::Player : acz::Side::Enemy;
            const acz::FieldSet& fields = (index == 0) ? model.player : model.enemy;
            if (!fields.Any()) continue;  // side not configured for this model

            std::string offenders;
            if (fields.maxHomingAngle) offenders += " MaxHomingAngle=" + Describe(fields.maxHomingAngle);
            if (fields.maxRotationAngle) offenders += " MaxRotationAngle=" + Describe(fields.maxRotationAngle);
            if (fields.lifeTime) offenders += " LifeTime=" + Describe(fields.lifeTime);
            if (fields.lockonRange) offenders += " LockonRange=" + Describe(fields.lockonRange);
            Check(offenders.empty(),
                  std::string("reference: ") + acz::SideName(side) + " " + model.key +
                      " sets only HomingForesightAmount",
                  offenders);
        }
    }

    // Resolve() against realistic game identifiers.
    const acz::FieldSet* resolved = cfg.Resolve(acz::Side::Player, "MSL_STD");
    Check(resolved != nullptr && EqOpt(resolved->homingForesightAmount, 0.7f),
          "reference: Resolve(player, MSL_STD) -> msl 0.7",
          resolved == nullptr ? std::string("nullptr") : Describe(resolved->homingForesightAmount));
    resolved = cfg.Resolve(acz::Side::Player, "4AAM_X");
    Check(resolved != nullptr && EqOpt(resolved->homingForesightAmount, 0.85f),
          "reference: Resolve(player, 4AAM_X) -> 4aam 0.85",
          resolved == nullptr ? std::string("nullptr") : Describe(resolved->homingForesightAmount));
    resolved = cfg.Resolve(acz::Side::Player, "QAAM");
    Check(resolved != nullptr && EqOpt(resolved->homingForesightAmount, 1.0f),
          "reference: Resolve(player, QAAM) -> qaam 1.0");
    Check(cfg.Resolve(acz::Side::Player, "GUN") == nullptr,
          "reference: Resolve(player, GUN) -> nullptr");
    resolved = cfg.Resolve(acz::Side::Enemy, "MSL");
    Check(resolved != nullptr && EqOpt(resolved->homingForesightAmount, 0.3f),
          "reference: Resolve(enemy, MSL) -> msl 0.3");
    Check(cfg.Resolve(acz::Side::Enemy, "QAAM") == nullptr,
          "reference: Resolve(enemy, QAAM) -> nullptr (only enemy msl configured)");

    // The reference values are all in range: sanitizing must not warn.
    const size_t warningsBefore = cfg.warnings.size();
    cfg.Sanitize();
    Check(cfg.warnings.size() == warningsBefore, "reference: Sanitize adds no warnings",
          cfg.WarningSummary());

    // [runtime] absent -> documented defaults.
    const acz::RuntimeSection& rt = cfg.runtime;
    Check(rt.logLevel == 1 && !rt.dryRun && !rt.probe && rt.tickHz == 120 && rt.requireVerifiedOffsets &&
              rt.guidanceOverride && !rt.forceOffsets && !rt.entityOverride && rt.hookArgIndex == 1 &&
              rt.hookSignature.empty() && !rt.entityPosOffset && !rt.entityVelOffset &&
              !rt.targetPosOffset && !rt.targetVelOffset && !rt.hookRva && rt.leadScale == 260.0f &&
              rt.leadMode == "add",
          "reference: [runtime] defaults (AC8 config has no extension section)");
}

// ===========================================================================
//  2. block / legacy / blank forms
// ===========================================================================

void TestBlockForms() {
    Header("block form (AC8 layout)");

    {
        acz::Config cfg;
        if (!LoadOrFail("block: multi-line load",
                        "[player]\n"
                        "msl = {\n"
                        "    HomingForesightAmount = 0.7\n"
                        "    MaxHomingAngle        = 90\n"
                        "    MaxRotationAngle      = 40\n"
                        "    LifeTime              = 9\n"
                        "    LockonRange           = 2000\n"
                        "}\n",
                        cfg))
            return;
        CheckWarningCount(cfg, 0, "block: multi-line has no warnings");
        Check(cfg.models.size() == 1, "block: multi-line creates exactly one model");
        const acz::FieldSet* fields = FindFields(cfg, acz::Side::Player, "msl");
        Check(fields != nullptr, "block: multi-line model 'msl' exists");
        if (fields) {
            CheckFloat(fields->homingForesightAmount, 0.7f, "block: multi-line HomingForesightAmount = 0.7");
            CheckFloat(fields->maxHomingAngle, 90.0f, "block: multi-line MaxHomingAngle = 90");
            CheckFloat(fields->maxRotationAngle, 40.0f, "block: multi-line MaxRotationAngle = 40");
            CheckFloat(fields->lifeTime, 9.0f, "block: multi-line LifeTime = 9");
            CheckFloat(fields->lockonRange, 2000.0f, "block: multi-line LockonRange = 2000");
        }
    }

    {
        acz::Config cfg;
        if (!LoadOrFail("block: single-line load",
                        "[player]\n"
                        "4aam = { HomingForesightAmount = 0.5 MaxRotationAngle = 60 }\n",
                        cfg))
            return;
        CheckWarningCount(cfg, 0, "block: single-line has no warnings");
        const acz::FieldSet* fields = FindFields(cfg, acz::Side::Player, "4aam");
        Check(fields != nullptr, "block: single-line model '4aam' exists");
        if (fields) {
            CheckFloat(fields->homingForesightAmount, 0.5f, "block: single-line HomingForesightAmount = 0.5");
            CheckFloat(fields->maxRotationAngle, 60.0f, "block: single-line MaxRotationAngle = 60");
            Check(!fields->maxHomingAngle && !fields->lifeTime && !fields->lockonRange,
                  "block: single-line leaves unmentioned properties unset");
        }
    }

    {
        acz::Config cfg;
        if (!LoadOrFail("block: brace-on-next-line load",
                        "[player]\n"
                        "msl =\n"
                        "{\n"
                        "    HomingForesightAmount = 0.4\n"
                        "}\n",
                        cfg))
            return;
        CheckWarningCount(cfg, 0, "block: brace-on-next-line has no warnings");
        const acz::FieldSet* fields = FindFields(cfg, acz::Side::Player, "msl");
        Check(fields != nullptr, "block: brace-on-next-line model exists");
        if (fields) CheckFloat(fields->homingForesightAmount, 0.4f, "block: brace-on-next-line parses the value");
    }

    {
        acz::Config cfg;
        if (!LoadOrFail("block: packed load",
                        "[player]\n"
                        "qaam={MaxHomingAngle=120 MaxRotationAngle=75 LifeTime=12 LockonRange=3000 "
                        "HomingForesightAmount=1}\n",
                        cfg))
            return;
        CheckWarningCount(cfg, 0, "block: packed properties have no warnings");
        const acz::FieldSet* fields = FindFields(cfg, acz::Side::Player, "qaam");
        Check(fields != nullptr, "block: packed model exists");
        if (fields) {
            CheckFloat(fields->maxHomingAngle, 120.0f, "block: packed MaxHomingAngle = 120");
            CheckFloat(fields->maxRotationAngle, 75.0f, "block: packed MaxRotationAngle = 75");
            CheckFloat(fields->lifeTime, 12.0f, "block: packed LifeTime = 12");
            CheckFloat(fields->lockonRange, 3000.0f, "block: packed LockonRange = 3000");
            CheckFloat(fields->homingForesightAmount, 1.0f, "block: packed HomingForesightAmount = 1");
        }
    }

    {
        // "LockonRange = MaxHomingAngle = 90": the first key is blank, the second
        // starts on the same line.
        acz::Config cfg;
        if (!LoadOrFail("block: blank-then-property load",
                        "[player]\n"
                        "msl = { LockonRange = MaxHomingAngle = 90 }\n",
                        cfg))
            return;
        CheckWarningCount(cfg, 0, "block: blank-then-property has no warnings");
        const acz::FieldSet* fields = FindFields(cfg, acz::Side::Player, "msl");
        Check(fields != nullptr, "block: blank-then-property model exists");
        if (fields) {
            Check(!fields->lockonRange, "block: 'LockonRange =' before another key stays unset");
            CheckFloat(fields->maxHomingAngle, 90.0f, "block: the following key is still parsed (MaxHomingAngle = 90)");
        }
    }
}

void TestLegacyAndBlank() {
    Header("legacy shorthand, blank and absent values");

    {
        acz::Config cfg;
        if (!LoadOrFail("legacy: load", "[player]\nmsl = 0.8\n", cfg)) return;
        CheckWarningCount(cfg, 0, "legacy: no warnings");
        const acz::FieldSet* fields = FindFields(cfg, acz::Side::Player, "msl");
        Check(fields != nullptr, "legacy: model exists");
        if (fields) {
            CheckFloat(fields->homingForesightAmount, 0.8f, "legacy: 'msl = 0.8' sets HomingForesightAmount");
            Check(!fields->maxHomingAngle && !fields->maxRotationAngle && !fields->lifeTime && !fields->lockonRange,
                  "legacy: only HomingForesightAmount is set");
            Check(fields->Count() == 1, "legacy: FieldSet::Count() == 1", Fmt("got %d", fields->Count()));
        }
    }

    {
        acz::Config cfg;
        if (!LoadOrFail("legacy: integer load", "[player]\nmsl = 1\n", cfg)) return;
        const acz::FieldSet* fields = FindFields(cfg, acz::Side::Player, "msl");
        Check(fields != nullptr && EqOpt(fields->homingForesightAmount, 1.0f), "legacy: 'msl = 1' -> 1.0");
    }

    {
        acz::Config cfg;
        if (!LoadOrFail("legacy: comment load", "[player]\nmsl = 0.85 ; legacy with comment\n", cfg)) return;
        const acz::FieldSet* fields = FindFields(cfg, acz::Side::Player, "msl");
        Check(fields != nullptr && EqOpt(fields->homingForesightAmount, 0.85f),
              "legacy: trailing comment after the shorthand is stripped");
        CheckWarningCount(cfg, 0, "legacy: trailing comment produces no warning");
    }

    {
        acz::Config cfg;
        if (!LoadOrFail("legacy: garbage load", "[player]\nmsl = abc\n", cfg)) return;
        Check(cfg.models.empty(), "legacy: garbage value creates no model");
        Check(cfg.warnings.size() == 1 && AnyWarningContains(cfg, "invalid value"),
              "legacy: garbage value warns", cfg.WarningSummary());
    }

    {
        acz::Config cfg;
        if (!LoadOrFail("blank: load",
                        "[player]\n"
                        "msl = {\n"
                        "    HomingForesightAmount =\n"
                        "    LockonRange           = ; vanilla MSL\n"
                        "    LifeTime\n"
                        "    MaxHomingAngle        = 75\n"
                        "}\n",
                        cfg))
            return;
        CheckWarningCount(cfg, 0, "blank: blank values produce no warnings");
        const acz::FieldSet* fields = FindFields(cfg, acz::Side::Player, "msl");
        Check(fields != nullptr, "blank: model exists");
        if (fields) {
            Check(!fields->homingForesightAmount, "blank: 'Key =' with nothing stays unset");
            Check(!fields->lockonRange, "blank: 'Key = ; comment' stays unset");
            Check(!fields->lifeTime, "blank: key with no '=' at all stays unset");
            CheckFloat(fields->maxHomingAngle, 75.0f, "blank: other properties are still parsed");
        }
    }
}

// ===========================================================================
//  3. names, comments, line endings, BOM
// ===========================================================================

void TestPropertyNames() {
    Header("case/underscore/space-insensitive names");

    {
        acz::Config cfg;
        if (!LoadOrFail("names: load",
                        "[player]\n"
                        "MSL = {\n"
                        "    MAX_HOMING_ANGLE          = 75\n"
                        "    Max Rotation Angle        = 60\n"
                        "    homing_foresight_amount   = 0.42\n"
                        "    lifetime                  = 12\n"
                        "    LOCKONRANGE               = 1800\n"
                        "}\n",
                        cfg))
            return;
        CheckWarningCount(cfg, 0, "names: no warnings");
        const acz::MissileModel* model = FindModel(cfg.models, "msl");
        Check(model != nullptr, "names: upper-case model key is lower-cased");
        if (model) {
            CheckFloat(model->player.homingForesightAmount, 0.42f, "names: homing_foresight_amount");
            CheckFloat(model->player.maxHomingAngle, 75.0f, "names: MAX_HOMING_ANGLE");
            CheckFloat(model->player.maxRotationAngle, 60.0f, "names: 'Max Rotation Angle' (spaces ignored)");
            CheckFloat(model->player.lifeTime, 12.0f, "names: lifetime");
            CheckFloat(model->player.lockonRange, 1800.0f, "names: LOCKONRANGE");
        }
    }

    {
        std::string name = "  Max_Homing  Angle ";
        acz::NormalizePropertyName(name);
        Check(name == "maxhomingangle",
              "names: NormalizePropertyName('  Max_Homing  Angle ') == 'maxhomingangle'",
              "got '" + name + "'");
        std::string upper = "MAXROTATIONANGLE";
        acz::NormalizePropertyName(upper);
        Check(upper == "maxrotationangle", "names: NormalizePropertyName('MAXROTATIONANGLE') == 'maxrotationangle'");
    }
}

void TestCommentsAndLineEndings() {
    Header("comments, tabs, CRLF and BOM");

    {
        acz::Config cfg;
        if (!LoadOrFail("comments: load",
                        "; header comment\n"
                        "# hash comment\n"
                        "[player]\n"
                        "msl = { ; block starts here\n"
                        "    ; leading comment\n"
                        "    HomingForesightAmount = 0.7 ; vanilla MSL\n"
                        "    MaxHomingAngle        = 90 # seeker FOV\n"
                        "    # hash-only line\n"
                        "    MaxRotationAngle      = 50;no space before the comment\n"
                        "    LockonRange           = ; blank with a comment\n"
                        "}\n",
                        cfg))
            return;
        CheckWarningCount(cfg, 0, "comments: no warnings");
        const acz::FieldSet* fields = FindFields(cfg, acz::Side::Player, "msl");
        Check(fields != nullptr, "comments: model exists");
        if (fields) {
            CheckFloat(fields->homingForesightAmount, 0.7f, "comments: trailing '; ...' comment stripped");
            CheckFloat(fields->maxHomingAngle, 90.0f, "comments: trailing '# ...' comment stripped");
            CheckFloat(fields->maxRotationAngle, 50.0f, "comments: ';' without a space still starts a comment");
            Check(!fields->lockonRange, "comments: blank value followed by a comment stays unset");
        }
    }

    {
        acz::Config cfg;
        if (!LoadOrFail("crlf: load",
                        "[player]\r\n"
                        "\tmsl\t=\t{\r\n"
                        "\t\tHomingForesightAmount\t=\t0.65\r\n"
                        "\t\tMaxHomingAngle = 80 ; note\r\n"
                        "\t}\r\n"
                        "[enemy]\r\n"
                        "msl = 0.25\r\n",
                        cfg))
            return;
        CheckWarningCount(cfg, 0, "crlf: no warnings");
        const acz::FieldSet* player = FindFields(cfg, acz::Side::Player, "msl");
        const acz::FieldSet* enemy = FindFields(cfg, acz::Side::Enemy, "msl");
        Check(player != nullptr && EqOpt(player->homingForesightAmount, 0.65f), "crlf: player value parsed");
        Check(player != nullptr && EqOpt(player->maxHomingAngle, 80.0f),
              "crlf: trailing comment on a CRLF line parsed");
        Check(enemy != nullptr && EqOpt(enemy->homingForesightAmount, 0.25f), "crlf: enemy value parsed");
        Check(cfg.models.size() == 1, "crlf: msl shared by player and enemy is a single entry");
    }

    {
        // Opening brace on the line after a comment following '='.
        acz::Config cfg;
        if (!LoadOrFail("comment: brace-after-comment load",
                        "[player]\n"
                        "msl = ; the lead\n"
                        "{\n"
                        "    HomingForesightAmount = 0.35\n"
                        "}\n",
                        cfg))
            return;
        CheckWarningCount(cfg, 0, "comment: brace-after-comment has no warnings");
        const acz::FieldSet* fields = FindFields(cfg, acz::Side::Player, "msl");
        Check(fields != nullptr && EqOpt(fields->homingForesightAmount, 0.35f),
              "comment: opening brace after a comment line is found");
    }

    {
        acz::Config cfg;
        if (!LoadOrFail("bom: load", "[player]\nmsl = 0.55\n", cfg, /*withBom=*/true)) return;
        CheckWarningCount(cfg, 0, "bom: UTF-8 BOM produces no warnings");
        const acz::FieldSet* fields = FindFields(cfg, acz::Side::Player, "msl");
        Check(fields != nullptr && EqOpt(fields->homingForesightAmount, 0.55f), "bom: UTF-8 BOM is stripped");
    }
}

// ===========================================================================
//  4. diagnostics and duplicate merging
// ===========================================================================

void TestDiagnostics() {
    Header("diagnostics: unknown keys/sections, stray braces, duplicates");

    {
        acz::Config cfg;
        if (!LoadOrFail("diag: unknown property load",
                        "[player]\n"
                        "msl = {\n"
                        "    Bogus = 1\n"
                        "    HomingForesightAmount = 0.5\n"
                        "}\n",
                        cfg))
            return;
        CheckWarningCount(cfg, 1, "diag: unknown property warns once");
        Check(AnyWarningContains(cfg, "Bogus") && AnyWarningContains(cfg, "unknown property"),
              "diag: the warning names the unknown property", cfg.WarningSummary());
        const acz::FieldSet* fields = FindFields(cfg, acz::Side::Player, "msl");
        Check(fields != nullptr && EqOpt(fields->homingForesightAmount, 0.5f),
              "diag: a known property after an unknown one is still parsed");
    }

    {
        acz::Config cfg;
        if (!LoadOrFail("diag: unknown section load",
                        "[weapons]\n"
                        "msl = 0.5\n"
                        "[player]\n"
                        "msl = 0.4\n",
                        cfg))
            return;
        CheckWarningCount(cfg, 1, "diag: unknown section warns once");
        Check(AnyWarningContains(cfg, "weapons"), "diag: the warning names the unknown section");
        Check(cfg.models.size() == 1, "diag: unknown section creates no model");
        const acz::FieldSet* fields = FindFields(cfg, acz::Side::Player, "msl");
        Check(fields != nullptr && EqOpt(fields->homingForesightAmount, 0.4f),
              "diag: unknown section contents are skipped");
    }

    {
        acz::Config cfg;
        if (!LoadOrFail("diag: stray brace load", "[player]\nmsl = 0.5\n}\n}\n", cfg)) return;
        Check(cfg.warnings.size() >= 1, "diag: stray '}' warns", cfg.WarningSummary());
        const acz::FieldSet* fields = FindFields(cfg, acz::Side::Player, "msl");
        Check(fields != nullptr && EqOpt(fields->homingForesightAmount, 0.5f),
              "diag: parsing continues after stray '}' characters");
    }

    {
        acz::Config cfg;
        if (!LoadOrFail("diag: nested brace load",
                        "[player]\n"
                        "msl = { HomingForesightAmount = 0.5 { MaxHomingAngle = 90 }\n",
                        cfg))
            return;
        Check(cfg.warnings.size() >= 1, "diag: stray '{' inside a block warns", cfg.WarningSummary());
        const acz::FieldSet* fields = FindFields(cfg, acz::Side::Player, "msl");
        Check(fields != nullptr && EqOpt(fields->homingForesightAmount, 0.5f) &&
                  EqOpt(fields->maxHomingAngle, 90.0f),
              "diag: parsing continues after a stray '{' (both values parsed)");
    }

    {
        acz::Config cfg;
        if (!LoadOrFail("diag: unterminated block load",
                        "[player]\n"
                        "msl = {\n"
                        "    HomingForesightAmount = 0.3\n",
                        cfg))
            return;
        CheckWarningCount(cfg, 1, "diag: unterminated block warns once");
        Check(AnyWarningContains(cfg, "closing"), "diag: unterminated block warning mentions the missing '}'",
              cfg.WarningSummary());
        const acz::FieldSet* fields = FindFields(cfg, acz::Side::Player, "msl");
        Check(fields != nullptr && EqOpt(fields->homingForesightAmount, 0.3f),
              "diag: unterminated block still applies its values");
    }

    {
        acz::Config cfg;
        if (!LoadOrFail("duplicate: load",
                        "[player]\n"
                        "msl = {\n"
                        "    HomingForesightAmount = 0.7\n"
                        "    MaxRotationAngle = 40\n"
                        "}\n"
                        "msl = {\n"
                        "    MaxHomingAngle = 90\n"
                        "    MaxRotationAngle = 55\n"
                        "}\n",
                        cfg))
            return;
        CheckWarningCount(cfg, 1, "duplicate: duplicate model warns once");
        Check(AnyWarningContains(cfg, "duplicate"), "duplicate: the warning contains 'duplicate'");
        Check(cfg.models.size() == 1, "duplicate: entries are merged, not appended",
              Fmt("got %u entries", static_cast<unsigned>(cfg.models.size())));
        const acz::FieldSet* fields = FindFields(cfg, acz::Side::Player, "msl");
        if (fields) {
            CheckFloat(fields->homingForesightAmount, 0.7f, "duplicate: property only in the first block is kept");
            CheckFloat(fields->maxRotationAngle, 55.0f, "duplicate: later block overrides MaxRotationAngle");
            CheckFloat(fields->maxHomingAngle, 90.0f, "duplicate: property only in the second block is added");
        } else {
            Check(false, "duplicate: merged model exists");
        }
    }

    {
        acz::Config cfg;
        if (!LoadOrFail("sides: load",
                        "[player]\n"
                        "msl = { HomingForesightAmount = 0.7 }\n"
                        "[enemy]\n"
                        "msl = { HomingForesightAmount = 0.3 }\n",
                        cfg))
            return;
        CheckWarningCount(cfg, 0, "sides: the same key on both sides is not a duplicate");
        Check(cfg.models.size() == 1, "sides: player and enemy share one entry");
        const acz::FieldSet* player = FindFields(cfg, acz::Side::Player, "msl");
        const acz::FieldSet* enemy = FindFields(cfg, acz::Side::Enemy, "msl");
        Check(player != nullptr && EqOpt(player->homingForesightAmount, 0.7f), "sides: player value");
        Check(enemy != nullptr && EqOpt(enemy->homingForesightAmount, 0.3f), "sides: enemy value");
    }
}

// ===========================================================================
//  5. Config::Resolve
// ===========================================================================

void TestResolve() {
    Header("Config::Resolve longest-key-first");

    acz::Config cfg;
    if (!LoadOrFail("resolve: load",
                    "[player]\n"
                    "aam = 0.22\n"
                    "4aam = 0.44\n"
                    "qaam = 0.66\n"
                    "[enemy]\n"
                    "msl = 0.33\n",
                    cfg))
        return;
    CheckWarningCount(cfg, 0, "resolve: no warnings");

    const acz::FieldSet* resolved = cfg.Resolve(acz::Side::Player, "4AAM_X");
    Check(resolved != nullptr && EqOpt(resolved->homingForesightAmount, 0.44f),
          "resolve: '4AAM_X' picks the longest key '4aam', not 'aam'",
          resolved ? Describe(resolved->homingForesightAmount) : std::string("nullptr"));

    resolved = cfg.Resolve(acz::Side::Player, "QAAM");
    Check(resolved != nullptr && EqOpt(resolved->homingForesightAmount, 0.66f),
          "resolve: 'QAAM' matches 'qaam' case-insensitively");

    resolved = cfg.Resolve(acz::Side::Player, "AAM");
    Check(resolved != nullptr && EqOpt(resolved->homingForesightAmount, 0.22f),
          "resolve: bare 'AAM' matches 'aam'");

    Check(cfg.Resolve(acz::Side::Player, "MSL") == nullptr,
          "resolve: an enemy-only model does not resolve for the player");
    Check(cfg.Resolve(acz::Side::Enemy, "AAM") == nullptr,
          "resolve: a player-only model does not resolve for the enemy");

    resolved = cfg.Resolve(acz::Side::Enemy, "msl");
    Check(resolved != nullptr && EqOpt(resolved->homingForesightAmount, 0.33f),
          "resolve: enemy 'msl' resolves");

    Check(cfg.Resolve(acz::Side::Player, "") == nullptr, "resolve: empty typeName -> nullptr");
    Check(cfg.Resolve(acz::Side::Player, "   ") == nullptr, "resolve: blank typeName -> nullptr");
    Check(cfg.Resolve(acz::Side::Player, "GUN") == nullptr, "resolve: unmatched typeName -> nullptr");

    acz::Config empty;
    Check(empty.Resolve(acz::Side::Player, "MSL") == nullptr, "resolve: empty config -> nullptr");

    acz::Config tie;
    if (!LoadOrFail("resolve: tie load", "[player]\nab = 0.2\naa = 0.3\n", tie)) return;
    const acz::FieldSet* first = tie.Resolve(acz::Side::Player, "XAAB");
    Check(first != nullptr && EqOpt(first->homingForesightAmount, 0.2f),
          "resolve: equal-length tie is broken by declaration order ('ab' declared first)");
}

// ===========================================================================
//  6. Config::Sanitize
// ===========================================================================

void TestSanitize() {
    Header("Config::Sanitize clamps");

    acz::Config cfg;
    if (!LoadOrFail("sanitize: load",
                    "[player]\n"
                    "msl = {\n"
                    "    HomingForesightAmount = 2.5\n"
                    "    MaxHomingAngle = 400\n"
                    "    MaxRotationAngle = 900\n"
                    "    LifeTime = 0.01\n"
                    "    LockonRange = 250000\n"
                    "}\n"
                    "qaam = {\n"
                    "    HomingForesightAmount = 0.5\n"
                    "    MaxHomingAngle = 90\n"
                    "}\n"
                    "hvaa = {\n"
                    "    HomingForesightAmount = -1\n"
                    "    LockonRange = -5\n"
                    "}\n",
                    cfg))
        return;

    // LoadFromFile sanitizes as its last step, so the clamps are already in.
    CheckWarningCount(cfg, 7, "sanitize: LoadFromFile clamps and records 7 warnings");
    Check(cfg.loaded, "sanitize: loaded flag stays set");

    const acz::FieldSet* msl = FindFields(cfg, acz::Side::Player, "msl");
    Check(msl != nullptr, "sanitize: msl exists");
    if (msl) {
        CheckFloat(msl->homingForesightAmount, 1.0f, "sanitize: HomingForesightAmount 2.5 -> 1");
        CheckFloat(msl->maxHomingAngle, 360.0f, "sanitize: MaxHomingAngle 400 -> 360");
        CheckFloat(msl->maxRotationAngle, 720.0f, "sanitize: MaxRotationAngle 900 -> 720");
        CheckFloat(msl->lifeTime, 0.1f, "sanitize: LifeTime 0.01 -> 0.1");
        CheckFloat(msl->lockonRange, 100000.0f, "sanitize: LockonRange 250000 -> 100000");
    }

    const acz::FieldSet* qaam = FindFields(cfg, acz::Side::Player, "qaam");
    Check(qaam != nullptr && EqOpt(qaam->homingForesightAmount, 0.5f) && EqOpt(qaam->maxHomingAngle, 90.0f),
          "sanitize: in-range values are untouched");
    Check(qaam != nullptr && !qaam->lifeTime && !qaam->lockonRange, "sanitize: unset values stay unset");

    const acz::FieldSet* hvaa = FindFields(cfg, acz::Side::Player, "hvaa");
    Check(hvaa != nullptr && EqOpt(hvaa->homingForesightAmount, 0.0f), "sanitize: HomingForesightAmount -1 -> 0");
    Check(hvaa != nullptr && EqOpt(hvaa->lockonRange, 0.0f), "sanitize: LockonRange -5 -> 0");

    cfg.Sanitize();
    CheckWarningCount(cfg, 7, "sanitize: explicit Sanitize is idempotent (no new warnings)");
    cfg.Sanitize();
    CheckWarningCount(cfg, 7, "sanitize: repeated Sanitize calls stay stable");

    // Standalone Sanitize on a hand-built config.
    {
        acz::Config manual;
        acz::MissileModel model;
        model.key = "msl";
        model.player.maxHomingAngle = 1000.0f;
        manual.models.push_back(model);
        manual.Sanitize();
        Check(manual.warnings.size() == 1, "sanitize: standalone call clamps and warns");
        Check(EqOpt(manual.models[0].player.maxHomingAngle, 360.0f), "sanitize: standalone 1000 -> 360");
        Check(!manual.loaded, "sanitize: standalone call does not invent a loaded flag");
    }
}

// ===========================================================================
//  7. ParseFloatValue
// ===========================================================================

void TestParseFloat() {
    Header("ParseFloatValue edge cases");

    struct Case {
        const char* text;
        bool ok;
        float value;
    };
    static const Case kCases[] = {
        {"0.7", true, 0.7f},      {"1", true, 1.0f},        {"1.0", true, 1.0f},
        {"+0.5", true, 0.5f},     {"-2.5", true, -2.5f},    {"1e-1", true, 0.1f},
        {"2E2", true, 200.0f},    {"0.7f", true, 0.7f},     {"1.0F", true, 1.0f},
        {"  0.8  ", true, 0.8f},  {".5", true, 0.5f},       {"1.", true, 1.0f},
        {"", false, 0.0f},        {"abc", false, 0.0f},     {"0.7x", false, 0.0f},
        {"1.2.3", false, 0.0f},   {"nan", false, 0.0f},     {"inf", false, 0.0f},
        {"1e999", false, 0.0f},   {"0.7 ; note", false, 0.0f},
        {"--1", false, 0.0f},     {"0x10", false, 0.0f},
    };

    for (const Case& test : kCases) {
        float value = -12345.0f;
        const bool parsed = acz::ParseFloatValue(test.text, value);
        const std::string what = Fmt("ParseFloatValue(\"%s\") %s", test.text, test.ok ? "accepts" : "rejects");
        if (parsed != test.ok) {
            Check(false, what, Fmt("returned %s (value %g)", parsed ? "true" : "false",
                                   static_cast<double>(value)));
        } else if (parsed && !Eq(value, test.value)) {
            Check(false, what, Fmt("parsed %g, expected %g", static_cast<double>(value),
                                   static_cast<double>(test.value)));
        } else {
            Check(true, what);
        }
    }
}

// ===========================================================================
//  8. [runtime]
// ===========================================================================

void TestRuntime() {
    Header("[runtime] extension section");

    {
        acz::Config cfg;
        if (!LoadOrFail("runtime: load",
                        "[runtime]\n"
                        "LogLevel = 1\n"
                        "DryRun = yes\n"
                        "Probe = TRUE\n"
                        "TickHz = 90\n"
                        "RequireVerifiedOffsets = 0\n"
                        "GuidanceOverride = no\n"
                        "ForceOffsets = true\n"
                        "EntityPosOffset = 0x80\n"
                        "EntityVelOffset = 128\n"
                        "TargetPosOffset = 0x1C\n"
                        "TargetVelOffset = 0\n"
                        "HookRva = 0x345678\n"
                        "HookArgIndex = 3\n"
                        "EntityOverride = 1\n"
                        "HookSignature = 48 8B 05 ?? ?? ?? ?? 48 85 C0\n"
                        "LeadScale = 312.5\n"
                        "LeadMode = Blend\n"
                        "PnGain = 5\n"
                        "TerminalZone = 45\n"
                        "[player]\n"
                        "msl = 0.5\n",
                        cfg))
            return;
        CheckWarningCount(cfg, 0, "runtime: no warnings");
        Check(cfg.runtime.logLevel == 1, "runtime: LogLevel = 1");
        Check(cfg.runtime.dryRun, "runtime: DryRun = yes -> true");
        Check(cfg.runtime.probe, "runtime: Probe = TRUE -> true");
        Check(cfg.runtime.tickHz == 90, "runtime: TickHz = 90");
        Check(!cfg.runtime.requireVerifiedOffsets, "runtime: RequireVerifiedOffsets = 0 -> false");
        Check(!cfg.runtime.guidanceOverride, "runtime: GuidanceOverride = no -> false");
        Check(cfg.runtime.forceOffsets, "runtime: ForceOffsets = true");
        Check(cfg.runtime.entityPosOffset.has_value() && *cfg.runtime.entityPosOffset == 0x80u,
              "runtime: EntityPosOffset = 0x80 (hex)");
        Check(cfg.runtime.entityVelOffset.has_value() && *cfg.runtime.entityVelOffset == 128u,
              "runtime: EntityVelOffset = 128 (decimal)");
        Check(cfg.runtime.targetPosOffset.has_value() && *cfg.runtime.targetPosOffset == 0x1Cu,
              "runtime: TargetPosOffset = 0x1C");
        Check(cfg.runtime.targetVelOffset.has_value() && *cfg.runtime.targetVelOffset == 0u,
              "runtime: TargetVelOffset = 0");
        Check(cfg.runtime.hookRva.has_value() && *cfg.runtime.hookRva == 0x345678u,
              "runtime: HookRva = 0x345678");
        Check(cfg.runtime.hookArgIndex == 3, "runtime: HookArgIndex = 3");
        Check(cfg.runtime.entityOverride, "runtime: EntityOverride = 1 -> true");
        Check(cfg.runtime.hookSignature == "48 8B 05 ?? ?? ?? ?? 48 85 C0",
              "runtime: HookSignature kept verbatim", "got '" + cfg.runtime.hookSignature + "'");
        Check(Eq(cfg.runtime.leadScale, 312.5f), "runtime: LeadScale = 312.5",
              Fmt("got %g", static_cast<double>(cfg.runtime.leadScale)));
        Check(cfg.runtime.leadMode == "blend", "runtime: LeadMode = Blend is accepted case-insensitively",
              "got '" + cfg.runtime.leadMode + "'");
        Check(Eq(cfg.runtime.pnGain, 5.0f), "runtime: PnGain = 5 (navigation constant)",
              Fmt("got %g", static_cast<double>(cfg.runtime.pnGain)));
        Check(Eq(cfg.runtime.terminalZone, 45.0f), "runtime: TerminalZone = 45 m accepted",
              Fmt("got %g", static_cast<double>(cfg.runtime.terminalZone)));
        const acz::FieldSet* fields = FindFields(cfg, acz::Side::Player, "msl");
        Check(fields != nullptr && EqOpt(fields->homingForesightAmount, 0.5f),
              "runtime: a model section after [runtime] is still parsed");
    }

    {
        acz::Config cfg;
        if (!LoadOrFail("runtime: defaults load", "[player]\nmsl = 0.5\n", cfg)) return;
        const acz::RuntimeSection& rt = cfg.runtime;
        Check(rt.logLevel == 1 && !rt.dryRun && !rt.probe && rt.tickHz == 120 && rt.requireVerifiedOffsets &&
                  rt.guidanceOverride && !rt.forceOffsets && !rt.entityOverride && rt.hookArgIndex == 1 &&
                  rt.hookSignature.empty() && !rt.entityPosOffset && !rt.entityVelOffset &&
                  !rt.targetPosOffset && !rt.targetVelOffset && !rt.hookRva && rt.leadScale == 260.0f &&
                  rt.leadMode == "add" && rt.pnGain == 4.0f && rt.terminalZone == 0.0f,
              "runtime: documented defaults without a [runtime] section");
        // TerminalZone = 0 means "PN owns the whole flight" (the shipped behaviour);
        // a non-zero value is the opt-in terminal hand-back used for A/B runs.
        Check(rt.terminalZone == 0.0f, "runtime: TerminalZone defaults to 0 (full PN, no hand-back)",
              Fmt("got %g", static_cast<double>(rt.terminalZone)));
    }

    {
        acz::Config cfg;
        if (!LoadOrFail("runtime: invalid load",
                        "[runtime]\n"
                        "LogLevel =\n"
                        "TickHz = 0\n"
                        "DryRun = maybe\n"
                        "HookArgIndex = 9\n"
                        "EntityPosOffset = 0x20000\n"
                        "HookRva = 0x5000000\n"
                        "HookSignature =\n"
                        "BogusKey = 1\n",
                        cfg))
            return;
        Check(cfg.runtime.logLevel == 1, "runtime: blank LogLevel keeps the default (1 = quiet)");
        Check(cfg.runtime.tickHz == 1, "runtime: TickHz 0 is clamped to 1");
        Check(!cfg.runtime.dryRun, "runtime: invalid DryRun keeps the default");
        Check(cfg.runtime.hookArgIndex == 1, "runtime: HookArgIndex 9 is rejected (stays 1)");
        Check(!cfg.runtime.entityPosOffset, "runtime: EntityPosOffset above 0x10000 is rejected (stays unset)");
        Check(!cfg.runtime.hookRva, "runtime: HookRva above 0x4000000 is rejected (stays unset)");
        Check(cfg.runtime.hookSignature.empty(), "runtime: blank HookSignature keeps the default (empty)");
        // TickHz clamp, DryRun, HookArgIndex, EntityPosOffset, HookRva, unknown key
        CheckWarningCount(cfg, 6, "runtime: invalid values warn while valid entries still apply");
        Check(AnyWarningContains(cfg, "BogusKey"), "runtime: unknown [runtime] key warns");
        Check(AnyWarningContains(cfg, "TickHz") && AnyWarningContains(cfg, "clamped"),
              "runtime: TickHz clamp is reported");
    }

    {
        acz::Config cfg;
        if (!LoadOrFail("runtime: bad text load",
                        "[runtime]\n"
                        "EntityVelOffset = zz\n"
                        "GuidanceOverride = 2\n"
                        "UnknownBlank =\n",
                        cfg))
            return;
        Check(!cfg.runtime.entityVelOffset, "runtime: non-numeric offset text is rejected (stays unset)");
        Check(cfg.runtime.guidanceOverride, "runtime: invalid GuidanceOverride keeps the default (true)");
        Check(cfg.warnings.size() >= 2 && AnyWarningContains(cfg, "invalid value"),
              "runtime: non-numeric offset warns", cfg.WarningSummary());
        Check(AnyWarningContains(cfg, "UnknownBlank") || AnyWarningContains(cfg, "Unknown Key") ||
                  AnyWarningContains(cfg, "unknown [runtime] key"),
              "runtime: unknown key with a blank value still warns", cfg.WarningSummary());
    }

    {
        // LeadScale/LeadMode: invalid values warn and keep the documented defaults.
        acz::Config cfg;
        if (!LoadOrFail("runtime: invalid lead load",
                        "[runtime]\n"
                        "LeadScale = -5\n"
                        "LeadScale = abc\n"
                        "LeadScale = 2500\n"
                        "LeadMode = weird\n"
                        "LeadMode =\n",
                        cfg))
            return;
        Check(Eq(cfg.runtime.leadScale, 260.0f), "runtime: invalid LeadScale keeps the default 260",
              Fmt("got %g", static_cast<double>(cfg.runtime.leadScale)));
        Check(cfg.runtime.leadMode == "add", "runtime: invalid LeadMode keeps the default \"add\"",
              "got '" + cfg.runtime.leadMode + "'");
        CheckWarningCount(cfg, 4, "runtime: three bad LeadScale values and one bad LeadMode warn");
        Check(AnyWarningContains(cfg, "LeadScale") && AnyWarningContains(cfg, "LeadMode"),
              "runtime: lead warnings name their keys", cfg.WarningSummary());
    }

    {
        // The three valid LeadMode spellings, case-insensitively.
        static const char* kModes[] = {"add", "blend", "absolute"};
        for (int i = 0; i < 3; ++i) {
            acz::Config cfg;
            const std::string text = std::string("[runtime]\nLeadMode = ") + kModes[i] + "\n";
            if (!LoadOrFail(std::string("runtime: LeadMode ") + kModes[i] + " load", text, cfg)) return;
            Check(cfg.runtime.leadMode == kModes[i],
                  std::string("runtime: LeadMode ") + kModes[i] + " is accepted",
                  "got '" + cfg.runtime.leadMode + "'");
        }
        acz::Config upper;
        if (!LoadOrFail("runtime: LeadMode ABSOLUTE load", "[runtime]\nLeadMode = ABSOLUTE\n", upper)) return;
        Check(upper.runtime.leadMode == "absolute", "runtime: LeadMode = ABSOLUTE is lower-cased canonically",
              "got '" + upper.runtime.leadMode + "'");
    }
}

// ===========================================================================
//  9. [vanilla]
// ===========================================================================

void TestVanilla() {
    Header("[vanilla] discovery section");

    {
        acz::Config cfg;
        if (!LoadOrFail("vanilla: load",
                        "[player]\n"
                        "msl = 0.7\n"
                        "[vanilla]\n"
                        "msl = {\n"
                        "    MaxHomingAngle = 75\n"
                        "    MaxRotationAngle = 40\n"
                        "    LifeTime = 9\n"
                        "    LockonRange = 2000\n"
                        "}\n"
                        "qaam = 0.25\n"
                        "[enemy]\n"
                        "msl = 0.3\n",
                        cfg))
            return;
        CheckWarningCount(cfg, 0, "vanilla: no warnings");
        Check(cfg.vanilla.size() == 2, "vanilla: two discovery models parsed",
              Fmt("got %u", static_cast<unsigned>(cfg.vanilla.size())));
        Check(cfg.models.size() == 1, "vanilla: discovery models stay out of Config::models",
              Fmt("got %u", static_cast<unsigned>(cfg.models.size())));

        const acz::MissileModel* model = FindModel(cfg.vanilla, "msl");
        Check(model != nullptr, "vanilla: model 'msl' exists in Config::vanilla");
        if (model) {
            CheckFloat(model->player.maxHomingAngle, 75.0f, "vanilla: MaxHomingAngle = 75");
            CheckFloat(model->player.maxRotationAngle, 40.0f, "vanilla: MaxRotationAngle = 40");
            CheckFloat(model->player.lifeTime, 9.0f, "vanilla: LifeTime = 9");
            CheckFloat(model->player.lockonRange, 2000.0f, "vanilla: LockonRange = 2000");
            Check(!model->player.homingForesightAmount, "vanilla: an unset property stays unset");
        }
        const acz::MissileModel* legacy = FindModel(cfg.vanilla, "qaam");
        Check(legacy != nullptr && EqOpt(legacy->player.homingForesightAmount, 0.25f),
              "vanilla: legacy form 'qaam = 0.25' works in [vanilla]");

        Check(cfg.Resolve(acz::Side::Player, "QAAM") == nullptr,
              "vanilla: Config::Resolve ignores [vanilla] entries");
        const acz::FieldSet* resolved = cfg.Resolve(acz::Side::Player, "MSL");
        Check(resolved != nullptr && EqOpt(resolved->homingForesightAmount, 0.7f),
              "vanilla: Config::Resolve still uses [player] (msl 0.7)");
    }

    {
        acz::Config cfg;
        if (!LoadOrFail("vanilla: diagnostics load",
                        "[vanilla]\n"
                        "msl = { MaxHomingAngle = 75 }\n"
                        "msl = { MaxRotationAngle = 40 }\n"
                        "msl = { Bogus = 1 }\n",
                        cfg))
            return;
        Check(cfg.vanilla.size() == 1, "vanilla: duplicate keys merge");
        // Two duplicate declarations + one unknown property.
        CheckWarningCount(cfg, 3, "vanilla: duplicate and unknown-property warnings");
        Check(AnyWarningContains(cfg, "duplicate"), "vanilla: duplicate warning present", cfg.WarningSummary());
        Check(AnyWarningContains(cfg, "Bogus"), "vanilla: unknown property warning present", cfg.WarningSummary());
        const acz::MissileModel* model = FindModel(cfg.vanilla, "msl");
        Check(model != nullptr && EqOpt(model->player.maxHomingAngle, 75.0f) &&
                  EqOpt(model->player.maxRotationAngle, 40.0f),
              "vanilla: merged values are all kept");
    }

    {
        acz::Config cfg;
        if (!LoadOrFail("vanilla: clamp load", "[vanilla]\nmsl = { MaxHomingAngle = 999 }\n", cfg)) return;
        CheckWarningCount(cfg, 1, "vanilla: out-of-range discovery value is clamped with a warning");
        const acz::MissileModel* model = FindModel(cfg.vanilla, "msl");
        Check(model != nullptr && EqOpt(model->player.maxHomingAngle, 360.0f),
              "vanilla: MaxHomingAngle 999 -> 360");
        Check(AnyWarningContains(cfg, "[vanilla]"), "vanilla: the clamp warning is labelled [vanilla]",
              cfg.WarningSummary());
    }

    {
        // A signature may legitimately carry a single value; the patcher decides
        // whether that is enough to identify a parameter table.
        acz::Config cfg;
        if (!LoadOrFail("vanilla: single-value load", "[vanilla]\nmsl = { LifeTime = 9 }\n", cfg)) return;
        CheckWarningCount(cfg, 0, "vanilla: a single-value entry is accepted without warnings");
        Check(cfg.vanilla.size() == 1, "vanilla: single-value entry exists");
        const acz::MissileModel* model = FindModel(cfg.vanilla, "msl");
        Check(model != nullptr && EqOpt(model->player.lifeTime, 9.0f) && model->player.Count() == 1,
              "vanilla: single-value entry keeps exactly that one value",
              model ? Fmt("Count=%d", model->player.Count()) : std::string("model missing"));
    }
}

// ===========================================================================
//  10. file handling
// ===========================================================================

void TestFileHandling() {
    Header("file handling");

    {
        std::wstring missing = MakeTempPath();
        if (!missing.empty()) ::DeleteFileW(missing.c_str());
        acz::Config cfg;
        Check(!cfg.LoadFromFile(missing), "file: missing file -> LoadFromFile returns false");
        Check(!cfg.loaded, "file: missing file leaves loaded == false");
        Check(!cfg.sourcePath.empty(), "file: missing file still records sourcePath for logging");
    }

    {
        acz::Config cfg;
        if (!LoadOrFail("file: empty load", "", cfg)) return;
        Check(cfg.loaded, "file: empty file loads successfully (true)");
        Check(cfg.models.empty() && cfg.warnings.empty(), "file: empty file yields an empty config without warnings");
    }

    {
        TempIni file;
        if (!file.Write("[player]\nmsl = 0.5\n")) {
            Check(false, "file: sourcePath load", "cannot create the temporary file");
        } else {
            acz::Config cfg;
            Check(cfg.LoadFromFile(file.path()), "file: sourcePath load");
            Check(cfg.sourcePath == Narrow(file.path()),
                  "file: sourcePath is the UTF-8 path that was read", cfg.sourcePath);
            Check(cfg.loaded, "file: a valid file sets loaded == true");
        }
    }

    {
        const std::string oversized(1024u * 1024u + 16u, ';');
        TempIni file;
        if (!file.Write(oversized)) {
            Check(false, "file: 1 MiB+ write", "cannot create the temporary file");
        } else {
            acz::Config cfg;
            Check(!cfg.LoadFromFile(file.path()), "file: larger than 1 MiB -> LoadFromFile returns false");
            Check(!cfg.loaded, "file: oversized file leaves loaded == false");
        }
    }
}

}  // namespace

// ===========================================================================
//  entry point
// ===========================================================================

int main() {
    std::printf("ACZPropNav initest - INI parser acceptance test\n");

    TestReferenceIni();
    TestBlockForms();
    TestLegacyAndBlank();
    TestPropertyNames();
    TestCommentsAndLineEndings();
    TestDiagnostics();
    TestResolve();
    TestSanitize();
    TestParseFloat();
    TestRuntime();
    TestVanilla();
    TestFileHandling();

    std::printf("\n================ %s ================\n", g_fail == 0 ? "ALL PASS" : "FAILURES");
    std::printf("PASS %d / FAIL %d", g_pass, g_fail);
    if (g_skip != 0) std::printf("   (skipped %d)", g_skip);
    std::printf("\n");
    return g_fail == 0 ? 0 : 1;
}
