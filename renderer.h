// renderer.h - D3D11 + D2D/DWrite setup and draw helpers.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <d2d1.h>
#include <dwrite.h>
#include <cstdio>
#include <vector>
#include <string>
#include "math3d.h"
#include "mesh.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "user32.lib")

template <class T> void safeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

inline void logLine(const char* fmt, ...) {
    char buf[2048];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    FILE* f = nullptr;
    if (fopen_s(&f, "space_log.txt", "a") == 0 && f) { fprintf(f, "%s\n", buf); fclose(f); }
    OutputDebugStringA(buf); OutputDebugStringA("\n");
}

struct CB { // must match cbuffer CB in shaders.h
    M4 world, viewProj;
    float camPos[4], camRight[4], camUp[4], camFwd[4];
    float sunPos[4], sunColor[4];
    float nebulaA[4], nebulaB[4];
    float tint[4], colorB[4], misc[4];
};

struct GpuMesh {
    ID3D11Buffer* vb = nullptr;
    ID3D11Buffer* ib = nullptr;
    UINT idxCount = 0;
};

struct Renderer {
    int width = 1600, height = 900;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain* swap = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11Texture2D* depthTex = nullptr;

    ID3D11VertexShader* vsMesh = nullptr;
    ID3D11VertexShader* vsFull = nullptr;
    ID3D11PixelShader* psSky = nullptr;
    ID3D11PixelShader* psLit = nullptr;
    ID3D11PixelShader* psPlanet = nullptr;
    ID3D11PixelShader* psGlow = nullptr;
    ID3D11PixelShader* psWarp = nullptr;
    ID3D11PixelShader* psHangar = nullptr;
    ID3D11InputLayout* layout = nullptr;
    ID3D11Buffer* cbuf = nullptr;
    ID3D11Buffer* dynVB = nullptr; // billboard quads

    ID3D11DepthStencilState* dsOn = nullptr;
    ID3D11DepthStencilState* dsOff = nullptr;
    ID3D11DepthStencilState* dsReadOnly = nullptr;
    ID3D11BlendState* blOpaque = nullptr;
    ID3D11BlendState* blAdd = nullptr;
    ID3D11RasterizerState* raster = nullptr;

    // D2D / DWrite
    ID2D1Factory* d2dFactory = nullptr;
    ID2D1RenderTarget* d2dRT = nullptr;
    IDWriteFactory* dwFactory = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    IDWriteTextFormat* fmtSmall = nullptr;   // 13
    IDWriteTextFormat* fmtBody = nullptr;    // 16
    IDWriteTextFormat* fmtHead = nullptr;    // 22
    IDWriteTextFormat* fmtTitle = nullptr;   // 36

    CB cb = {};

    bool compileShader(const char* src, const char* entry, const char* target, ID3DBlob** blob) {
        ID3DBlob* err = nullptr;
        HRESULT hr = D3DCompile(src, strlen(src), "shaders", nullptr, nullptr, entry, target,
                                D3DCOMPILE_OPTIMIZATION_LEVEL2, 0, blob, &err);
        if (FAILED(hr)) {
            logLine("shader %s failed: %s", entry, err ? (const char*)err->GetBufferPointer() : "(no log)");
            if (err) err->Release();
            return false;
        }
        if (err) err->Release();
        return true;
    }

    bool init(HWND hwnd, const char* hlsl) {
        DXGI_SWAP_CHAIN_DESC sd = {};
        sd.BufferCount = 1;
        sd.BufferDesc.Width = width; sd.BufferDesc.Height = height;
        sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.BufferDesc.RefreshRate = { 60, 1 };
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hwnd;
        sd.SampleDesc = { 1, 0 };
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0, got;
        HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1, D3D11_SDK_VERSION, &sd, &swap, &dev, &got, &ctx);
        if (FAILED(hr)) { logLine("D3D11CreateDeviceAndSwapChain failed 0x%08lx", hr); return false; }

        ID3D11Texture2D* back = nullptr;
        swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back);
        dev->CreateRenderTargetView(back, nullptr, &rtv);

        // D2D on the backbuffer surface
        IDXGISurface* surf = nullptr;
        back->QueryInterface(__uuidof(IDXGISurface), (void**)&surf);
        back->Release();
        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2dFactory);
        if (FAILED(hr)) { logLine("D2D1CreateFactory failed 0x%08lx", hr); return false; }
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        hr = d2dFactory->CreateDxgiSurfaceRenderTarget(surf, &props, &d2dRT);
        surf->Release();
        if (FAILED(hr)) { logLine("CreateDxgiSurfaceRenderTarget failed 0x%08lx", hr); return false; }
        d2dRT->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brush);

        hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&dwFactory);
        if (FAILED(hr)) { logLine("DWriteCreateFactory failed 0x%08lx", hr); return false; }
        dwFactory->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us", &fmtSmall);
        dwFactory->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"en-us", &fmtBody);
        dwFactory->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 22.0f, L"en-us", &fmtHead);
        dwFactory->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 36.0f, L"en-us", &fmtTitle);

        // depth buffer
        D3D11_TEXTURE2D_DESC dt = {};
        dt.Width = width; dt.Height = height;
        dt.MipLevels = 1; dt.ArraySize = 1;
        dt.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dt.SampleDesc = { 1, 0 };
        dt.Usage = D3D11_USAGE_DEFAULT;
        dt.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        dev->CreateTexture2D(&dt, nullptr, &depthTex);
        dev->CreateDepthStencilView(depthTex, nullptr, &dsv);

        // shaders
        ID3DBlob* b = nullptr;
        if (!compileShader(hlsl, "VSMesh", "vs_5_0", &b)) return false;
        dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vsMesh);
        D3D11_INPUT_ELEMENT_DESC ied[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        dev->CreateInputLayout(ied, 3, b->GetBufferPointer(), b->GetBufferSize(), &layout);
        b->Release();
        if (!compileShader(hlsl, "VSFull", "vs_5_0", &b)) return false;
        dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vsFull); b->Release();
        struct { const char* e; ID3D11PixelShader** p; } ps[] = {
            { "PSSky", &psSky }, { "PSLit", &psLit }, { "PSPlanet", &psPlanet },
            { "PSGlow", &psGlow }, { "PSWarp", &psWarp }, { "PSHangar", &psHangar },
        };
        for (auto& s : ps) {
            if (!compileShader(hlsl, s.e, "ps_5_0", &b)) return false;
            dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, s.p);
            b->Release();
        }

        // constant buffer
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = sizeof(CB);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        dev->CreateBuffer(&bd, nullptr, &cbuf);

        // dynamic billboard VB (one quad)
        D3D11_BUFFER_DESC dvb = {};
        dvb.ByteWidth = sizeof(Vtx) * 6;
        dvb.Usage = D3D11_USAGE_DYNAMIC;
        dvb.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        dvb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        dev->CreateBuffer(&dvb, nullptr, &dynVB);

        // states
        D3D11_DEPTH_STENCIL_DESC ds = {};
        ds.DepthEnable = TRUE;
        ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        ds.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        dev->CreateDepthStencilState(&ds, &dsOn);
        ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        dev->CreateDepthStencilState(&ds, &dsReadOnly);
        ds.DepthEnable = FALSE;
        dev->CreateDepthStencilState(&ds, &dsOff);

        D3D11_BLEND_DESC bl = {};
        bl.RenderTarget[0].BlendEnable = FALSE;
        bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        dev->CreateBlendState(&bl, &blOpaque);
        bl.RenderTarget[0].BlendEnable = TRUE;
        bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        bl.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        dev->CreateBlendState(&bl, &blAdd);

        D3D11_RASTERIZER_DESC rs = {};
        rs.FillMode = D3D11_FILL_SOLID;
        rs.CullMode = D3D11_CULL_NONE;
        rs.DepthClipEnable = TRUE;
        dev->CreateRasterizerState(&rs, &raster);

        D3D11_VIEWPORT vp = { 0, 0, (float)width, (float)height, 0, 1 };
        ctx->RSSetViewports(1, &vp);
        return true;
    }

    GpuMesh createMesh(const MeshData& md) {
        GpuMesh g;
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = (UINT)(md.verts.size() * sizeof(Vtx));
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA sd = { md.verts.data() };
        dev->CreateBuffer(&bd, &sd, &g.vb);
        bd.ByteWidth = (UINT)(md.idx.size() * sizeof(uint32_t));
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        sd.pSysMem = md.idx.data();
        dev->CreateBuffer(&bd, &sd, &g.ib);
        g.idxCount = (UINT)md.idx.size();
        return g;
    }

    void beginFrame() {
        float clear[4] = { 0, 0, 0, 1 };
        ctx->OMSetRenderTargets(1, &rtv, dsv);
        ctx->ClearRenderTargetView(rtv, clear);
        ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        ctx->RSSetState(raster);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetConstantBuffers(0, 1, &cbuf);
        ctx->PSSetConstantBuffers(0, 1, &cbuf);
    }

    void pushCB() { ctx->UpdateSubresource(cbuf, 0, nullptr, &cb, 0, 0); }

    void drawFullscreen(ID3D11PixelShader* ps, ID3D11DepthStencilState* dss, ID3D11BlendState* blend) {
        pushCB();
        ctx->IASetInputLayout(nullptr);
        ID3D11Buffer* nullVB = nullptr; UINT zero = 0;
        ctx->IASetVertexBuffers(0, 1, &nullVB, &zero, &zero);
        ctx->VSSetShader(vsFull, nullptr, 0);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->OMSetDepthStencilState(dss, 0);
        float bf[4] = { 0,0,0,0 };
        ctx->OMSetBlendState(blend, bf, 0xffffffff);
        ctx->Draw(3, 0);
    }

    void drawMesh(const GpuMesh& g, const M4& world, ID3D11PixelShader* ps) {
        cb.world = world;
        pushCB();
        ctx->IASetInputLayout(layout);
        UINT stride = sizeof(Vtx), offset = 0;
        ctx->IASetVertexBuffers(0, 1, &g.vb, &stride, &offset);
        ctx->IASetIndexBuffer(g.ib, DXGI_FORMAT_R32_UINT, 0);
        ctx->VSSetShader(vsMesh, nullptr, 0);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->OMSetDepthStencilState(dsOn, 0);
        float bf[4] = { 0,0,0,0 };
        ctx->OMSetBlendState(blOpaque, bf, 0xffffffff);
        ctx->DrawIndexed(g.idxCount, 0, 0);
    }

    // additive billboard quad; uv corners packed in normal.xy
    void drawGlow(V3 center, float size, V3 camR, V3 camU, float r, float g, float b, float intensity) {
        cb.world = M4::identity();
        cb.tint[0] = r; cb.tint[1] = g; cb.tint[2] = b; cb.tint[3] = intensity;
        pushCB();
        V3 R = camR * size, U = camU * size;
        V3 c00 = center - R - U, c10 = center + R - U, c11 = center + R + U, c01 = center - R + U;
        Vtx v[6] = {
            { {c00.x,c00.y,c00.z}, {-1,-1,0}, {1,1,1,1} },
            { {c01.x,c01.y,c01.z}, {-1, 1,0}, {1,1,1,1} },
            { {c11.x,c11.y,c11.z}, { 1, 1,0}, {1,1,1,1} },
            { {c00.x,c00.y,c00.z}, {-1,-1,0}, {1,1,1,1} },
            { {c11.x,c11.y,c11.z}, { 1, 1,0}, {1,1,1,1} },
            { {c10.x,c10.y,c10.z}, { 1,-1,0}, {1,1,1,1} },
        };
        D3D11_MAPPED_SUBRESOURCE map;
        if (SUCCEEDED(ctx->Map(dynVB, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
            memcpy(map.pData, v, sizeof(v));
            ctx->Unmap(dynVB, 0);
        }
        ctx->IASetInputLayout(layout);
        UINT stride = sizeof(Vtx), offset = 0;
        ctx->IASetVertexBuffers(0, 1, &dynVB, &stride, &offset);
        ctx->VSSetShader(vsMesh, nullptr, 0);
        ctx->PSSetShader(psGlow, nullptr, 0);
        ctx->OMSetDepthStencilState(dsReadOnly, 0);
        float bf[4] = { 0,0,0,0 };
        ctx->OMSetBlendState(blAdd, bf, 0xffffffff);
        ctx->Draw(6, 0);
    }

    void present() { swap->Present(1, 0); }

    // ------------------------------------------------------------- 2D helpers
    void text(const wchar_t* s, float x, float y, float w, float h, IDWriteTextFormat* fmt,
              D2D1_COLOR_F col, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
        brush->SetColor(col);
        fmt->SetTextAlignment(align);
        d2dRT->DrawTextW(s, (UINT32)wcslen(s), fmt, D2D1::RectF(x, y, x + w, y + h), brush);
    }
    void fillRect(float x, float y, float w, float h, D2D1_COLOR_F col) {
        brush->SetColor(col);
        d2dRT->FillRectangle(D2D1::RectF(x, y, x + w, y + h), brush);
    }
    void line(float x0, float y0, float x1, float y1, D2D1_COLOR_F col, float width = 1.5f) {
        brush->SetColor(col);
        d2dRT->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), brush, width);
    }
    void circle(float x, float y, float r, D2D1_COLOR_F col) {
        brush->SetColor(col);
        d2dRT->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), r, r), brush);
    }
    void rectOutline(float x, float y, float w, float h, D2D1_COLOR_F col, float lw = 1.0f) {
        brush->SetColor(col);
        d2dRT->DrawRectangle(D2D1::RectF(x, y, x + w, y + h), brush, lw);
    }

    bool saveScreenshotBMP(const wchar_t* path) {
        ID3D11Texture2D* back = nullptr;
        swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back);
        D3D11_TEXTURE2D_DESC d;
        back->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        d.MiscFlags = 0;
        ID3D11Texture2D* staging = nullptr;
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &staging))) { back->Release(); return false; }
        ctx->CopyResource(staging, back);
        back->Release();
        D3D11_MAPPED_SUBRESOURCE map;
        if (FAILED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map))) { staging->Release(); return false; }

        int W = d.Width, H = d.Height;
        BITMAPFILEHEADER fh = {};
        BITMAPINFOHEADER ih = {};
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof(fh) + sizeof(ih);
        fh.bfSize = fh.bfOffBits + W * H * 4;
        ih.biSize = sizeof(ih);
        ih.biWidth = W; ih.biHeight = H;
        ih.biPlanes = 1; ih.biBitCount = 32; ih.biCompression = BI_RGB;
        FILE* f = nullptr;
        bool ok = false;
        if (_wfopen_s(&f, path, L"wb") == 0 && f) {
            fwrite(&fh, sizeof(fh), 1, f);
            fwrite(&ih, sizeof(ih), 1, f);
            std::vector<unsigned char> row(W * 4);
            for (int y = H - 1; y >= 0; y--) {
                const unsigned char* src = (const unsigned char*)map.pData + (size_t)y * map.RowPitch;
                memcpy(row.data(), src, W * 4);
                for (int x = 0; x < W; x++) row[x * 4 + 3] = 255;
                fwrite(row.data(), 1, W * 4, f);
            }
            fclose(f);
            ok = true;
        }
        ctx->Unmap(staging, 0);
        staging->Release();
        return ok;
    }
};
