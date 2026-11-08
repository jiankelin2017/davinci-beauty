// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Jian
// pipe_stage_probe.cpp —— 把一次 ROI 往返拆成 6 段，每段单独 fence 排空
//
// 目的：gpuBeautyVerifyRoi 只给出「上传 / 计算 / 下载」三段，其中「计算」段
// 同时包含 DYNAMIC→DEFAULT 拷贝、CS、DEFAULT→STAGING 拷贝三件事。P2 选 16F 还是
// 32F、以及在哪一步加优化，取决于这三件事各占多少。

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

static double nowMs() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static const char* kCs = R"(
Texture2D<float4>   s : register(t0);
RWTexture2D<float4> d : register(u0);
[numthreads(8,8,1)]
void CSMain(uint3 t : SV_DispatchThreadID) {
    uint w,h; d.GetDimensions(w,h);
    if (t.x>=w||t.y>=h) return;
    d[t.xy] = s.Load(int3((int)t.x,(int)t.y,0));
}
)";

template <typename T> static void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

int main() {
    IDXGIFactory1* fac = nullptr;
    CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&fac);
    IDXGIAdapter1* best = nullptr; unsigned long long bs = 0;
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* a = nullptr;
        if (fac->EnumAdapters1(i, &a) != S_OK) break;
        DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) { a->Release(); continue; }
        unsigned long long s = d.DedicatedVideoMemory / 1048576ull + 1;
        if (d.VendorId != 0x8086) s += 100000;
        if (s > bs) { if (best) best->Release(); best = a; bs = s; } else a->Release();
    }
    fac->Release();

    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(best, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, want, 2,
                                 D3D11_SDK_VERSION, &dev, &fl, &ctx))) return 1;
    best->Release();

    ID3DBlob *code = nullptr, *err = nullptr;
    D3DCompile(kCs, strlen(kCs), "p", nullptr, nullptr, "CSMain", "cs_5_0",
               D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    ID3D11ComputeShader* cs = nullptr;
    dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs);

    D3D11_QUERY_DESC qd{}; qd.Query = D3D11_QUERY_EVENT;
    ID3D11Query* q = nullptr; dev->CreateQuery(&qd, &q);

    struct Case { const char* name; int w, h; DXGI_FORMAT f; int bpp; };
    Case cases[] = {
        {"4K人脸 32F",  1280, 1440, DXGI_FORMAT_R32G32B32A32_FLOAT, 16},
        {"4K人脸 16F",  1280, 1440, DXGI_FORMAT_R16G16B16A16_FLOAT,  8},
        {"4K全帧 32F",  3840, 2160, DXGI_FORMAT_R32G32B32A32_FLOAT, 16},
        {"4K全帧 16F",  3840, 2160, DXGI_FORMAT_R16G16B16A16_FLOAT,  8},
    };

    std::printf("%-12s %6s %6s %6s %6s %6s %6s %6s\n",
                "场景", "Map写", "升D", "CS", "降S", "等待", "Map读", "合计");
    std::printf("--------------------------------------------------------------------------\n");

    for (Case& c : cases) {
        const size_t rowBytes = (size_t)c.w * c.bpp;
        std::vector<unsigned char> host((size_t)c.h * rowBytes, 0x3C);

        D3D11_TEXTURE2D_DESC td{};
        td.Width = c.w; td.Height = c.h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = c.f; td.SampleDesc.Count = 1;

        td.Usage = D3D11_USAGE_DYNAMIC; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ID3D11Texture2D* pack = nullptr; dev->CreateTexture2D(&td, nullptr, &pack);

        td.Usage = D3D11_USAGE_DEFAULT; td.CPUAccessFlags = 0;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        ID3D11Texture2D* a = nullptr, *b = nullptr;
        dev->CreateTexture2D(&td, nullptr, &a);
        dev->CreateTexture2D(&td, nullptr, &b);

        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* st = nullptr; dev->CreateTexture2D(&td, nullptr, &st);

        ID3D11ShaderResourceView* srv = nullptr; dev->CreateShaderResourceView(a, nullptr, &srv);
        ID3D11UnorderedAccessView* uav = nullptr; dev->CreateUnorderedAccessView(b, nullptr, &uav);

        double t[6] = {0, 0, 0, 0, 0, 0};
        const int R = 8;
        for (int it = 0; it <= R; ++it) {
            double t0 = nowMs();
            D3D11_MAPPED_SUBRESOURCE m{};
            ctx->Map(pack, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
            for (int y = 0; y < c.h; ++y)
                memcpy((BYTE*)m.pData + (size_t)y * m.RowPitch, &host[(size_t)y * rowBytes], rowBytes);
            ctx->Unmap(pack, 0);
            ctx->End(q); ctx->Flush();
            while (ctx->GetData(q, nullptr, 0, 0) == S_FALSE) {}
            double t1 = nowMs();

            ctx->CopyResource(a, pack);
            ctx->End(q); ctx->Flush();
            while (ctx->GetData(q, nullptr, 0, 0) == S_FALSE) {}
            double t2 = nowMs();

            ctx->CSSetShader(cs, nullptr, 0);
            ctx->CSSetShaderResources(0, 1, &srv);
            ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
            ctx->Dispatch((c.w + 7) / 8, (c.h + 7) / 8, 1);
            ID3D11ShaderResourceView* ns = nullptr;
            ID3D11UnorderedAccessView* nu = nullptr;
            ctx->CSSetShaderResources(0, 1, &ns);
            ctx->CSSetUnorderedAccessViews(0, 1, &nu, nullptr);
            ctx->CSSetShader(nullptr, nullptr, 0);
            ctx->End(q); ctx->Flush();
            while (ctx->GetData(q, nullptr, 0, 0) == S_FALSE) {}
            double t3 = nowMs();

            ctx->CopyResource(st, b);
            ctx->End(q); ctx->Flush();
            while (ctx->GetData(q, nullptr, 0, 0) == S_FALSE) {}
            double t4 = nowMs();

            D3D11_MAPPED_SUBRESOURCE md{};
            double t5 = nowMs();
            ctx->Map(st, 0, D3D11_MAP_READ, 0, &md);
            double t6 = nowMs();
            for (int y = 0; y < c.h; ++y)
                memcpy(&host[(size_t)y * rowBytes], (BYTE*)md.pData + (size_t)y * md.RowPitch, rowBytes);
            ctx->Unmap(st, 0);
            double t7 = nowMs();
            (void)t5;

            if (it > 0) {
                t[0] += t1 - t0; t[1] += t2 - t1; t[2] += t3 - t2;
                t[3] += t4 - t3; t[4] += t6 - t5; t[5] += t7 - t6;
            }
        }
        double tot = 0;
        for (int i = 0; i < 6; ++i) { t[i] /= R; tot += t[i]; }
        std::printf("%-12s %6.2f %6.2f %6.2f %6.2f %6.2f %6.2f %6.2f\n",
                    c.name, t[0], t[1], t[2], t[3], t[4], t[5], tot);

        rel(srv); rel(uav); rel(pack); rel(a); rel(b); rel(st);
    }

    rel(cs); rel(q); rel(code); rel(err);
    ctx->Release(); dev->Release();
    return 0;
}
