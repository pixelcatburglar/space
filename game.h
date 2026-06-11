// game.h - procedural universe generation and game state. Units: km, seconds.
// buildUniverse() is deterministic (fixed seed) and shared by client AND server,
// so both sides agree on system names, security, and gate topology.
#pragma once
#include <string>
#include <vector>
#include <algorithm>
#include <cwctype>
#include "math3d.h"
#include "mesh.h"

enum class EType { Sun, Planet, Station, Gate, Belt };

struct Entity {
    EType type;
    std::wstring name;
    V3 pos;
    float radius = 1;       // physical/visual radius (km)
    Col colA, colB;         // planet surface palette
    V3 dir = { 1,0,0 };     // station undock direction / gate facing
    int linkSystem = -1;    // gate: destination system index
    int linkGate = -1;      // gate: entity index of arrival gate in destination
};

struct StarSystem {
    std::wstring name;
    float security = 0;
    float mapX = 0, mapY = 0;   // star map position
    Col sunColor;
    Col nebulaA, nebulaB;
    std::vector<Entity> ents;
    std::vector<M4> rockXforms; // asteroid world matrices
};

struct Lcg {
    uint32_t s = 0xC0FFEEu;
    float uni() { s = s * 1664525u + 1013904223u; return (s >> 8) / 16777216.0f; }
    int ri(int n) { return n > 0 ? (int)(uni() * n) % n : 0; }
    float range(float a, float b) { return a + uni() * (b - a); }
};

inline std::wstring genSysName(Lcg& r, bool nullsec) {
    if (nullsec) {
        const wchar_t* cs = L"ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
        std::wstring s;
        int n1 = 2 + r.ri(2), n2 = 2 + r.ri(2);
        for (int i = 0; i < n1; i++) s += cs[r.ri(32)];
        s += L'-';
        for (int i = 0; i < n2; i++) s += cs[r.ri(32)];
        return s;
    }
    const wchar_t* syl[20] = { L"au",L"ve",L"ka",L"mi",L"or",L"tha",L"lu",L"se",L"no",L"za",
                               L"cal",L"dor",L"hel",L"ish",L"ran",L"tov",L"eya",L"bri",L"mok",L"sul" };
    std::wstring s;
    int n = 2 + r.ri(2);
    for (int i = 0; i < n; i++) s += syl[r.ri(20)];
    s[0] = (wchar_t)towupper(s[0]);
    return s;
}

inline std::vector<StarSystem> buildUniverse() {
    const int NSYS = 24;
    Lcg rng;

    // --- map layout: jittered golden spiral, sorted center-out
    struct Pt { float x, y, r; };
    std::vector<Pt> pts;
    for (int i = 0; i < NSYS; i++) {
        float r = sqrtf((i + 0.7f) / NSYS) * rng.range(0.90f, 1.12f);
        float a = i * 2.39996f + rng.range(-0.3f, 0.3f);
        pts.push_back({ r * cosf(a), r * sinf(a), r });
    }
    std::sort(pts.begin(), pts.end(), [](const Pt& a, const Pt& b) { return a.r < b.r; });

    std::vector<StarSystem> u(NSYS);
    for (int i = 0; i < NSYS; i++) {
        StarSystem& s = u[i];
        float sec = 0.95f - pts[i].r * 1.30f;
        sec = floorf(sec * 10.0f + 0.5f) / 10.0f;
        s.security = sec < 0.05f ? 0.0f : (sec > 0.9f ? 0.9f : sec);
        s.mapX = pts[i].x; s.mapY = pts[i].y;
        s.name = i == 0 ? L"Aurelia" : genSysName(rng, s.security <= 0.0f);
    }
    u[0].security = 0.9f;
    for (int i = 1; i < NSYS; i++)                        // keep the old neighbor's name alive
        if (fabsf(u[i].security - 0.3f) < 0.01f) { u[i].name = L"Vehl-Tarra"; break; }

    // --- palettes
    const Col suns[5] = { {0.75f,0.85f,1.00f},{1.00f,0.95f,0.82f},{1.00f,0.85f,0.60f},{1.00f,0.70f,0.40f},{1.00f,0.45f,0.28f} };
    const Col nebHi[3][2] = { {{0.05f,0.18f,0.24f},{0.16f,0.10f,0.30f}},
                              {{0.06f,0.12f,0.28f},{0.05f,0.22f,0.26f}},
                              {{0.08f,0.16f,0.20f},{0.20f,0.14f,0.28f}} };
    const Col nebLo[2][2] = { {{0.22f,0.14f,0.06f},{0.18f,0.08f,0.24f}},
                              {{0.16f,0.10f,0.08f},{0.10f,0.14f,0.26f}} };
    const Col nebNull[3][2] = { {{0.26f,0.06f,0.08f},{0.22f,0.10f,0.28f}},
                                {{0.08f,0.09f,0.14f},{0.24f,0.08f,0.10f}},
                                {{0.14f,0.05f,0.16f},{0.06f,0.10f,0.12f}} };
    const Col planets[6][2] = {
        {{0.55f,0.42f,0.30f},{0.72f,0.62f,0.48f}}, // rocky
        {{0.08f,0.22f,0.45f},{0.30f,0.55f,0.60f}}, // ocean
        {{0.75f,0.50f,0.25f},{0.50f,0.28f,0.15f}}, // gas
        {{0.70f,0.80f,0.90f},{0.45f,0.60f,0.78f}}, // ice
        {{0.45f,0.12f,0.05f},{0.95f,0.45f,0.10f}}, // lava
        {{0.35f,0.45f,0.18f},{0.60f,0.65f,0.30f}}, // toxic
    };
    const wchar_t* rom[6] = { L"I",L"II",L"III",L"IV",L"V",L"VI" };
    const wchar_t* corps[5] = { L"Core Dynamics",L"Lai Dai",L"Quafe",L"Thukker Mix",L"Spacelane Patrol" };
    const wchar_t* stypes[5] = { L"Assembly Plant",L"Refinery",L"Trade Hub",L"Logistics Depot",L"Research Outpost" };

    // --- per-system contents
    for (int i = 0; i < NSYS; i++) {
        StarSystem& s = u[i];
        s.sunColor = suns[rng.ri(5)];
        if (s.security >= 0.5f) { const Col* n = nebHi[rng.ri(3)]; s.nebulaA = n[0]; s.nebulaB = n[1]; }
        else if (s.security > 0.01f) { const Col* n = nebLo[rng.ri(2)]; s.nebulaA = n[0]; s.nebulaB = n[1]; }
        else { const Col* n = nebNull[rng.ri(3)]; s.nebulaA = n[0]; s.nebulaB = n[1]; }

        s.ents.push_back({ EType::Sun, s.name + L" - Star", {0,0,0}, rng.range(260.0f, 420.0f) });

        int np = 1 + rng.ri(4);
        for (int p = 0; p < np; p++) {
            int t = rng.ri(6);
            float ang = rng.range(0.0f, 6.2831853f);
            float dist = 1700.0f + 1350.0f * p + rng.range(0.0f, 900.0f);
            Entity pl{ EType::Planet, s.name + L" " + rom[p],
                       { cosf(ang) * dist, rng.range(-220.0f, 220.0f), sinf(ang) * dist },
                       rng.range(52.0f, 180.0f), planets[t][0], planets[t][1] };
            s.ents.push_back(pl);
        }

        float stationChance = s.security >= 0.5f ? 1.0f : (s.security > 0.01f ? 0.7f : 0.10f);
        if ((i == 0 || rng.uni() < stationChance) && np > 0) {
            const Entity& pl = s.ents[1 + rng.ri(np)];
            float oa = rng.range(0.0f, 6.2831853f);
            V3 off = { cosf(oa), 0.12f, sinf(oa) };
            Entity st{ EType::Station,
                       pl.name + L" - " + corps[rng.ri(5)] + std::wstring(L" ") + stypes[rng.ri(5)],
                       pl.pos + norm(off) * (pl.radius + rng.range(90.0f, 170.0f)), 2.4f };
            float da = rng.range(0.0f, 6.2831853f);
            st.dir = norm(V3{ cosf(da), rng.range(0.0f, 0.15f), sinf(da) });
            s.ents.push_back(st);
        }

        if (np > 0 && rng.uni() < 0.55f) {
            const Entity& pl = s.ents[1 + rng.ri(np)];
            float ba = rng.range(0.0f, 6.2831853f);
            V3 bc = pl.pos + V3{ cosf(ba), 0.05f, sinf(ba) } *(pl.radius + rng.range(25.0f, 50.0f));
            s.ents.push_back({ EType::Belt, L"Asteroid Belt " + std::wstring(rom[rng.ri(3)]) + L"-A", bc, 6.0f });
            int nr = 10 + rng.ri(8);
            for (int k = 0; k < nr; k++) {
                float a = 6.2831853f * k / nr + rng.range(0.0f, 0.6f);
                float r = 3.5f + rng.range(0.0f, 3.5f);
                V3 p = bc + V3{ cosf(a) * r, rng.range(-1.4f, 1.4f), sinf(a) * r };
                float sx = 0.10f + rng.range(0.0f, 0.30f);
                M4 w = mul(scaling3({ sx, sx * rng.range(0.6f, 1.1f), sx * rng.range(0.7f, 1.3f) }),
                           mul(rotationY(rng.range(0.0f, 6.28f)), translation(p)));
                s.rockXforms.push_back(w);
            }
        }
    }

    // --- gate graph: MST over map positions + a few shortcut edges
    auto mapDist = [&](int a, int b) {
        float dx = u[a].mapX - u[b].mapX, dy = u[a].mapY - u[b].mapY;
        return sqrtf(dx * dx + dy * dy);
    };
    std::vector<std::pair<int, int>> edges;
    std::vector<bool> inTree(NSYS, false);
    inTree[0] = true;
    for (int k = 1; k < NSYS; k++) {
        float best = 1e9f; int ba = -1, bb = -1;
        for (int a = 0; a < NSYS; a++) if (inTree[a])
            for (int b = 0; b < NSYS; b++) if (!inTree[b]) {
                float d = mapDist(a, b);
                if (d < best) { best = d; ba = a; bb = b; }
            }
        edges.push_back({ ba, bb });
        inTree[bb] = true;
    }
    auto adjacent = [&](int a, int b) {
        for (auto& e : edges)
            if ((e.first == a && e.second == b) || (e.first == b && e.second == a)) return true;
        return false;
    };
    auto degree = [&](int a) {
        int d = 0;
        for (auto& e : edges) if (e.first == a || e.second == a) d++;
        return d;
    };
    int extra = 0;
    for (int a = 0; a < NSYS && extra < 8; a++) {
        if (degree(a) >= 4) continue;
        float best = 1e9f; int bb = -1;
        for (int b = 0; b < NSYS; b++)
            if (b != a && !adjacent(a, b) && degree(b) < 4) {
                float d = mapDist(a, b);
                if (d < best) { best = d; bb = b; }
            }
        if (bb >= 0 && best < 0.40f) { edges.push_back({ a, bb }); extra++; }
    }

    // --- physical gates, oriented toward the neighbor on the map
    struct GateRef { int sa, ea, sb, eb; };
    std::vector<GateRef> refs;
    for (auto& e : edges) {
        int a = e.first, b = e.second;
        float dx = u[b].mapX - u[a].mapX, dy = u[b].mapY - u[a].mapY;
        float dl = sqrtf(dx * dx + dy * dy);
        dx /= dl; dy /= dl;
        auto makeGate = [&](int from, int to, float ox, float oy) {
            V3 d3 = { ox, 0, oy };
            Entity g{ EType::Gate, L"Stargate (" + u[to].name + L")",
                      d3 * rng.range(5500.0f, 8500.0f) + V3{ 0, rng.range(-280.0f, 280.0f), 0 }, 1.6f };
            g.dir = d3 * -1.0f; // faces back into the system
            g.linkSystem = to;
            u[from].ents.push_back(g);
            return (int)u[from].ents.size() - 1;
        };
        int ea = makeGate(a, b, dx, dy);
        int eb = makeGate(b, a, -dx, -dy);
        refs.push_back({ a, ea, b, eb });
    }
    for (auto& r : refs) {
        u[r.sa].ents[r.ea].linkGate = r.eb;
        u[r.sb].ents[r.eb].linkGate = r.ea;
    }
    return u;
}

// ----------------------------------------------------------------- game state
enum class GState { Docked, Undocking, Flying, Aligning, Warping, Jumping };

struct Ship {
    V3 pos;
    M4 rot = M4::identity();
    float speed = 0;       // km/s along fwd
    float throttle = 0;    // 0..1
    static constexpr float maxSpeed = 0.25f;  // 250 m/s
    static constexpr float turnRate = 1.0f;   // rad/s
};

struct Game {
    std::vector<StarSystem> universe = buildUniverse();
    int sysIdx = 0;
    GState state = GState::Docked;
    Ship ship;
    int dockedAt = -1;       // entity index of station we're docked at

    // warp
    V3 warpDir, warpDropPos;
    float warpDist = 0, warpX = 0, warpVmax = 0, warpVis = 0;
    std::wstring warpTargetName;

    // timers
    float stateT = 0;        // time in current state
    float whiteout = 0;
    int jumpToSystem = -1, jumpToGate = -1;

    std::wstring msg;
    float msgT = 0;

    StarSystem& sys() { return universe[sysIdx]; }
    void say(const std::wstring& s, float dur = 3.5f) { msg = s; msgT = dur; }
};
