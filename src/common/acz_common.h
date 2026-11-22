// ACZPropNav - shared basic types and helpers
// Target: ACE COMBAT ZERO: THE BELKAN WAR (Steam) - x86_64 Windows
#pragma once

#include <cstdint>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

namespace acz {

struct Vec3 {
    float x = 0.f, y = 0.f, z = 0.f;

    Vec3() = default;
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    Vec3 operator+(const Vec3& o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
    Vec3 operator-(const Vec3& o) const { return Vec3(x - o.x, y - o.y, z - o.z); }
    Vec3 operator*(float s) const { return Vec3(x * s, y * s, z * s); }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
};

inline float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(const Vec3& a, const Vec3& b) {
    return Vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
inline float LengthSq(const Vec3& v) { return Dot(v, v); }
inline float Length(const Vec3& v) { return std::sqrt(LengthSq(v)); }

// Normalize; returns false (and a zero vector) for degenerate input.
inline bool Normalize(const Vec3& v, Vec3& out, float eps = 1e-6f) {
    const float len = Length(v);
    if (len < eps) { out = Vec3(); return false; }
    out = v * (1.0f / len);
    return true;
}

inline Vec3 NormalizedOr(const Vec3& v, const Vec3& fallback, float eps = 1e-6f) {
    Vec3 n;
    return Normalize(v, n, eps) ? n : fallback;
}

inline float Clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

inline float DegToRad(float d) { return d * 0.017453292519943295f; }
inline float RadToDeg(float r) { return r * 57.29577951308232f; }

// Angle between two unit (or non-unit) vectors, radians.
inline float AngleBetween(const Vec3& a, const Vec3& b) {
    const float la = Length(a), lb = Length(b);
    if (la < 1e-6f || lb < 1e-6f) return 0.f;
    float c = Dot(a, b) / (la * lb);
    c = Clampf(c, -1.f, 1.f);
    return std::acos(c);
}

// Move direction `from` toward direction `to` by at most maxAngle rad, on the great
// circle between them. Works for arbitrary vectors; result is normalized.
inline Vec3 RotateTowards(const Vec3& from, const Vec3& to, float maxAngleRad) {
    Vec3 f, t;
    if (!Normalize(from, f)) return NormalizedOr(to, Vec3(0, 0, 1));
    if (!Normalize(to, t)) return f;
    const float ang = AngleBetween(f, t);
    if (ang <= maxAngleRad || ang < 1e-6f) return t;
    // axis = f x t
    Vec3 axis;
    if (!Normalize(Cross(f, t), axis)) {
        // Exactly (anti)parallel: the cross product is zero and there is no unique
        // rotation plane, so the old code gave up and returned `from` unchanged -
        // i.e. a missile pointed exactly 180 deg away from its target never turned
        // and flew on forever. Pick any axis perpendicular to `from` and take a
        // normal rate-limited step along it; any choice is correct here because
        // every direction in that plane is equally valid.
        Vec3 perp = Cross(f, Vec3(0, 0, 1));
        if (!Normalize(perp, perp)) perp = Cross(f, Vec3(1, 0, 0));
        if (!Normalize(perp, perp)) return f;   // f is degenerate; nothing to do
        const float step = (maxAngleRad > 0.f) ? maxAngleRad : 0.f;
        const float c = std::cos(step), s = std::sin(step);
        return NormalizedOr(perp * s + f * c, f);
    }
    // Rodrigues rotation of f about axis by maxAngleRad
    const float c = std::cos(maxAngleRad), s = std::sin(maxAngleRad);
    const Vec3 r = f * c + Cross(axis, f) * s + axis * (Dot(axis, f) * (1.f - c));
    return NormalizedOr(r, t);
}

// ---- small string helpers (ASCII/UTF-8 only) ----
inline std::string TrimAscii(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}
inline std::string LowerAscii(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}
inline bool EndsWithNoCase(const std::string& s, const std::string& suffix) {
    if (suffix.size() > s.size()) return false;
    return LowerAscii(s.substr(s.size() - suffix.size())) == LowerAscii(suffix);
}

} // namespace acz
