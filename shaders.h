// shaders.h - all HLSL, compiled at runtime with D3DCompile.
#pragma once

static const char* g_hlsl = R"HLSL(
cbuffer CB : register(b0)
{
    row_major float4x4 world;
    row_major float4x4 viewProj;
    float4 camPos;    // xyz = camera pos, w = time
    float4 camRight;  // xyz, w = tan(fovX/2)
    float4 camUp;     // xyz, w = tan(fovY/2)
    float4 camFwd;    // xyz, w = warp factor 0..1
    float4 sunPos;    // xyz, w = sun radius
    float4 sunColor;  // rgb
    float4 nebulaA;   // rgb
    float4 nebulaB;   // rgb
    float4 tint;      // object tint rgb, a = intensity/alpha
    float4 colorB;    // secondary color (planets)
    float4 misc;      // x = warp, y = whiteout, z = aspect, w = free
};

struct VSO
{
    float4 pos : SV_Position;
    float3 wp  : TEXCOORD0;
    float3 n   : NORMAL;
    float4 c   : COLOR;
    float2 uv  : TEXCOORD1;
};

// ------------------------------------------------------------- vertex shaders
VSO VSMesh(float3 p : POSITION, float3 n : NORMAL, float4 c : COLOR)
{
    VSO o;
    float4 wp = mul(float4(p, 1), world);
    o.wp = wp.xyz;
    o.pos = mul(wp, viewProj);
    o.n = mul(float4(n, 0), world).xyz;
    o.c = c;
    o.uv = float2(0, 0);
    return o;
}

VSO VSFull(uint id : SV_VertexID)
{
    VSO o;
    float2 v = float2(id == 1 ? 3.0 : -1.0, id == 2 ? 3.0 : -1.0);
    o.pos = float4(v, 0.5, 1);
    o.uv = v;            // -1..1 clip-style coords, y up
    o.wp = float3(0, 0, 0); o.n = float3(0, 0, 1); o.c = float4(1, 1, 1, 1);
    return o;
}

// ----------------------------------------------------------------- noise lib
float h31(float3 p) { return frac(sin(dot(p, float3(127.1, 311.7, 74.7))) * 43758.5453); }
float3 h33(float3 p)
{
    return frac(sin(float3(dot(p, float3(127.1, 311.7, 74.7)),
                           dot(p, float3(269.5, 183.3, 246.1)),
                           dot(p, float3(113.5, 271.9, 124.6)))) * 43758.5453);
}
float h21(float2 p) { return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }

float vnoise(float3 p)
{
    float3 i = floor(p), f = frac(p);
    f = f * f * (3 - 2 * f);
    float v000 = h31(i), v100 = h31(i + float3(1,0,0)), v010 = h31(i + float3(0,1,0)), v110 = h31(i + float3(1,1,0));
    float v001 = h31(i + float3(0,0,1)), v101 = h31(i + float3(1,0,1)), v011 = h31(i + float3(0,1,1)), v111 = h31(i + float3(1,1,1));
    return lerp(lerp(lerp(v000, v100, f.x), lerp(v010, v110, f.x), f.y),
                lerp(lerp(v001, v101, f.x), lerp(v011, v111, f.x), f.y), f.z);
}
float fbm(float3 p)
{
    float v = 0, a = 0.5;
    [unroll] for (int i = 0; i < 4; i++) { v += a * vnoise(p); p = p * 2.13 + 1.71; a *= 0.5; }
    return v;
}

float3 starfield(float3 rd, float warp)
{
    float3 col = 0;
    [unroll] for (int k = 0; k < 2; k++)
    {
        float sc = k == 0 ? 22.0 : 48.0;
        float3 p = rd * sc;
        float3 id = floor(p);
        float3 j = h33(id);
        float presence = step(k == 0 ? 0.72 : 0.55, h31(id + 3.3));
        float3 sp = id + 0.15 + 0.7 * j;
        float d = length(p - sp);
        float mag = pow(h31(id + 19.3), 6.0);
        float star = exp(-d * d * (k == 0 ? 350.0 : 750.0)) * (k == 0 ? 9.0 : 3.0) * mag * presence;
        float3 stint = lerp(float3(0.70, 0.80, 1.0), float3(1.0, 0.85, 0.70), h31(id + 7.7));
        col += star * stint;
    }
    return col * (1.0 - 0.55 * warp);
}

float3 skyColor(float3 rd, float warp)
{
    float3 col = float3(0.004, 0.005, 0.010);
    // nebula
    float nb = fbm(rd * 2.5 + 13.7);
    nb = pow(saturate(nb * 1.35 - 0.28), 2.0);
    float nb2 = fbm(rd * 5.0 + 7.1);
    col += lerp(nebulaA.rgb, nebulaB.rgb, saturate(nb2 * 1.3)) * nb * 0.60 * (1.0 - 0.5 * warp);
    col += starfield(rd, warp);
    // sun disc + glow
    float3 sv = sunPos.xyz - camPos.xyz;
    float sdist = max(length(sv), 1.0);
    float3 sd = sv / sdist;
    float ang = acos(clamp(dot(rd, sd), -1.0, 1.0));
    float angR = asin(saturate(sunPos.w / sdist));
    float disc = smoothstep(angR * 1.03, angR * 0.96, ang);
    float glow = exp(-(ang - angR) * 16.0) * 0.55 + exp(-(ang - angR) * 3.0) * 0.12;
    col += sunColor.rgb * (disc * 5.0 + max(glow, 0.0));
    return col;
}

// -------------------------------------------------------------- pixel shaders
float4 PSSky(VSO i) : SV_Target
{
    float tanX = camRight.w, tanY = camUp.w;
    float3 rd = normalize(camFwd.xyz + i.uv.x * tanX * camRight.xyz + i.uv.y * tanY * camUp.xyz);
    float3 col = skyColor(rd, misc.x);
    col = 1.0 - exp(-col * 1.6);
    col += misc.y; // jump whiteout
    return float4(col, 1);
}

float4 PSLit(VSO i) : SV_Target
{
    float3 nrm = normalize(i.n);
    float3 L = sunPos.xyz - i.wp;
    L = normalize(L);
    float dif = saturate(dot(nrm, L));
    float3 V = normalize(camPos.xyz - i.wp);
    float3 amb = lerp(nebulaA.rgb, nebulaB.rgb, 0.5) * 0.35 + 0.075;
    float rim = pow(1.0 - saturate(dot(nrm, V)), 3.0) * 0.25;
    float spec = pow(saturate(dot(reflect(-L, nrm), V)), 24.0) * 0.4;
    float3 alb = i.c.rgb * tint.rgb;
    float3 col = alb * (dif * sunColor.rgb + amb) + (spec + rim) * sunColor.rgb * 0.6 * dif;
    col += i.c.rgb * i.c.a * 2.4; // emissive
    col = 1.0 - exp(-col * 1.5);
    return float4(col, 1);
}

float4 PSPlanet(VSO i) : SV_Target
{
    float3 nrm = normalize(i.n);
    float3 center = world[3].xyz;
    float3 nl = normalize(i.wp - center);
    float band = 0.5 + 0.5 * sin((nl.y + fbm(nl * 3.0) * 0.85) * 7.0);
    float detail = fbm(nl * 9.0 + 4.2);
    float3 alb = lerp(tint.rgb, colorB.rgb, saturate(band * 0.8 + detail * 0.4));
    float3 L = normalize(sunPos.xyz - i.wp);
    float dif = saturate(dot(nrm, L));
    float3 V = normalize(camPos.xyz - i.wp);
    float fres = pow(1.0 - saturate(dot(nrm, V)), 2.6);
    float3 atmo = lerp(colorB.rgb, float3(1, 1, 1), 0.45);
    float3 col = alb * (dif * sunColor.rgb * 1.15 + 0.035)
               + fres * atmo * (0.16 + dif * 0.55);
    col = 1.0 - exp(-col * 1.7);
    return float4(col, 1);
}

// billboard glow; quad corner uv packed in normal.xy (-1..1)
float4 PSGlow(VSO i) : SV_Target
{
    float r2 = dot(i.n.xy, i.n.xy);
    float g = exp(-r2 * 5.0) * 0.8 + exp(-r2 * 28.0) * 1.7;
    return float4(tint.rgb * g * tint.a, 1);
}

// fullscreen additive warp tunnel
float4 PSWarp(VSO i) : SV_Target
{
    float2 p = float2(i.uv.x * misc.z, i.uv.y);
    float warp = misc.x;
    float t = camPos.w;
    float r = length(p) + 0.001;
    float a = atan2(p.y, p.x) / 6.2831853 + 0.5; // 0..1
    float3 col = 0;
    [unroll] for (int k = 0; k < 2; k++)
    {
        float lanes = k == 0 ? 90.0 : 48.0;
        float lane = a * lanes;
        float id = floor(lane);
        float h = h21(float2(id, k * 13.1));
        float on = step(0.45, h21(float2(id, k * 7.3 + 2.1))); // not every lane lit
        float core = smoothstep(0.38, 0.04, abs(frac(lane) - 0.5));
        float ph = frac(h * 9.0 + t * (1.4 + h * 2.2) - (0.22 + h * 0.18) / r);
        float streak = core * on * smoothstep(0.0, 0.3, ph) * smoothstep(1.0, 0.5, ph);
        streak *= smoothstep(0.12, 0.50, r) * (0.35 + 0.65 * h);
        col += streak * lerp(float3(0.35, 0.65, 1.0), float3(1, 1, 1), h);
    }
    col *= warp * 0.9;
    col += misc.y; // whiteout
    return float4(col, 1);
}

// ------------------------------------------------- hangar (docked backdrop)
float2 rot2(float2 p, float a) { float c = cos(a), s = sin(a); return float2(c * p.x - s * p.y, s * p.x + c * p.y); }

float sdRBox(float2 p, float2 b, float r)
{
    float2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

float4 PSHangar(VSO i) : SV_Target
{
    float t = camPos.w;
    float2 p = float2(i.uv.x * misc.z, i.uv.y); // aspect-corrected, y up
    float3 col = float3(0.012, 0.018, 0.026);

    // back wall: vertical gradient + panel seams
    col += float3(0.020, 0.030, 0.042) * saturate(0.6 + 0.5 * p.y);
    float seamV = smoothstep(0.480, 0.498, abs(frac(p.x * 2.2 + 0.5) - 0.5));
    float seamH = smoothstep(0.475, 0.495, abs(frac(p.y * 3.0 + 0.5) - 0.5));
    col *= 1.0 - 0.35 * max(seamV, seamH);

    // ---- big viewport window onto space
    float2 wp = p - float2(0.0, 0.34);
    float dwin = sdRBox(wp, float2(1.10, 0.40), 0.06);
    if (dwin < 0.0)
    {
        float3 rd = normalize(float3(wp.x, wp.y + 0.1, 1.35));
        rd.xz = rot2(rd.xz, t * 0.015);
        float3 sky = float3(0.006, 0.009, 0.018);
        float nb = pow(saturate(fbm(rd * 2.6 + 13.7) * 1.5 - 0.25), 1.8);
        sky += lerp(nebulaA.rgb, nebulaB.rgb, saturate(fbm(rd * 5.0 + 7.1) * 1.3)) * nb * 1.1;
        sky += starfield(rd, 0.0) * 2.2;
        // distant sun peeking at window edge
        float sg = exp(-length(wp - float2(0.85, 0.18)) * 3.0);
        sky += sunColor.rgb * sg * 0.8;
        col = 1.0 - exp(-sky * 2.2);
        // window struts
        float strut = smoothstep(0.455, 0.470, abs(frac(wp.x * 1.4 + 0.5) - 0.5));
        col = lerp(col, float3(0.05, 0.06, 0.08), strut);
    }
    // glowing window frame
    col += float3(0.25, 0.65, 0.85) * smoothstep(0.016, 0.0, abs(dwin) - 0.006) * 0.8;

    // ---- floor
    if (p.y < -0.28)
    {
        float fy = (-0.28 - p.y);
        float3 fcol = float3(0.020, 0.026, 0.034) + float3(0.012, 0.018, 0.026) * fy * 1.5;
        // faint reflection of the window glow
        fcol += float3(0.04, 0.10, 0.14) * exp(-fy * 4.0) * smoothstep(1.3, 0.0, abs(p.x));
        // perspective floor lines
        float persp = abs(p.x) / max(fy + 0.10, 0.02);
        fcol *= 1.0 - 0.25 * smoothstep(0.465, 0.495, abs(frac(persp * 0.8 + 0.5) - 0.5));
        fcol *= 1.0 - 0.30 * smoothstep(0.455, 0.490, abs(frac(fy * 6.0 + 0.5) - 0.5));
        col = fcol;
        // landing pad glow ellipse under ship
        float pad = length((p - float2(0, -0.52)) * float2(1.0, 3.2));
        col += float3(0.14, 0.60, 0.72) * smoothstep(0.05, 0.0, abs(pad - 0.52) - 0.012);
        col += float3(0.07, 0.30, 0.38) * exp(-pad * pad * 3.0) * (0.7 + 0.15 * sin(t * 2.0));
    }

    // hazard stripe band at floor edge
    float band = smoothstep(0.012, 0.004, abs(p.y + 0.28) - 0.014);
    float stripes = step(0.5, frac((p.x + p.y + t * 0.02) * 9.0));
    col = lerp(col, lerp(float3(0.45, 0.30, 0.05), float3(0.04, 0.04, 0.04), stripes), band * 0.85);

    // ---- the ship silhouette, hovering
    float2 sp = p - float2(0.0, -0.30 + 0.012 * sin(t * 0.9));
    float hull = length(sp * float2(1.0, 3.4)) - 0.34;
    float fin = sdRBox(sp - float2(0.0, 0.10), float2(0.025, 0.10), 0.01);
    float nose = sdRBox(rot2(sp - float2(0.30, 0.0), 0.0), float2(0.10, 0.035), 0.02);
    float d = min(hull, min(fin, nose));
    float shipMask = smoothstep(0.004, -0.004, d);
    float3 shipCol = float3(0.055, 0.068, 0.088) + float3(0.05, 0.06, 0.07) * saturate(sp.y * 4.0 + 0.7);
    // rim light from window above + warm floor bounce
    shipCol += float3(0.22, 0.45, 0.60) * smoothstep(0.035, 0.0, abs(d) - 0.002) * saturate(sp.y * 6.0 + 0.8);
    shipCol += float3(0.55, 0.32, 0.08) * smoothstep(0.030, 0.0, abs(d) - 0.002) * saturate(-sp.y * 6.0);
    col = lerp(col, shipCol, shipMask);
    // engine idle glow
    col += float3(0.20, 0.55, 0.85) * exp(-length((sp - float2(-0.40, 0.0)) * float2(6.0, 14.0))) * (0.9 + 0.3 * sin(t * 5.0));
    // blinking nav lights
    col += float3(1.0, 0.2, 0.15) * exp(-length(sp - float2(0.42, 0.02)) * 90.0) * step(0.6, frac(t * 0.8));
    col += float3(0.2, 1.0, 0.4) * exp(-length(sp - float2(-0.42, 0.06)) * 90.0) * step(0.7, frac(t * 0.6 + 0.3));

    // holo ring rotating around the ship
    float2 hp = sp * float2(1.0, 3.0);
    float ringd = abs(length(hp) - 0.50);
    float ha = atan2(hp.y, hp.x) + t * 0.7;
    float dash = step(0.30, frac(ha * 1.909859)); // 12 dashes
    col += float3(0.15, 0.7, 0.9) * smoothstep(0.010, 0.0, ringd - 0.004) * dash * 0.55;

    // overhead light shafts
    [unroll] for (int kk = 0; kk < 3; kk++)
    {
        float lx = -1.1 + 1.1 * kk;
        float shaft = exp(-abs(p.x - lx) * (5.0 - p.y * 2.0)) * saturate(p.y * 0.5 + 0.75);
        col += float3(0.030, 0.045, 0.060) * shaft;
    }

    // status lights row, bottom edge
    [unroll] for (int b = 0; b < 6; b++)
    {
        float2 bp = p - float2(-1.45 + 0.10 * b, -0.93);
        float on = step(0.5, h21(float2(b, floor(t * (0.5 + b * 0.21)))));
        col += lerp(float3(0.9, 0.25, 0.1), float3(0.2, 0.9, 0.4), on) * exp(-dot(bp, bp) * 2600.0) * 1.2;
    }

    // vignette + scanlines
    col = col * 1.35 + float3(0.004, 0.006, 0.009);
    col *= 1.0 - 0.45 * dot(i.uv * 0.62, i.uv * 0.62);
    col *= 0.97 + 0.03 * sin(i.uv.y * 700.0);
    col += misc.y; // undock flash
    return float4(col, 1);
}
)HLSL";
