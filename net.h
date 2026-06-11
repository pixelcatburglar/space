// net.h - client networking: connect, login, state sync, chat, combat events.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include "math3d.h"
#pragma comment(lib, "ws2_32.lib")

inline std::wstring utf8ToW(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
inline std::string wToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

inline std::vector<std::string> splitStr(const std::string& s, char sep) {
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

struct RemotePlayer {
    int id = -1;
    std::wstring name;
    int sys = 0;
    V3 pos, fwd = { 0,0,1 }, up = { 0,1,0 };
    float speed = 0;
    bool docked = true;
    bool hasState = false;
    float shield = 100, armor = 100;
};

struct BeamFx { int shooterId, targetId; float ttl; };
struct BoomFx { V3 pos; int sys; float ttl; };
// sound cues for the audio layer: 0 = weapon fire, 1 = explosion, 2 = chat
struct NetSfx { int type; V3 pos; int sys; bool positional; };

struct Net {
    SOCKET sock = INVALID_SOCKET;
    bool connected = false, loggedIn = false;
    int myId = -1;
    float shield = 100, armor = 100;
    std::string rx;
    std::wstring lastError;
    std::vector<RemotePlayer> players;
    std::deque<std::wstring> chat;
    std::vector<BeamFx> beams;
    std::vector<BoomFx> booms;
    std::vector<NetSfx> sfx;
    std::map<int, std::wstring> sov;   // system index -> alliance holding it
    bool killedMe = false;
    std::wstring killedBy;

    bool startup() {
        WSADATA wsa;
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }

    bool connectTo(const std::wstring& host, int port) {
        disconnect();
        addrinfoW hints = {}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        wchar_t portStr[16];
        swprintf(portStr, 16, L"%d", port);
        if (GetAddrInfoW(host.c_str(), portStr, &hints, &res) != 0 || !res) {
            lastError = L"cannot resolve host";
            return false;
        }
        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (connect(sock, res->ai_addr, (int)res->ai_addrlen) != 0) {
            FreeAddrInfoW(res);
            closesocket(sock);
            sock = INVALID_SOCKET;
            lastError = L"connection refused (is server.exe running?)";
            return false;
        }
        FreeAddrInfoW(res);
        u_long nb = 1;
        ioctlsocket(sock, FIONBIO, &nb);
        connected = true;
        lastError.clear();
        return true;
    }

    void disconnect() {
        if (sock != INVALID_SOCKET) closesocket(sock);
        sock = INVALID_SOCKET;
        connected = loggedIn = false;
        myId = -1;
        players.clear();
        beams.clear();
    }

    void sendLine(const std::string& line) {
        if (!connected) return;
        std::string out = line + "\n";
        send(sock, out.c_str(), (int)out.size(), 0);
    }

    void login(const std::wstring& user, const std::wstring& pass) {
        sendLine("LOGIN|" + wToUtf8(user) + "|" + wToUtf8(pass));
    }

    RemotePlayer* find(int id) {
        for (auto& p : players) if (p.id == id) return &p;
        return nullptr;
    }

    void addChat(const std::wstring& line) {
        chat.push_back(line);
        while (chat.size() > 50) chat.pop_front();
    }

    void handleLine(const std::string& line) {
        std::vector<std::string> f = splitStr(line, '|');
        if (f.empty()) return;
        if (f[0] == "OK" && f.size() >= 2) {
            myId = atoi(f[1].c_str());
            loggedIn = true;
            addChat(L"→ connected to New Eden relay");
        } else if (f[0] == "ERR" && f.size() >= 2) {
            lastError = utf8ToW(f[1]);
        } else if (f[0] == "JOIN" && f.size() >= 3) {
            RemotePlayer p;
            p.id = atoi(f[1].c_str());
            p.name = utf8ToW(f[2]);
            if (!find(p.id)) players.push_back(p);
            addChat(L"→ " + p.name + L" entered Local");
        } else if (f[0] == "LEAVE" && f.size() >= 2) {
            int id = atoi(f[1].c_str());
            for (size_t i = 0; i < players.size(); i++)
                if (players[i].id == id) {
                    addChat(L"← " + players[i].name + L" left Local");
                    players.erase(players.begin() + i);
                    break;
                }
        } else if (f[0] == "ST" && f.size() >= 14) {
            RemotePlayer* p = find(atoi(f[1].c_str()));
            if (!p) return;
            p->sys = atoi(f[2].c_str());
            V3 np = { (float)atof(f[3].c_str()), (float)atof(f[4].c_str()), (float)atof(f[5].c_str()) };
            p->fwd = norm(V3{ (float)atof(f[6].c_str()), (float)atof(f[7].c_str()), (float)atof(f[8].c_str()) });
            p->up = norm(V3{ (float)atof(f[9].c_str()), (float)atof(f[10].c_str()), (float)atof(f[11].c_str()) });
            p->speed = (float)atof(f[12].c_str());
            p->docked = f[13] == "1";
            p->pos = p->hasState ? lerp(p->pos, np, 0.5f) : np;
            p->hasState = true;
        } else if (f[0] == "CHAT" && f.size() >= 3) {
            addChat(utf8ToW(f[1]) + L" > " + utf8ToW(f[2]));
            sfx.push_back({ 2, {}, 0, false });
        } else if (f[0] == "FIRE" && f.size() >= 4) {
            int sid = atoi(f[1].c_str()), tid = atoi(f[2].c_str());
            beams.push_back({ sid, tid, 0.30f });
            if (sid == myId || tid == myId) sfx.push_back({ 0, {}, 0, false });
            else if (RemotePlayer* p = find(sid)) sfx.push_back({ 0, p->pos, p->sys, true });
        } else if (f[0] == "SOV" && f.size() >= 3) {
            sov[atoi(f[1].c_str())] = utf8ToW(f[2]);
        } else if (f[0] == "HP" && f.size() >= 4) {
            int id = atoi(f[1].c_str());
            float s = (float)atof(f[2].c_str()), a = (float)atof(f[3].c_str());
            if (id == myId) { shield = s; armor = a; }
            else if (RemotePlayer* p = find(id)) { p->shield = s; p->armor = a; }
        } else if (f[0] == "KILL" && f.size() >= 3) {
            int id = atoi(f[1].c_str());
            std::wstring killer = utf8ToW(f[2]);
            if (id == myId) {
                killedMe = true;
                killedBy = killer;
                sfx.push_back({ 1, {}, 0, false });
            } else if (RemotePlayer* p = find(id)) {
                booms.push_back({ p->pos, p->sys, 0.8f });
                sfx.push_back({ 1, p->pos, p->sys, true });
                p->docked = true;
                addChat(L"☠ " + p->name + L"'s ship destroyed by " + killer);
            }
        }
    }

    void poll() {
        if (!connected) return;
        char buf[4096];
        while (true) {
            int r = recv(sock, buf, sizeof(buf), 0);
            if (r > 0) { rx.append(buf, r); continue; }
            if (r == 0 || WSAGetLastError() != WSAEWOULDBLOCK) {
                disconnect();
                lastError = L"disconnected from server";
                addChat(L"← connection lost");
                return;
            }
            break;
        }
        size_t p;
        while ((p = rx.find('\n')) != std::string::npos) {
            std::string line = rx.substr(0, p);
            rx.erase(0, p + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) handleLine(line);
        }
    }

    void tickFx(float dt) {
        for (size_t i = 0; i < beams.size();)
            if ((beams[i].ttl -= dt) <= 0) beams.erase(beams.begin() + i); else i++;
        for (size_t i = 0; i < booms.size();)
            if ((booms[i].ttl -= dt) <= 0) booms.erase(booms.begin() + i); else i++;
    }
};
