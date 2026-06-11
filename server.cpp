// server.cpp - tiny authoritative relay server for SPACE.
// Accounts in users.txt ("name password" per line, auto-created on first login).
// Protocol: '\n'-terminated lines, '|'-separated fields.
//   C->S  LOGIN|name|pass
//         ST|sys|px|py|pz|fx|fy|fz|ux|uy|uz|spd|docked
//         CHAT|text
//         FIRE|targetId
//         CLAIM|allianceName        (claim sovereignty of current 0.0 system)
//   S->C  OK|id   ERR|reason   JOIN|id|name   LEAVE|id
//         ST|id|sys|...|docked   CHAT|name|text
//         FIRE|shooterId|targetId|dmg   HP|id|shield|armor   KILL|id|killerName
//         SOV|sysIdx|allianceName
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include "math3d.h"
#include "mesh.h"
#include "game.h"
#pragma comment(lib, "ws2_32.lib")

static const int PORT = 5577;
static const float WEAPON_RANGE_KM = 8.0f;
static const double FIRE_COOLDOWN = 0.9;

struct Client {
    SOCKET s = INVALID_SOCKET;
    std::string rx;
    bool logged = false;
    int id = -1;
    std::string name;
    int sys = 0;
    float x = 0, y = 0, z = 0;
    bool docked = true;
    bool hasState = false;
    float shield = 100, armor = 100;
    double lastFire = 0;
};

static std::vector<Client> g_cls;
static std::map<std::string, std::string> g_users;
static std::map<int, std::string> g_sov;          // system index -> alliance
static std::vector<StarSystem> g_uni;             // same deterministic universe as clients
static int g_nextId = 1;

static std::string narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += c < 128 ? (char)c : '?';
    return s;
}

static void loadSov() {
    FILE* f = nullptr;
    if (fopen_s(&f, "sov.txt", "r") != 0 || !f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        int idx; char name[200];
        if (sscanf_s(line, "%d %[^\n]", &idx, name, (unsigned)200) == 2)
            if (idx >= 0 && idx < (int)g_uni.size()) g_sov[idx] = name;
    }
    fclose(f);
    printf("loaded %zu sov claims\n", g_sov.size());
}
static void saveSov() {
    FILE* f = nullptr;
    if (fopen_s(&f, "sov.txt", "w") != 0 || !f) return;
    for (auto& kv : g_sov) fprintf(f, "%d %s\n", kv.first, kv.second.c_str());
    fclose(f);
}

static double nowSec() { return GetTickCount64() / 1000.0; }

static void loadUsers() {
    FILE* f = nullptr;
    if (fopen_s(&f, "users.txt", "r") != 0 || !f) return;
    char n[128], p[128];
    while (fscanf_s(f, "%127s %127s", n, (unsigned)128, p, (unsigned)128) == 2)
        g_users[n] = p;
    fclose(f);
    printf("loaded %zu accounts\n", g_users.size());
}
static void saveUsers() {
    FILE* f = nullptr;
    if (fopen_s(&f, "users.txt", "w") != 0 || !f) return;
    for (auto& kv : g_users) fprintf(f, "%s %s\n", kv.first.c_str(), kv.second.c_str());
    fclose(f);
}

static void sendLine(Client& c, const std::string& line) {
    std::string out = line + "\n";
    send(c.s, out.c_str(), (int)out.size(), 0);
}
static void broadcast(const std::string& line, int exceptId = -1) {
    for (auto& c : g_cls)
        if (c.logged && c.id != exceptId) sendLine(c, line);
}

static std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t p = 0;
    while (true) {
        size_t q = s.find(sep, p);
        if (q == std::string::npos) { out.push_back(s.substr(p)); break; }
        out.push_back(s.substr(p, q - p));
        p = q + 1;
    }
    return out;
}

static std::string sanitize(std::string s) {
    for (char& c : s) if (c == '|' || c == '\n' || c == '\r') c = ' ';
    return s;
}

static Client* findById(int id) {
    for (auto& c : g_cls) if (c.logged && c.id == id) return &c;
    return nullptr;
}

static void handleLine(Client& c, const std::string& line) {
    std::vector<std::string> f = split(line, '|');
    if (f.empty()) return;

    if (f[0] == "LOGIN" && f.size() >= 3) {
        std::string name = sanitize(f[1]), pass = f[2];
        if (name.empty() || name.size() > 24) { sendLine(c, "ERR|invalid name"); return; }
        for (auto& o : g_cls)
            if (o.logged && o.name == name) { sendLine(c, "ERR|pilot already online"); return; }
        auto it = g_users.find(name);
        if (it == g_users.end()) {
            g_users[name] = pass;
            saveUsers();
            printf("new account: %s\n", name.c_str());
        } else if (it->second != pass) {
            sendLine(c, "ERR|wrong password");
            return;
        }
        c.logged = true;
        c.name = name;
        c.id = g_nextId++;
        sendLine(c, "OK|" + std::to_string(c.id));
        // introduce everyone
        for (auto& o : g_cls)
            if (o.logged && o.id != c.id)
                sendLine(c, "JOIN|" + std::to_string(o.id) + "|" + o.name);
        broadcast("JOIN|" + std::to_string(c.id) + "|" + c.name, c.id);
        // current sovereignty picture
        for (auto& kv : g_sov)
            sendLine(c, "SOV|" + std::to_string(kv.first) + "|" + kv.second);
        printf("login: %s (id %d)\n", name.c_str(), c.id);
        return;
    }
    if (!c.logged) return;

    if (f[0] == "ST" && f.size() >= 13) {
        c.sys = atoi(f[1].c_str());
        c.x = (float)atof(f[2].c_str());
        c.y = (float)atof(f[3].c_str());
        c.z = (float)atof(f[4].c_str());
        c.docked = f[12] == "1";
        c.hasState = true;
        broadcast("ST|" + std::to_string(c.id) + "|" + line.substr(3), c.id);
        return;
    }
    if (f[0] == "CHAT" && f.size() >= 2) {
        std::string msg = sanitize(f[1]).substr(0, 120);
        if (msg.empty()) return;
        broadcast("CHAT|" + c.name + "|" + msg);
        printf("[chat] %s: %s\n", c.name.c_str(), msg.c_str());
        return;
    }
    if (f[0] == "CLAIM" && f.size() >= 2) {
        std::string alliance = sanitize(f[1]);
        while (!alliance.empty() && alliance.back() == ' ') alliance.pop_back();
        if (alliance.empty() || alliance.size() > 24) { sendLine(c, "CHAT|SOV|Claim denied: invalid alliance name"); return; }
        if (!c.hasState || c.sys < 0 || c.sys >= (int)g_uni.size()) { sendLine(c, "CHAT|SOV|Claim denied: unknown location"); return; }
        if (g_uni[c.sys].security > 0.01f) { sendLine(c, "CHAT|SOV|Claim denied: sovereignty only applies to 0.0 systems"); return; }
        std::string sysName = narrow(g_uni[c.sys].name);
        auto prev = g_sov.find(c.sys);
        std::string msg;
        if (prev != g_sov.end() && prev->second == alliance)
            msg = alliance + " reinforces its hold on " + sysName;
        else if (prev != g_sov.end())
            msg = alliance + " has seized " + sysName + " from " + prev->second + "!";
        else
            msg = alliance + " has claimed sovereignty of " + sysName;
        g_sov[c.sys] = alliance;
        saveSov();
        broadcast("SOV|" + std::to_string(c.sys) + "|" + alliance);
        broadcast("CHAT|SOV|" + msg);
        printf("sov: %s -> %s (by %s)\n", sysName.c_str(), alliance.c_str(), c.name.c_str());
        return;
    }
    if (f[0] == "FIRE" && f.size() >= 2) {
        double t = nowSec();
        if (t - c.lastFire < FIRE_COOLDOWN) return;
        Client* tgt = findById(atoi(f[1].c_str()));
        if (!tgt || tgt->id == c.id) return;
        if (tgt->sys != c.sys || c.docked || tgt->docked || !c.hasState || !tgt->hasState) return;
        float dx = tgt->x - c.x, dy = tgt->y - c.y, dz = tgt->z - c.z;
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        if (dist > WEAPON_RANGE_KM) return;
        c.lastFire = t;
        int dmg = 10 + rand() % 7;
        float d = (float)dmg;
        if (tgt->shield >= d) tgt->shield -= d;
        else { float rem = d - tgt->shield; tgt->shield = 0; tgt->armor -= rem; }
        broadcast("FIRE|" + std::to_string(c.id) + "|" + std::to_string(tgt->id) + "|" + std::to_string(dmg));
        if (tgt->armor <= 0) {
            printf("kill: %s destroyed by %s\n", tgt->name.c_str(), c.name.c_str());
            broadcast("KILL|" + std::to_string(tgt->id) + "|" + c.name);
            tgt->shield = 100; tgt->armor = 100;
            tgt->docked = true;
        }
        broadcast("HP|" + std::to_string(tgt->id) + "|" + std::to_string((int)tgt->shield) + "|" + std::to_string((int)tgt->armor));
        return;
    }
}

int main() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { printf("WSAStartup failed\n"); return 1; }
    g_uni = buildUniverse();
    int nullCount = 0;
    for (auto& s : g_uni) if (s.security <= 0.0f) nullCount++;
    printf("universe: %zu systems (%d nullsec)\n", g_uni.size(), nullCount);
    loadUsers();
    loadSov();

    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);
    BOOL yes = TRUE;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    if (bind(ls, (sockaddr*)&addr, sizeof(addr)) != 0) { printf("bind failed (port %d busy?)\n", PORT); return 1; }
    listen(ls, 8);
    printf("SPACE server listening on port %d\n", PORT);

    while (true) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(ls, &rd);
        for (auto& c : g_cls) FD_SET(c.s, &rd);
        timeval tv = { 0, 50000 };
        int n = select(0, &rd, nullptr, nullptr, &tv);
        if (n <= 0) continue;

        if (FD_ISSET(ls, &rd)) {
            SOCKET s = accept(ls, nullptr, nullptr);
            if (s != INVALID_SOCKET) {
                u_long nb = 1;
                ioctlsocket(s, FIONBIO, &nb);
                Client c; c.s = s;
                g_cls.push_back(c);
                printf("connection accepted\n");
            }
        }
        for (size_t i = 0; i < g_cls.size();) {
            Client& c = g_cls[i];
            bool drop = false;
            if (FD_ISSET(c.s, &rd)) {
                char buf[4096];
                while (true) {
                    int r = recv(c.s, buf, sizeof(buf), 0);
                    if (r > 0) { c.rx.append(buf, r); continue; }
                    if (r == 0) drop = true;
                    else if (WSAGetLastError() != WSAEWOULDBLOCK) drop = true;
                    break;
                }
                size_t p;
                while ((p = c.rx.find('\n')) != std::string::npos) {
                    std::string line = c.rx.substr(0, p);
                    c.rx.erase(0, p + 1);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (!line.empty()) handleLine(c, line);
                }
            }
            if (drop) {
                if (c.logged) {
                    printf("logout: %s\n", c.name.c_str());
                    int id = c.id;
                    closesocket(c.s);
                    g_cls.erase(g_cls.begin() + i);
                    broadcast("LEAVE|" + std::to_string(id));
                    continue;
                }
                closesocket(c.s);
                g_cls.erase(g_cls.begin() + i);
                continue;
            }
            i++;
        }
    }
    return 0;
}
