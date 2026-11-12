// Small vector/matrix helpers. Row-vector convention (p' = p * M) like the original D3D engine,
// stored row-major. Uploaded as-is, GLSL reads it as the column-major transpose, so the
// shader's `M * v` computes `v * M`.
#pragma once
#include <cmath>
#include <cstring>

namespace hg2 {

struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(float a, float b, float c) : x(a), y(b), z(c) {}
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3& operator+=(const Vec3& o) { x += o.x, y += o.y, z += o.z; return *this; }
};
inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(const Vec3& a) { return std::sqrt(dot(a, a)); }
inline Vec3 normalize(const Vec3& a) {
    float l = length(a);
    return l > 0 ? a * (1.f / l) : a;
}

// m[r][c], row-vector convention: p' = p * M, translation in row 3.
struct Mat4 {
    float m[4][4];
    static Mat4 identity() {
        Mat4 r{};
        for (int i = 0; i < 4; i++) r.m[i][i] = 1;
        return r;
    }
    Mat4 operator*(const Mat4& b) const {
        Mat4 r{};
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                for (int k = 0; k < 4; k++) r.m[i][j] += m[i][k] * b.m[k][j];
        return r;
    }
    Vec3 transformPoint(const Vec3& p) const {
        return {p.x * m[0][0] + p.y * m[1][0] + p.z * m[2][0] + m[3][0],
                p.x * m[0][1] + p.y * m[1][1] + p.z * m[2][1] + m[3][1],
                p.x * m[0][2] + p.y * m[1][2] + p.z * m[2][2] + m[3][2]};
    }
    Vec3 transformDir(const Vec3& p) const {
        return {p.x * m[0][0] + p.y * m[1][0] + p.z * m[2][0], p.x * m[0][1] + p.y * m[1][1] + p.z * m[2][1],
                p.x * m[0][2] + p.y * m[1][2] + p.z * m[2][2]};
    }
    static Mat4 translate(const Vec3& t) {
        Mat4 r = identity();
        r.m[3][0] = t.x, r.m[3][1] = t.y, r.m[3][2] = t.z;
        return r;
    }
    static Mat4 scale(float s) {
        Mat4 r = identity();
        r.m[0][0] = r.m[1][1] = r.m[2][2] = s;
        return r;
    }
    static Mat4 rotY(float a) {  // left-handed, positive = clockwise seen from above
        Mat4 r = identity();
        float c = std::cos(a), s = std::sin(a);
        r.m[0][0] = c, r.m[0][2] = -s, r.m[2][0] = s, r.m[2][2] = c;
        return r;
    }
    static Mat4 rotX(float a) {
        Mat4 r = identity();
        float c = std::cos(a), s = std::sin(a);
        r.m[1][1] = c, r.m[1][2] = s, r.m[2][1] = -s, r.m[2][2] = c;
        return r;
    }
    static Mat4 rotZ(float a) {
        Mat4 r = identity();
        float c = std::cos(a), s = std::sin(a);
        r.m[0][0] = c, r.m[0][1] = s, r.m[1][0] = -s, r.m[1][1] = c;
        return r;
    }
    // Left-handed look-at (D3DXMatrixLookAtLH)
    static Mat4 lookAt(const Vec3& eye, const Vec3& at, const Vec3& up) {
        Vec3 z = normalize(at - eye), x = normalize(cross(up, z)), y = cross(z, x);
        Mat4 r = identity();
        r.m[0][0] = x.x, r.m[1][0] = x.y, r.m[2][0] = x.z;
        r.m[0][1] = y.x, r.m[1][1] = y.y, r.m[2][1] = y.z;
        r.m[0][2] = z.x, r.m[1][2] = z.y, r.m[2][2] = z.z;
        r.m[3][0] = -dot(x, eye), r.m[3][1] = -dot(y, eye), r.m[3][2] = -dot(z, eye);
        return r;
    }
    // Left-handed perspective, depth 0..1 (D3DXMatrixPerspectiveFovLH)
    static Mat4 perspective(float fovY, float aspect, float zn, float zf) {
        Mat4 r{};
        float ys = 1.f / std::tan(fovY * 0.5f), xs = ys / aspect;
        r.m[0][0] = xs, r.m[1][1] = ys, r.m[2][2] = zf / (zf - zn), r.m[2][3] = 1, r.m[3][2] = -zn * zf / (zf - zn);
        return r;
    }
    // For GLSL (column-major, M * v): the transpose of a row-vector matrix has the same memory layout.
    const float* data() const { return &m[0][0]; }
};

}  // namespace hg2
