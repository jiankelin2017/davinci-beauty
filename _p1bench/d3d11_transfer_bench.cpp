// =============================================================================
// d3d11_transfer_bench.cpp —— P1 阶段：D3D11 搬运吞吐基准
//
// 目的：在决定 GPU 传输管道怎么设计之前，先量出本机的真实数字：
//   1) 枚举适配器，确认选中独显而不是 Intel 核显（双显卡笔记本必查）
//   2) 分别量 upload / gpu-compute / download 三段的墙钟耗时
//   3) 对比「同步单缓冲」与「双缓冲异步」两种模式的端到端帧时间
//   4) 对比 RGBA16F 与 RGBA32F 两种中间纹理格式（OBS 在此卡上踩过 32F 崩溃）
//   5) 覆盖小分辨率预览帧（640x394）与 ROI 裁剪（人脸包围盒）场景
//
// 数据流（与未来生产路径一致）：
//   host f32 RGBA --(DYNAMIC Map)--> texUp(32F)
//      --(passes 趟 ping-pong, 中间纹理 16F/32F)--> texOut(32F)
//      --(CopyResource)--> staging(32F) --(Map READ)--> host f32 RGBA
//
// 编译：cl /O2 /EHsc /MD /std:c++17 /utf-8 d3d11_transfer_bench.cpp
// 输出：全英文（避免 MSVC 执行字符集导致控制台乱码）
// =============================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

// ---------------------------------------------------------------------------
static double now_ms() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
}

// ---------------------------------------------------------------------------
// 测试用 compute shader：纯 load -> 乘系数 -> store，等价于纹理带宽压测
// 真实磨皮是 21 趟可分离盒式，这里用 passes 次 ping-pong 模拟同等访存量
// ---------------------------------------------------------------------------
static const char* kCS = R"(
Texture2D<float4>   srcTex : register(t0);
RWTexture2D<float4> dstTex : register(u0);
cbuffer CB : register(b0) { float gK; float3 pad; };
[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID) {
    uint w, h;
    dstTex.GetDimensions(w, h);
    if (tid.x >= w || tid.y >= h) return;
    float4 c = srcTex.Load(int3((int2)tid.xy, 0));
    dstTex[tid.xy] = float4(c.rgb * gK, c.a);
}
)";

struct GpuCtx {
    ID3D11Device*        dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11ComputeShader* cs  = nullptr;
    ID3D11Buffer*        cb  = nullptr;
    ID3D11Query*         evt = nullptr;
    int                  adapterIdx = -1;
    char                 adapterName[256] = {0};
    double               vramGB = 0.0;
    bool                 isDiscrete = false;
};

static bool createDevice(GpuCtx& g) {
    IDXGIFactory1* fac = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&fac))) {
        printf("[FATAL] CreateDXGIFactory1 failed\n");
        return false;
    }

    IDXGIAdapter1* best = nullptr;
    double bestScore = -1.0;
    printf("=== Adapters ===\n");
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* a = nullptr;
        if (fac->EnumAdapters1(i, &a) != S_OK) break;
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        double vram = (double)d.DedicatedVideoMemory / (1024.0 * 1024.0 * 1024.0);
        bool intel = (d.VendorId == 0x8086);
        printf("  [%u] %ls  vendor=0x%04X vram=%.2fGB flags=0x%X\n",
               i, d.Description, d.VendorId, vram, d.Flags);
        double score = vram + (intel ? 0.0 : 1000.0);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) score = -1.0;
        if (score > bestScore) {
            bestScore = score;
            if (best) best->Release();
            best = a;
            g.adapterIdx = (int)i;
        } else {
            a->Release();
        }
    }
    printf("\n");

    if (!best) { printf("[FATAL] no usable adapter\n"); fac->Release(); return false; }

    DXGI_ADAPTER_DESC1 bd{};
    best->GetDesc1(&bd);
    WideCharToMultiByte(CP_UTF8, 0, bd.Description, -1, g.adapterName, sizeof(g.adapterName), nullptr, nullptr);
    g.vramGB = (double)bd.DedicatedVideoMemory / (1024.0 * 1024.0 * 1024.0);
    g.isDiscrete = (bd.VendorId != 0x8086);

    D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(best, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                                   fl, _countof(fl), D3D11_SDK_VERSION,
                                   &g.dev, &got, &g.ctx);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(best, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                               &fl[1], 1, D3D11_SDK_VERSION, &g.dev, &got, &g.ctx);
    }
    best->Release();
    fac->Release();

    if (FAILED(hr)) { printf("[FATAL] D3D11CreateDevice hr=0x%08X\n", (unsigned)hr); return false; }

    printf("Selected adapter [%d] %s  vram=%.2fGB  discrete=%s  featureLevel=0x%X\n\n",
           g.adapterIdx, g.adapterName, g.vramGB, g.isDiscrete ? "yes" : "no", (unsigned)got);

    ID3DBlob *code = nullptr, *err = nullptr;
    hr = D3DCompile(kCS, strlen(kCS), "bench.hlsl", nullptr, nullptr, "CSMain", "cs_5_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        printf("[FATAL] D3DCompile failed: %s\n", err ? (const char*)err->GetBufferPointer() : "?");
        if (err) err->Release();
        return false;
    }
    g.dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g.cs);
    code->Release();
    if (err) err->Release();

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = 16;
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g.dev->CreateBuffer(&cbd, nullptr, &g.cb);

    D3D11_QUERY_DESC qd{};
    qd.Query = D3D11_QUERY_EVENT;
    g.dev->CreateQuery(&qd, &g.evt);

    return g.cs && g.cb && g.evt;
}

static void waitGpu(GpuCtx& g) {
    g.ctx->End(g.evt);
    g.ctx->Flush();
    while (g.ctx->GetData(g.evt, nullptr, 0, 0) == S_FALSE) { YieldProcessor(); }
}

struct CaseResult {
    char  label[96];
    int   w, h;
    double upMs, gpuMs, downMs, syncTotalMs, asyncFrameMs;
    double upGBs, downGBs;
    bool  ok;
};

static bool runCase(GpuCtx& g, const char* label, int W, int H, DXGI_FORMAT midFmt,
                    int passes, int frames, CaseResult& out) {
    memset(&out, 0, sizeof(out));
    snprintf(out.label, sizeof(out.label), "%s", label);
    out.w = W; out.h = H;
    out.ok = false;

    const size_t rowBytes = (size_t)W * 16;   // host 恒为 f32 RGBA

    std::vector<float> host((size_t)W * H * 4);
    for (size_t i = 0; i < host.size(); ++i) host[i] = (float)((i * 37) % 251) / 251.0f;

    std::vector<ID3D11Texture2D*> owned;
    auto makeTex = [&](D3D11_USAGE usage, UINT bind, UINT cpu, DXGI_FORMAT f) -> ID3D11Texture2D* {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = W; d.Height = H;
        d.MipLevels = 1; d.ArraySize = 1; d.Format = f;
        d.SampleDesc.Count = 1;
        d.Usage = usage; d.BindFlags = bind; d.CPUAccessFlags = cpu;
        ID3D11Texture2D* t = nullptr;
        HRESULT hr = g.dev->CreateTexture2D(&d, nullptr, &t);
        if (FAILED(hr)) {
            printf("  [skip] CreateTexture2D usage=%d fmt=%d hr=0x%08X\n",
                   (int)usage, (int)f, (unsigned)hr);
            return nullptr;
        }
        owned.push_back(t);
        return t;
    };

    const DXGI_FORMAT F32 = DXGI_FORMAT_R32G32B32A32_FLOAT;
    ID3D11Texture2D* texUp  = makeTex(D3D11_USAGE_DYNAMIC, D3D11_BIND_SHADER_RESOURCE,
                                      D3D11_CPU_ACCESS_WRITE, F32);
    ID3D11Texture2D* texA   = makeTex(D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0, midFmt);
    ID3D11Texture2D* texB   = makeTex(D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0, midFmt);
    ID3D11Texture2D* texOut = makeTex(D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0, F32);
    ID3D11Texture2D* stage0 = makeTex(D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, F32);
    ID3D11Texture2D* stage1 = makeTex(D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, F32);

    if (!texUp || !texA || !texB || !texOut || !stage0 || !stage1) {
        for (auto* t : owned) t->Release();
        return false;
    }

    ID3D11ShaderResourceView* srvUp = nullptr;
    ID3D11ShaderResourceView *srvA = nullptr, *srvB = nullptr;
    ID3D11UnorderedAccessView *uavA = nullptr, *uavB = nullptr, *uavOut = nullptr;
    g.dev->CreateShaderResourceView(texUp, nullptr, &srvUp);
    g.dev->CreateShaderResourceView(texA, nullptr, &srvA);
    g.dev->CreateShaderResourceView(texB, nullptr, &srvB);
    g.dev->CreateUnorderedAccessView(texA, nullptr, &uavA);
    g.dev->CreateUnorderedAccessView(texB, nullptr, &uavB);
    g.dev->CreateUnorderedAccessView(texOut, nullptr, &uavOut);
    std::vector<ID3D11View*> views = { srvUp, srvA, srvB, uavA, uavB, uavOut };

    const UINT gx = (UINT)((W + 7) / 8), gy = (UINT)((H + 7) / 8);
    g.ctx->CSSetShader(g.cs, nullptr, 0);
    g.ctx->CSSetConstantBuffers(0, 1, &g.cb);

    auto setK = [&](float k) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(g.ctx->Map(g.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            float v[4] = { k, 0, 0, 0 };
            memcpy(m.pData, v, 16);
            g.ctx->Unmap(g.cb, 0);
        }
    };

    auto upload = [&]() -> bool {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(g.ctx->Map(texUp, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
        for (int y = 0; y < H; ++y)
            memcpy((BYTE*)m.pData + (size_t)y * m.RowPitch,
                   (const BYTE*)host.data() + (size_t)y * rowBytes, rowBytes);
        g.ctx->Unmap(texUp, 0);
        return true;
    };

    // passes 趟 ping-pong，末趟落到 texOut(f32)
    auto compute = [&]() {
        ID3D11ShaderResourceView* cur = srvUp;
        for (int p = 0; p < passes; ++p) {
            ID3D11UnorderedAccessView* dst = (p & 1) ? uavB : uavA;
            ID3D11ShaderResourceView*  nxt = (p & 1) ? srvB : srvA;
            setK(0.9995f);
            g.ctx->CSSetShaderResources(0, 1, &cur);
            g.ctx->CSSetUnorderedAccessViews(0, 1, &dst, nullptr);
            g.ctx->Dispatch(gx, gy, 1);
            cur = nxt;
        }
        setK(1.0f);
        g.ctx->CSSetShaderResources(0, 1, &cur);
        g.ctx->CSSetUnorderedAccessViews(0, 1, &uavOut, nullptr);
        g.ctx->Dispatch(gx, gy, 1);
    };

    auto unbind = [&]() {
        ID3D11ShaderResourceView*   ns[1] = { nullptr };
        ID3D11UnorderedAccessView*  nu[1] = { nullptr };
        g.ctx->CSSetShaderResources(0, 1, ns);
        g.ctx->CSSetUnorderedAccessViews(0, 1, nu, nullptr);
    };

    auto readback = [&](ID3D11Texture2D* st) {
        g.ctx->CopyResource(st, texOut);
        D3D11_MAPPED_SUBRESOURCE md{};
        if (SUCCEEDED(g.ctx->Map(st, 0, D3D11_MAP_READ, 0, &md))) {
            volatile float sink = 0;
            for (int y = 0; y < H; y += 64) sink += ((float*)md.pData)[(size_t)y * W * 4];
            (void)sink;
            g.ctx->Unmap(st, 0);
        }
    };

    // warmup
    for (int i = 0; i < 3; ++i) {
        if (!upload()) { for (auto* v : views) if (v) v->Release(); for (auto* t : owned) t->Release(); return false; }
        compute();
        waitGpu(g);
        unbind();
    }

    // ---------- 同步单缓冲 ----------
    double tUp = 0, tGpu = 0, tDown = 0, tTotal = 0;
    int done = 0;
    for (int f = 0; f < frames; ++f) {
        double t0 = now_ms();
        if (!upload()) break;
        double t1 = now_ms();
        compute();
        waitGpu(g);
        double t2 = now_ms();
        unbind();
        readback(stage0);
        double t3 = now_ms();
        tUp += (t1 - t0); tGpu += (t2 - t1); tDown += (t3 - t2); tTotal += (t3 - t0);
        ++done;
    }
    if (done == 0) { for (auto* v : views) if (v) v->Release(); for (auto* t : owned) t->Release(); return false; }
    tUp /= done; tGpu /= done; tDown /= done; tTotal /= done;

    // ---------- 双缓冲异步（下载与下一帧计算重叠）----------
    double tAsync = 0;
    {
        ID3D11Texture2D* st[2] = { stage0, stage1 };
        int f = 0;
        double tStart = now_ms();
        for (; f < frames; ++f) {
            if (!upload()) break;
            compute();
            unbind();
            g.ctx->CopyResource(st[f & 1], texOut);   // 提交即返回
            if (f >= 1) {
                D3D11_MAPPED_SUBRESOURCE md{};
                if (SUCCEEDED(g.ctx->Map(st[(f - 1) & 1], 0, D3D11_MAP_READ, 0, &md))) {
                    volatile float sink = 0;
                    for (int y = 0; y < H; y += 64) sink += ((float*)md.pData)[(size_t)y * W * 4];
                    (void)sink;
                    g.ctx->Unmap(st[(f - 1) & 1], 0);
                }
            }
        }
        if (f > 0) {
            D3D11_MAPPED_SUBRESOURCE md{};
            if (SUCCEEDED(g.ctx->Map(st[(f - 1) & 1], 0, D3D11_MAP_READ, 0, &md)))
                g.ctx->Unmap(st[(f - 1) & 1], 0);
        }
        tAsync = (f > 0) ? (now_ms() - tStart) / (double)f : 0.0;
    }

    out.upMs = tUp; out.gpuMs = tGpu; out.downMs = tDown;
    out.syncTotalMs = tTotal; out.asyncFrameMs = tAsync;
    out.upGBs = ((double)W * H * 16.0) / (tUp * 1e6);
    out.downGBs = ((double)W * H * 16.0) / (tDown * 1e6);
    out.ok = true;

    unbind();
    for (auto* v : views) if (v) v->Release();
    for (auto* t : owned) t->Release();
    return true;
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    printf("=== D3D11 Transfer Bench (P1) ===\n\n");

    GpuCtx g;
    if (!createDevice(g)) return 1;

    std::vector<CaseResult> results;
    const int PASSES = 21;
    const int FRAMES = 40;

    struct Dim { const char* label; int w, h; };
    Dim dims[] = {
        { "4K full",     3840, 2160 },
        { "1080p full",  1920, 1080 },
        { "preview",      640,  394 },
        { "4K face ROI", 1280, 1440 },
    };

    for (auto& d : dims) {
        for (DXGI_FORMAT fmt : { DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT }) {
            char lbl[96];
            snprintf(lbl, sizeof(lbl), "%s %s", d.label,
                     (fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) ? "(mid16F)" : "(mid32F)");
            CaseResult r{};
            printf("running: %s ...\n", lbl);
            fflush(stdout);
            if (runCase(g, lbl, d.w, d.h, fmt, PASSES, FRAMES, r)) results.push_back(r);
        }
    }

    printf("\n\n=== Results (passes=%d, frames=%d) ===\n\n", PASSES, FRAMES);
    printf("| case | upload | gpu 21x | download | sync total | async/frame | up GB/s | down GB/s |\n");
    printf("|---|---|---|---|---|---|---|---|\n");
    for (auto& r : results) {
        printf("| %s | %.2f ms | %.2f ms | %.2f ms | %.2f ms | %.2f ms | %.1f | %.1f |\n",
               r.label, r.upMs, r.gpuMs, r.downMs, r.syncTotalMs, r.asyncFrameMs,
               r.upGBs, r.downGBs);
    }

    printf("\n=== Reference: current CPU path (measured earlier, 8 threads) ===\n");
    printf("  1080p blur80+red30 : 24.6 ms\n");
    printf("  4K    all-on       : 127.0 ms\n");

    return 0;
}
