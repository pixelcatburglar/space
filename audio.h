// audio.h - XAudio2 output; every sound and both music tracks are synthesized
// at startup (no asset files). 44.1 kHz, 16-bit mono.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xaudio2.h>
#include <vector>
#include <cmath>
#include <cstdint>
#include "math3d.h"
#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

static const int SR = 44100;
static const float TAU = 6.2831853f;

// deterministic noise
struct Rng {
    uint32_t s = 0x12345u;
    float next() { s = s * 1664525u + 1013904223u; return (s >> 8) / 16777216.0f * 2.0f - 1.0f; } // -1..1
    float uni() { return next() * 0.5f + 0.5f; }
};

static float midiHz(int n) { return 440.0f * powf(2.0f, (n - 69) / 12.0f); }

// add a note into a float mix buffer; writes wrap modulo N so loops stay seamless
static void addNote(std::vector<float>& b, float t0, float dur, float freq, float amp,
                    float attack, float release, float detune = 0.0015f, float h2 = 0.25f, float h3 = 0.10f) {
    int N = (int)b.size();
    int n = (int)(dur * SR);
    float ph1 = 0, ph2 = 0;
    float w1 = TAU * freq * (1 + detune) / SR, w2 = TAU * freq * (1 - detune) / SR;
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR;
        float env = 1.0f;
        if (t < attack) env = t / attack;
        float tail = dur - t;
        if (tail < release) env *= tail / release;
        env = env * env * (3 - 2 * env); // smooth
        float s = 0.5f * (sinf(ph1) + sinf(ph2));
        s += h2 * sinf(ph1 * 2) + h3 * sinf(ph1 * 3);
        b[(size_t)(((int)(t0 * SR) + i) % N)] += s * amp * env;
        ph1 += w1; ph2 += w2;
    }
}

// bell: exponentially decaying sine with an inharmonic partial
static void addBell(std::vector<float>& b, float t0, float freq, float amp, float decay) {
    int N = (int)b.size();
    int n = (int)(decay * 4.0f * SR);
    float w1 = TAU * freq / SR, w2 = TAU * freq * 2.76f / SR;
    float ph1 = 0, ph2 = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR;
        float env = expf(-t / decay) * fminf(t / 0.004f, 1.0f);
        float s = sinf(ph1) + 0.35f * sinf(ph2) * expf(-t * 3.0f);
        b[(size_t)(((int)(t0 * SR) + i) % N)] += s * amp * env;
        ph1 += w1; ph2 += w2;
    }
}

static std::vector<int16_t> toPcm(const std::vector<float>& f, float gain) {
    std::vector<int16_t> out(f.size());
    for (size_t i = 0; i < f.size(); i++) {
        float v = tanhf(f[i] * gain);
        out[i] = (int16_t)(v * 32000.0f);
    }
    return out;
}

static bool writeWav(const wchar_t* path, const std::vector<int16_t>& pcm) {
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path, L"wb") != 0 || !fp) return false;
    uint32_t dataBytes = (uint32_t)(pcm.size() * 2);
    uint32_t riffSize = 36 + dataBytes;
    uint32_t fmtSize = 16, byteRate = SR * 2;
    uint16_t fmtTag = 1, channels = 1, blockAlign = 2, bits = 16;
    uint32_t sr = SR;
    fwrite("RIFF", 1, 4, fp); fwrite(&riffSize, 4, 1, fp); fwrite("WAVE", 1, 4, fp);
    fwrite("fmt ", 1, 4, fp); fwrite(&fmtSize, 4, 1, fp);
    fwrite(&fmtTag, 2, 1, fp); fwrite(&channels, 2, 1, fp); fwrite(&sr, 4, 1, fp);
    fwrite(&byteRate, 4, 1, fp); fwrite(&blockAlign, 2, 1, fp); fwrite(&bits, 2, 1, fp);
    fwrite("data", 1, 4, fp); fwrite(&dataBytes, 4, 1, fp);
    fwrite(pcm.data(), 2, pcm.size(), fp);
    fclose(fp);
    return true;
}

static void pcmStats(const std::vector<int16_t>& pcm, int& peak, int& rms) {
    long long sum = 0;
    peak = 0;
    for (int16_t s : pcm) {
        int a = s < 0 ? -s : s;
        if (a > peak) peak = a;
        sum += (long long)s * s;
    }
    rms = pcm.empty() ? 0 : (int)sqrt((double)sum / pcm.size());
}

// --------------------------------------------------------------------- SFX
static std::vector<int16_t> synthPew() {
    int n = (int)(0.26f * SR);
    std::vector<float> b(n, 0.0f);
    Rng rng;
    float ph = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR;
        float f = 1500.0f * expf(-t * 9.0f) + 240.0f;
        ph += TAU * f / SR;
        float env = expf(-t * 16.0f) * fminf(t / 0.002f, 1.0f);
        float s = sinf(ph) + 0.4f * sinf(ph * 2) + 0.15f * sinf(ph * 3);
        s += rng.next() * 0.25f * expf(-t * 55.0f); // muzzle crack
        b[i] = s * env;
    }
    return toPcm(b, 1.1f);
}

static std::vector<int16_t> synthBoom() {
    int n = (int)(1.6f * SR);
    std::vector<float> b(n, 0.0f);
    Rng rng;
    float lp = 0, ph = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR;
        float fc = 2400.0f * expf(-t * 3.0f) + 60.0f;
        float a = 1.0f - expf(-TAU * fc / SR);
        lp += a * (rng.next() - lp);
        float s = lp * 1.8f * expf(-t * 2.2f);
        float f = 30.0f + 65.0f * expf(-t * 4.0f);
        ph += TAU * f / SR;
        s += sinf(ph) * 0.9f * expf(-t * 2.6f);            // sub thump
        if (rng.uni() < 0.0015f * expf(-t * 2.0f)) s += rng.next() * 1.5f; // debris crackle
        b[i] = s * fminf(t / 0.004f, 1.0f);
    }
    return toPcm(b, 1.3f);
}

static std::vector<int16_t> synthWhoosh() {
    int n = (int)(1.7f * SR);
    std::vector<float> b(n, 0.0f);
    Rng rng;
    float lp1 = 0, lp2 = 0, ph = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR;
        float rise = fminf(t / 1.0f, 1.0f);
        float fc = 250.0f + 3800.0f * rise * rise;
        float a1 = 1.0f - expf(-TAU * fc / SR);
        float a2 = 1.0f - expf(-TAU * fc * 0.4f / SR);
        float x = rng.next();
        lp1 += a1 * (x - lp1);
        lp2 += a2 * (x - lp2);
        float band = (lp1 - lp2) * 2.2f;
        float env = rise * (t < 1.15f ? 1.0f : expf(-(t - 1.15f) * 5.0f));
        float f = 50.0f + 130.0f * rise;
        ph += TAU * f / SR;
        b[i] = (band + 0.35f * sinf(ph)) * env;
    }
    return toPcm(b, 0.8f);
}

static std::vector<int16_t> synthZap() {
    int n = (int)(0.6f * SR);
    std::vector<float> b(n, 0.0f);
    Rng rng;
    float ph = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR;
        float f = 180.0f + 2400.0f * (t / 0.6f) + 220.0f * sinf(TAU * 27.0f * t);
        ph += TAU * f / SR;
        float env = expf(-t * 5.0f) * fminf(t / 0.004f, 1.0f);
        float s = sinf(ph) + 0.3f * sinf(ph * 2);
        s += rng.next() * 0.3f * expf(-t * 10.0f);
        b[i] = s * env;
    }
    return toPcm(b, 0.9f);
}

static std::vector<int16_t> synthThunk() {
    int n = (int)(0.5f * SR);
    std::vector<float> b(n, 0.0f);
    Rng rng;
    float ph = 0, p1 = 0, p2 = 0, p3 = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR;
        ph += TAU * 72.0f / SR;
        p1 += TAU * 317.0f / SR; p2 += TAU * 524.0f / SR; p3 += TAU * 791.0f / SR;
        float s = sinf(ph) * expf(-t * 11.0f) * 1.2f;
        s += (sinf(p1) + 0.7f * sinf(p2) + 0.5f * sinf(p3)) * 0.16f * expf(-t * 8.0f);
        s += rng.next() * 0.5f * expf(-t * 120.0f);
        b[i] = s * fminf(t / 0.002f, 1.0f);
    }
    return toPcm(b, 1.0f);
}

static std::vector<int16_t> synthBlip() {
    int n = (int)(0.16f * SR);
    std::vector<float> b(n, 0.0f);
    float ph = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR;
        float f = t < 0.07f ? 980.0f : 1470.0f;
        ph += TAU * f / SR;
        float env = fminf(t / 0.008f, 1.0f) * fminf((0.16f - t) / 0.05f, 1.0f);
        b[i] = sinf(ph) * env * 0.6f;
    }
    return toPcm(b, 0.7f);
}

// seamless 1 s engine hum loop (integer-Hz partials so the loop is click-free)
static std::vector<int16_t> synthHum() {
    int n = SR;
    std::vector<float> b(n, 0.0f);
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR;
        float s = 0.50f * sinf(TAU * 55 * t) + 0.22f * sinf(TAU * 56 * t)   // 1 Hz beat
                + 0.30f * sinf(TAU * 110 * t) + 0.12f * sinf(TAU * 165 * t)
                + 0.08f * sinf(TAU * 220 * t);
        b[i] = s * 0.55f;
    }
    return toPcm(b, 0.8f);
}

// ------------------------------------------------------------------- music
// warm hangar loop: Am - F - C - G twice, soft pads + bass + pentatonic bells
static std::vector<int16_t> synthMusicDocked() {
    const float chordDur = 4.5f;
    const int chords[8][4] = {
        {57,60,64,69}, {53,57,60,65}, {48,52,55,60}, {55,59,62,67},
        {57,60,64,69}, {53,57,60,65}, {48,52,55,60}, {55,59,62,67},
    };
    int N = (int)(8 * chordDur * SR);
    std::vector<float> b(N, 0.0f);
    for (int c = 0; c < 8; c++) {
        float t0 = c * chordDur;
        for (int v = 0; v < 4; v++)
            addNote(b, t0, chordDur + 1.8f, midiHz(chords[c][v]), 0.055f, 1.6f, 2.0f);
        addNote(b, t0, chordDur + 1.0f, midiHz(chords[c][0] - 24), 0.10f, 0.8f, 1.4f, 0.0005f, 0.0f, 0.0f); // bass
    }
    Rng rng; rng.s = 777;
    const int pent[6] = { 69, 72, 74, 76, 79, 81 };
    for (int k = 0; k < 22; k++) {
        float t0 = floorf(rng.uni() * 48.0f) * 0.75f;
        addBell(b, t0, midiHz(pent[(int)(rng.uni() * 5.99f)]), 0.035f, 1.6f);
    }
    return toPcm(b, 1.2f);
}

// dark space loop: D drone, slow chord colors, sparse high bells, void wash
static std::vector<int16_t> synthMusicSpace() {
    const float dur = 48.0f;
    int N = (int)(dur * SR);
    std::vector<float> b(N, 0.0f);
    // constant drones with loop-periodic LFO (period divides 48 s)
    struct Drone { int midi; float amp; float lfoPeriod, lfoPhase; };
    Drone drones[4] = { {38, 0.060f, 12, 0.0f}, {45, 0.045f, 16, 1.7f}, {50, 0.040f, 12, 3.1f}, {26, 0.085f, 24, 0.5f} };
    for (const Drone& d : drones) {
        float w = TAU * midiHz(d.midi) / SR, w2 = TAU * midiHz(d.midi) * 1.002f / SR;
        float ph = 0, ph2 = 0;
        for (int i = 0; i < N; i++) {
            float t = (float)i / SR;
            float lfo = 0.7f + 0.3f * sinf(TAU * t / d.lfoPeriod + d.lfoPhase);
            b[i] += (sinf(ph) * 0.6f + sinf(ph2) * 0.4f) * d.amp * lfo;
            ph += w; ph2 += w2;
        }
    }
    // chord colors, 12 s each: Dm, Bb, F, Am
    const int colors[4][2] = { {53,57}, {46,53}, {53,60}, {52,57} };
    for (int c = 0; c < 4; c++)
        for (int v = 0; v < 2; v++)
            addNote(b, c * 12.0f, 13.5f, midiHz(colors[c][v]), 0.035f, 4.0f, 4.5f);
    // sparse high bells (D minor pentatonic, up an octave)
    Rng rng; rng.s = 4242;
    const int pent[5] = { 74, 77, 79, 81, 84 };
    for (int k = 0; k < 11; k++) {
        float t0 = floorf(rng.uni() * 32.0f) * 1.5f;
        addBell(b, t0, midiHz(pent[(int)(rng.uni() * 4.99f)]), 0.028f, 2.6f);
    }
    // faint void wash
    float lp = 0;
    Rng wr;
    for (int i = 0; i < N; i++) {
        float t = (float)i / SR;
        lp += 0.012f * (wr.next() - lp);
        b[i] += lp * 0.35f * (0.6f + 0.4f * sinf(TAU * t / 16.0f));
    }
    return toPcm(b, 1.2f);
}

// ------------------------------------------------------------------ engine
struct Audio {
    bool ok = false;
    bool muted = false;
    IXAudio2* xa = nullptr;
    IXAudio2MasteringVoice* master = nullptr;
    IXAudio2SourceVoice* vMusicDock = nullptr;
    IXAudio2SourceVoice* vMusicSpace = nullptr;
    IXAudio2SourceVoice* vHum = nullptr;
    static const int POOL = 10;
    IXAudio2SourceVoice* pool[POOL] = {};
    int poolIdx = 0;
    float dockVol = 1.0f, spaceVol = 0.0f;
    uint32_t jit = 99;

    std::vector<int16_t> pewB, boomB, whooshB, zapB, thunkB, blipB, humB, mDockB, mSpaceB;

    IXAudio2SourceVoice* makeVoice(float maxRatio = 4.0f) {
        WAVEFORMATEX f = {};
        f.wFormatTag = WAVE_FORMAT_PCM;
        f.nChannels = 1;
        f.nSamplesPerSec = SR;
        f.wBitsPerSample = 16;
        f.nBlockAlign = 2;
        f.nAvgBytesPerSec = SR * 2;
        IXAudio2SourceVoice* v = nullptr;
        xa->CreateSourceVoice(&v, &f, 0, maxRatio);
        return v;
    }

    void submitLoop(IXAudio2SourceVoice* v, const std::vector<int16_t>& buf, float vol) {
        if (!v) return;
        XAUDIO2_BUFFER xb = {};
        xb.AudioBytes = (UINT32)(buf.size() * 2);
        xb.pAudioData = (const BYTE*)buf.data();
        xb.LoopCount = XAUDIO2_LOOP_INFINITE;
        v->SubmitSourceBuffer(&xb);
        v->SetVolume(vol);
        v->Start();
    }

    bool init() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(XAudio2Create(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR))) return false;
        if (FAILED(xa->CreateMasteringVoice(&master))) return false;
        master->SetVolume(0.85f);

        pewB = synthPew(); boomB = synthBoom(); whooshB = synthWhoosh();
        zapB = synthZap(); thunkB = synthThunk(); blipB = synthBlip();
        humB = synthHum(); mDockB = synthMusicDocked(); mSpaceB = synthMusicSpace();

        vMusicDock = makeVoice(2.0f);
        vMusicSpace = makeVoice(2.0f);
        vHum = makeVoice(4.0f);
        for (int i = 0; i < POOL; i++) pool[i] = makeVoice(4.0f);
        if (!vMusicDock || !vMusicSpace || !vHum) return false;

        submitLoop(vMusicDock, mDockB, 0.55f);
        submitLoop(vMusicSpace, mSpaceB, 0.0f);
        submitLoop(vHum, humB, 0.0f);
        ok = true;
        return true;
    }

    void play(const std::vector<int16_t>& buf, float vol, float pitch = 1.0f) {
        if (!ok || vol <= 0.001f) return;
        IXAudio2SourceVoice* v = nullptr;
        for (int i = 0; i < POOL; i++) {
            XAUDIO2_VOICE_STATE st;
            pool[i]->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            if (st.BuffersQueued == 0) { v = pool[i]; break; }
        }
        if (!v) { v = pool[poolIdx]; poolIdx = (poolIdx + 1) % POOL; v->Stop(); v->FlushSourceBuffers(); }
        XAUDIO2_BUFFER xb = {};
        xb.AudioBytes = (UINT32)(buf.size() * 2);
        xb.pAudioData = (const BYTE*)buf.data();
        v->SetVolume(vol);
        v->SetFrequencyRatio(pitch);
        v->SubmitSourceBuffer(&xb);
        v->Start();
    }

    float jitter() { jit = jit * 1664525u + 1013904223u; return (jit >> 8) / 16777216.0f; }

    // one-shots
    void pew(float vol)    { play(pewB, vol, 0.92f + 0.16f * jitter()); }
    void boom(float vol)   { play(boomB, vol, 0.90f + 0.20f * jitter()); }
    void whoosh()          { play(whooshB, 0.8f); }
    void zap()             { play(zapB, 0.8f); }
    void thunk()           { play(thunkB, 0.9f); }
    void blip()            { play(blipB, 0.5f); }

    void update(float dt, bool dockedView, float throttle, float warpVis) {
        if (!ok) return;
        master->SetVolume(muted ? 0.0f : 0.85f);
        float target = dockedView ? 1.0f : 0.0f;
        float a = 1.0f - expf(-dt * 1.3f);
        dockVol += (target - dockVol) * a;
        spaceVol += ((1.0f - target) - spaceVol) * a;
        vMusicDock->SetVolume(dockVol * 0.55f);
        vMusicSpace->SetVolume(spaceVol * 0.50f);
        float humV = dockedView ? 0.0f : (0.10f + 0.22f * throttle + 0.38f * warpVis);
        vHum->SetVolume(humV);
        vHum->SetFrequencyRatio(1.0f + 0.55f * throttle + 1.15f * warpVis);
    }

    void shutdown() {
        if (xa) { xa->StopEngine(); }
        ok = false;
    }

    void dumpWavs() {
        struct { const wchar_t* name; std::vector<int16_t>* b; } all[] = {
            { L"snd_pew.wav", &pewB }, { L"snd_explosion.wav", &boomB },
            { L"snd_warp.wav", &whooshB }, { L"snd_jump.wav", &zapB },
            { L"snd_dock.wav", &thunkB }, { L"snd_chat.wav", &blipB },
            { L"snd_engine_loop.wav", &humB },
            { L"music_hangar.wav", &mDockB }, { L"music_space.wav", &mSpaceB },
        };
        for (auto& w : all) writeWav(w.name, *w.b);
    }
};
