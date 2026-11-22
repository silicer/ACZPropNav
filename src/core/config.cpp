// ACZPropNav - AC8PropNav.ini compatible configuration parser, resolver and
// sanitizer.  The contract lives in src/core/config.h.
//
// Parsing strategy
// ----------------
// The file is scanned once as a character stream (not pre-split into lines), so
// all AC8-compatible layout variants fall out of the same code path:
//
//     msl = { HomingForesightAmount = 0.7 MaxHomingAngle = 90 }   single line
//     msl =                                                       brace next line
//     {
//         HomingForesightAmount = 0.7
//     }
//     msl = 0.8                                                   legacy shorthand
//     msl = 0.8 ; trailing comment
//
// Rules implemented (mirroring AC8PropNav's INI behaviour):
//   * ';' and '#' always open a comment that runs to the end of the line - even
//     inside a value or inside a model block.
//   * a property with a blank value, or with no '=' at all, means "keep the
//     vanilla value" (std::nullopt);
//   * property names are case-insensitive and spaces/underscores are ignored
//     ("max_rotation_angle" == "MaxRotationAngle");
//   * a legacy "<model> = <number>" entry equals "only HomingForesightAmount";
//   * a duplicate model on the same side merges property-by-property (later
//     value wins) and is reported as a warning;
//   * unknown property  -> warning, key ignored;
//     unknown section   -> warning, its contents skipped;
//     stray brace       -> warning, parser keeps going (never crashes).
//
// Sections:
//   [player] / [enemy]  live missile overrides  -> Config::models
//   [vanilla]           the game's ORIGINAL values, same block syntax as
//                       [player]; discovery signatures only, never used by
//                       Resolve()                       -> Config::vanilla
//   [runtime]           extension keys              -> Config::runtime
//
// The file is built with -fno-exceptions, so nothing here throws: problems are
// appended to Config::warnings and the parse continues with sane defaults.

#include "core/config.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace acz {
namespace {

// ===========================================================================
//  tiny ASCII helpers - config.cpp must not depend on log.cpp or acz_common.h
// ===========================================================================

constexpr size_t kMaxConfigBytes = 1024u * 1024u;  // 1 MiB hard limit (contract)

bool IsBlank(char c) { return c == ' ' || c == '\t'; }
bool IsLineEnd(char c) { return c == '\r' || c == '\n'; }
bool IsCommentStart(char c) { return c == ';' || c == '#'; }

// Characters that terminate a key name (property, model or [runtime] key).
bool IsKeyBreak(char c) { return c == '=' || c == '{' || c == '}' || c == ']' || IsLineEnd(c) || IsCommentStart(c); }

std::string TrimCopy(const std::string& s) {
    size_t begin = 0, end = s.size();
    while (begin < end && (IsBlank(s[begin]) || IsLineEnd(s[begin]))) ++begin;
    while (end > begin && (IsBlank(s[end - 1]) || IsLineEnd(s[end - 1]))) --end;
    return s.substr(begin, end - begin);
}

std::string LowerCopy(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

std::string FormatFloat(float value) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(value));
    return std::string(buf);
}

std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return std::string();
    const int wideLen = static_cast<int>(wide.size());
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), wideLen, nullptr, 0, nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string out(static_cast<size_t>(need), '\0');
    const int wrote = ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), wideLen, &out[0], need, nullptr, nullptr);
    if (wrote <= 0) return std::string();
    out.resize(static_cast<size_t>(wrote));
    return out;
}

// ===========================================================================
//  file IO - plain Win32, no iostream, never throws
// ===========================================================================

enum class ReadStatus { Ok, TooLarge, Failed };

ReadStatus ReadTextFile(const std::wstring& path, std::string& out) {
    out.clear();
    const HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_READ,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return ReadStatus::Failed;

    // Cheap reject before reading anything (stat may fail for exotic paths, in
    // which case the read loop below still enforces the limit).
    LARGE_INTEGER size{};
    if (::GetFileSizeEx(handle, &size) && size.QuadPart > static_cast<LONGLONG>(kMaxConfigBytes)) {
        ::CloseHandle(handle);
        return ReadStatus::TooLarge;
    }

    char buffer[64 * 1024];
    ReadStatus status = ReadStatus::Ok;
    for (;;) {
        DWORD got = 0;
        if (!::ReadFile(handle, buffer, static_cast<DWORD>(sizeof(buffer)), &got, nullptr)) {
            status = ReadStatus::Failed;
            break;
        }
        if (got == 0) break;  // end of file
        if (out.size() + got > kMaxConfigBytes) {
            status = ReadStatus::TooLarge;
            break;
        }
        out.append(buffer, got);
    }
    ::CloseHandle(handle);
    return status;
}

// ===========================================================================
//  scalar value parsers (strict, locale independent)
// ===========================================================================

bool ParseIntValue(const std::string& text, int& out) {
    const std::string s = TrimCopy(text);
    if (s.empty()) return false;
    size_t i = 0;
    bool negative = false;
    if (s[i] == '+' || s[i] == '-') {
        negative = (s[i] == '-');
        ++i;
    }
    long long value = 0;
    int digits = 0;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
        ++digits;
        if (value > 2000000000LL) return false;  // out of int range / absurd
    }
    if (digits == 0) return false;
    out = static_cast<int>(negative ? -value : value);
    return true;
}

// Decimal or 0x-prefixed hexadecimal, as used by the [runtime] offset keys.
bool ParseUIntValue(const std::string& text, uint32_t& out) {
    const std::string s = TrimCopy(text);
    if (s.empty()) return false;

    size_t i = 0;
    bool hex = false;
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        hex = true;
        i = 2;
        if (i >= s.size()) return false;
    }

    uint64_t value = 0;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        uint32_t digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<uint32_t>(c - '0');
        } else if (hex && c >= 'a' && c <= 'f') {
            digit = static_cast<uint32_t>(c - 'a' + 10);
        } else if (hex && c >= 'A' && c <= 'F') {
            digit = static_cast<uint32_t>(c - 'A' + 10);
        } else {
            return false;
        }
        value = value * (hex ? 16u : 10u) + digit;
        if (value > 0xFFFFFFFFull) return false;
    }
    out = static_cast<uint32_t>(value);
    return true;
}

// Booleans accept 0/1/true/false/yes/no, case-insensitive (contract).
bool ParseBoolValue(const std::string& text, bool& out) {
    const std::string s = LowerCopy(TrimCopy(text));
    if (s == "1" || s == "true" || s == "yes") {
        out = true;
        return true;
    }
    if (s == "0" || s == "false" || s == "no") {
        out = false;
        return true;
    }
    return false;
}

// Keys understood by the [runtime] extension section (normalized spelling).
bool IsKnownRuntimeKey(const std::string& key) {
    return key == "loglevel" || key == "dryrun" || key == "probe" || key == "tickhz" ||
           key == "requireverifiedoffsets" || key == "guidanceoverride" || key == "entityposoffset" ||
           key == "entityveloffset" || key == "targetposoffset" || key == "targetveloffset" ||
           key == "forceoffsets" || key == "hookrva" || key == "hooksignature" || key == "hookargindex" ||
           key == "entityoverride" || key == "leadscale" || key == "leadmode" || key == "ringsurvey" ||
           key == "hwwatch" || key == "hwwatchseconds" || key == "pngain" ||
           key == "terminalzone" || key == "terminalhandback" || key == "handback";
}

// ===========================================================================
//  property lookup (name already normalized: lowercase, no spaces/underscores)
// ===========================================================================

std::optional<float>* FindField(FieldSet& fields, const std::string& normalizedName) {
    if (normalizedName == "homingforesightamount") return &fields.homingForesightAmount;
    if (normalizedName == "maxhomingangle") return &fields.maxHomingAngle;
    if (normalizedName == "maxrotationangle") return &fields.maxRotationAngle;
    if (normalizedName == "lifetime") return &fields.lifeTime;
    if (normalizedName == "lockonrange") return &fields.lockonRange;
    return nullptr;
}

// ===========================================================================
//  the parser
// ===========================================================================

class IniParser {
public:
    IniParser(const std::string& text, Config& cfg) : text_(text), cfg_(cfg) {}

    void Run() {
        // UTF-8 BOM is not part of the first section header.
        if (text_.size() >= 3 && static_cast<unsigned char>(text_[0]) == 0xEF &&
            static_cast<unsigned char>(text_[1]) == 0xBB && static_cast<unsigned char>(text_[2]) == 0xBF) {
            Skip(3);
        }

        for (;;) {
            SkipTrivia();
            if (AtEnd()) break;

            const size_t before = pos_;
            const char c = text_[pos_];

            if (c == '[') {
                if (inBlock_) {
                    Warn(line_, "model '%s' block is missing its closing '}' before the next section", blockKey_.c_str());
                    inBlock_ = false;
                    blockIndex_ = -1;
                    blockTable_ = nullptr;
                }
                ParseSectionHeader();
            } else if (!inBlock_ && c == '}') {
                Warn(line_, "unexpected '}' outside of a model block (ignored)");
                Skip(1);
            } else if (inBlock_) {
                ParseBlockStatement();
            } else if (section_ == SectionKind::Player || section_ == SectionKind::Enemy ||
                       section_ == SectionKind::Vanilla) {
                ParseModelStatement();
            } else if (section_ == SectionKind::Runtime) {
                ParseRuntimeStatement();
            } else {
                // Contents of an unknown section, or junk before the first
                // section: skip line by line (the section warning was emitted
                // when its header was parsed).
                if (section_ == SectionKind::None && !warnedPreamble_) {
                    Warn(line_, "text outside of any section (ignored)");
                    warnedPreamble_ = true;
                }
                SkipRestOfLine();
            }

            // Defensive: a malformed construct must never stall the scanner.
            if (pos_ == before) Skip(1);
        }

        if (inBlock_) {
            Warn(line_, "model '%s' block is missing its closing '}' at end of file", blockKey_.c_str());
            inBlock_ = false;
        }
    }

private:
    enum class SectionKind { None, Player, Enemy, Vanilla, Runtime, Unknown };

    // ---- low level cursor ------------------------------------------------

    bool AtEnd() const { return pos_ >= text_.size(); }

    void Skip(size_t count) {
        size_t end = pos_ + count;
        if (end > text_.size()) end = text_.size();
        for (; pos_ < end; ++pos_) {
            if (text_[pos_] == '\n') ++line_;
        }
    }

    void SkipBlanks() {
        while (!AtEnd() && IsBlank(text_[pos_])) Skip(1);
    }

    void SkipRestOfLine() {
        while (!AtEnd() && text_[pos_] != '\n') Skip(1);
        if (!AtEnd()) Skip(1);
    }

    // Blanks, newlines and comments (';' / '#' to end of line).
    void SkipTrivia() {
        for (;;) {
            if (AtEnd()) return;
            const char c = text_[pos_];
            if (IsBlank(c) || IsLineEnd(c)) {
                Skip(1);
                continue;
            }
            if (IsCommentStart(c)) {
                while (!AtEnd() && text_[pos_] != '\n') Skip(1);
                continue;
            }
            return;
        }
    }

    // A single value token: stops at blanks, line end, comment, brace or '='.
    std::string ReadToken() {
        const size_t start = pos_;
        while (!AtEnd()) {
            const char c = text_[pos_];
            if (IsBlank(c) || IsLineEnd(c) || IsCommentStart(c) || c == '=' || c == '{' || c == '}') break;
            Skip(1);
        }
        return TrimCopy(text_.substr(start, pos_ - start));
    }

    // After "key =" and the following blanks: is there no value on this line?
    bool AtValueEnd() const {
        if (AtEnd()) return true;
        const char c = text_[pos_];
        return IsLineEnd(c) || IsCommentStart(c) || c == '}' || c == '{';
    }

    bool OnlyBlankOrCommentLeft() const {
        if (AtEnd()) return true;
        const char c = text_[pos_];
        return IsLineEnd(c) || IsCommentStart(c);
    }

    void Warn(int line, const char* fmt, ...) {
        char body[1024];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(body, sizeof(body), fmt, args);
        va_end(args);
        char prefix[64];
        std::snprintf(prefix, sizeof(prefix), "line %d: ", line);
        cfg_.warnings.push_back(std::string(prefix) + body);
    }

    // ---- section helpers -------------------------------------------------

    const char* SectionLabel() const {
        switch (section_) {
            case SectionKind::Player: return "player";
            case SectionKind::Enemy: return "enemy";
            case SectionKind::Vanilla: return "vanilla";
            default: return "player";
        }
    }

    std::vector<MissileModel>& CurrentTable() {
        return (section_ == SectionKind::Vanilla) ? cfg_.vanilla : cfg_.models;
    }

    std::vector<std::string>& DeclaredForCurrentSection() {
        if (section_ == SectionKind::Vanilla) return declaredVanilla_;
        if (section_ == SectionKind::Enemy) return declaredEnemy_;
        return declaredPlayer_;
    }

    static FieldSet& FieldsOf(std::vector<MissileModel>& table, int modelIndex, Side side) {
        MissileModel& model = table[static_cast<size_t>(modelIndex)];
        return (side == Side::Enemy) ? model.enemy : model.player;
    }

    // Find or create the model entry for `key`. Emits the duplicate warning when
    // the key was already declared in the *same section* (player and enemy are
    // tracked separately - the same model may be configured per side, and
    // [vanilla] has its own table).
    int EnsureModel(std::vector<MissileModel>& table, std::vector<std::string>& declared,
                    const std::string& key, int line, Side side, const char* sectionLabel) {
        bool seenInSection = false;
        for (const std::string& k : declared) {
            if (k == key) {
                seenInSection = true;
                break;
            }
        }

        int index = -1;
        for (size_t i = 0; i < table.size(); ++i) {
            if (table[i].key == key) {
                index = static_cast<int>(i);
                break;
            }
        }
        if (index < 0) {
            MissileModel model;
            model.key = key;
            model.sourceLine = line;
            table.push_back(model);
            index = static_cast<int>(table.size()) - 1;
        } else if (seenInSection) {
            Warn(line, "duplicate model '%s' in [%s] - later values override earlier ones",
                 key.c_str(), sectionLabel);
        }
        if (!seenInSection) declared.push_back(key);
        (void)side;  // the side only selects the FieldSet at the call sites
        return index;
    }

    // ---- statements ------------------------------------------------------

    void ParseSectionHeader() {
        const int line = line_;
        Skip(1);  // '['
        const size_t start = pos_;
        while (!AtEnd() && text_[pos_] != ']' && text_[pos_] != '\n') Skip(1);
        if (AtEnd() || text_[pos_] != ']') {
            Warn(line, "malformed section header (missing ']') - line ignored");
            SkipRestOfLine();
            section_ = SectionKind::None;
            return;
        }
        const std::string rawName = TrimCopy(text_.substr(start, pos_ - start));
        Skip(1);  // ']'

        std::string name = rawName;
        NormalizePropertyName(name);  // lowercase, drop spaces/underscores
        if (name == "player") {
            section_ = SectionKind::Player;
        } else if (name == "enemy") {
            section_ = SectionKind::Enemy;
        } else if (name == "vanilla") {
            section_ = SectionKind::Vanilla;
        } else if (name == "runtime") {
            section_ = SectionKind::Runtime;
        } else {
            section_ = SectionKind::Unknown;
            Warn(line, "unknown section '[%s]' - its contents are ignored", rawName.c_str());
        }
    }

    // [player] / [enemy] / [vanilla]: "<model> = <number>" (legacy) or
    // "<model> = { ... }".
    void ParseModelStatement() {
        const int line = line_;
        const size_t start = pos_;
        while (!AtEnd() && !IsKeyBreak(text_[pos_])) Skip(1);
        const std::string key = LowerCopy(TrimCopy(text_.substr(start, pos_ - start)));

        if (key.empty()) {
            Warn(line, "model entry without a name (ignored)");
            SkipRestOfLine();
            return;
        }

        const Side side = (section_ == SectionKind::Enemy) ? Side::Enemy : Side::Player;

        if (AtEnd() || text_[pos_] != '=') {
            // Accepted variant: the key alone, with the opening brace on a
            // following line ("msl\n{ ... }").
            const size_t savePos = pos_;
            const int saveLine = line_;
            SkipTrivia();
            if (!AtEnd() && text_[pos_] == '{') {
                BeginBlock(key, line, side);
                return;
            }
            pos_ = savePos;
            line_ = saveLine;
            Warn(line, "model '%s' has no value and no block (ignored)", key.c_str());
            SkipRestOfLine();
            return;
        }

        Skip(1);  // '='
        SkipBlanks();

        if (!AtEnd() && text_[pos_] == '{') {
            BeginBlock(key, line, side);
            return;
        }

        // No value on this line: the opening brace may still be on a later line
        // ("msl =" / newline / "{").
        if (OnlyBlankOrCommentLeft()) {
            const size_t savePos = pos_;
            const int saveLine = line_;
            SkipTrivia();
            if (!AtEnd() && text_[pos_] == '{') {
                BeginBlock(key, line, side);
                return;
            }
            pos_ = savePos;
            line_ = saveLine;
            Warn(line, "model '%s' has no value and no block (ignored)", key.c_str());
            SkipRestOfLine();
            return;
        }

        // Legacy shorthand: only HomingForesightAmount.
        const std::string token = ReadToken();
        float value = 0.0f;
        if (!ParseFloatValue(token, value)) {
            Warn(line, "model '%s': invalid value '%s' (ignored)", key.c_str(), token.c_str());
            SkipRestOfLine();
            return;
        }
        std::vector<MissileModel>& table = CurrentTable();
        const int index = EnsureModel(table, DeclaredForCurrentSection(), key, line, side, SectionLabel());
        FieldsOf(table, index, side).homingForesightAmount = value;

        SkipBlanks();
        if (!OnlyBlankOrCommentLeft()) {
            const std::string rest = TrimCopy(text_.substr(pos_));
            Warn(line, "model '%s': unexpected text after the value '%s' (ignored)", key.c_str(), rest.c_str());
        }
        SkipRestOfLine();
    }

    void BeginBlock(const std::string& key, int line, Side side) {
        Skip(1);  // '{'
        blockSide_ = side;
        blockTable_ = &CurrentTable();
        blockIndex_ = EnsureModel(*blockTable_, DeclaredForCurrentSection(), key, line, side, SectionLabel());
        blockKey_ = key;
        inBlock_ = true;
    }

    // One statement inside "<model> = { ... }": a property, a stray brace, or a
    // blank value that is immediately followed by the next property on the same
    // line ("LockonRange = MaxRotationAngle = 40").
    void ParseBlockStatement() {
        const int line = line_;
        const char c = text_[pos_];

        if (c == '}') {
            Skip(1);
            inBlock_ = false;
            blockIndex_ = -1;
            blockTable_ = nullptr;
            blockKey_.clear();
            return;
        }
        if (c == '{') {
            Warn(line, "stray '{' inside model '%s' block (nested blocks are not supported)", blockKey_.c_str());
            Skip(1);
            return;
        }
        if (c == '=') {
            Warn(line, "unexpected '=' inside model '%s' block (ignored)", blockKey_.c_str());
            Skip(1);
            return;
        }
        if (c == ']') {
            Warn(line, "unexpected ']' inside model '%s' block (ignored)", blockKey_.c_str());
            Skip(1);
            return;
        }

        const size_t start = pos_;
        while (!AtEnd() && !IsKeyBreak(text_[pos_])) Skip(1);
        const std::string keyRaw = TrimCopy(text_.substr(start, pos_ - start));
        if (keyRaw.empty()) {
            // Cannot happen for the branches above, but keep the scanner safe.
            Skip(1);
            return;
        }

        bool hasEquals = false;
        if (!AtEnd() && text_[pos_] == '=') {
            hasEquals = true;
            Skip(1);
            SkipBlanks();
        }

        bool blank = true;
        std::string value;

        if (hasEquals && !AtValueEnd()) {
            const size_t tokenStart = pos_;
            const std::string token = ReadToken();

            // Look ahead: "Blank = NextKey = 42" means Blank has *no* value and
            // the token we just read was really the next property's name.
            size_t probe = pos_;
            while (probe < text_.size() && IsBlank(text_[probe])) ++probe;
            const bool nextIsAssignment = (probe < text_.size() && text_[probe] == '=');
            float ignored = 0.0f;
            if (nextIsAssignment && !ParseFloatValue(token, ignored)) {
                pos_ = tokenStart;  // tokens never contain a newline, line_ stays valid
            } else {
                blank = false;
                value = token;
            }
        }

        ApplyProperty(keyRaw, blank, value, line);
    }

    void ApplyProperty(const std::string& keyRaw, bool blank, const std::string& value, int line) {
        std::string name = keyRaw;
        NormalizePropertyName(name);
        if (name.empty()) return;

        if (blockTable_ == nullptr || blockIndex_ < 0) return;
        FieldSet& fields = FieldsOf(*blockTable_, blockIndex_, blockSide_);
        std::optional<float>* field = FindField(fields, name);
        if (field == nullptr) {
            Warn(line, "unknown property '%s' in model '%s' (ignored)", keyRaw.c_str(), blockKey_.c_str());
            return;
        }
        if (blank) {
            field->reset();  // explicit blank == keep vanilla
            return;
        }
        float parsed = 0.0f;
        if (!ParseFloatValue(value, parsed)) {
            field->reset();
            Warn(line, "model '%s': invalid numeric value '%s' for property '%s' (left unset)",
                 blockKey_.c_str(), value.c_str(), keyRaw.c_str());
            return;
        }
        *field = parsed;
    }

    // ---- [runtime] -------------------------------------------------------

    void ParseRuntimeStatement() {
        const int line = line_;
        const char c = text_[pos_];
        if (c == '{' || c == '}') {
            Warn(line, "unexpected '%c' in [runtime] - model blocks belong to [player]/[enemy] (ignored)", c);
            Skip(1);
            return;
        }

        const size_t start = pos_;
        while (!AtEnd() && !IsKeyBreak(text_[pos_])) Skip(1);
        const std::string keyRaw = TrimCopy(text_.substr(start, pos_ - start));
        if (keyRaw.empty()) {
            Skip(1);
            return;
        }
        std::string key = keyRaw;
        NormalizePropertyName(key);

        bool hasEquals = false;
        if (!AtEnd() && text_[pos_] == '=') {
            hasEquals = true;
            Skip(1);
            SkipBlanks();
        }

        // HookSignature is free text ("48 8B 05 ?? ?? ?? ?? 48 85 C0"), not a
        // token: take the rest of the line verbatim (a comment still ends it).
        if (key == "hooksignature") {
            std::string signature;
            if (hasEquals) {
                const size_t valueStart = pos_;
                while (!AtEnd() && !IsLineEnd(text_[pos_]) && !IsCommentStart(text_[pos_])) Skip(1);
                signature = TrimCopy(text_.substr(valueStart, pos_ - valueStart));
            }
            SkipRestOfLine();
            ApplyHookSignature(signature, line);
            return;
        }

        bool blank = true;
        std::string value;
        if (hasEquals && !AtValueEnd()) {
            const size_t tokenStart = pos_;
            const std::string token = ReadToken();
            size_t probe = pos_;
            while (probe < text_.size() && IsBlank(text_[probe])) ++probe;
            const bool nextIsAssignment = (probe < text_.size() && text_[probe] == '=');
            float ignored = 0.0f;
            if (nextIsAssignment && !ParseFloatValue(token, ignored)) {
                pos_ = tokenStart;
            } else {
                blank = false;
                value = token;
            }
        }
        SkipRestOfLine();
        ApplyRuntime(keyRaw, key, blank, value, line);
    }

    void ApplyHookSignature(const std::string& signature, int line) {
        if (signature.empty()) return;  // blank keeps the default (empty)
        constexpr size_t kMaxSignatureLength = 512;
        if (signature.size() > kMaxSignatureLength) {
            Warn(line, "[runtime] HookSignature is %u characters (max %u) - truncated",
                 static_cast<unsigned>(signature.size()), static_cast<unsigned>(kMaxSignatureLength));
        }
        cfg_.runtime.hookSignature = signature.substr(0, kMaxSignatureLength);
    }

    void ApplyRuntime(const std::string& keyRaw, const std::string& key, bool blank,
                      const std::string& value, int line) {
        if (key.empty()) return;
        if (!IsKnownRuntimeKey(key)) {
            Warn(line, "unknown [runtime] key '%s' (ignored)", keyRaw.c_str());
            return;
        }
        if (blank) return;  // blank keeps the documented default

        if (key == "loglevel") {
            int v = 0;
            if (!ParseIntValue(value, v)) {
                Warn(line, "[runtime] LogLevel: invalid integer '%s' (ignored)", value.c_str());
                return;
            }
            if (v < 0 || v > 2) {
                const int clamped = (v < 0) ? 0 : 2;
                Warn(line, "[runtime] LogLevel %d is outside 0..2 (clamped to %d)", v, clamped);
                v = clamped;
            }
            cfg_.runtime.logLevel = v;
            return;
        }
        if (key == "tickhz") {
            int v = 0;
            if (!ParseIntValue(value, v)) {
                Warn(line, "[runtime] TickHz: invalid integer '%s' (ignored)", value.c_str());
                return;
            }
            if (v < 1 || v > 1000) {
                const int clamped = (v < 1) ? 1 : 1000;
                Warn(line, "[runtime] TickHz %d is outside 1..1000 (clamped to %d)", v, clamped);
                v = clamped;
            }
            cfg_.runtime.tickHz = v;
            return;
        }
        if (key == "leadscale") {
            // Frames of lead added at HomingForesightAmount = 1.0 (60 Hz scale).
            float v = 0.0f;
            if (!ParseFloatValue(value, v) || !std::isfinite(v) || v < 0.0f || v > 2000.0f) {
                Warn(line, "[runtime] LeadScale: invalid value '%s' (expected a number in 0..2000, ignored)",
                     value.c_str());
                return;
            }
            cfg_.runtime.leadScale = v;
            return;
        }
        if (key == "leadmode") {
            const std::string mode = LowerCopy(TrimCopy(value));
            if (mode == "add" || mode == "blend" || mode == "absolute") {
                cfg_.runtime.leadMode = mode;  // stored canonically, lower-case
            } else {
                Warn(line, "[runtime] LeadMode: invalid mode '%s' (expected add/blend/absolute, ignored)",
                     value.c_str());
            }
            return;
        }
        if (key == "pngain") {
            // Navigation constant N of the plain-MSL true-PN law (docs/MSL_PN.md).
            float v = 0.0f;
            if (!ParseFloatValue(value, v) || !std::isfinite(v) || v < 0.5f || v > 12.0f) {
                Warn(line,
                     "[runtime] PnGain: invalid value '%s' (expected a number in 0.5..12, ignored)",
                     value.c_str());
                return;
            }
            cfg_.runtime.pnGain = v;
            return;
        }
        if (key == "terminalzone" || key == "terminalhandback" || key == "handback") {
            // Plain-MSL terminal hand-back radius in metres. 0 = full PN (default).
            float v = 0.0f;
            if (!ParseFloatValue(value, v) || !std::isfinite(v) || v < 0.0f || v > 2000.0f) {
                Warn(line,
                     "[runtime] TerminalZone: invalid value '%s' (expected a number in 0..2000, "
                     "ignored)",
                     value.c_str());
                return;
            }
            cfg_.runtime.terminalZone = v;
            // This is an informational note about a legitimate setting, not a problem
            // with the config: use Warn() and every caller that asserts "no warnings"
            // (initest, and users reading the log) would treat a valid option as an
            // error. The value is echoed in the MSLPN: startup line instead.
            (void)line;
            return;
        }
        if (key == "hookargindex") {
            int v = 0;
            if (!ParseIntValue(value, v)) {
                Warn(line, "[runtime] HookArgIndex: invalid integer '%s' (ignored)", value.c_str());
                return;
            }
            if (v < 1 || v > 4) {
                Warn(line, "[runtime] HookArgIndex %d is outside 1..4 (ignored)", v);
                return;
            }
            cfg_.runtime.hookArgIndex = v;
            return;
        }
        if (key == "ringsurvey") {
            int v = 0;
            if (!ParseIntValue(value, v)) {
                Warn(line, "[runtime] RingSurvey: invalid integer '%s' (expected 0 or 1, ignored)",
                     value.c_str());
                return;
            }
            cfg_.runtime.ringSurvey = (v != 0) ? 1 : 0;
            return;
        }
        if (key == "hwwatch") {
            int v = 0;
            if (!ParseIntValue(value, v)) {
                Warn(line, "[runtime] HwWatch: invalid integer '%s' (expected 0 or 1, ignored)",
                     value.c_str());
                return;
            }
            cfg_.runtime.hwWatch = (v != 0) ? 1 : 0;
            return;
        }
        if (key == "hwwatchseconds") {
            int v = 0;
            if (!ParseIntValue(value, v)) {
                Warn(line, "[runtime] HwWatchSeconds: invalid integer '%s' (ignored)",
                     value.c_str());
                return;
            }
            if (v < 1 || v > 600) {
                Warn(line, "[runtime] HwWatchSeconds %d is outside 1..600 (ignored)", v);
                return;
            }
            cfg_.runtime.hwWatchSeconds = v;
            return;
        }
        if (key == "dryrun" || key == "probe" || key == "requireverifiedoffsets" ||
            key == "guidanceoverride" || key == "forceoffsets" || key == "entityoverride") {
            bool b = false;
            if (!ParseBoolValue(value, b)) {
                Warn(line, "[runtime] %s: invalid boolean '%s' (expected 0/1/true/false/yes/no, ignored)",
                     keyRaw.c_str(), value.c_str());
                return;
            }
            if (key == "dryrun") cfg_.runtime.dryRun = b;
            else if (key == "probe") cfg_.runtime.probe = b;
            else if (key == "requireverifiedoffsets") cfg_.runtime.requireVerifiedOffsets = b;
            else if (key == "guidanceoverride") cfg_.runtime.guidanceOverride = b;
            else if (key == "forceoffsets") cfg_.runtime.forceOffsets = b;
            else cfg_.runtime.entityOverride = b;
            return;
        }
        if (key == "entityposoffset" || key == "entityveloffset" || key == "targetposoffset" ||
            key == "targetveloffset" || key == "hookrva") {
            uint32_t offset = 0;
            if (!ParseUIntValue(value, offset)) {
                Warn(line, "[runtime] %s: invalid value '%s' (expected decimal or 0x-hex, ignored)",
                     keyRaw.c_str(), value.c_str());
                return;
            }
            const uint32_t limit = (key == "hookrva") ? 0x4000000u : 0x10000u;
            if (offset > limit) {
                Warn(line, "[runtime] %s = 0x%X exceeds the maximum 0x%X (ignored)",
                     keyRaw.c_str(), static_cast<unsigned>(offset), static_cast<unsigned>(limit));
                return;
            }
            if (key == "entityposoffset") cfg_.runtime.entityPosOffset = offset;
            else if (key == "entityveloffset") cfg_.runtime.entityVelOffset = offset;
            else if (key == "targetposoffset") cfg_.runtime.targetPosOffset = offset;
            else if (key == "targetveloffset") cfg_.runtime.targetVelOffset = offset;
            else cfg_.runtime.hookRva = offset;
            return;
        }
        // Defensive: every key accepted by IsKnownRuntimeKey() is dispatched above.
        Warn(line, "[runtime] key '%s' is recognised but not handled (ignored)", keyRaw.c_str());
    }

    // ---- state -----------------------------------------------------------

    const std::string& text_;
    Config& cfg_;
    size_t pos_ = 0;
    int line_ = 1;
    SectionKind section_ = SectionKind::None;
    bool inBlock_ = false;
    int blockIndex_ = -1;
    Side blockSide_ = Side::Player;
    std::vector<MissileModel>* blockTable_ = nullptr;
    std::string blockKey_;
    std::vector<std::string> declaredPlayer_;
    std::vector<std::string> declaredEnemy_;
    std::vector<std::string> declaredVanilla_;
    bool warnedPreamble_ = false;
};

// ===========================================================================
//  post-parse clamping
// ===========================================================================

void SanitizeField(std::optional<float>& field, float low, float high, const char* name,
                   const std::string& where, std::vector<std::string>& warnings) {
    if (!field) return;  // blank/unknown stays unset
    const float value = *field;
    if (!std::isfinite(value)) {
        field.reset();
        warnings.push_back("Sanitize: " + where + " " + name + " is not a finite number - left unset");
        return;
    }
    if (value < low) {
        field = low;
        warnings.push_back("Sanitize: " + where + " " + name + " " + FormatFloat(value) +
                           " clamped to " + FormatFloat(low));
    } else if (value > high) {
        field = high;
        warnings.push_back("Sanitize: " + where + " " + name + " " + FormatFloat(value) +
                           " clamped to " + FormatFloat(high));
    }
}

void SanitizeSet(FieldSet& fields, const std::string& where, std::vector<std::string>& warnings) {
    SanitizeField(fields.homingForesightAmount, 0.0f, 1.0f, "HomingForesightAmount", where, warnings);
    SanitizeField(fields.maxHomingAngle, 0.0f, 360.0f, "MaxHomingAngle", where, warnings);
    SanitizeField(fields.maxRotationAngle, 0.0f, 720.0f, "MaxRotationAngle", where, warnings);
    SanitizeField(fields.lifeTime, 0.1f, 120.0f, "LifeTime", where, warnings);
    SanitizeField(fields.lockonRange, 0.0f, 100000.0f, "LockonRange", where, warnings);
}

}  // namespace

// ===========================================================================
//  public API
// ===========================================================================

const char* SideName(Side side) {
    switch (side) {
        case Side::Player: return "player";
        case Side::Enemy: return "enemy";
    }
    return "player";
}

void NormalizePropertyName(std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        if (c == ' ' || c == '\t' || c == '_' || c == '\r' || c == '\n') continue;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        out.push_back(c);
    }
    name.swap(out);
}

bool ParseFloatValue(const std::string& text, float& out) {
    const std::string s = TrimCopy(text);
    if (s.empty()) return false;

    size_t i = 0;
    bool negative = false;
    if (s[i] == '+' || s[i] == '-') {
        negative = (s[i] == '-');
        ++i;
    }

    double mantissa = 0.0;
    int mantissaDigits = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        mantissa = mantissa * 10.0 + static_cast<double>(s[i] - '0');
        ++mantissaDigits;
        ++i;
    }
    if (i < s.size() && s[i] == '.') {
        ++i;
        double scale = 0.1;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            mantissa += static_cast<double>(s[i] - '0') * scale;
            scale *= 0.1;
            ++mantissaDigits;
            ++i;
        }
    }
    if (mantissaDigits == 0) return false;

    int exponent = 0;
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        bool exponentNegative = false;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
            exponentNegative = (s[i] == '-');
            ++i;
        }
        int expDigits = 0;
        int expValue = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            if (expValue < 100000) expValue = expValue * 10 + (s[i] - '0');
            ++expDigits;
            ++i;
        }
        if (expDigits == 0) return false;
        exponent = exponentNegative ? -expValue : expValue;
    }

    if (i < s.size() && (s[i] == 'f' || s[i] == 'F')) ++i;  // C float suffix
    if (i != s.size()) return false;                        // trailing garbage

    double value = mantissa * std::pow(10.0, static_cast<double>(exponent));
    if (!std::isfinite(value)) return false;
    if (value > 3.4028234663852886e38 || value < -3.4028234663852886e38) return false;
    if (negative) value = -value;

    const float result = static_cast<float>(value);
    if (!std::isfinite(result)) return false;
    out = result;
    return true;
}

bool Config::LoadFromFile(const std::wstring& path) {
    sourcePath = WideToUtf8(path);
    models.clear();
    vanilla.clear();
    warnings.clear();
    runtime = RuntimeSection();
    loaded = false;

    std::string text;
    const ReadStatus status = ReadTextFile(path, text);
    if (status == ReadStatus::TooLarge) {
        warnings.push_back("config file is larger than 1 MiB - refusing to parse");
        return false;
    }
    if (status != ReadStatus::Ok) {
        return false;  // cannot be opened/read: the only other hard failure
    }

    IniParser parser(text, *this);
    parser.Run();

    loaded = true;  // the file was read and parsed (possibly with warnings)
    Sanitize();     // post-parse clamps; Sanitize preserves the loaded flag
    return true;
}

const FieldSet* Config::Resolve(Side side, const std::string& typeName) const {
    const std::string needle = LowerCopy(TrimCopy(typeName));
    if (needle.empty() || models.empty()) return nullptr;

    const FieldSet* best = nullptr;
    size_t bestLength = 0;
    // Declaration order: on an equal-length tie the first declared model wins.
    for (const MissileModel& model : models) {
        if (model.key.empty()) continue;
        const FieldSet& fields = (side == Side::Enemy) ? model.enemy : model.player;
        if (!fields.Any()) continue;  // nothing configured for this side
        if (needle.find(model.key) == std::string::npos) continue;
        if (model.key.size() > bestLength) {
            best = &fields;
            bestLength = model.key.size();
        }
    }
    return best;
}

std::string Config::WarningSummary() const {
    std::string out;
    for (size_t i = 0; i < warnings.size(); ++i) {
        if (i != 0) out += " | ";
        out += warnings[i];
    }
    return out;
}

void Config::Sanitize() {
    for (MissileModel& model : models) {
        SanitizeSet(model.player, std::string("[") + SideName(Side::Player) + "] " + model.key, warnings);
        SanitizeSet(model.enemy, std::string("[") + SideName(Side::Enemy) + "] " + model.key, warnings);
    }
    // [vanilla] values are discovery signatures, not overrides, but they must
    // stay inside the same valid ranges.
    for (MissileModel& model : vanilla) {
        SanitizeSet(model.player, std::string("[vanilla] ") + model.key, warnings);
    }
    // `loaded` is intentionally untouched: it records whether the file itself
    // was readable/parsed, which is independent of value clamping.
}

}  // namespace acz
