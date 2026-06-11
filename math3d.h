// math3d.h - minimal row-major / row-vector (v' = v*M) 3D math, D3D left-handed.
#pragma once
#include <cmath>

struct V3 { float x = 0, y = 0, z = 0; };

inline V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline V3 operator*(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
inline V3 operator-(V3 a) { return { -a.x, -a.y, -a.z }; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
inline float len(V3 a) { return sqrtf(dot(a, a)); }
inline V3 norm(V3 a) { float l = len(a); return l > 1e-9f ? a * (1.0f / l) : V3{ 0,0,1 }; }
inline V3 lerp(V3 a, V3 b, float t) { return a + (b - a) * t; }

struct M4 {
    float m[4][4] = {};
    static M4 identity() {
        M4 r; r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1; return r;
    }
    // basis row accessors (row-vector convention: rows are the local axes)
    V3 right() const { return { m[0][0], m[0][1], m[0][2] }; }
    V3 up()    const { return { m[1][0], m[1][1], m[1][2] }; }
    V3 fwd()   const { return { m[2][0], m[2][1], m[2][2] }; }
    V3 pos()   const { return { m[3][0], m[3][1], m[3][2] }; }
    void setRow(int i, V3 v) { m[i][0] = v.x; m[i][1] = v.y; m[i][2] = v.z; }
};

inline M4 mul(const M4& a, const M4& b) {
    M4 r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a.m[i][k] * b.m[k][j];
            r.m[i][j] = s;
        }
    return r;
}

inline V3 transformPoint(V3 v, const M4& M) {
    return { v.x * M.m[0][0] + v.y * M.m[1][0] + v.z * M.m[2][0] + M.m[3][0],
             v.x * M.m[0][1] + v.y * M.m[1][1] + v.z * M.m[2][1] + M.m[3][1],
             v.x * M.m[0][2] + v.y * M.m[1][2] + v.z * M.m[2][2] + M.m[3][2] };
}
inline V3 transformDir(V3 v, const M4& M) {
    return { v.x * M.m[0][0] + v.y * M.m[1][0] + v.z * M.m[2][0],
             v.x * M.m[0][1] + v.y * M.m[1][1] + v.z * M.m[2][1],
             v.x * M.m[0][2] + v.y * M.m[1][2] + v.z * M.m[2][2] };
}

inline M4 translation(V3 t) { M4 r = M4::identity(); r.m[3][0] = t.x; r.m[3][1] = t.y; r.m[3][2] = t.z; return r; }
inline M4 scaling(float s) { M4 r = M4::identity(); r.m[0][0] = r.m[1][1] = r.m[2][2] = s; return r; }
inline M4 scaling3(V3 s) { M4 r = M4::identity(); r.m[0][0] = s.x; r.m[1][1] = s.y; r.m[2][2] = s.z; return r; }

// rotation about arbitrary (unit) axis, row-vector convention (matches D3DXMatrixRotationAxis)
inline M4 rotationAxis(V3 a, float ang) {
    float c = cosf(ang), s = sinf(ang), t = 1 - c;
    M4 r = M4::identity();
    r.m[0][0] = c + a.x * a.x * t;       r.m[0][1] = a.x * a.y * t + a.z * s; r.m[0][2] = a.x * a.z * t - a.y * s;
    r.m[1][0] = a.x * a.y * t - a.z * s; r.m[1][1] = c + a.y * a.y * t;       r.m[1][2] = a.y * a.z * t + a.x * s;
    r.m[2][0] = a.x * a.z * t + a.y * s; r.m[2][1] = a.y * a.z * t - a.x * s; r.m[2][2] = c + a.z * a.z * t;
    return r;
}
inline M4 rotationY(float ang) { return rotationAxis({ 0,1,0 }, ang); }
inline M4 rotationX(float ang) { return rotationAxis({ 1,0,0 }, ang); }

// build a rotation whose forward row points along dir
inline M4 basisFromFwd(V3 dir) {
    V3 f = norm(dir);
    V3 upHint = fabsf(f.y) > 0.98f ? V3{ 1,0,0 } : V3{ 0,1,0 };
    V3 r = norm(cross(upHint, f));
    V3 u = cross(f, r);
    M4 M = M4::identity();
    M.setRow(0, r); M.setRow(1, u); M.setRow(2, f);
    return M;
}

inline M4 lookAtLH(V3 eye, V3 at, V3 upHint) {
    V3 z = norm(at - eye);
    V3 x = norm(cross(upHint, z));
    V3 y = cross(z, x);
    M4 r = M4::identity();
    r.m[0][0] = x.x; r.m[0][1] = y.x; r.m[0][2] = z.x;
    r.m[1][0] = x.y; r.m[1][1] = y.y; r.m[1][2] = z.y;
    r.m[2][0] = x.z; r.m[2][1] = y.z; r.m[2][2] = z.z;
    r.m[3][0] = -dot(x, eye); r.m[3][1] = -dot(y, eye); r.m[3][2] = -dot(z, eye);
    return r;
}

inline M4 perspectiveFovLH(float fovY, float aspect, float zn, float zf) {
    float ys = 1.0f / tanf(fovY * 0.5f);
    float xs = ys / aspect;
    M4 r;
    r.m[0][0] = xs; r.m[1][1] = ys;
    r.m[2][2] = zf / (zf - zn); r.m[2][3] = 1;
    r.m[3][2] = -zn * zf / (zf - zn);
    return r;
}

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
