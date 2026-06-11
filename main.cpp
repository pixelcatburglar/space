// main.cpp - a very small EVE. Dock, undock, fly, warp, jump, and now: multiplayer.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>
#include <cwchar>
#include "math3d.h"
#include "mesh.h"
#include "shaders.h"
#include "game.h"
#include "renderer.h"
#include "net.h"
#include "audio.h"

static Renderer R;
static Game G;
static Net N;
static Audio A;
static std::vector<WPARAM> g_keys;     // discrete key presses from WndProc
static std::vector<wchar_t> g_chars;   // typed characters from WM_CHAR
static bool g_quit = false;
static bool g_showHelp = true;
static bool g_shotRequest = false;
static int g_shotCounter = 0;

// selection: 0 = none, 1 = entity (id = index in sys ents), 2 = player (id = net id)
static int g_selKind = 0, g_selId = -1;
struct OvRow { int kind; int id; };
static std::vector<OvRow> g_rows;

// login UI
static bool g_loginUI = true;
static int g_loginField = 0; // 0 user, 1 pass, 2 host
static std::wstring g_user, g_pass, g_host = L"127.0.0.1";
static std::wstring g_loginStatus = L"";
static bool g_loginPending = false;

// chat
static bool g_chatting = false;
static std::wstring g_chatInput;

// star map
static bool g_mapOpen = false;
static int g_mouseX = 0, g_mouseY = 0;
static bool g_click = false;
static int g_dest = -1;             // route destination system
static std::vector<int> g_route;    // BFS path, current system first

// combat
static float g_fireCD = 0;
static float g_stateTimer = 0;

// camera
static V3 g_camEye;
static M4 g_view, g_proj, g_viewProj;
static float g_fovY = 1.22f; // ~70 deg

// auto-test mode (command line)
static std::wstring g_autoShot, g_autoUser, g_autoChatMsg;
static bool g_autoSpace = false, g_autoWarp = false, g_autoGate = false, g_autoJump = false, g_autoFire = false;
static bool g_autoSoak = false;
static std::vector<std::pair<int, int>> g_jumpList; // (system, gate entity index) for every gate
static size_t g_soakIdx = 0;
static float g_autoSpawnOff = 0;
static int g_autoFrames = 0, g_autoChatFrame = 0, g_frame = 0;
static float g_autoRunSec = 0;     // wall-clock test duration (overrides frames)
static double g_lastAutoFire = 0;

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_KEYDOWN:
        if (!(l & (1 << 30))) g_keys.push_back(w); // ignore autorepeat
        return 0;
    case WM_SYSKEYDOWN:
        if (w == VK_F10) { g_keys.push_back(w); return 0; } // F10 = star map, not the menu
        break;
    case WM_CHAR:
        g_chars.push_back((wchar_t)w);
        return 0;
    case WM_MOUSEMOVE:
        g_mouseX = (short)LOWORD(l); g_mouseY = (short)HIWORD(l);
        return 0;
    case WM_LBUTTONDOWN:
        g_click = true;
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(h, m, w, l);
}

// ---------------------------------------------------------------- formatting
static std::wstring fmtInt(long long v) {
    wchar_t raw[32];
    swprintf(raw, 32, L"%lld", v);
    std::wstring s(raw), out;
    int n = (int)s.size();
    for (int i = 0; i < n; i++) {
        out += s[i];
        int rem = n - 1 - i;
        if (rem > 0 && rem % 3 == 0) out += L',';
    }
    return out;
}
static std::wstring fmtDist(float km) {
    if (km < 1.0f) return fmtInt((long long)(km * 1000)) + L" m";
    if (km < 10.0f) { wchar_t b[32]; swprintf(b, 32, L"%.1f km", km); return b; }
    return fmtInt((long long)km) + L" km";
}
static const wchar_t* typeName(EType t) {
    switch (t) {
    case EType::Sun: return L"STAR";
    case EType::Planet: return L"PLANET";
    case EType::Station: return L"STATION";
    case EType::Gate: return L"STARGATE";
    case EType::Belt: return L"BELT";
    }
    return L"?";
}

// ------------------------------------------------------------------ helpers
static int nearestOfType(EType t, float maxDist) {
    int best = -1; float bd = maxDist;
    for (int i = 0; i < (int)G.sys().ents.size(); i++) {
        const Entity& e = G.sys().ents[i];
        if (e.type != t) continue;
        float d = len(e.pos - G.ship.pos) - e.radius;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

static void buildOverview() {
    g_rows.clear();
    for (int i = 0; i < (int)G.sys().ents.size(); i++)
        g_rows.push_back({ 1, i });
    for (const RemotePlayer& p : N.players)
        if (p.sys == G.sysIdx && !p.docked && p.hasState)
            g_rows.push_back({ 2, p.id });
    // drop selection if it vanished
    if (g_selKind == 2) {
        RemotePlayer* p = N.find(g_selId);
        if (!p || p->sys != G.sysIdx || p->docked) { g_selKind = 0; g_selId = -1; }
    }
}
static bool rowSelected(const OvRow& r) { return r.kind == g_selKind && r.id == g_selId; }
static bool selectedPos(V3& out, float& radius, std::wstring& name) {
    if (g_selKind == 1 && g_selId >= 0 && g_selId < (int)G.sys().ents.size()) {
        const Entity& e = G.sys().ents[g_selId];
        out = e.pos; radius = e.radius; name = e.name;
        return true;
    }
    if (g_selKind == 2) {
        RemotePlayer* p = N.find(g_selId);
        if (p && p->sys == G.sysIdx && p->hasState && !p->docked) {
            out = p->pos; radius = 0.1f; name = p->name;
            return true;
        }
    }
    return false;
}

static void startWarpTo(V3 targetPos, float targetRadius, const std::wstring& name) {
    V3 dir = norm(targetPos - G.ship.pos);
    V3 drop = targetPos - dir * (targetRadius + 7.0f);
    float dist = len(drop - G.ship.pos);
    if (dist < 15.0f) { G.say(L"Target is too close for warp"); return; }
    G.warpDir = norm(drop - G.ship.pos);
    G.warpDropPos = drop;
    G.warpDist = dist;
    G.warpX = 0;
    G.warpVmax = dist / 8.0f < 60.0f ? 60.0f : dist / 8.0f;
    G.warpTargetName = name;
    G.state = GState::Aligning;
    G.stateT = 0;
    G.say(L"Aligning to " + name, 2.5f);
    A.whoosh();
}
static void startWarp(int entIdx) {
    const Entity& e = G.sys().ents[entIdx];
    g_selKind = 1; g_selId = entIdx;
    startWarpTo(e.pos, e.radius, e.name);
}

static void doUndock() {
    const Entity& st = G.sys().ents[G.dockedAt];
    G.ship.pos = st.pos + st.dir * (st.radius + 1.2f);
    G.ship.rot = basisFromFwd(st.dir);
    G.ship.speed = 0.12f;
    G.ship.throttle = 0.5f;
    G.state = GState::Undocking;
    G.stateT = 0;
    g_selKind = 0; g_selId = -1;
    G.say(L"Undock sequence in progress", 2.5f);
    A.thunk();
}

static void doJump(int gateIdx) {
    G.state = GState::Jumping;
    G.stateT = 0;
    G.jumpToSystem = G.sys().ents[gateIdx].linkSystem;
    G.jumpToGate = G.sys().ents[gateIdx].linkGate;
    G.say(L"Jumping...", 2.0f);
    A.zap();
    A.whoosh();
}

static void dockAt(int stationIdx, const std::wstring& reason) {
    G.state = GState::Docked;
    G.dockedAt = stationIdx;
    G.ship.speed = 0; G.ship.throttle = 0;
    g_selKind = 0; g_selId = -1;
    if (!reason.empty()) G.say(reason, 3.5f);
    A.thunk();
}

// map position -> screen position (used by both rendering and click hit-testing)
static void mapPoint(int i, float& sx, float& sy) {
    float mnx = 1e9f, mny = 1e9f, mxx = -1e9f, mxy = -1e9f;
    for (const StarSystem& s : G.universe) {
        mnx = mnx < s.mapX ? mnx : s.mapX; mxx = mxx > s.mapX ? mxx : s.mapX;
        mny = mny < s.mapY ? mny : s.mapY; mxy = mxy > s.mapY ? mxy : s.mapY;
    }
    float W = (float)R.width, H = (float)R.height;
    float sx_ = (W - 320.0f) / (mxx - mnx + 0.001f);
    float sy_ = (H - 280.0f) / (mxy - mny + 0.001f);
    float sc = sx_ < sy_ ? sx_ : sy_;
    sx = W * 0.5f + (G.universe[i].mapX - (mnx + mxx) * 0.5f) * sc;
    sy = H * 0.5f - 10.0f + (G.universe[i].mapY - (mny + mxy) * 0.5f) * sc;
}

static void computeRoute() {
    g_route.clear();
    if (g_dest < 0 || g_dest == G.sysIdx) { g_dest = -1; return; }
    int n = (int)G.universe.size();
    std::vector<int> parent(n, -2);
    std::vector<int> q;
    parent[G.sysIdx] = -1;
    q.push_back(G.sysIdx);
    for (size_t h = 0; h < q.size(); h++) {
        int cur = q[h];
        if (cur == g_dest) break;
        for (const Entity& e : G.universe[cur].ents)
            if (e.type == EType::Gate && parent[e.linkSystem] == -2) {
                parent[e.linkSystem] = cur;
                q.push_back(e.linkSystem);
            }
    }
    if (parent[g_dest] == -2) { g_dest = -1; return; }
    for (int at = g_dest; at != -1; at = parent[at]) g_route.insert(g_route.begin(), at);
}

static void handleCommand(const std::wstring& cmd) {
    if (cmd.rfind(L"/claim", 0) == 0) {
        std::wstring name = cmd.size() > 7 ? cmd.substr(7) : L"";
        while (!name.empty() && name.front() == L' ') name.erase(name.begin());
        while (!name.empty() && name.back() == L' ') name.pop_back();
        if (name.empty()) { N.addChat(L"(SOV) usage: /claim <AllianceName>"); return; }
        if (name.size() > 24) { N.addChat(L"(SOV) alliance name too long (24 max)"); return; }
        if (G.sys().security > 0.01f) {
            N.addChat(L"(SOV) sovereignty can only be claimed in 0.0 systems — check the star map [N]");
            return;
        }
        if (N.loggedIn) N.sendLine("CLAIM|" + wToUtf8(name));
        else {
            N.sov[G.sysIdx] = name;
            N.addChat(L"☼ " + name + L" has claimed sovereignty of " + G.sys().name + L" (offline)");
        }
        return;
    }
    if (cmd.rfind(L"/help", 0) == 0) {
        N.addChat(L"commands: /claim <Alliance> — claim the 0.0 system you are in");
        return;
    }
    N.addChat(L"(unknown command — try /help)");
}

static void sendChat(const std::wstring& msg) {
    if (msg.empty()) return;
    if (msg[0] == L'/') { handleCommand(msg); return; }
    if (N.loggedIn) N.sendLine("CHAT|" + wToUtf8(msg));
    else N.addChat(L"(offline) " + msg);
}

static void fireSelected() {
    if (g_fireCD > 0) return;
    if (g_selKind != 2) { G.say(L"Select a pilot to engage (overview)"); return; }
    RemotePlayer* p = N.find(g_selId);
    if (!p || p->sys != G.sysIdx || p->docked || !p->hasState) return;
    float d = len(p->pos - G.ship.pos);
    if (d > 8.0f) { G.say(L"Target out of weapon range (8 km)"); return; }
    if (G.state != GState::Flying) return;
    N.sendLine("FIRE|" + std::to_string(p->id));
    g_fireCD = 1.0f;
}

static bool keyHeld(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

static void submitLogin() {
    if (g_user.empty()) { g_loginStatus = L"enter a pilot name"; return; }
    g_loginStatus = L"connecting to " + g_host + L"...";
    if (!N.connectTo(g_host, 5577)) {
        g_loginStatus = N.lastError;
        return;
    }
    N.login(g_user, g_pass);
    g_loginPending = true;
    g_loginStatus = L"authenticating...";
}

// ------------------------------------------------------------------- update
static void updateTyping() {
    for (wchar_t c : g_chars) {
        if (g_loginUI) {
            std::wstring* f = g_loginField == 0 ? &g_user : g_loginField == 1 ? &g_pass : &g_host;
            if (c == L'\r') submitLogin();
            else if (c == L'\t') g_loginField = (g_loginField + 1) % 3;
            else if (c == 27) { g_loginUI = false; N.disconnect(); G.say(L"Playing offline. Restart to log in.", 4.0f); }
            else if (c == 8) { if (!f->empty()) f->pop_back(); }
            else if (c >= 32 && f->size() < 32) *f += c;
        } else if (g_chatting) {
            if (c == L'\r') { sendChat(g_chatInput); g_chatInput.clear(); g_chatting = false; }
            else if (c == 27) { g_chatInput.clear(); g_chatting = false; }
            else if (c == 8) { if (!g_chatInput.empty()) g_chatInput.pop_back(); }
            else if (c >= 32 && g_chatInput.size() < 100) g_chatInput += c;
        } else {
            if (c == L'\r') g_chatting = true;
        }
    }
    g_chars.clear();
}

static void update(float dt, bool focused) {
    G.stateT += dt;
    if (G.msgT > 0) G.msgT -= dt;
    if (g_fireCD > 0) g_fireCD -= dt;

    updateTyping();

    // network housekeeping
    N.poll();
    N.tickFx(dt);
    // sound cues from the server (positional ones attenuate with distance)
    for (const NetSfx& s : N.sfx) {
        float vol = 1.0f;
        if (s.positional) {
            if (s.sys != G.sysIdx) continue;
            float d = len(s.pos - G.ship.pos);
            float range = s.type == 1 ? 30.0f : 14.0f;
            vol = clampf(1.0f - d / range, 0.1f, 1.0f);
        }
        if (s.type == 0) A.pew(vol);
        else if (s.type == 1) A.boom(vol);
        else if (s.type == 2) A.blip();
    }
    N.sfx.clear();
    if (g_loginPending && N.loggedIn) { g_loginPending = false; g_loginUI = false; G.say(L"Welcome, " + g_user, 3.0f); A.blip(); }
    if (g_loginPending && !N.lastError.empty()) { g_loginPending = false; g_loginStatus = N.lastError; }
    if (N.killedMe) {
        N.killedMe = false;
        int si = -1;
        for (int i = 0; i < (int)G.sys().ents.size(); i++)
            if (G.sys().ents[i].type == EType::Station) { si = i; break; }
        if (si < 0) { // no station here (deep null) — towed home
            G.sysIdx = 0;
            for (int i = 0; i < (int)G.sys().ents.size(); i++)
                if (G.sys().ents[i].type == EType::Station) { si = i; break; }
        }
        G.warpVis = 0;
        dockAt(si, L"Your ship was destroyed by " + N.killedBy + L". Wreck towed to station.");
    }
    // dead-reckon remote ships
    for (RemotePlayer& p : N.players)
        if (p.hasState && !p.docked)
            p.pos = p.pos + p.fwd * p.speed * dt;

    Ship& sh = G.ship;
    V3 fwd = sh.rot.fwd(), up = sh.rot.up(), right = sh.rot.right();

    // discrete keys (suppressed while typing)
    bool typing = g_loginUI || g_chatting;
    for (WPARAM k : g_keys) {
        if (k == VK_F12) g_shotRequest = true;
        if (typing) continue;
        if (k == VK_ESCAPE) { if (g_mapOpen) g_mapOpen = false; else g_quit = true; }
        if (k == 'N' || k == VK_F10) { g_mapOpen = !g_mapOpen; A.blip(); }
        if (k == 'H') g_showHelp = !g_showHelp;
        if (k == 'M') { A.muted = !A.muted; G.say(A.muted ? L"Audio muted" : L"Audio on", 1.5f); }

        if (G.state == GState::Docked) {
            if (k == 'U') doUndock();
        } else if (G.state == GState::Flying) {
            if (k >= '1' && k <= '9') {
                int idx = (int)(k - '1');
                if (idx < (int)g_rows.size()) { g_selKind = g_rows[idx].kind; g_selId = g_rows[idx].id; }
            }
            if (k == VK_TAB && !g_rows.empty()) {
                int cur = -1;
                for (int i = 0; i < (int)g_rows.size(); i++)
                    if (rowSelected(g_rows[i])) { cur = i; break; }
                const OvRow& r = g_rows[(cur + 1) % (int)g_rows.size()];
                g_selKind = r.kind; g_selId = r.id;
            }
            if (k == VK_SPACE) {
                V3 tp; float tr; std::wstring tn;
                if (selectedPos(tp, tr, tn)) startWarpTo(tp, tr, tn);
            }
            if (k == 'F') fireSelected();
            if (k == 'J') {
                int gi = nearestOfType(EType::Gate, 4.0f);
                if (gi >= 0) doJump(gi);
                else G.say(L"No stargate in range");
            }
            if (k == 'G') {
                int si = nearestOfType(EType::Station, 3.0f);
                if (si >= 0) dockAt(si, L"Docking request accepted");
                else G.say(L"No station in docking range");
            }
        } else if (G.state == GState::Aligning) {
            if (k == VK_SPACE) { G.state = GState::Flying; G.say(L"Warp aborted"); }
        }
    }
    g_keys.clear();

    // star map clicks set a route destination
    if (g_click) {
        if (g_mapOpen) {
            int best = -1; float bd = 22.0f;
            for (int i = 0; i < (int)G.universe.size(); i++) {
                float sx, sy;
                mapPoint(i, sx, sy);
                float d = sqrtf((sx - g_mouseX) * (sx - g_mouseX) + (sy - g_mouseY) * (sy - g_mouseY));
                if (d < bd) { bd = d; best = i; }
            }
            if (best >= 0) {
                if (best == G.sysIdx || best == g_dest) { g_dest = -1; g_route.clear(); }
                else { g_dest = best; A.blip(); }
            }
        }
        g_click = false;
    }
    if (g_dest >= 0) {
        computeRoute();
        if (g_dest < 0) G.say(L"Destination reached", 2.5f);
    } else g_route.clear();

    // continuous flight controls
    if (G.state == GState::Flying && focused && !typing) {
        float yaw = 0, pitch = 0, roll = 0;
        if (keyHeld('W')) sh.throttle += 0.6f * dt;
        if (keyHeld('S')) sh.throttle -= 0.6f * dt;
        sh.throttle = clampf(sh.throttle, 0, 1);
        if (keyHeld('D')) yaw += 1;
        if (keyHeld('A')) yaw -= 1;
        if (keyHeld(VK_UP)) pitch -= 1;   // nose up
        if (keyHeld(VK_DOWN)) pitch += 1;
        if (keyHeld('Q')) roll += 1;
        if (keyHeld('E')) roll -= 1;
        if (yaw)   sh.rot = mul(sh.rot, rotationAxis(up, yaw * Ship::turnRate * dt));
        if (pitch) sh.rot = mul(sh.rot, rotationAxis(right, pitch * Ship::turnRate * 0.9f * dt));
        if (roll)  sh.rot = mul(sh.rot, rotationAxis(fwd, roll * Ship::turnRate * 1.4f * dt));
    }

    // re-orthonormalize ship basis
    {
        V3 f = norm(sh.rot.fwd());
        V3 r = norm(cross(sh.rot.up(), f));
        V3 u = cross(f, r);
        sh.rot.setRow(0, r); sh.rot.setRow(1, u); sh.rot.setRow(2, f);
    }
    fwd = sh.rot.fwd();

    float warpTarget = 0;

    switch (G.state) {
    case GState::Docked:
        break;
    case GState::Undocking:
        sh.pos = sh.pos + fwd * sh.speed * dt;
        if (G.stateT > 3.0f) { G.state = GState::Flying; G.say(L"You have undocked. Fly safe.", 3.0f); }
        break;
    case GState::Flying: {
        sh.speed += (sh.throttle * Ship::maxSpeed - sh.speed) * (1 - expf(-1.4f * dt));
        sh.pos = sh.pos + fwd * sh.speed * dt;
        break;
    }
    case GState::Aligning: {
        V3 want = norm(G.warpDropPos - sh.pos);
        float c = clampf(dot(fwd, want), -1, 1);
        float ang = acosf(c);
        if (ang > 0.001f) {
            V3 axis = cross(fwd, want);
            if (len(axis) < 1e-5f) axis = sh.rot.up(); else axis = norm(axis);
            float step = Ship::turnRate * 1.2f * dt;
            if (step > ang) step = ang;
            sh.rot = mul(sh.rot, rotationAxis(axis, step));
        }
        sh.speed += (Ship::maxSpeed * 0.75f - sh.speed) * (1 - expf(-1.2f * dt));
        sh.pos = sh.pos + sh.rot.fwd() * sh.speed * dt;
        if (ang < 0.03f) {
            G.warpDir = norm(G.warpDropPos - sh.pos);
            G.warpDist = len(G.warpDropPos - sh.pos);
            G.warpX = 0;
            G.state = GState::Warping;
            G.stateT = 0;
            G.say(L"Warp drive active", 2.5f);
            A.whoosh();
        }
        break;
    }
    case GState::Warping: {
        float D = G.warpDist;
        float edge = 0.15f * D;
        float v = G.warpVmax * clampf((G.warpX < D - G.warpX ? G.warpX : D - G.warpX) / edge, 0.02f, 1.0f);
        G.warpX += v * dt;
        sh.speed = v;
        if (G.warpX >= D) {
            sh.pos = G.warpDropPos;
            sh.speed = Ship::maxSpeed * 0.4f;
            sh.throttle = 0.4f;
            G.state = GState::Flying;
            G.say(L"Warp drive disengaged", 2.5f);
        } else {
            sh.pos = G.warpDropPos - G.warpDir * (D - G.warpX);
            warpTarget = clampf(v / G.warpVmax, 0, 1);
        }
        break;
    }
    case GState::Jumping: {
        float t = G.stateT;
        warpTarget = clampf(t * 1.2f, 0, 1);
        float prev = t - dt;
        G.whiteout = expf(-(t - 1.5f) * (t - 1.5f) * 9.0f) * 1.3f;
        if (prev < 1.5f && t >= 1.5f) {
            G.sysIdx = G.jumpToSystem;
            const Entity& g = G.sys().ents[G.jumpToGate];
            sh.pos = g.pos + g.dir * (g.radius + 1.5f);
            sh.rot = basisFromFwd(g.dir);
            g_selKind = 0; g_selId = -1;
        }
        if (t > 3.0f) {
            G.state = GState::Flying;
            sh.speed = 0.15f; sh.throttle = 0.6f;
            G.whiteout = 0;
            G.say(L"Welcome to " + G.sys().name, 4.0f);
        }
        break;
    }
    }

    if (G.state != GState::Jumping) G.whiteout = G.whiteout > dt * 2 ? G.whiteout - dt * 2 : 0;
    G.warpVis += (warpTarget - G.warpVis) * (1 - expf(-3.5f * dt));

    // rebuild AFTER the state machine: a gate jump swaps sysIdx above, and the
    // overview must never carry entity indices from the previous system into
    // the HUD (that was a use-after-swap crash when arriving in small systems)
    buildOverview();

    // send our state to the server ~12 Hz
    if (N.loggedIn) {
        g_stateTimer -= dt;
        if (g_stateTimer <= 0) {
            g_stateTimer = 0.08f;
            V3 f = sh.rot.fwd(), u = sh.rot.up();
            char buf[256];
            snprintf(buf, sizeof(buf), "ST|%d|%.3f|%.3f|%.3f|%.3f|%.3f|%.3f|%.3f|%.3f|%.3f|%.3f|%d",
                G.sysIdx, sh.pos.x, sh.pos.y, sh.pos.z, f.x, f.y, f.z, u.x, u.y, u.z,
                sh.speed, G.state == GState::Docked ? 1 : 0);
            N.sendLine(buf);
        }
    }

    // chase camera
    {
        V3 f = sh.rot.fwd(), u = sh.rot.up();
        float back = 0.32f + 0.30f * G.warpVis;
        V3 desired = sh.pos - f * back + u * 0.105f;
        float a = 1 - expf(-6.0f * dt);
        g_camEye = lerp(g_camEye, desired, a);
        V3 at = sh.pos + f * 0.6f;
        g_view = lookAtLH(g_camEye, at, u);
        float aspect = (float)R.width / R.height;
        g_proj = perspectiveFovLH(g_fovY, aspect, 0.008f, 60000.0f);
        g_viewProj = mul(g_view, g_proj);
    }
}

// ------------------------------------------------------------------- render
static GpuMesh g_shipMesh, g_stationMesh, g_gateMesh, g_planetMesh, g_rockMesh;

static void fillCommonCB(float time) {
    StarSystem& s = G.sys();
    float aspect = (float)R.width / R.height;
    float tanY = tanf(g_fovY * 0.5f), tanX = tanY * aspect;
    V3 cr = { g_view.m[0][0], g_view.m[1][0], g_view.m[2][0] };
    V3 cu = { g_view.m[0][1], g_view.m[1][1], g_view.m[2][1] };
    V3 cf = { g_view.m[0][2], g_view.m[1][2], g_view.m[2][2] };
    CB& c = R.cb;
    c.viewProj = g_viewProj;
    c.camPos[0] = g_camEye.x; c.camPos[1] = g_camEye.y; c.camPos[2] = g_camEye.z; c.camPos[3] = time;
    c.camRight[0] = cr.x; c.camRight[1] = cr.y; c.camRight[2] = cr.z; c.camRight[3] = tanX;
    c.camUp[0] = cu.x; c.camUp[1] = cu.y; c.camUp[2] = cu.z; c.camUp[3] = tanY;
    c.camFwd[0] = cf.x; c.camFwd[1] = cf.y; c.camFwd[2] = cf.z; c.camFwd[3] = G.warpVis;
    const Entity& sun = s.ents[0];
    c.sunPos[0] = sun.pos.x; c.sunPos[1] = sun.pos.y; c.sunPos[2] = sun.pos.z; c.sunPos[3] = sun.radius;
    c.sunColor[0] = s.sunColor.r; c.sunColor[1] = s.sunColor.g; c.sunColor[2] = s.sunColor.b; c.sunColor[3] = 1;
    c.nebulaA[0] = s.nebulaA.r; c.nebulaA[1] = s.nebulaA.g; c.nebulaA[2] = s.nebulaA.b; c.nebulaA[3] = 1;
    c.nebulaB[0] = s.nebulaB.r; c.nebulaB[1] = s.nebulaB.g; c.nebulaB[2] = s.nebulaB.b; c.nebulaB[3] = 1;
    c.tint[0] = c.tint[1] = c.tint[2] = c.tint[3] = 1;
    c.colorB[0] = c.colorB[1] = c.colorB[2] = c.colorB[3] = 1;
    c.misc[0] = G.warpVis; c.misc[1] = G.whiteout; c.misc[2] = aspect; c.misc[3] = 0;
}

static bool project(V3 p, float& sx, float& sy) {
    const M4& M = g_viewProj;
    float x = p.x * M.m[0][0] + p.y * M.m[1][0] + p.z * M.m[2][0] + M.m[3][0];
    float y = p.x * M.m[0][1] + p.y * M.m[1][1] + p.z * M.m[2][1] + M.m[3][1];
    float w = p.x * M.m[0][3] + p.y * M.m[1][3] + p.z * M.m[2][3] + M.m[3][3];
    if (w < 0.01f) return false;
    sx = (x / w * 0.5f + 0.5f) * R.width;
    sy = (1.0f - (y / w * 0.5f + 0.5f)) * R.height;
    return true;
}

static void setTint(float r, float g, float b, float a) {
    R.cb.tint[0] = r; R.cb.tint[1] = g; R.cb.tint[2] = b; R.cb.tint[3] = a;
}
static void setColorB(Col c) {
    R.cb.colorB[0] = c.r; R.cb.colorB[1] = c.g; R.cb.colorB[2] = c.b; R.cb.colorB[3] = c.a;
}

static M4 remoteRot(const RemotePlayer& p) {
    V3 f = p.fwd;
    V3 r = norm(cross(p.up, f));
    V3 u = cross(f, r);
    M4 m = M4::identity();
    m.setRow(0, r); m.setRow(1, u); m.setRow(2, f);
    return m;
}

static void renderSpace(float time) {
    StarSystem& s = G.sys();
    R.drawFullscreen(R.psSky, R.dsOff, R.blOpaque);

    V3 cr = { g_view.m[0][0], g_view.m[1][0], g_view.m[2][0] };
    V3 cu = { g_view.m[0][1], g_view.m[1][1], g_view.m[2][1] };

    for (const Entity& e : s.ents) {
        if (e.type == EType::Planet) {
            setTint(e.colA.r, e.colA.g, e.colA.b, 1);
            setColorB(e.colB);
            R.drawMesh(g_planetMesh, mul(scaling(e.radius), translation(e.pos)), R.psPlanet);
        } else if (e.type == EType::Station) {
            setTint(1, 1, 1, 1);
            R.drawMesh(g_stationMesh, mul(scaling(1.4f), translation(e.pos)), R.psLit);
        } else if (e.type == EType::Gate) {
            setTint(1, 1, 1, 1);
            R.drawMesh(g_gateMesh, mul(scaling(1.7f), mul(basisFromFwd(e.dir), translation(e.pos))), R.psLit);
        }
    }
    for (const M4& w : s.rockXforms) {
        setTint(1, 1, 1, 1);
        R.drawMesh(g_rockMesh, w, R.psLit);
    }

    // our ship
    setTint(1, 1, 1, 1);
    R.drawMesh(g_shipMesh, mul(G.ship.rot, translation(G.ship.pos)), R.psLit);

    // remote ships (reddish hull tint so hostiles read as hostiles)
    for (const RemotePlayer& p : N.players) {
        if (p.sys != G.sysIdx || p.docked || !p.hasState) continue;
        setTint(1.0f, 0.72f, 0.66f, 1);
        R.drawMesh(g_shipMesh, mul(remoteRot(p), translation(p.pos)), R.psLit);
    }

    // glows
    for (const Entity& e : s.ents)
        if (e.type == EType::Gate) {
            float pulse = 0.45f + 0.20f * sinf(time * 2.2f);
            R.drawGlow(e.pos, e.radius * 0.62f, cr, cu, 0.35f, 0.70f, 1.0f, pulse);
        }
    {
        V3 f = G.ship.rot.fwd();
        float thr = G.ship.throttle;
        float inten = 0.25f + thr * 0.9f + G.warpVis * 2.0f;
        float size = 0.014f + thr * 0.018f + G.warpVis * 0.05f;
        R.drawGlow(G.ship.pos - f * 0.068f, size, cr, cu, 0.30f, 0.75f, 1.0f, inten);
    }
    for (const RemotePlayer& p : N.players) {
        if (p.sys != G.sysIdx || p.docked || !p.hasState) continue;
        float thr = clampf(p.speed / Ship::maxSpeed, 0, 1);
        R.drawGlow(p.pos - p.fwd * 0.068f, 0.014f + thr * 0.018f, cr, cu, 1.0f, 0.55f, 0.30f, 0.25f + thr * 0.9f);
    }
    // explosions
    for (const BoomFx& b : N.booms) {
        if (b.sys != G.sysIdx) continue;
        float age = 0.8f - b.ttl;
        R.drawGlow(b.pos, 0.04f + age * 0.35f, cr, cu, 1.0f, 0.55f, 0.15f, (b.ttl / 0.8f) * 3.0f);
    }

    if (G.warpVis > 0.003f || G.whiteout > 0.003f)
        R.drawFullscreen(R.psWarp, R.dsOff, R.blAdd);
}

// ---------------------------------------------------------------------- HUD
static const D2D1_COLOR_F AMBER = { 1.0f, 0.72f, 0.25f, 1.0f };
static const D2D1_COLOR_F CYAN = { 0.45f, 0.85f, 1.0f, 1.0f };
static const D2D1_COLOR_F WHITE = { 0.92f, 0.95f, 1.0f, 1.0f };
static const D2D1_COLOR_F DIM = { 0.55f, 0.62f, 0.70f, 1.0f };
static const D2D1_COLOR_F PANEL = { 0.03f, 0.07f, 0.10f, 0.78f };
static const D2D1_COLOR_F RED = { 1.0f, 0.35f, 0.25f, 1.0f };
static const D2D1_COLOR_F GREEN = { 0.30f, 0.90f, 0.50f, 1.0f };

static V3 shipPosOf(int id) {
    if (id == N.myId) return G.ship.pos;
    RemotePlayer* p = N.find(id);
    return p ? p->pos : V3{ 0, 0, 0 };
}
static bool shipInSys(int id) {
    if (id == N.myId) return true;
    RemotePlayer* p = N.find(id);
    return p && p->sys == G.sysIdx && p->hasState;
}

static void hudChat(float baseY) {
    float W = (float)R.width;
    int show = (int)N.chat.size() < 9 ? (int)N.chat.size() : 9;
    float ch = 20.0f;
    float boxH = show * ch + (g_chatting ? 30.0f : 8.0f) + 34.0f;
    float y0 = baseY - boxH;
    R.fillRect(20, y0, 520, boxH, PANEL);
    R.text(N.loggedIn ? L"COMMS — Local" : L"COMMS — offline", 34, y0 + 6, 480, 18, R.fmtSmall, AMBER);
    R.line(30, y0 + 27, 530, y0 + 27, { 1.0f, 0.72f, 0.25f, 0.4f }, 1.0f);
    for (int i = 0; i < show; i++) {
        const std::wstring& ln = N.chat[N.chat.size() - show + i];
        R.text(ln.c_str(), 34, y0 + 32 + i * ch, 492, ch, R.fmtSmall, i == show - 1 ? WHITE : DIM);
    }
    if (g_chatting) {
        float iy = y0 + 32 + show * ch;
        R.fillRect(30, iy, 500, 24, { 0.08f, 0.14f, 0.18f, 0.9f });
        std::wstring s = L"> " + g_chatInput + L"_";
        R.text(s.c_str(), 38, iy + 3, 484, 20, R.fmtSmall, CYAN);
    }
}

static void hudSpace() {
    StarSystem& s = G.sys();
    float W = (float)R.width, H = (float)R.height;
    wchar_t buf[256];

    // system panel
    int local = 1;
    for (const RemotePlayer& p : N.players) if (p.sys == G.sysIdx) local++;
    R.fillRect(20, 20, 360, 106, PANEL);
    R.text(s.name.c_str(), 34, 28, 330, 32, R.fmtHead, AMBER);
    swprintf(buf, 256, L"SECURITY %.1f  ·  LOCAL [%d]", s.security, local);
    R.text(buf, 34, 58, 330, 20, R.fmtSmall, DIM);
    std::wstring sovLine;
    D2D1_COLOR_F sovCol = DIM;
    auto sovIt = N.sov.find(G.sysIdx);
    if (sovIt != N.sov.end()) { sovLine = L"SOV: " + sovIt->second; sovCol = RED; }
    else if (s.security <= 0.0f) sovLine = L"UNCLAIMED NULLSEC — /claim <Alliance>";
    else sovLine = L"CONCORD PROTECTED SPACE";
    R.text(sovLine.c_str(), 34, 78, 330, 20, R.fmtSmall, sovCol);
    std::wstring pilot = N.loggedIn ? (L"PILOT: " + g_user) : L"PILOT: CAPSULEER (offline)";
    R.text(pilot.c_str(), 34, 98, 330, 20, R.fmtSmall, DIM);

    // active route
    if (!g_route.empty() && g_dest >= 0) {
        int jumps = (int)g_route.size() - 1;
        std::wstring nextGate = L"?";
        if (g_route.size() > 1)
            for (const Entity& e : s.ents)
                if (e.type == EType::Gate && e.linkSystem == g_route[1]) { nextGate = e.name; break; }
        swprintf(buf, 256, L"ROUTE: %d jump%s to %s — next: %s",
            jumps, jumps == 1 ? L"" : L"s", G.universe[g_dest].name.c_str(), nextGate.c_str());
        R.fillRect(20, 132, 520, 26, PANEL);
        R.text(buf, 34, 136, 500, 20, R.fmtSmall, CYAN);
    }

    // overview (entities + pilots)
    float ox = W - 400, oy = 80, ow = 380;
    int rows = (int)g_rows.size();
    R.fillRect(ox, oy, ow, 36.0f + rows * 26.0f + 10, PANEL);
    R.text(L"OVERVIEW", ox + 14, oy + 8, ow - 28, 22, R.fmtBody, AMBER);
    R.line(ox + 10, oy + 33, ox + ow - 10, oy + 33, { 1.0f, 0.72f, 0.25f, 0.5f }, 1.0f);
    for (int i = 0; i < rows; i++) {
        const OvRow& r = g_rows[i];
        float ry = oy + 40 + i * 26.0f;
        bool sel = rowSelected(r);
        if (sel) R.fillRect(ox + 6, ry - 2, ow - 12, 24, { 1.0f, 0.72f, 0.25f, 0.20f });
        swprintf(buf, 256, L"%d", i + 1);
        R.text(buf, ox + 14, ry, 24, 20, R.fmtSmall, DIM);
        std::wstring nm, ty;
        float d = 0;
        D2D1_COLOR_F nameCol = sel ? AMBER : WHITE;
        if (r.kind == 1) {
            if (r.id < 0 || r.id >= (int)s.ents.size()) continue;
            const Entity& e = s.ents[r.id];
            nm = e.name; ty = typeName(e.type);
            d = len(e.pos - G.ship.pos) - e.radius;
        } else {
            RemotePlayer* p = N.find(r.id);
            if (!p) continue;
            nm = p->name; ty = L"PILOT";
            d = len(p->pos - G.ship.pos);
            if (!sel) nameCol = RED;
        }
        if (d < 0) d = 0;
        R.text(nm.c_str(), ox + 38, ry, 200, 20, R.fmtSmall, nameCol);
        R.text(ty.c_str(), ox + 238, ry, 70, 20, R.fmtSmall, DIM);
        R.text(fmtDist(d).c_str(), ox + 280, ry, ow - 294, 20, R.fmtSmall, CYAN, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    // speed + hp panel
    float sw = 460, sx = (W - sw) / 2, sy = H - 118;
    R.fillRect(sx, sy, sw, 94, PANEL);
    float spd = G.ship.speed;
    if (spd >= 1.0f) swprintf(buf, 256, L"%.0f km/s", spd);
    else swprintf(buf, 256, L"%.0f m/s", spd * 1000.0f);
    R.text(buf, sx, sy + 6, sw, 30, R.fmtHead, WHITE, DWRITE_TEXT_ALIGNMENT_CENTER);
    float bx = sx + 30, bw = sw - 60;
    // shield / armor bars
    R.fillRect(bx, sy + 42, bw, 7, { 0.15f, 0.20f, 0.26f, 0.8f });
    R.fillRect(bx, sy + 42, bw * clampf(N.shield / 100.0f, 0, 1), 7, { 0.35f, 0.65f, 1.0f, 0.95f });
    R.fillRect(bx, sy + 52, bw, 7, { 0.20f, 0.18f, 0.15f, 0.8f });
    R.fillRect(bx, sy + 52, bw * clampf(N.armor / 100.0f, 0, 1), 7, { 0.95f, 0.60f, 0.25f, 0.95f });
    // throttle
    R.fillRect(bx, sy + 64, bw, 9, { 0.2f, 0.25f, 0.3f, 0.8f });
    R.fillRect(bx, sy + 64, bw * G.ship.throttle, 9, { 0.45f, 0.85f, 1.0f, 0.9f });
    swprintf(buf, 256, L"SHD %d · ARM %d · THROTTLE %d%% · MAX 250 m/s",
        (int)N.shield, (int)N.armor, (int)(G.ship.throttle * 100));
    R.text(buf, sx, sy + 76, sw, 14, R.fmtSmall, DIM, DWRITE_TEXT_ALIGNMENT_CENTER);

    // state banner / message
    if (G.state == GState::Aligning)
        R.text((L"ALIGNING TO " + G.warpTargetName).c_str(), 0, H * 0.32f, W, 30, R.fmtHead, CYAN, DWRITE_TEXT_ALIGNMENT_CENTER);
    if (G.state == GState::Warping)
        R.text(L"WARP DRIVE ACTIVE", 0, H * 0.32f, W, 30, R.fmtHead, CYAN, DWRITE_TEXT_ALIGNMENT_CENTER);
    if (G.msgT > 0)
        R.text(G.msg.c_str(), 0, H * 0.38f, W, 30, R.fmtBody, WHITE, DWRITE_TEXT_ALIGNMENT_CENTER);

    // contextual prompts
    std::wstring prompt;
    if (G.state == GState::Flying) {
        int gi = nearestOfType(EType::Gate, 4.0f);
        int si = nearestOfType(EType::Station, 3.0f);
        if (g_selKind == 2) {
            RemotePlayer* p = N.find(g_selId);
            if (p) {
                float d = len(p->pos - G.ship.pos);
                prompt = d <= 8.0f ? (L"[F] Fire on " + p->name) : (p->name + L" out of range — [SPACE] warp to pilot");
            }
        } else if (gi >= 0) prompt = L"[J] Activate " + G.sys().ents[gi].name;
        else if (si >= 0) prompt = L"[G] Request docking";
        else if (g_selKind == 1) prompt = L"[SPACE] Warp to " + G.sys().ents[g_selId].name;
        else prompt = L"[TAB] Cycle overview targets";
    }
    if (!prompt.empty())
        R.text(prompt.c_str(), 0, H - 156, W, 24, R.fmtBody, AMBER, DWRITE_TEXT_ALIGNMENT_CENTER);

    // selection bracket
    V3 tp; float tr; std::wstring tn;
    if (selectedPos(tp, tr, tn)) {
        float px, py;
        if (project(tp, px, py)) {
            float g = 20;
            D2D1_COLOR_F bc = { 1.0f, 0.72f, 0.25f, 0.9f };
            R.line(px - g, py - g, px - g + 10, py - g, bc); R.line(px - g, py - g, px - g, py - g + 10, bc);
            R.line(px + g, py - g, px + g - 10, py - g, bc); R.line(px + g, py - g, px + g, py - g + 10, bc);
            R.line(px - g, py + g, px - g + 10, py + g, bc); R.line(px - g, py + g, px - g, py + g - 10, bc);
            R.line(px + g, py + g, px + g - 10, py + g, bc); R.line(px + g, py + g, px + g, py + g - 10, bc);
        }
    }

    // pilot name tags + hp bars
    for (const RemotePlayer& p : N.players) {
        if (p.sys != G.sysIdx || p.docked || !p.hasState) continue;
        float px, py;
        if (!project(p.pos, px, py)) continue;
        float d = len(p.pos - G.ship.pos);
        std::wstring tag = p.name + L"  " + fmtDist(d);
        R.text(tag.c_str(), px - 100, py - 46, 200, 18, R.fmtSmall, RED, DWRITE_TEXT_ALIGNMENT_CENTER);
        R.fillRect(px - 30, py - 26, 60, 4, { 0.15f, 0.20f, 0.26f, 0.85f });
        R.fillRect(px - 30, py - 26, 60 * clampf(p.shield / 100.0f, 0, 1), 4, { 0.35f, 0.65f, 1.0f, 0.95f });
        R.fillRect(px - 30, py - 20, 60, 4, { 0.20f, 0.18f, 0.15f, 0.85f });
        R.fillRect(px - 30, py - 20, 60 * clampf(p.armor / 100.0f, 0, 1), 4, { 0.95f, 0.60f, 0.25f, 0.95f });
    }

    // weapon beams
    for (const BeamFx& b : N.beams) {
        if (!shipInSys(b.shooterId) || !shipInSys(b.targetId)) continue;
        float x0, y0, x1, y1;
        if (!project(shipPosOf(b.shooterId), x0, y0) || !project(shipPosOf(b.targetId), x1, y1)) continue;
        float a = clampf(b.ttl / 0.30f, 0, 1);
        R.line(x0, y0, x1, y1, { 1.0f, 0.25f, 0.15f, 0.30f * a }, 5.0f);
        R.line(x0, y0, x1, y1, { 1.0f, 0.75f, 0.55f, 0.9f * a }, 1.6f);
        R.circle(x1, y1, 7.0f * a, { 1.0f, 0.55f, 0.25f, 0.7f * a });
    }

    hudChat(g_showHelp ? H - 126 : H - 24);

    // help
    if (g_showHelp) {
        R.fillRect(20, H - 116, 520, 96, PANEL);
        R.text(L"W/S throttle  A/D yaw  UP/DN pitch  Q/E roll", 34, H - 106, 500, 18, R.fmtSmall, DIM);
        R.text(L"1-9/TAB select  SPACE warp  F fire  J jump  G dock", 34, H - 84, 500, 18, R.fmtSmall, DIM);
        R.text(L"N star map  ENTER chat  M mute  H help  F12 shot  ESC quit", 34, H - 62, 500, 18, R.fmtSmall, DIM);
        R.text(L"Tip: open the map [N], click a system to plot a route.", 34, H - 40, 500, 18, R.fmtSmall, CYAN);
    }
}

static void hudDocked() {
    StarSystem& s = G.sys();
    const Entity& st = s.ents[G.dockedAt];
    float W = (float)R.width, H = (float)R.height;

    R.text(st.name.c_str(), 0, 36, W, 44, R.fmtTitle, WHITE, DWRITE_TEXT_ALIGNMENT_CENTER);
    std::wstring sub = s.name + L" system  ·  docking bay 4";
    R.text(sub.c_str(), 0, 84, W, 22, R.fmtBody, DIM, DWRITE_TEXT_ALIGNMENT_CENTER);

    float px = 40, py = H * 0.30f, pw = 320;
    const wchar_t* svc[5] = { L"REPAIR SHOP", L"REFITTING", L"REGIONAL MARKET", L"MISSION AGENTS", L"INSURANCE" };
    R.fillRect(px, py, pw, 46.0f + 5 * 34.0f, PANEL);
    R.text(L"STATION SERVICES", px + 16, py + 10, pw - 32, 24, R.fmtBody, AMBER);
    R.line(px + 12, py + 38, px + pw - 12, py + 38, { 1.0f, 0.72f, 0.25f, 0.5f }, 1.0f);
    for (int i = 0; i < 5; i++) {
        float ry = py + 50 + i * 34.0f;
        R.text(svc[i], px + 16, ry, 200, 22, R.fmtSmall, WHITE);
        R.text(L"ONLINE", px + pw - 90, ry, 74, 22, R.fmtSmall, GREEN);
    }

    float qx = W - 360, qy = H * 0.30f, qw = 320;
    R.fillRect(qx, qy, qw, 150, PANEL);
    R.text(L"ACTIVE SHIP", qx + 16, qy + 10, qw - 32, 24, R.fmtBody, AMBER);
    R.line(qx + 12, qy + 38, qx + qw - 12, qy + 38, { 1.0f, 0.72f, 0.25f, 0.5f }, 1.0f);
    R.text(L"WAYFARER · frigate", qx + 16, qy + 50, qw - 32, 22, R.fmtSmall, WHITE);
    wchar_t hp[64];
    swprintf(hp, 64, L"SHIELD %d%%   ARMOR %d%%", (int)N.shield, (int)N.armor);
    R.text(hp, qx + 16, qy + 78, qw - 32, 22, R.fmtSmall, DIM);
    R.text(N.loggedIn ? L"COMMS ONLINE · all systems nominal" : L"OFFLINE MODE · all systems nominal",
        qx + 16, qy + 106, qw - 32, 22, R.fmtSmall, DIM);

    R.fillRect(W / 2 - 160, H - 120, 320, 54, { 0.10f, 0.06f, 0.01f, 0.85f });
    R.text(L"[ U ]  UNDOCK", W / 2 - 160, H - 108, 320, 30, R.fmtHead, AMBER, DWRITE_TEXT_ALIGNMENT_CENTER);

    if (G.msgT > 0)
        R.text(G.msg.c_str(), 0, H * 0.62f, W, 26, R.fmtBody, WHITE, DWRITE_TEXT_ALIGNMENT_CENTER);

    hudChat(H - 140);
}

static D2D1_COLOR_F secColor(float sec) {
    if (sec >= 0.7f) return { 0.35f, 0.75f, 1.00f, 1.0f };
    if (sec >= 0.5f) return { 0.35f, 0.90f, 0.55f, 1.0f };
    if (sec >= 0.1f) return { 0.95f, 0.65f, 0.20f, 1.0f };
    return { 0.90f, 0.28f, 0.25f, 1.0f };
}

static void hudMap(float time) {
    float W = (float)R.width, H = (float)R.height;
    wchar_t buf[256];
    R.fillRect(0, 0, W, H, { 0.010f, 0.018f, 0.028f, 0.94f });
    R.text(L"STAR MAP — NEW EDEN FRINGE", 0, 28, W, 34, R.fmtHead, AMBER, DWRITE_TEXT_ALIGNMENT_CENTER);
    swprintf(buf, 256, L"%zu systems · click a system to plot a route · [N] close",
        G.universe.size());
    R.text(buf, 0, 62, W, 20, R.fmtSmall, DIM, DWRITE_TEXT_ALIGNMENT_CENTER);

    // gate links (route segments highlighted)
    auto onRoute = [&](int a, int b) {
        for (size_t i = 0; i + 1 < g_route.size(); i++)
            if ((g_route[i] == a && g_route[i + 1] == b) || (g_route[i] == b && g_route[i + 1] == a))
                return true;
        return false;
    };
    for (int i = 0; i < (int)G.universe.size(); i++)
        for (const Entity& e : G.universe[i].ents)
            if (e.type == EType::Gate && e.linkSystem > i) {
                float x0, y0, x1, y1;
                mapPoint(i, x0, y0);
                mapPoint(e.linkSystem, x1, y1);
                if (onRoute(i, e.linkSystem))
                    R.line(x0, y0, x1, y1, { 1.0f, 0.72f, 0.25f, 0.95f }, 2.5f);
                else
                    R.line(x0, y0, x1, y1, { 0.30f, 0.50f, 0.65f, 0.30f }, 1.0f);
            }

    // systems
    int hover = -1;
    float hd = 22.0f;
    for (int i = 0; i < (int)G.universe.size(); i++) {
        float sx, sy;
        mapPoint(i, sx, sy);
        float d = sqrtf((sx - g_mouseX) * (sx - g_mouseX) + (sy - g_mouseY) * (sy - g_mouseY));
        if (d < hd) { hd = d; hover = i; }
    }
    for (int i = 0; i < (int)G.universe.size(); i++) {
        const StarSystem& s = G.universe[i];
        float sx, sy;
        mapPoint(i, sx, sy);
        D2D1_COLOR_F c = secColor(s.security);
        if (i == G.sysIdx) {
            float pulse = 10.0f + 2.5f * sinf(time * 4.0f);
            R.circle(sx, sy, pulse, { 1.0f, 0.72f, 0.25f, 0.25f });
        }
        if (i == g_dest) R.circle(sx, sy, 11.0f, { 0.45f, 0.85f, 1.0f, 0.30f });
        R.circle(sx, sy, i == hover ? 7.5f : 5.5f, c);
        swprintf(buf, 256, L"%s  %.1f", s.name.c_str(), s.security);
        R.text(buf, sx - 90, sy + 9, 180, 16, R.fmtSmall,
            i == G.sysIdx ? AMBER : (i == hover ? WHITE : DIM), DWRITE_TEXT_ALIGNMENT_CENTER);
        auto it = N.sov.find(i);
        if (it != N.sov.end()) {
            std::wstring tag = L"[" + it->second + L"]";
            R.text(tag.c_str(), sx - 90, sy - 24, 180, 16, R.fmtSmall, RED, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }

    // hover tooltip
    if (hover >= 0) {
        const StarSystem& s = G.universe[hover];
        float tx = (float)g_mouseX + 18, ty = (float)g_mouseY - 10;
        if (tx > W - 280) tx = (float)g_mouseX - 280;
        R.fillRect(tx, ty, 262, 84, { 0.03f, 0.07f, 0.10f, 0.95f });
        R.text(s.name.c_str(), tx + 12, ty + 6, 240, 20, R.fmtBody, WHITE);
        swprintf(buf, 256, L"security %.1f", s.security);
        R.text(buf, tx + 12, ty + 30, 240, 16, R.fmtSmall, secColor(s.security));
        auto it = N.sov.find(hover);
        std::wstring sv = it != N.sov.end() ? (L"sov: " + it->second)
                          : (s.security <= 0.0f ? L"sov: unclaimed" : L"empire space");
        R.text(sv.c_str(), tx + 12, ty + 48, 240, 16, R.fmtSmall, it != N.sov.end() ? RED : DIM);
        int st = 0, gt = 0;
        for (const Entity& e : s.ents) { if (e.type == EType::Station) st++; if (e.type == EType::Gate) gt++; }
        swprintf(buf, 256, L"%d station%s · %d gate%s", st, st == 1 ? L"" : L"s", gt, gt == 1 ? L"" : L"s");
        R.text(buf, tx + 12, ty + 64, 240, 16, R.fmtSmall, DIM);
    }

    // legend
    float ly = H - 44;
    R.circle(W / 2 - 290, ly + 8, 5, secColor(0.9f)); R.text(L"highsec", W / 2 - 278, ly, 90, 18, R.fmtSmall, DIM);
    R.circle(W / 2 - 180, ly + 8, 5, secColor(0.5f)); R.text(L"0.5", W / 2 - 168, ly, 60, 18, R.fmtSmall, DIM);
    R.circle(W / 2 - 100, ly + 8, 5, secColor(0.3f)); R.text(L"lowsec", W / 2 - 88, ly, 90, 18, R.fmtSmall, DIM);
    R.circle(W / 2 + 10, ly + 8, 5, secColor(0.0f)); R.text(L"0.0 — claimable with /claim", W / 2 + 22, ly, 300, 18, R.fmtSmall, DIM);
}

static void hudLogin(float time) {
    float W = (float)R.width, H = (float)R.height;
    float pw = 480, ph = 330, px = (W - pw) / 2, py = (H - ph) / 2 - 30;
    R.fillRect(px, py, pw, ph, { 0.02f, 0.05f, 0.08f, 0.92f });
    R.rectOutline(px, py, pw, ph, { 1.0f, 0.72f, 0.25f, 0.6f }, 1.0f);
    R.text(L"NEW EDEN RELAY", px, py + 18, pw, 34, R.fmtHead, AMBER, DWRITE_TEXT_ALIGNMENT_CENTER);
    R.text(L"capsuleer authentication", px, py + 52, pw, 20, R.fmtSmall, DIM, DWRITE_TEXT_ALIGNMENT_CENTER);

    const wchar_t* labels[3] = { L"PILOT NAME", L"PASSWORD", L"SERVER" };
    std::wstring vals[3] = { g_user, std::wstring(g_pass.size(), L'•'), g_host };
    bool cursor = fmodf(time, 1.0f) < 0.55f;
    for (int i = 0; i < 3; i++) {
        float fy = py + 88 + i * 58.0f;
        R.text(labels[i], px + 50, fy, 160, 18, R.fmtSmall, DIM);
        bool focus = g_loginField == i;
        R.fillRect(px + 50, fy + 20, pw - 100, 28, { 0.06f, 0.11f, 0.15f, 0.95f });
        R.rectOutline(px + 50, fy + 20, pw - 100, 28,
            focus ? D2D1_COLOR_F{ 1.0f, 0.72f, 0.25f, 0.9f } : D2D1_COLOR_F{ 0.3f, 0.4f, 0.5f, 0.5f }, focus ? 1.5f : 1.0f);
        std::wstring v = vals[i];
        if (focus && cursor) v += L"_";
        R.text(v.c_str(), px + 60, fy + 24, pw - 120, 22, R.fmtBody, WHITE);
    }
    if (!g_loginStatus.empty())
        R.text(g_loginStatus.c_str(), px, py + 262, pw, 20, R.fmtSmall,
            g_loginPending ? CYAN : RED, DWRITE_TEXT_ALIGNMENT_CENTER);
    R.text(L"[TAB] next field   [ENTER] connect   [ESC] play offline",
        px, py + 292, pw, 20, R.fmtSmall, DIM, DWRITE_TEXT_ALIGNMENT_CENTER);
    R.text(L"new pilot names are registered automatically",
        0, py + ph + 14, W, 18, R.fmtSmall, DIM, DWRITE_TEXT_ALIGNMENT_CENTER);
}

// --------------------------------------------------------------------- main
static std::wstring argValue(const std::wstring& cmd, const wchar_t* key) {
    size_t p = cmd.find(key);
    if (p == std::wstring::npos) return L"";
    p += wcslen(key);
    size_t e = cmd.find(L' ', p);
    return cmd.substr(p, e == std::wstring::npos ? std::wstring::npos : e - p);
}

static LONG WINAPI crashFilter(EXCEPTION_POINTERS* ep) {
    HMODULE base = GetModuleHandleW(nullptr);
    unsigned long long addr = (unsigned long long)ep->ExceptionRecord->ExceptionAddress;
    logLine("CRASH: code 0x%08lX at %p (module base %p, offset +0x%llX)",
        ep->ExceptionRecord->ExceptionCode, (void*)addr, (void*)base,
        addr - (unsigned long long)base);
    return EXCEPTION_EXECUTE_HANDLER;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    SetProcessDPIAware();
    SetUnhandledExceptionFilter(crashFilter);
    N.startup();
    std::wstring cmd = GetCommandLineW();
    g_autoShot = argValue(cmd, L"shot=");
    g_autoUser = argValue(cmd, L"user=");
    g_autoChatMsg = argValue(cmd, L"chatmsg=");
    for (wchar_t& c : g_autoChatMsg) if (c == L'_') c = L' ';
    std::wstring cf = argValue(cmd, L"chatframe=");
    g_autoChatFrame = cf.empty() ? 120 : _wtoi(cf.c_str());
    std::wstring so = argValue(cmd, L"spawnoff=");
    g_autoSpawnOff = so.empty() ? 0.0f : (float)_wtof(so.c_str());
    std::wstring hostArg = argValue(cmd, L"server=");
    if (!hostArg.empty()) g_host = hostArg;
    g_autoSpace = cmd.find(L"mode=space") != std::wstring::npos;
    g_autoGate = cmd.find(L"mode=gate") != std::wstring::npos;
    g_autoWarp = cmd.find(L"warp=1") != std::wstring::npos;
    g_autoJump = cmd.find(L"jump=1") != std::wstring::npos;
    g_autoFire = cmd.find(L"fire=1") != std::wstring::npos;
    bool autoMap = cmd.find(L"map=1") != std::wstring::npos;
    g_autoSoak = cmd.find(L"soak=1") != std::wstring::npos;
    std::wstring destArg = argValue(cmd, L"dest=");
    std::wstring sysArg = argValue(cmd, L"sys=");
    std::wstring fr = argValue(cmd, L"frames=");
    if (!fr.empty()) g_autoFrames = _wtoi(fr.c_str());
    std::wstring rs = argValue(cmd, L"runsec=");
    if (!rs.empty()) g_autoRunSec = (float)_wtof(rs.c_str());
    bool autoMode = !g_autoShot.empty();
    if (autoMode && g_autoFrames <= 0) g_autoFrames = g_autoWarp ? 300 : (g_autoSpace ? 150 : 90);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"SpaceWnd";
    RegisterClassW(&wc);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc = { 0, 0, R.width, R.height };
    AdjustWindowRect(&rc, style, FALSE);
    HWND hwnd = CreateWindowW(L"SpaceWnd", L"SPACE - a very small EVE",
        style, CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, hInst, nullptr);
    ShowWindow(hwnd, SW_SHOW);

    if (!R.init(hwnd, g_hlsl)) {
        MessageBoxW(hwnd, L"Renderer init failed - see space_log.txt", L"SPACE", MB_ICONERROR);
        return 1;
    }
    if (A.init()) {
        struct { const char* n; std::vector<int16_t>* b; } chk[] = {
            { "pew", &A.pewB }, { "boom", &A.boomB }, { "whoosh", &A.whooshB },
            { "hum", &A.humB }, { "musicDock", &A.mDockB }, { "musicSpace", &A.mSpaceB },
        };
        for (auto& c : chk) {
            int peak, rms;
            pcmStats(*c.b, peak, rms);
            logLine("audio %s: %.2fs peak=%d rms=%d", c.n, c.b->size() / (float)SR, peak, rms);
        }
        if (cmd.find(L"dumpwav=1") != std::wstring::npos) { A.dumpWavs(); logLine("wav dump written"); }
    } else {
        logLine("audio init FAILED (continuing silent)");
    }

    g_shipMesh = R.createMesh(buildShipMesh());
    g_stationMesh = R.createMesh(buildStationMesh());
    g_gateMesh = R.createMesh(buildGateMesh());
    g_planetMesh = R.createMesh(buildPlanetMesh());
    g_rockMesh = R.createMesh(buildRockMesh());

    // log the generated universe once (helps tests pick systems)
    for (int i = 0; i < (int)G.universe.size(); i++) {
        const StarSystem& s = G.universe[i];
        int st = 0, gt = 0, pl = 0;
        for (const Entity& e : s.ents) {
            if (e.type == EType::Station) st++;
            if (e.type == EType::Gate) gt++;
            if (e.type == EType::Planet) pl++;
        }
        logLine("sys %2d: %-12ls sec %.1f  planets %d stations %d gates %d", i, s.name.c_str(), s.security, pl, st, gt);
    }

    // soak test: visit every gate in the universe
    for (int i = 0; i < (int)G.universe.size(); i++)
        for (int j = 0; j < (int)G.universe[i].ents.size(); j++)
            if (G.universe[i].ents[j].type == EType::Gate)
                g_jumpList.push_back({ i, j });
    if (g_autoSoak) {
        g_autoFrames = (int)g_jumpList.size() * 240 + 480;
        logLine("soak: %zu gate transitions to test", g_jumpList.size());
    }

    // test spawn-system override: sys=<idx> or sys=null (first 0.0 system)
    if (!sysArg.empty()) {
        if (sysArg == L"null") {
            for (int i = 0; i < (int)G.universe.size(); i++)
                if (G.universe[i].security <= 0.0f) { G.sysIdx = i; break; }
        } else G.sysIdx = (int)_wtoi(sysArg.c_str()) % (int)G.universe.size();
        logLine("spawn system override: %d (%ls)", G.sysIdx, G.sys().name.c_str());
    }

    // start docked at the first station of system 0
    for (int i = 0; i < (int)G.sys().ents.size(); i++)
        if (G.sys().ents[i].type == EType::Station) { G.dockedAt = i; break; }
    if (G.dockedAt < 0) { // spawned in a station-less system: start in space near the sun
        G.ship.pos = { 1200, 0, 0 };
        G.ship.rot = basisFromFwd({ 0, 0, 1 });
        G.state = GState::Flying;
    }

    // auto-test: log in synchronously, then position the ship
    if (autoMode) {
        g_loginUI = false;
        if (!g_autoUser.empty()) {
            g_user = g_autoUser;
            if (N.connectTo(g_host, 5577)) {
                N.login(g_user, L"pw");
                for (int i = 0; i < 60 && !N.loggedIn && N.lastError.empty(); i++) {
                    Sleep(50);
                    N.poll();
                }
                logLine("auto login %ls: %s", g_user.c_str(), N.loggedIn ? "ok" : "FAILED");
            } else logLine("auto connect failed: %ls", N.lastError.c_str());
        }
    }

    if (g_autoSpace || g_autoWarp) {
        const Entity& st = G.sys().ents[G.dockedAt];
        V3 lat = norm(cross(V3{ 0,1,0 }, st.dir));
        G.ship.pos = st.pos + st.dir * (4.5f - g_autoSpawnOff * 3.0f) + lat * g_autoSpawnOff;
        G.ship.rot = basisFromFwd(-st.dir);
        G.ship.speed = 0.06f; G.ship.throttle = g_autoRunSec > 0 ? 0.15f : 0.4f;
        G.state = GState::Flying;
    } else if (g_autoGate) {
        for (const Entity& e : G.sys().ents)
            if (e.type == EType::Gate) {
                G.ship.pos = e.pos - e.dir * (e.radius + 2.0f);
                G.ship.rot = basisFromFwd(norm(e.pos - G.ship.pos));
                G.ship.speed = 0.1f; G.ship.throttle = 0.4f;
                G.state = GState::Flying;
                break;
            }
    }
    g_camEye = G.ship.pos - G.ship.rot.fwd() * 0.32f + G.ship.rot.up() * 0.105f;

    LARGE_INTEGER qpf, prev, now, start;
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&prev);
    start = prev;
    float time = 0;

    MSG msg = {};
    while (!g_quit) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) g_quit = true;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (g_quit) break;

        QueryPerformanceCounter(&now);
        float dt = (float)(now.QuadPart - prev.QuadPart) / qpf.QuadPart;
        prev = now;
        if (dt > 0.05f) dt = 0.05f;
        if (autoMode) dt = 1.0f / 60.0f;
        time += dt;

        // scripted autopilot for tests
        if (autoMode && g_autoWarp && g_frame == 30) {
            for (int i = 0; i < (int)G.sys().ents.size(); i++)
                if (G.sys().ents[i].type == EType::Gate) { startWarp(i); break; }
        }
        if (autoMode && g_autoJump && g_frame == 60 && G.state == GState::Flying) {
            int gi = nearestOfType(EType::Gate, 6.0f);
            if (gi >= 0) doJump(gi);
        }
        if (autoMode && autoMap && g_frame == 30) g_mapOpen = true;
        if (autoMode && !destArg.empty() && g_frame == 35)
            g_dest = (int)_wtoi(destArg.c_str()) % (int)G.universe.size();
        if (autoMode && g_autoSoak) {
            if (g_frame % 240 == 0) {
                if (g_soakIdx >= g_jumpList.size()) {
                    logLine("SOAK COMPLETE: %zu gate transitions, no crash", g_jumpList.size());
                    g_autoFrames = g_frame; // exit + screenshot now
                } else {
                    int si = g_jumpList[g_soakIdx].first, gi = g_jumpList[g_soakIdx].second;
                    G.sysIdx = si;
                    const Entity& gt = G.sys().ents[gi];
                    G.ship.pos = gt.pos + gt.dir * 2.0f;
                    G.ship.rot = basisFromFwd(gt.dir * -1.0f);
                    G.ship.speed = 0.05f; G.ship.throttle = 0.2f;
                    G.state = GState::Flying;
                    G.warpVis = 0; G.whiteout = 0;
                    g_selKind = 0; g_selId = -1; g_dest = -1;
                    logLine("soak %zu/%zu: sys %d (%ls) gate %d -> sys %d",
                        g_soakIdx + 1, g_jumpList.size(), si, G.sys().name.c_str(), gi, gt.linkSystem);
                    doJump(gi);
                    g_soakIdx++;
                }
            }
            if (g_frame % 240 == 200 && G.state == GState::Flying) {
                // also exercise in-system warp in the arrival system
                for (int i = (int)G.sys().ents.size() - 1; i >= 0; i--)
                    if (G.sys().ents[i].type == EType::Planet || G.sys().ents[i].type == EType::Sun) {
                        logLine("soak warp: sys %d -> ent %d (%ls)", G.sysIdx, i, G.sys().ents[i].name.c_str());
                        startWarp(i);
                        break;
                    }
            }
        }
        double realT = (double)(now.QuadPart - start.QuadPart) / qpf.QuadPart;
        if (autoMode && !g_autoChatMsg.empty() && g_frame == g_autoChatFrame)
            sendChat(g_autoChatMsg);
        bool fireDue = g_autoRunSec > 0 ? (realT > 3.0 && realT - g_lastAutoFire >= 1.0)
                                        : (g_frame >= 173 && (g_frame - 173) % 40 == 0);
        if (autoMode && g_autoFire && fireDue) {
            for (const RemotePlayer& p : N.players)
                if (p.sys == G.sysIdx && !p.docked && p.hasState) {
                    g_selKind = 2; g_selId = p.id;
                    g_fireCD = 0;
                    fireSelected();
                    g_lastAutoFire = realT;
                    break;
                }
        }

        bool focused = !autoMode && GetForegroundWindow() == hwnd;
        try {
            update(dt, focused);
        } catch (const std::exception& e) {
            logLine("EXCEPTION in update: %s (frame %d, sys %d, state %d, soak %zu)",
                e.what(), g_frame, G.sysIdx, (int)G.state, g_soakIdx);
            if (!autoMode) MessageBoxW(hwnd, L"Fatal error - see space_log.txt", L"SPACE", MB_ICONERROR);
            break;
        }

        bool dockedView = G.state == GState::Docked || g_loginUI;
        A.update(dt, dockedView, G.ship.throttle, G.warpVis);

        R.beginFrame();
        fillCommonCB(time);
        if (dockedView) {
            R.drawFullscreen(R.psHangar, R.dsOff, R.blOpaque);
        } else {
            renderSpace(time);
        }

        try {
            R.d2dRT->BeginDraw();
            if (g_loginUI) hudLogin(time);
            else if (G.state == GState::Docked) hudDocked();
            else hudSpace();
            if (g_mapOpen && !g_loginUI) hudMap(time);
            R.d2dRT->EndDraw();
        } catch (const std::exception& e) {
            logLine("EXCEPTION in hud: %s (frame %d, sys %d, state %d, soak %zu)",
                e.what(), g_frame, G.sysIdx, (int)G.state, g_soakIdx);
            if (!autoMode) MessageBoxW(hwnd, L"Fatal error - see space_log.txt", L"SPACE", MB_ICONERROR);
            break;
        }

        if (g_shotRequest) {
            wchar_t name[64];
            swprintf(name, 64, L"screenshot_%d.bmp", g_shotCounter++);
            R.saveScreenshotBMP(name);
            G.say(std::wstring(L"Saved ") + name, 2.0f);
            g_shotRequest = false;
        }
        g_frame++;
        if (autoMode && (g_autoRunSec > 0 ? realT >= g_autoRunSec : g_frame >= g_autoFrames)) {
            bool ok = R.saveScreenshotBMP(g_autoShot.c_str());
            logLine("auto shot %ls -> %s", g_autoShot.c_str(), ok ? "ok" : "FAILED");
            break;
        }

        R.present();
    }
    A.shutdown();
    N.disconnect();
    WSACleanup();
    return 0;
}
