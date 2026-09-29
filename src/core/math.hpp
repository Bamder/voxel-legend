#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>

struct Vec2 {
    float x = 0.0f, y = 0.0f;
    Vec2() = default;
    Vec2(float X, float Y) : x(X), y(Y) {}
};

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    Vec3() = default;
    Vec3(float X, float Y, float Z) : x(X), y(Y), z(Z) {}

    Vec3 operator+(const Vec3& o) const { return { x + o.x, y + o.y, z + o.z }; }
    Vec3 operator-(const Vec3& o) const { return { x - o.x, y - o.y, z - o.z }; }
    Vec3 operator*(float s) const { return { x * s, y * s, z * s }; }
    Vec3 operator/(float s) const { float i = 1.0f / s; return { x * i, y * i, z * i }; }
    Vec3 operator-() const { return { -x, -y, -z }; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }

    float dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 cross(const Vec3& o) const {
        return { y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x };
    }
    float lengthSq() const { return x * x + y * y + z * z; }
    float length() const { return std::sqrt(lengthSq()); }
    Vec3 normalized() const {
        float l = length();
        return l > 1e-8f ? (*this) / l : Vec3{ 0.0f, 0.0f, 0.0f };
    }
};

struct IVec3 {
    int x = 0, y = 0, z = 0;
    IVec3() = default;
    IVec3(int X, int Y, int Z) : x(X), y(Y), z(Z) {}
    bool operator==(const IVec3& o) const { return x == o.x && y == o.y && z == o.z; }
};

struct Vec4 {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
    Vec4() = default;
    Vec4(float X, float Y, float Z, float W) : x(X), y(Y), z(Z), w(W) {}
};

// Column-major 4x4 matrix (OpenGL convention).
struct Mat4 {
    float m[16];

    Mat4() { setIdentity(); }

    void setIdentity() {
        for (int i = 0; i < 16; i++) m[i] = 0.0f;
        m[0] = m[5] = m[10] = m[15] = 1.0f;
    }

    static Mat4 fromBasis(const Vec3& x, const Vec3& y, const Vec3& z) {
        Mat4 r;
        r.m[0] = x.x; r.m[1] = x.y; r.m[2] = x.z;
        r.m[4] = y.x; r.m[5] = y.y; r.m[6] = y.z;
        r.m[8] = z.x; r.m[9] = z.y; r.m[10] = z.z;
        return r;
    }

    static Mat4 translate(const Vec3& t) {
        Mat4 r;
        r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
        return r;
    }

    static Mat4 scale(const Vec3& s) {
        Mat4 r;
        r.m[0] = s.x; r.m[5] = s.y; r.m[10] = s.z;
        return r;
    }

    static Mat4 ortho(float left, float right, float bottom, float top, float znear, float zfar) {
        Mat4 r;
        r.m[0] = 2.0f / (right - left);
        r.m[5] = 2.0f / (top - bottom);
        r.m[10] = -2.0f / (zfar - znear);
        r.m[12] = -(right + left) / (right - left);
        r.m[13] = -(top + bottom) / (top - bottom);
        r.m[14] = -(zfar + znear) / (zfar - znear);
        return r;
    }

    static Mat4 perspective(float fovyDeg, float aspect, float znear, float zfar) {
        Mat4 r;
        float f = 1.0f / std::tan(fovyDeg * 0.5f * 3.14159265358979323846f / 180.0f);
        r.m[0] = f / aspect;
        r.m[5] = f;
        r.m[10] = (zfar + znear) / (znear - zfar);
        r.m[11] = -1.0f;
        r.m[14] = (2.0f * zfar * znear) / (znear - zfar);
        r.m[15] = 0.0f;
        return r;
    }

    static Mat4 lookAt(const Vec3& eye, const Vec3& center, const Vec3& up) {
        Vec3 f = (center - eye).normalized();
        Vec3 s = f.cross(up).normalized();
        Vec3 u = s.cross(f);
        Mat4 r;
        r.m[0] = s.x; r.m[4] = s.y; r.m[8] = s.z;
        r.m[1] = u.x; r.m[5] = u.y; r.m[9] = u.z;
        r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
        r.m[12] = -s.dot(eye); r.m[13] = -u.dot(eye); r.m[14] = f.dot(eye);
        r.m[15] = 1.0f;
        return r;
    }
};

inline Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int c = 0; c < 4; c++) {
        for (int row = 0; row < 4; row++) {
            r.m[c * 4 + row] =
                a.m[0 * 4 + row] * b.m[c * 4 + 0] +
                a.m[1 * 4 + row] * b.m[c * 4 + 1] +
                a.m[2 * 4 + row] * b.m[c * 4 + 2] +
                a.m[3 * 4 + row] * b.m[c * 4 + 3];
        }
    }
    return r;
}

inline Vec4 operator*(const Mat4& m, const Vec4& v) {
    return {
        m.m[0] * v.x + m.m[4] * v.y + m.m[8] * v.z + m.m[12] * v.w,
        m.m[1] * v.x + m.m[5] * v.y + m.m[9] * v.z + m.m[13] * v.w,
        m.m[2] * v.x + m.m[6] * v.y + m.m[10] * v.z + m.m[14] * v.w,
        m.m[3] * v.x + m.m[7] * v.y + m.m[11] * v.z + m.m[15] * v.w,
    };
}

inline Mat4 inverse(const Mat4& a) {
    // Gauss-Jordan elimination on [a | I].
    float m[4][8];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) m[r][c] = a.m[c * 4 + r];
        for (int c = 4; c < 8; c++) m[r][c] = (r == c - 4) ? 1.0f : 0.0f;
    }
    for (int col = 0; col < 4; col++) {
        int piv = col;
        for (int r = col + 1; r < 4; r++)
            if (std::fabs(m[r][col]) > std::fabs(m[piv][col])) piv = r;
        if (std::fabs(m[piv][col]) < 1e-8f) return Mat4(); // singular
        if (piv != col)
            for (int c = 0; c < 8; c++) std::swap(m[piv][c], m[col][c]);
        float d = m[col][col];
        for (int c = 0; c < 8; c++) m[col][c] /= d;
        for (int r = 0; r < 4; r++) {
            if (r == col) continue;
            float f = m[r][col];
            for (int c = 0; c < 8; c++) m[r][c] -= f * m[col][c];
        }
    }
    Mat4 r;
    for (int rr = 0; rr < 4; rr++)
        for (int cc = 0; cc < 4; cc++) r.m[cc * 4 + rr] = m[rr][cc + 4];
    return r;
}

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr float kDeg2Rad = kPi / 180.0f;
inline float wrapPi(float a) {
    a = std::fmod(a + kPi, 2.0f * kPi);
    if (a < 0.0f) a += 2.0f * kPi;
    return a - kPi;
}
