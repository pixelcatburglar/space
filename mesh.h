// mesh.h - procedural geometry builders. Units are kilometers.
#pragma once
#include <vector>
#include <cstdint>
#include "math3d.h"

struct Col { float r = 1, g = 1, b = 1, a = 0; }; // a = emissive amount

struct Vtx { float p[3]; float n[3]; float c[4]; };

struct MeshData {
    std::vector<Vtx> verts;
    std::vector<uint32_t> idx;
};

inline void pushVert(MeshData& m, V3 p, V3 n, Col c, const M4* xf) {
    if (xf) { p = transformPoint(p, *xf); n = norm(transformDir(n, *xf)); }
    m.verts.push_back({ {p.x,p.y,p.z}, {n.x,n.y,n.z}, {c.r,c.g,c.b,c.a} });
}

// quad a,b,c,d (CCW when viewed against normal); we render cull-none so winding is forgiving
inline void addQuad(MeshData& m, V3 a, V3 b, V3 c, V3 d, V3 n, Col col, const M4* xf = nullptr) {
    uint32_t base = (uint32_t)m.verts.size();
    pushVert(m, a, n, col, xf); pushVert(m, b, n, col, xf);
    pushVert(m, c, n, col, xf); pushVert(m, d, n, col, xf);
    m.idx.insert(m.idx.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
}

inline void addBox(MeshData& m, V3 c, V3 h, Col col, const M4* xf = nullptr) {
    V3 p[8] = {
        {c.x - h.x, c.y - h.y, c.z - h.z}, {c.x + h.x, c.y - h.y, c.z - h.z},
        {c.x + h.x, c.y + h.y, c.z - h.z}, {c.x - h.x, c.y + h.y, c.z - h.z},
        {c.x - h.x, c.y - h.y, c.z + h.z}, {c.x + h.x, c.y - h.y, c.z + h.z},
        {c.x + h.x, c.y + h.y, c.z + h.z}, {c.x - h.x, c.y + h.y, c.z + h.z},
    };
    addQuad(m, p[0], p[3], p[2], p[1], { 0,0,-1 }, col, xf); // -z
    addQuad(m, p[4], p[5], p[6], p[7], { 0,0, 1 }, col, xf); // +z
    addQuad(m, p[0], p[4], p[7], p[3], { -1,0,0 }, col, xf); // -x
    addQuad(m, p[1], p[2], p[6], p[5], { 1,0,0 }, col, xf); // +x
    addQuad(m, p[3], p[7], p[6], p[2], { 0,1,0 }, col, xf); // +y
    addQuad(m, p[0], p[1], p[5], p[4], { 0,-1,0 }, col, xf); // -y
}

// cylinder along Y axis, centered at origin
inline void addCylinder(MeshData& m, float r, float halfH, int segs, Col col, const M4* xf = nullptr) {
    const float TAU = 6.2831853f;
    for (int i = 0; i < segs; i++) {
        float a0 = TAU * i / segs, a1 = TAU * (i + 1) / segs;
        V3 n0 = { cosf(a0), 0, sinf(a0) }, n1 = { cosf(a1), 0, sinf(a1) };
        V3 b0 = { n0.x * r, -halfH, n0.z * r }, b1 = { n1.x * r, -halfH, n1.z * r };
        V3 t0 = { n0.x * r,  halfH, n0.z * r }, t1 = { n1.x * r,  halfH, n1.z * r };
        // side (smooth normals)
        uint32_t base = (uint32_t)m.verts.size();
        pushVert(m, b0, n0, col, xf); pushVert(m, t0, n0, col, xf);
        pushVert(m, t1, n1, col, xf); pushVert(m, b1, n1, col, xf);
        m.idx.insert(m.idx.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
        // caps
        addQuad(m, { 0,halfH,0 }, t0, t1, { 0,halfH,0 }, { 0,1,0 }, col, xf);
        addQuad(m, { 0,-halfH,0 }, b1, b0, { 0,-halfH,0 }, { 0,-1,0 }, col, xf);
    }
}

// torus around Y axis (ring lies in XZ plane)
inline void addTorus(MeshData& m, float R, float r, int segMaj, int segMin, Col col, const M4* xf = nullptr) {
    const float TAU = 6.2831853f;
    uint32_t base = (uint32_t)m.verts.size();
    for (int i = 0; i <= segMaj; i++) {
        float th = TAU * i / segMaj;
        float ct = cosf(th), st = sinf(th);
        for (int j = 0; j <= segMin; j++) {
            float ph = TAU * j / segMin;
            float cp = cosf(ph), sp = sinf(ph);
            V3 p = { (R + r * cp) * ct, r * sp, (R + r * cp) * st };
            V3 n = { cp * ct, sp, cp * st };
            pushVert(m, p, n, col, xf);
        }
    }
    int W = segMin + 1;
    for (int i = 0; i < segMaj; i++)
        for (int j = 0; j < segMin; j++) {
            uint32_t a = base + i * W + j, b = base + (i + 1) * W + j;
            m.idx.insert(m.idx.end(), { a, b, b + 1, a, b + 1, a + 1 });
        }
}

// unit sphere
inline void addSphere(MeshData& m, int latSegs, int lonSegs, Col col) {
    const float PI = 3.14159265f, TAU = 6.2831853f;
    uint32_t base = (uint32_t)m.verts.size();
    for (int i = 0; i <= latSegs; i++) {
        float v = PI * i / latSegs;
        float sv = sinf(v), cv = cosf(v);
        for (int j = 0; j <= lonSegs; j++) {
            float u = TAU * j / lonSegs;
            V3 n = { sv * cosf(u), cv, sv * sinf(u) };
            pushVert(m, n, n, col, nullptr);
        }
    }
    int W = lonSegs + 1;
    for (int i = 0; i < latSegs; i++)
        for (int j = 0; j < lonSegs; j++) {
            uint32_t a = base + i * W + j, b = base + (i + 1) * W + j;
            m.idx.insert(m.idx.end(), { a, b, b + 1, a, b + 1, a + 1 });
        }
}

// ---------------------------------------------------------------------------
// The player's ship: a chunky frigate ~120 m long, built from primitives.
inline MeshData buildShipMesh() {
    MeshData m;
    Col hull   = { 0.46f, 0.50f, 0.56f, 0.0f };
    Col dark   = { 0.22f, 0.25f, 0.30f, 0.0f };
    Col stripe = { 1.00f, 0.55f, 0.15f, 0.55f };
    Col engine = { 0.30f, 0.80f, 1.00f, 1.0f };
    Col glass  = { 0.55f, 0.85f, 1.00f, 0.8f };

    addBox(m, { 0, 0, 0.005f }, { 0.012f, 0.0085f, 0.046f }, hull);           // main hull
    addBox(m, { 0, 0.0015f, 0.060f }, { 0.0070f, 0.0050f, 0.016f }, dark);    // nose
    addBox(m, { 0, 0.0045f, 0.043f }, { 0.0042f, 0.0028f, 0.009f }, glass);   // cockpit
    addBox(m, { 0, -0.001f, -0.014f }, { 0.034f, 0.0024f, 0.014f }, dark);    // wing
    addBox(m, { 0, 0.0135f, -0.030f }, { 0.0020f, 0.0085f, 0.013f }, dark);   // dorsal fin
    addBox(m, { 0, 0.0090f, 0.004f }, { 0.0125f, 0.0006f, 0.040f }, stripe);  // top stripe

    // engine nacelles
    for (int s = -1; s <= 1; s += 2) {
        M4 rot = rotationX(1.5707963f); // cylinder Y axis -> Z axis
        M4 xf = mul(rot, translation({ 0.019f * s, -0.0015f, -0.034f }));
        addCylinder(m, 0.0052f, 0.020f, 12, dark, &xf);
        addBox(m, { 0.019f * s, -0.0015f, -0.0545f }, { 0.0040f, 0.0040f, 0.0012f }, engine); // exhaust
    }
    addBox(m, { 0, -0.0005f, -0.0625f }, { 0.0058f, 0.0050f, 0.0014f }, engine); // center exhaust
    return m;
}

// ---------------------------------------------------------------------------
// Station: vertical spindle + habitation ring + arms, ~3 km tall.
inline MeshData buildStationMesh() {
    MeshData m;
    Col body  = { 0.40f, 0.43f, 0.48f, 0.0f };
    Col dark  = { 0.20f, 0.22f, 0.26f, 0.0f };
    Col win   = { 1.00f, 0.85f, 0.55f, 0.85f };
    Col bay   = { 0.20f, 0.95f, 0.70f, 1.0f };
    Col trim  = { 0.85f, 0.35f, 0.10f, 0.45f };

    addCylinder(m, 0.45f, 1.15f, 20, body);                       // central spindle
    addTorus(m, 1.30f, 0.17f, 36, 12, dark);                      // habitation ring
    addBox(m, { 0,0,0 }, { 1.30f, 0.055f, 0.055f }, body);        // arm X
    addBox(m, { 0,0,0 }, { 0.055f, 0.055f, 1.30f }, body);        // arm Z
    addCylinder(m, 0.70f, 0.09f, 20, dark, nullptr);              // mid collar (overlaps spindle)
    { M4 xf = translation({ 0, 1.28f, 0 }); addCylinder(m, 0.62f, 0.14f, 16, body, &xf); } // top cap
    { M4 xf = translation({ 0,-1.28f, 0 }); addCylinder(m, 0.62f, 0.14f, 16, body, &xf); } // bottom cap
    { M4 xf = translation({ 0, 1.75f, 0 }); addCylinder(m, 0.035f, 0.42f, 8, dark, &xf); } // antenna
    addBox(m, { 0, 2.18f, 0 }, { 0.02f, 0.02f, 0.02f }, trim);                              // beacon

    // docking bay protruding on +X with glowing entrance
    addBox(m, { 0.62f, 0, 0 }, { 0.26f, 0.20f, 0.30f }, dark);
    addBox(m, { 0.885f, 0, 0 }, { 0.006f, 0.145f, 0.23f }, bay);

    // window strips on the spindle
    float ys[7] = { -0.95f, -0.62f, -0.30f, 0.05f, 0.38f, 0.70f, 0.98f };
    for (int i = 0; i < 7; i++)
        addBox(m, { 0, ys[i], 0 }, { 0.46f, 0.009f, 0.10f + 0.10f * (i % 3) }, win);
    // windows on the ring (4 patches)
    for (int i = 0; i < 4; i++) {
        float a = 0.785f + 1.5707963f * i;
        addBox(m, { 1.30f * cosf(a), 0.10f, 1.30f * sinf(a) }, { 0.09f, 0.085f, 0.09f }, win);
    }
    addBox(m, { 0, 0.0f, 0 }, { 0.47f, 0.30f, 0.02f }, trim); // hazard trim panel
    return m;
}

// ---------------------------------------------------------------------------
// Stargate: big vertical ring (in XY plane, gate axis = +Z) with pylons.
inline MeshData buildGateMesh() {
    MeshData m;
    Col body = { 0.35f, 0.38f, 0.44f, 0.0f };
    Col dark = { 0.18f, 0.20f, 0.24f, 0.0f };
    Col blue = { 0.35f, 0.70f, 1.00f, 1.0f };

    M4 tilt = rotationX(1.5707963f); // torus ring from XZ plane into XY plane
    addTorus(m, 0.72f, 0.075f, 36, 12, body, &tilt);
    // inner glowing rim
    addTorus(m, 0.60f, 0.022f, 36, 8, blue, &tilt);
    // pylons
    addBox(m, { 0.92f, 0, -0.10f }, { 0.10f, 0.42f, 0.16f }, dark);
    addBox(m, { -0.92f, 0, -0.10f }, { 0.10f, 0.42f, 0.16f }, dark);
    addBox(m, { 0, -0.92f, -0.10f }, { 0.42f, 0.10f, 0.16f }, dark);
    // energy conduits to the ring
    addBox(m, { 0.80f, 0, -0.04f }, { 0.13f, 0.035f, 0.035f }, blue);
    addBox(m, { -0.80f, 0, -0.04f }, { 0.13f, 0.035f, 0.035f }, blue);
    addBox(m, { 0, -0.80f, -0.04f }, { 0.035f, 0.13f, 0.035f }, blue);
    return m;
}

// rough asteroid: a squashed sphere; per-instance nonuniform scale supplies variety
inline MeshData buildRockMesh() {
    MeshData m;
    addSphere(m, 7, 10, { 0.38f, 0.33f, 0.28f, 0.0f });
    return m;
}

inline MeshData buildPlanetMesh() {
    MeshData m;
    addSphere(m, 28, 44, { 1, 1, 1, 0 });
    return m;
}
