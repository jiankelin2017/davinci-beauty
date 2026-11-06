// gpu_smooth.cpp —— 磨皮生产路径的 D3D11 Compute 实现（P2）
//
// 设计动机、ROI 相位约束、外扩推导见 gpu_smooth.h 头注释。这里只记实现要点：
//
//   · 5 个 kernel：CSDown / CSBoxH / CSBoxV / CSAmp / CSCompose
//   · 平面统一 R32G32B32A32_FLOAT + float4，B1 的 RGB 塞在同一张纹理，
//     三通道共用一次 dispatch
//   · box 累加器与合成表达式全部 `precise` —— 这是「逐字节一致」的唯一保障
//
// ## precise 为什么必须加
//
// CPU 侧 MSVC /fp:precise 默认**不**做 FMA 融合：`c + (even - c) * skin` 是
// 先减、再乘、再加，三步各自舍入。fxc 默认会把同一表达式收缩成 mad
// （中间结果不落盘、不做中间舍入），最后一位起就差 1 ULP，经 +0.5 取整后
// 可能翻成相邻灰阶。`precise` 明确禁止这类重写。
//
// ## 不支持的参数组合一律回落 CPU
//
// gwp != 2（要用 powf，GPU 与 CPU 的 pow 末位不同）、halfRes == 0（生产不走）、
// 画幅过小 —— 这些情况直接返回 false，由调用方走 CPU。宁可慢，不要静默错。

#include "gpu_smooth.h"
#include "beauty_log.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace {

// ---------------------------------------------------------------------------
// HLSL
// ---------------------------------------------------------------------------
//
// 纹理寄存器布局（所有 kernel 共用一套）：
//   t0 uint4   src（RGBA8 上传矩形）
//   t1 uint4   mask/protect 打包（r=mask, g=protect）
//   t3 float4  平面 A：box 的输入 / amp 的 C / compose 的 B1
//   t4 float4  平面 B：amp 的 B2 / compose 的 B2
//   t5 float4  平面 S：compose 的 amp
//   u0 float4  平面输出
//   u1 uint4   dst
const char* kHlsl = R"HLSL(
cbuffer P : register(b0)
{
    float g_inv, g_sxw, g_syw;
    float g_invT, g_I, g_e, g_eyeK;
    int   g_imgRW, g_imgRH;
    int   g_pw, g_ph;
    int   g_planeW, g_planeH;
    int   g_planeX0, g_planeY0;
    int   g_r;
    int   g_gwpSq, g_halfRes;
    int   g_imgX0, g_imgY0;
};

Texture2D<uint4>    g_srcTex   : register(t0);
Texture2D<uint4>    g_mpTex    : register(t1);
Texture2D<float4>   g_pA       : register(t3);
Texture2D<float4>   g_pB       : register(t4);
Texture2D<float4>   g_pS       : register(t5);
RWTexture2D<float4> g_planeOut : register(u0);
RWTexture2D<uint4>  g_dstTex   : register(u1);

// ---------------------------------------------------------------- 下采样
// RGBA8 上传矩形 → 半分辨率 float 平面，取值 0..255（不是 0..1）。
// 与 CPU 同口径：四个整数先求和再乘 0.25；整数和 ≤1020，float 精确无舍入。
[numthreads(8, 8, 1)]
void CSDown(uint3 id : SV_DispatchThreadID)
{
    if ((int)id.x >= g_pw || (int)id.y >= g_ph) return;

    int i0x = (int)id.x * 2;
    int i0y = (int)id.y * 2;
    int i1x = min(i0x + 1, g_imgRW - 1);
    int i1y = min(i0y + 1, g_imgRH - 1);

    uint4 a = g_srcTex.Load(int3(i0x, i0y, 0));
    uint4 b = g_srcTex.Load(int3(i1x, i0y, 0));
    uint4 c = g_srcTex.Load(int3(i0x, i1y, 0));
    uint4 d = g_srcTex.Load(int3(i1x, i1y, 0));

    float4 o;
    o.x = (float)(a.x + b.x + c.x + d.x) * 0.25f;
    o.y = (float)(a.y + b.y + c.y + d.y) * 0.25f;
    o.z = (float)(a.z + b.z + c.z + d.z) * 0.25f;
    o.w = 1.0f;
    g_planeOut[id.xy] = o;
}

// ---------------------------------------------------------------- 水平盒式
// 一个线程负责 32 个连续像素。CPU 每 32 像素重算一次窗口和，块内状态不跨块，
// 所以一个 32 像素块可以交给一个线程独立算完，累加顺序与 CPU 完全相同。
[numthreads(1, 8, 1)]
void CSBoxH(uint3 id : SV_DispatchThreadID)
{
    int y  = (int)id.y;
    int xs = (int)id.x * 32;
    if (y >= g_ph || xs >= g_pw) return;

    int xe = min(xs + 31, g_pw - 1);
    precise float sr = 0.0f, sg = 0.0f, sb = 0.0f, sa = 0.0f;

    for (int x = xs; x <= xe; ++x)
    {
        if ((x & 31) == 0)
        {
            sr = 0.0f; sg = 0.0f; sb = 0.0f; sa = 0.0f;
            for (int k = -g_r; k <= g_r; ++k)
            {
                int xx = clamp(x + k, 0, g_pw - 1);
                float4 v = g_pA.Load(int3(xx, y, 0));
                sr = sr + v.x;  sg = sg + v.y;
                sb = sb + v.z;  sa = sa + v.w;
            }
        }
        float4 o;
        o.x = sr * g_inv; o.y = sg * g_inv;
        o.z = sb * g_inv; o.w = sa * g_inv;
        g_planeOut[int2(x, y)] = o;

        int xa = clamp(x + g_r + 1, 0, g_pw - 1);
        int xr = clamp(x - g_r,     0, g_pw - 1);
        float4 va = g_pA.Load(int3(xa, y, 0));
        float4 vr = g_pA.Load(int3(xr, y, 0));
        sr = sr + (va.x - vr.x);
        sg = sg + (va.y - vr.y);
        sb = sb + (va.z - vr.z);
        sa = sa + (va.w - vr.w);
    }
}

// ---------------------------------------------------------------- 垂直盒式
// 一个线程负责一列的 32 个连续行；flush 在 (y & 31) == 0，与 CPU 的 y % 32 一致。
[numthreads(8, 8, 1)]
void CSBoxV(uint3 id : SV_DispatchThreadID)
{
    int x  = (int)id.x;
    int ys = (int)id.y * 32;
    if (x >= g_pw || ys >= g_ph) return;

    int ye = min(ys + 31, g_ph - 1);
    precise float sr = 0.0f, sg = 0.0f, sb = 0.0f, sa = 0.0f;

    for (int y = ys; y <= ye; ++y)
    {
        if ((y & 31) == 0)
        {
            sr = 0.0f; sg = 0.0f; sb = 0.0f; sa = 0.0f;
            for (int k = -g_r; k <= g_r; ++k)
            {
                int yy = clamp(y + k, 0, g_ph - 1);
                float4 v = g_pA.Load(int3(x, yy, 0));
                sr = sr + v.x;  sg = sg + v.y;
                sb = sb + v.z;  sa = sa + v.w;
            }
        }
        float4 o;
        o.x = sr * g_inv; o.y = sg * g_inv;
        o.z = sb * g_inv; o.w = sa * g_inv;
        g_planeOut[int2(x, y)] = o;

        int ya = clamp(y + g_r + 1, 0, g_ph - 1);
        int yr = clamp(y - g_r,     0, g_ph - 1);
        float4 va = g_pA.Load(int3(x, ya, 0));
        float4 vr = g_pA.Load(int3(x, yr, 0));
        sr = sr + (va.x - vr.x);
        sg = sg + (va.y - vr.y);
        sb = sb + (va.z - vr.z);
        sa = sa + (va.w - vr.w);
    }
}

// ---------------------------------------------------------------- 振幅代理
// amp = |C.g − B2.g|，只测绿通道。存 .r，其余通道置 0（避免盒式把 NaN 放大）。
[numthreads(8, 8, 1)]
void CSAmp(uint3 id : SV_DispatchThreadID)
{
    if ((int)id.x >= g_pw || (int)id.y >= g_ph) return;
    float cg = g_pA.Load(int3((int)id.x, (int)id.y, 0)).y;   // t3 = C
    float bg = g_pB.Load(int3((int)id.x, (int)id.y, 0)).y;   // t4 = B2
    precise float d = cg - bg;
    float a = (d < 0.0f) ? -d : d;
    g_planeOut[id.xy] = float4(a, 0.0f, 0.0f, 0.0f);
}

// ---------------------------------------------------------------- 合成
// 双线性必须逐位复刻 planeBilinear：硬件采样器会差在插值精度上，不能用。
precise float bilinear(Texture2D<float4> t, precise float gx, precise float gy, int comp)
{
    precise float fx = gx - 0.5f;
    precise float fy = gy - 0.5f;
    int x0 = (int)floor(fx);
    int y0 = (int)floor(fy);
    precise float tx = fx - (float)x0;
    precise float ty = fy - (float)y0;
    int x1 = x0 + 1;
    int y1 = y0 + 1;

    x0 = clamp(x0, 0, g_planeW - 1);
    x1 = clamp(x1, 0, g_planeW - 1);
    y0 = clamp(y0, 0, g_planeH - 1);
    y1 = clamp(y1, 0, g_planeH - 1);

    // 全帧平面坐标 → ROI 局部。外扩保证合法输出像素的采样点不越界；
    // 越界只会落在不参与输出的外圈，clamp 一下不出错即可。
    x0 = clamp(x0 - g_planeX0, 0, g_pw - 1);
    x1 = clamp(x1 - g_planeX0, 0, g_pw - 1);
    y0 = clamp(y0 - g_planeY0, 0, g_ph - 1);
    y1 = clamp(y1 - g_planeY0, 0, g_ph - 1);

    precise float a = t.Load(int3(x0, y0, 0))[comp];
    precise float b = t.Load(int3(x1, y0, 0))[comp];
    precise float c = t.Load(int3(x0, y1, 0))[comp];
    precise float d = t.Load(int3(x1, y1, 0))[comp];

    precise float ab = a + (b - a) * tx;
    precise float cd = c + (d - c) * tx;
    return ab + (cd - ab) * ty;
}

[numthreads(8, 8, 1)]
void CSCompose(uint3 id : SV_DispatchThreadID)
{
    if ((int)id.x >= g_imgRW || (int)id.y >= g_imgRH) return;

    uint4 s  = g_srcTex.Load(int3((int)id.x, (int)id.y, 0));
    uint4 mp = g_mpTex.Load(int3((int)id.x, (int)id.y, 0));
    int m = (int)mp.x;
    if (m == 0)
    {
        g_dstTex[id.xy] = s;
        return;
    }

    int x = g_imgX0 + (int)id.x;
    int y = g_imgY0 + (int)id.y;

    precise float skin = (float)m * (1.0f / 255.0f);
    precise float eye = (float)mp.y * (1.0f / 255.0f) * g_eyeK;
    precise float keepEye = 1.0f - eye;
    precise float se = skin * g_e;

    precise float vx = ((float)x + 0.5f) * g_sxw;
    precise float vy = ((float)y + 0.5f) * g_syw;

    precise float araw = bilinear(g_pS, vx, vy, 0) * g_invT;
    precise float ap = araw * araw;            // gwp == 2 时才走到这里，见调用方门禁
    // 1.0.4.0：修掉 churn 台架抓到的极罕见 1 灰阶残留（1.0.2.0 就有）。
    //
    // 【可验证事实】差异像素的 CPU 复算值恰好是 156.500000000，正压在
    // +0.5 取整边界；逐项 ±2 ULP 敏感度扫描里只有 gw 的 +1 ULP 会翻成 156，
    // ampB / ap / v1 / v2 / skin / se / keepEye 全都不翻。把 bucket64 改回
    // 精确尺寸，差异帧/像素/数值完全不变 → 与 1.0.3.0 的桶化无关。
    // fxc 反汇编确认这里生成的是 div [precise]（不是 rcp），所以早期
    // 「SM5 用 rcp 近似倒数」的猜测是错的。
    // 【推测，未直接证实】驱动把 div 展开成硬件倒数序列，极少数分母上
    // 与 x86 divss 的正确舍入差 1 ULP；没有做 GPU 中间量回读，只能靠
    // 「精化后差异归零」这个结果反推。
    //
    // 修法：用融合乘加算残差 e = 1 - den*y（避开 2-den*y 的灾难性抵消），
    // 两次 Newton 精化把相对误差压到 float 舍入之下；不动 CPU 基线。
    // 反汇编可见 div → mad/mul/add ×2，全程 [precise]。
    // 注意：早期试过一次 y←y·(2−den·y) 的 Newton，**无效**；残差写成
    // 2−den·y 会有抵消，必须写成 mad(-den, y, 1) 且做两次才够。
    precise float den = 1.0f + ap;
    precise float invD = 1.0f / den;
    float e = mad(-den, invD, 1.0f);
    invD = invD + invD * e;
    e = mad(-den, invD, 1.0f);
    invD = invD + invD * e;
    precise float gw = invD;

    precise float v1x = bilinear(g_pA, vx, vy, 0);
    precise float v1y = bilinear(g_pA, vx, vy, 1);
    precise float v1z = bilinear(g_pA, vx, vy, 2);
    precise float v2x = bilinear(g_pB, vx, vy, 0);
    precise float v2y = bilinear(g_pB, vx, vy, 1);
    precise float v2z = bilinear(g_pB, vx, vy, 2);

    uint4 o;
    o.w = s.w;

    {
        precise float c = (float)s.x;
        precise float even = c - v1x + v2x;
        even = c + (even - c) * skin;
        even = even + (v2x - even) * se;
        even = c + (even - c) * gw;
        precise float res = c + (even - c) * g_I;
        precise float outv = c + (res - c) * keepEye;
        outv = clamp(outv, 0.0f, 255.0f);
        o.x = (uint)(outv + 0.5f);
    }
    {
        precise float c = (float)s.y;
        precise float even = c - v1y + v2y;
        even = c + (even - c) * skin;
        even = even + (v2y - even) * se;
        even = c + (even - c) * gw;
        precise float res = c + (even - c) * g_I;
        precise float outv = c + (res - c) * keepEye;
        outv = clamp(outv, 0.0f, 255.0f);
        o.y = (uint)(outv + 0.5f);
    }
    {
        precise float c = (float)s.z;
        precise float even = c - v1z + v2z;
        even = c + (even - c) * skin;
        even = even + (v2z - even) * se;
        even = c + (even - c) * gw;
        precise float res = c + (even - c) * g_I;
        precise float outv = c + (res - c) * keepEye;
        outv = clamp(outv, 0.0f, 255.0f);
        o.z = (uint)(outv + 0.5f);
    }

    g_dstTex[id.xy] = o;
}
)HLSL";

// ---------------------------------------------------------------------------
// 状态
// ---------------------------------------------------------------------------

enum { K_DOWN = 0, K_BOXH, K_BOXV, K_AMP, K_COMPOSE, K_COUNT };
static const char* kEntry[K_COUNT] = { "CSDown", "CSBoxH", "CSBoxV", "CSAmp", "CSCompose" };

enum { PL_C = 0, PL_B1, PL_B2, PL_AMP, PL_TMP, PL_COUNT };

struct Cb {
    float inv, sxw, syw;
    float invT, I, e, eyeK;
    int   imgRW, imgRH;
    int   pw, ph;
    int   planeW, planeH;
    int   planeX0, planeY0;
    int   r;
    int   gwpSq, halfRes;
    int   imgX0, imgY0;
};

std::mutex g_mtx;
bool  g_tried = false;
bool  g_ready = false;
char  g_err[512] = {0};

ID3D11Device*        g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
ID3D11ComputeShader* g_cs[K_COUNT] = {nullptr};
ID3D11Buffer*        g_cb = nullptr;

ID3D11Texture2D*          g_packSrc = nullptr;
ID3D11Texture2D*          g_packMp  = nullptr;
ID3D11Texture2D*          g_texSrc  = nullptr;
ID3D11Texture2D*          g_texMp   = nullptr;
ID3D11ShaderResourceView* g_srvSrc  = nullptr;
ID3D11ShaderResourceView* g_srvMp   = nullptr;

ID3D11Texture2D*           g_plane[PL_COUNT]    = {nullptr};
ID3D11ShaderResourceView*  g_planeSrv[PL_COUNT] = {nullptr};
ID3D11UnorderedAccessView* g_planeUav[PL_COUNT] = {nullptr};

ID3D11Texture2D*           g_texDst = nullptr;
ID3D11UnorderedAccessView* g_uavDst = nullptr;
ID3D11Texture2D*           g_stage  = nullptr;

// 真实尺寸（本帧逻辑边界）与分配尺寸（纹理实际大小）必须分开存：
// 桶化之后纹理比真实矩形大一圈，shader 的 clamp / dispatch 用的是真实值，
// 而 CreateTexture2D / CopyResource / RowPitch 用的是分配值。混用会静默算错。
int g_imgRW = 0, g_imgRH = 0, g_pw = 0, g_ph = 0;
int g_allocImgW = 0, g_allocImgH = 0, g_allocPw = 0, g_allocPh = 0;

long long g_frames = 0;
double g_sumTotal = 0.0, g_sumPlane = 0.0;
long long g_rebuilds = 0;        // 纹理组重建次数（诊断 churn，台架断言用）
long long g_deviceCreates = 0;   // 设备创建次数（应为 1；多于此值说明生命周期有 bug）

double nowMsQpc() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER v; QueryPerformanceFrequency(&v); return v; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

void setErr(const char* what, HRESULT hr) {
    std::snprintf(g_err, sizeof(g_err), "%s (hr=0x%08lX)", what, (unsigned long)hr);
}

// 只释放「按尺寸分配」的资源（纹理 / SRV / UAV / staging）。
// 故意不碰 shader 与常量缓冲：它们是设备级的，一次编译终身受用。
// 早期版本把 g_cb 也在这里释放，导致首次分配纹理后再 UpdateSubresource 空指针崩溃 ——
// 分配纹理 happens 在首次调用时，而 shader 在 createDevice 时已建好。
void releaseTextures() {
    auto rel = [](auto*& p) { if (p) { p->Release(); p = nullptr; } };
    for (int i = 0; i < PL_COUNT; ++i) { rel(g_plane[i]); rel(g_planeSrv[i]); rel(g_planeUav[i]); }
    rel(g_packSrc); rel(g_packMp);
    rel(g_texSrc);  rel(g_texMp);
    rel(g_srvSrc);  rel(g_srvMp);
    rel(g_texDst);  rel(g_uavDst); rel(g_stage);
    g_imgRW = g_imgRH = g_pw = g_ph = 0;
    g_allocImgW = g_allocImgH = g_allocPw = g_allocPh = 0;
}

// 设备级资源（shader / 常量缓冲）：只在设备销毁时释放
void releaseDeviceObjects() {
    auto rel = [](auto*& p) { if (p) { p->Release(); p = nullptr; } };
    rel(g_cb);
    for (auto& p : g_cs) rel(p);
}

bool compileOne(const char* entry, ID3D11ComputeShader** out) {
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    if (const char* od = getenv("BEAUTY_SMOOTH_OD")) { if (od[0] && od[0] != '0') flags = D3DCOMPILE_SKIP_OPTIMIZATION; }
    ID3DBlob* code = nullptr;
    ID3DBlob* err  = nullptr;
    HRESULT hr = D3DCompile(kHlsl, std::strlen(kHlsl), "gpu_smooth", nullptr, nullptr,
                            entry, "cs_5_0", flags, 0, &code, &err);
    if (FAILED(hr)) {
        std::snprintf(g_err, sizeof(g_err), "编译 %s 失败: %.300s", entry,
                      err ? (const char*)err->GetBufferPointer() : "(no message)");
        if (err) err->Release();
        return false;
    }
    if (err) err->Release();
    hr = g_dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(),
                                    nullptr, out);
    code->Release();
    if (FAILED(hr)) { setErr("CreateComputeShader", hr); return false; }
    return true;
}

IDXGIAdapter1* pickAdapter() {
    IDXGIFactory1* f = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f))) return nullptr;
    IDXGIAdapter1* best = nullptr;
    SIZE_T bestScore = 0;
    for (UINT i = 0; ; ++i) {
        IDXGIAdapter1* a = nullptr;
        if (f->EnumAdapters1(i, &a) != S_OK) break;
        DXGI_ADAPTER_DESC1 d{};
        const bool ok = SUCCEEDED(a->GetDesc1(&d)) &&
                        (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0;
        const SIZE_T score = d.DedicatedVideoMemory;
        if (ok && (!best || score > bestScore)) {
            if (best) best->Release();
            best = a;
            bestScore = score;
            continue;
        }
        a->Release();
    }
    f->Release();
    return best;
}

// 整套拆掉（纹理 + 设备级对象 + 设备本身）。设备丢失后的自愈、以及 shutdown 都走这里。
// 为什么必须成组拆：纹理属于某个具体设备，只换 g_dev 不拆纹理的话，
// 新设备 Map 旧纹理会返回 DXGI_ERROR_DEVICE_REMOVED（0x887A0005）——
// 报错是「设备已移除」，实际是「资源不属于这个设备」，极其容易误判成驱动崩了。
void teardownDevice() {
    releaseTextures();
    releaseDeviceObjects();
    if (g_ctx) { g_ctx->ClearState(); g_ctx->Flush(); g_ctx->Release(); g_ctx = nullptr; }
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
    g_ready = false;
    g_tried = false;
}

bool createDevice() {
    // 已经建过就先拆干净：直接覆盖 g_dev 会泄漏旧设备，且旧设备建的纹理还在，
    // 新上下文一碰就返回 DEVICE_REMOVED。这条在 mock_host 的多实例序列里必现。
    if (g_dev) teardownDevice();

    IDXGIAdapter1* ad = pickAdapter();
    if (!ad) { std::snprintf(g_err, sizeof(g_err), "找不到硬件适配器"); return false; }

    // Flags 必须是 0。
    // 1.0.2.0 曾传 D3D11_CREATE_DEVICE_DISABLE_GPU_TIMEOUT：它**关掉 TDR**，
    // GPU 一旦卡住不会被超时重置，而 Resolve 与本插件共用同一块 GPU，
    // 卡住的后果是宿主一起被拖死。06:15 的闪退没有 dump，这条只是疑似诱因，
    // 但「插件不该改变宿主的 GPU 超时策略」本身是无争议的，所以直接去掉。
    // 同时删掉「失败后去掉该 flag 重试」的兜底：首选已经不带 flag，兜底没有意义。
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(ad, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                   0, nullptr, 0, D3D11_SDK_VERSION,
                                   &g_dev, &fl, &g_ctx);
    ad->Release();
    if (FAILED(hr)) { setErr("D3D11CreateDevice", hr); return false; }
    ++g_deviceCreates;

    for (int i = 0; i < K_COUNT; ++i)
        if (!compileOne(kEntry[i], &g_cs[i])) return false;

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(Cb);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = g_dev->CreateBuffer(&bd, nullptr, &g_cb);
    if (FAILED(hr)) { setErr("CreateBuffer(cb)", hr); return false; }
    return true;
}

// 纹理按 64 像素向上取整分桶，且**始终多留一档**。
//
// 旧实现直接比真实 pw/ph：真机人脸框每帧抖 1~3 像素 → 每帧 release +
// Create 一整套 24 个 D3D11 对象。实测 GPU 磨皮 6.41ms 里 96% 是搬运
// （读回 3.9MB 花 3.48ms ≈ 1.1GB/s，而带宽标定是 20.4GB/s），根因就在这里；
// 1138 帧累计约 2.7 万次创建销毁，也是 06:15 闪退的疑似诱因之一。
//
// 为什么是「(v/64 + 1)*64」而不是标准的 ceil：标准 ceil 对正好整除的值
// 余量为 0（例如 640 → 640），抖动到 641 就跨桶重建一次，来回抖就每帧重建。
// 强制多留一档后每个尺寸都有 1~64 像素余量。
//
// 判重再叠一层「容量式复用」：只要现有纹理装得下就复用，不要求桶完全相等。
// 这一点是台架的多实例用例逼出来的 —— 两个画幅略有不同的实例交替渲染，
// 或尺寸在桶边界两侧来回抖，精确匹配都会退化成每帧重建。
// 但也不能无限复用大纹理：超过需求 2 倍就收缩，否则 4K 切回 640 小图后
// 每帧还在 CopyResource 一整张 4K 纹理，反而更慢、更占显存。
//
// 桶外的多余像素不参与任何计算：所有 kernel 的边界检查都用 cbuffer 里的真实
// pw/ph/imgRW/imgRH，只有 CreateTexture2D / CopyResource / Map 的 RowPitch
// 用分配尺寸。这两套尺寸绝不能混。
inline int bucket64(int v) { return (v > 0) ? (((v / 64) + 1) * 64) : 0; }

bool ensureResources(int imgRW, int imgRH, int pw, int ph) {
    const int aImgW = bucket64(imgRW), aImgH = bucket64(imgRH);
    const int aPw   = bucket64(pw),   aPh   = bucket64(ph);
    // D3D11 11.0 的 2D 纹理上限 16384：超了就回落 CPU，别让 CreateTexture2D
    // 返回失败后再被误判成「设备丢了」。
    if (aImgW > 16384 || aImgH > 16384 || aPw > 16384 || aPh > 16384) {
        std::snprintf(g_err, sizeof(g_err), "桶化尺寸越界 %dx%d / %dx%d",
                      aImgW, aImgH, aPw, aPh);
        return false;
    }
    const bool fits = g_texDst && g_allocImgW >= aImgW && g_allocImgH >= aImgH &&
                      g_allocPw >= aPw && g_allocPh >= aPh;
    const bool tooLarge = g_texDst &&
        (g_allocImgW > aImgW * 2 || g_allocImgH > aImgH * 2 ||
         g_allocPw > aPw * 2 || g_allocPh > aPh * 2);
    if (fits && !tooLarge) {
        // 只刷新逻辑尺寸，纹理复用。debugPlane / 日志读的是这四个真实值。
        g_imgRW = imgRW; g_imgRH = imgRH; g_pw = pw; g_ph = ph;
        return true;
    }
    releaseTextures();

    HRESULT hr;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)aImgW; td.Height = (UINT)aImgH;
    td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UINT;

    D3D11_TEXTURE2D_DESC up = td;
    up.Usage = D3D11_USAGE_DYNAMIC;
    up.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    up.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(hr = g_dev->CreateTexture2D(&up, nullptr, &g_packSrc))) { setErr("packSrc", hr); return false; }
    if (FAILED(hr = g_dev->CreateTexture2D(&up, nullptr, &g_packMp)))  { setErr("packMp", hr);  return false; }

    D3D11_TEXTURE2D_DESC dv = td;
    dv.Usage = D3D11_USAGE_DEFAULT;
    dv.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(hr = g_dev->CreateTexture2D(&dv, nullptr, &g_texSrc))) { setErr("texSrc", hr); return false; }
    if (FAILED(hr = g_dev->CreateTexture2D(&dv, nullptr, &g_texMp)))  { setErr("texMp", hr);  return false; }
    if (FAILED(hr = g_dev->CreateShaderResourceView(g_texSrc, nullptr, &g_srvSrc))) { setErr("srvSrc", hr); return false; }
    if (FAILED(hr = g_dev->CreateShaderResourceView(g_texMp,  nullptr, &g_srvMp)))  { setErr("srvMp", hr);  return false; }

    D3D11_TEXTURE2D_DESC pd{};
    pd.Width = (UINT)aPw; pd.Height = (UINT)aPh;
    pd.MipLevels = 1; pd.ArraySize = 1; pd.SampleDesc.Count = 1;
    pd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    pd.Usage = D3D11_USAGE_DEFAULT;
    pd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    for (int i = 0; i < PL_COUNT; ++i) {
        if (FAILED(hr = g_dev->CreateTexture2D(&pd, nullptr, &g_plane[i]))) { setErr("plane", hr); return false; }
        if (FAILED(hr = g_dev->CreateShaderResourceView(g_plane[i], nullptr, &g_planeSrv[i]))) { setErr("planeSrv", hr); return false; }
        if (FAILED(hr = g_dev->CreateUnorderedAccessView(g_plane[i], nullptr, &g_planeUav[i]))) { setErr("planeUav", hr); return false; }
    }

    D3D11_TEXTURE2D_DESC dd = td;
    dd.Usage = D3D11_USAGE_DEFAULT;
    dd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(hr = g_dev->CreateTexture2D(&dd, nullptr, &g_texDst))) { setErr("texDst", hr); return false; }
    if (FAILED(hr = g_dev->CreateUnorderedAccessView(g_texDst, nullptr, &g_uavDst))) { setErr("uavDst", hr); return false; }

    D3D11_TEXTURE2D_DESC st = td;
    st.Usage = D3D11_USAGE_STAGING;
    st.BindFlags = 0;
    st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(hr = g_dev->CreateTexture2D(&st, nullptr, &g_stage))) { setErr("stage", hr); return false; }

    g_imgRW = imgRW; g_imgRH = imgRH; g_pw = pw; g_ph = ph;
    g_allocImgW = aImgW; g_allocImgH = aImgH;
    g_allocPw = aPw;     g_allocPh = aPh;
    ++g_rebuilds;
    return true;
}

inline int alignDown32(int v) { return (v > 0) ? ((v / 32) * 32) : 0; }
inline int clampi(int v, int lo, int hi) { return (v < lo) ? lo : ((v > hi) ? hi : v); }

// bandBoxW 是磨皮半径的唯一出处，必须与 CPU 调同一个函数（见 beauty_smooth.h）。
inline int boxW(int ww, float frac, int minW) { return beautycore::bandBoxW(ww, frac, minW); }

void setConst(const Cb& cb) {
    g_ctx->UpdateSubresource(g_cb, 0, nullptr, &cb, 0, 0);
    g_ctx->CSSetConstantBuffers(0, 1, &g_cb);
}

// 解绑全部 SRV/UAV 槽位。
// 不解绑会踩 D3D11 的读写冲突：同一张纹理上一次当 SRV、这一次当 UAV，
// 驱动会自己把 SRV 槽置空并打警告，但实际行为按驱动而异 —— 显式清干净最稳。
void unbindAll() {
    ID3D11ShaderResourceView*  nullSrv[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    ID3D11UnorderedAccessView* nullUav[2] = {nullptr, nullptr};
    g_ctx->CSSetShaderResources(0, 6, nullSrv);
    g_ctx->CSSetUnorderedAccessViews(0, 2, nullUav, nullptr);
}

// 一次 dispatch：srvs 按 t0..t5 顺序给（nullptr = 该槽不绑），uav 给 u0/u1。
// dispX/dispY 是线程组数。
void dispatchOne(int kernel, ID3D11ShaderResourceView* const* srvs, int nSrv,
                 ID3D11UnorderedAccessView* uav0, ID3D11UnorderedAccessView* uav1,
                 UINT dispX, UINT dispY) {
    unbindAll();
    g_ctx->CSSetShader(g_cs[kernel], nullptr, 0);
    if (nSrv > 0) g_ctx->CSSetShaderResources(0, (UINT)nSrv, srvs);
    if (uav0 || uav1) {
        ID3D11UnorderedAccessView* u[2] = { uav0, uav1 };
        g_ctx->CSSetUnorderedAccessViews(0, (uav1 ? 2u : 1u), u, nullptr);
    }
    g_ctx->Dispatch(dispX, dispY, 1);
}

// 一组 (H, V) 盒式。in 是输入也是最终结果，out 只是中转：
//   H: in → out
//   V: out → in
// 与 CPU 的 bandBoxH/bandBoxV 一一对应。
void runBox(int inIdx, int outIdx, int r, int pw, int ph) {
    Cb cb{};
    cb.pw = pw; cb.ph = ph; cb.r = r;
    cb.inv = 1.0f / (float)(2 * r + 1);
    setConst(cb);

    ID3D11ShaderResourceView* sH[6] = { nullptr, nullptr, nullptr, g_planeSrv[inIdx],  nullptr, nullptr };
    dispatchOne(K_BOXH, sH, 4, g_planeUav[outIdx], nullptr,
                (UINT)((pw + 31) / 32), (UINT)((ph + 7) / 8));

    ID3D11ShaderResourceView* sV[6] = { nullptr, nullptr, nullptr, g_planeSrv[outIdx], nullptr, nullptr };
    const int chunks = (ph + 31) / 32;
    dispatchOne(K_BOXV, sV, 4, g_planeUav[inIdx], nullptr,
                (UINT)((pw + 7) / 8), (UINT)((chunks + 7) / 8));
}

}  // namespace

// ---------------------------------------------------------------- 公开接口

bool gpuSmoothInit() {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_tried) return g_ready;
    g_tried = true;
    g_ready = createDevice();
    if (!g_ready)
        NALOG(BEAUTY_LOG_WARNING, "GPU 磨皮初始化失败：%s（回退 CPU）", g_err);
    else
        NALOG(BEAUTY_LOG_INFO, "GPU 磨皮后端已就绪");
    return g_ready;
}

bool gpuSmoothReady() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_ready;
}

void gpuSmoothShutdown() {
    std::lock_guard<std::mutex> lk(g_mtx);
    teardownDevice();
    g_frames = 0;
    g_sumTotal = g_sumPlane = 0.0;
}

const char* gpuSmoothLastError() { return g_err; }

void gpuSmoothStats(long long* frames, double* avgTotal, double* avgPlane) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (frames)   *frames   = g_frames;
    if (avgTotal) *avgTotal = g_frames ? (g_sumTotal / (double)g_frames) : 0.0;
    if (avgPlane) *avgPlane = g_frames ? (g_sumPlane / (double)g_frames) : 0.0;
}

// 资源生命周期诊断（台架断言用，生产不调用）。
// rebuilds 应该远小于 frames：真机上 ROI 每帧抖动，桶化前两者基本 1:1。
// deviceCreates 应该恒为 1；>1 说明设备被反复重建（1.0.2.0 的 g_ready bug）。
void gpuSmoothResourceStats(long long* rebuilds, long long* deviceCreates,
                            int* allocImgW, int* allocImgH,
                            int* allocPw, int* allocPh) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (rebuilds)      *rebuilds      = g_rebuilds;
    if (deviceCreates) *deviceCreates = g_deviceCreates;
    if (allocImgW)     *allocImgW     = g_allocImgW;
    if (allocImgH)     *allocImgH     = g_allocImgH;
    if (allocPw)       *allocPw       = g_allocPw;
    if (allocPh)       *allocPh       = g_allocPh;
}

bool gpuSmoothRgbBand(const unsigned char* src, unsigned char* dst,                      int w, int h,
                      const unsigned char* mask, const unsigned char* protect,
                      const beautycore::BandParams& p,
                      const beautycore::RoiRect& bbox,
                      GpuSmoothTiming* timing) {
    if (!src || !dst || w <= 0 || h <= 0 || bbox.empty()) return false;

    // ---- 门禁：不支持的参数直接回落 CPU，不静默算错 ----
    if (!p.halfRes) return false;                       // 生产不走，且平面换算出入口不同
    const float gwp = (p.gwp < 1.0f) ? 1.0f : ((p.gwp > 8.0f) ? 8.0f : p.gwp);
    if (!(gwp > 1.99f && gwp < 2.01f)) return false;    // 非 2 次幂要 powf，GPU/CPU 末位不同

    int ww = (w + 1) / 2;
    int wh = (h + 1) / 2;
    if (ww < 8 || wh < 8) return false;                 // CPU 侧同样退回全分辨率

    const int w1 = boxW(ww, p.f1, 3);
    int       w2 = boxW(ww, p.f2, 5);
    if (w2 <= w1 + 1) w2 = w1 + 2;
    const int wa = boxW(ww, p.fa, 3);
    const int r1 = (w1 - 1) / 2;
    const int r2 = (w2 - 1) / 2;
    const int ra = (wa - 1) / 2;
    const int passes = (p.passes < 1) ? 1 : ((p.passes > 6) ? 6 : p.passes);

    const float sxw = (float)ww / (float)w;
    const float syw = (float)wh / (float)h;

    // ---- 输出矩形 = 掩码包围盒外扩 8（掩码在包围盒外应恒为 0）----
    const int outGrow = 8;
    const int ox0 = clampi(bbox.x0 - outGrow, 0, w);
    const int oy0 = clampi(bbox.y0 - outGrow, 0, h);
    const int ox1 = clampi(bbox.x1 + outGrow, 0, w);
    const int oy1 = clampi(bbox.y1 + outGrow, 0, h);
    if (ox1 <= ox0 || oy1 <= oy0) return false;

    // ---- 合成要用到的平面范围 ----
    // 平面坐标 ≈ (图像坐标 + 0.5) × sxw − 0.5，双线性足迹再 ±1
    const int pxLo = (int)std::floor(((float)ox0 + 0.5f) * sxw - 0.5f) - 1;
    const int pxHi = (int)std::floor(((float)(ox1 - 1) + 0.5f) * sxw - 0.5f) + 2;
    const int pyLo = (int)std::floor(((float)oy0 + 0.5f) * syw - 0.5f) - 1;
    const int pyHi = (int)std::floor(((float)(oy1 - 1) + 0.5f) * syw - 0.5f) + 2;

    // amp 那一路要先经 B2 再经 ra，所以外扩取 passes×(r2+ra)；+2 给双线性。
    //
    // 还要再加 passes×32 —— 这不是保守估计，是硬需求：
    //   bandBoxH/V 每 32 像素用「从头重算窗口」纠正累积误差（kFlush=32）。
    //   ROI 的最外一块（局部 x=0）重算时左边没有数据，只能 clamp 到自身，
    //   而整帧版那一处读到的是真实邻居 → 该块整块值不同。下一块重算时要读
    //   左邻 r 个像素，于是污染向内容方向传染一块，一共传递 passes 次。
    //   实测：ROI 与整帧的 B1 差异在内缩 64 处还剩 1613 字节、96 处归零，
    //   与 32×passes = 96 完全吻合。少这一项会稳定差出 1~2 灰阶。
    const int growP = passes * 32 + passes * (r2 + ra) + 2;

    const int plx0 = alignDown32(clampi(pxLo - growP, 0, ww - 1));
    const int ply0 = alignDown32(clampi(pyLo - growP, 0, wh - 1));
    const int plx1 = clampi(pxHi + growP, 0, ww);
    const int ply1 = clampi(pyHi + growP, 0, wh);
    const int pw = plx1 - plx0;
    const int ph = ply1 - ply0;
    if (pw <= 0 || ph <= 0 || pw > ww || ph > wh) return false;

    // ---- 上传矩形（图像坐标）----
    const int imgX0 = 2 * plx0;
    const int imgY0 = 2 * ply0;
    const int imgX1 = (std::min)(w, 2 * plx1);
    const int imgY1 = (std::min)(h, 2 * ply1);
    const int imgRW = imgX1 - imgX0;
    const int imgRH = imgY1 - imgY0;
    if (imgRW <= 0 || imgRH <= 0) return false;

    std::lock_guard<std::mutex> lk(g_mtx);
    // 注意 g_ready 必须在这里赋值：早期写成 `if (!g_ready && !createDevice()) return false;`
    // —— createDevice 成功了但 g_ready 仍是 false，于是**每次调用都重建一个设备**，
    // 覆盖 g_dev 的同时 ensureResources 因「尺寸没变」直接返回，留下一批属于旧设备的
    // 纹理。第二次 Map 就返回 DEVICE_REMOVED，表现成「首帧好用、之后每帧都回退 CPU」。
    if (!g_ready) {
        if (!createDevice()) return false;
        g_ready = true;
    }
    if (!ensureResources(imgRW, imgRH, pw, ph)) return false;

    const double t0 = nowMsQpc();

    // ---- 1. 上传 ----
    {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(g_ctx->Map(g_packSrc, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            std::snprintf(g_err, sizeof(g_err), "Map(packSrc) 失败");
            return false;
        }
        for (int y = 0; y < imgRH; ++y) {
            const unsigned char* s = src + ((size_t)(imgY0 + y) * (size_t)w + (size_t)imgX0) * 4;
            std::memcpy((unsigned char*)m.pData + (size_t)y * m.RowPitch, s, (size_t)imgRW * 4);
        }
        g_ctx->Unmap(g_packSrc, 0);

        if (FAILED(g_ctx->Map(g_packMp, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            std::snprintf(g_err, sizeof(g_err), "Map(packMp) 失败");
            return false;
        }
        for (int y = 0; y < imgRH; ++y) {
            const size_t gRow = (size_t)(imgY0 + y) * (size_t)w + (size_t)imgX0;
            unsigned char* d = (unsigned char*)m.pData + (size_t)y * m.RowPitch;
            for (int x = 0; x < imgRW; ++x) {
                d[(size_t)x * 4 + 0] = mask    ? mask[gRow + (size_t)x]    : 255;
                d[(size_t)x * 4 + 1] = protect ? protect[gRow + (size_t)x] : 0;
                d[(size_t)x * 4 + 2] = 0;
                d[(size_t)x * 4 + 3] = 255;
            }
        }
        g_ctx->Unmap(g_packMp, 0);

        // DYNAMIC 不能直当 SRV：拷进 DEFAULT 再用（P1 实测直用会从 1ms 变 15ms）
        g_ctx->CopyResource(g_texSrc, g_packSrc);
        g_ctx->CopyResource(g_texMp,  g_packMp);
    }
    const double t1 = nowMsQpc();

    // ---- 2. 计算 ----
    {
        Cb cb{};
        cb.sxw = sxw; cb.syw = syw;
        cb.invT = (p.T > 1e-3f) ? (1.0f / p.T) : 0.0f;
        cb.I = (p.I < 0.0f) ? 0.0f : ((p.I > 1.0f) ? 1.0f : p.I);
        cb.e = (p.e < 0.0f) ? 0.0f : ((p.e > 1.0f) ? 1.0f : p.e);
        cb.eyeK = (p.protectK < 0.0f) ? 0.0f : ((p.protectK > 100.0f) ? 1.0f : p.protectK / 100.0f);
        cb.imgRW = imgRW; cb.imgRH = imgRH;
        cb.pw = pw; cb.ph = ph;
        cb.planeW = ww; cb.planeH = wh;
        cb.planeX0 = plx0; cb.planeY0 = ply0;
        cb.gwpSq = 1; cb.halfRes = 1;
        cb.imgX0 = imgX0; cb.imgY0 = imgY0;
        setConst(cb);

        // 下采样
        {
            ID3D11ShaderResourceView* s[6] = { g_srvSrc, nullptr, nullptr, nullptr, nullptr, nullptr };
            dispatchOne(K_DOWN, s, 1, g_planeUav[PL_C], nullptr,
                        (UINT)((pw + 7) / 8), (UINT)((ph + 7) / 8));
        }

        // B1 / B2 起点都是 C
        g_ctx->CopyResource(g_plane[PL_B1], g_plane[PL_C]);
        g_ctx->CopyResource(g_plane[PL_B2], g_plane[PL_C]);

        // runBox 本身 = 一趟 H+V（结果回到起点纹理），所以一个 i 就是一趟。
        // 早期写成每轮调两次，等于把 passes=3 跑成 6 趟 —— 模糊半径翻倍，
        // 画面差异到 9~14 灰阶。这个错在离线台上一次就对出来了。
        for (int i = 0; i < passes; ++i) runBox(PL_B1, PL_TMP, r1, pw, ph);
        for (int i = 0; i < passes; ++i) runBox(PL_B2, PL_TMP, r2, pw, ph);

        // amp = |C.g − B2.g|（t3 = C，t4 = B2）
        // 注意 runBox 里 setConst 只填了 box 需要的字段，这里要重新填全量。
        setConst(cb);
        {
            ID3D11ShaderResourceView* s[6] = { nullptr, nullptr, nullptr,
                                               g_planeSrv[PL_C], g_planeSrv[PL_B2], nullptr };
            dispatchOne(K_AMP, s, 5, g_planeUav[PL_AMP], nullptr,
                        (UINT)((pw + 7) / 8), (UINT)((ph + 7) / 8));
        }
        for (int i = 0; i < passes; ++i) runBox(PL_AMP, PL_TMP, ra, pw, ph);

        // 合成：t0 src / t1 mp / t3 B1 / t4 B2 / t5 amp
        setConst(cb);
        {
            ID3D11ShaderResourceView* s[6] = { g_srvSrc, g_srvMp, nullptr,
                                               g_planeSrv[PL_B1], g_planeSrv[PL_B2], g_planeSrv[PL_AMP] };
            dispatchOne(K_COMPOSE, s, 6, nullptr, g_uavDst,
                        (UINT)((imgRW + 7) / 8), (UINT)((imgRH + 7) / 8));
        }
    }
    unbindAll();
    g_ctx->Flush();
    const double t2 = nowMsQpc();

    // ---- 3. 读回 ----
    {
        g_ctx->CopyResource(g_stage, g_texDst);
        D3D11_MAPPED_SUBRESOURCE m{};
        const HRESULT hrMap = g_ctx->Map(g_stage, 0, D3D11_MAP_READ, 0, &m);
        if (FAILED(hrMap)) {
            // 带 HRESULT 与资源尺寸：只写「Map(stage) 失败」的话，
            // 分不清是设备丢了、资源尺寸不对、还是上一次没 Unmap。
            std::snprintf(g_err, sizeof(g_err),
                          "Map(stage) 失败 hr=0x%08X stage=%p real=%dx%d pw=%d ph=%d alloc=%dx%d/%dx%d",
                          (unsigned)hrMap, (void*)g_stage, imgRW, imgRH, pw, ph,
                          g_allocImgW, g_allocImgH, g_allocPw, g_allocPh);
            // 设备真丢了（驱动更新 / TDR / 独显休眠）就整套拆掉，下一帧重建。
            // 不拆的话会一直失败到进程结束，而日志里只见「Map 失败」，
            // 看不出设备已经没了 —— 真实环境里这条迟早会遇到。
            if (hrMap == DXGI_ERROR_DEVICE_REMOVED || hrMap == DXGI_ERROR_DEVICE_RESET) {
                NALOG(BEAUTY_LOG_WARNING, "GPU 磨皮：设备已移除（hr=0x%08X），下次调用重建",
                      (unsigned)hrMap);
                teardownDevice();
            }
            return false;
        }
        for (int y = oy0; y < oy1; ++y) {
            const unsigned char* s = (const unsigned char*)m.pData + (size_t)(y - imgY0) * m.RowPitch;
            unsigned char* d = dst + (size_t)y * (size_t)w * 4;
            std::memcpy(d + (size_t)ox0 * 4, s + (size_t)(ox0 - imgX0) * 4,
                        (size_t)(ox1 - ox0) * 4);
        }
        g_ctx->Unmap(g_stage, 0);
    }
    const double t3 = nowMsQpc();

    // ---- 4. ROI 外补齐 ----
    // CPU 版 surface3RgbBand 是「整帧逐像素写 dst」：mask=0 处写 dst=src，
    // mask>0 处写磨皮结果。上面只下载了输出矩形 [ox0,ox1)×[oy0,oy1)，
    // ROI 外若不补，dst 会留下调用前的内容 —— 在 Resolve 里就是上一帧的残影。
    //
    // 生产调用点（beauty_core）在进来之前**不保证** dst 已拷过 src：
    // 那里只有两条早退分支会 memcpy，正常路径直接进来。所以等价性必须在这里补全，
    // 不能指望调用方。这一条是本函数能被当作 surface3RgbBand 直接替换的前提。
    //
    // 用四条带而不是整帧 memcpy：被替换掉的正是 ROI 那一块（4K 实测占 34%），
    // 上下两条是整行连续拷贝，左右两条在大矩形边上各拷一小段。
    //
    // dst == src（原地处理）时整段跳过：矩形外本来就已经是 src 的内容，
    // 再 memcpy 一遍只是自己拷自己。CPU 版逐像素「读 src 写 dst」在同址下也是安全的。
    if (dst != src) {
        const size_t stride = (size_t)w * 4;
        if (oy0 > 0)
            std::memcpy(dst, src, (size_t)oy0 * stride);
        if (oy1 < h)
            std::memcpy(dst + (size_t)oy1 * stride, src + (size_t)oy1 * stride,
                        (size_t)(h - oy1) * stride);
        for (int y = oy0; y < oy1; ++y) {
            unsigned char* d = dst + (size_t)y * stride;
            const unsigned char* s = src + (size_t)y * stride;
            if (ox0 > 0)
                std::memcpy(d, s, (size_t)ox0 * 4);
            if (ox1 < w)
                std::memcpy(d + (size_t)ox1 * 4, s + (size_t)ox1 * 4,
                            (size_t)(w - ox1) * 4);
        }
    }

    // 补齐也算进 totalMs —— 它是这一帧的真实开销；但不算进 downloadMs，
    // 那一项要能直接对上台架量的「读回」口径。
    const double t4 = nowMsQpc();

    if (timing) {
        timing->uploadMs   = t1 - t0;
        timing->computeMs  = t2 - t1;
        timing->downloadMs = t3 - t2;
        timing->totalMs    = t4 - t0;
        timing->imgX0 = imgX0; timing->imgY0 = imgY0;
        timing->imgW = imgRW;  timing->imgH = imgRH;
        timing->planeX0 = plx0; timing->planeY0 = ply0;
        timing->planeW = ww;    timing->planeH = wh;
        timing->pw = pw;        timing->ph = ph;
        timing->r1 = r1; timing->r2 = r2; timing->ra = ra; timing->passes = passes;
    }
    g_frames += 1;
    g_sumTotal += t4 - t0;
    g_sumPlane += (double)pw * (double)ph;
    return true;
}

// ---------------------------------------------------------------- 调试回读
// 离线台架用它逐段核对：C 对了再看 B1，B1 对了再看 B2、amp。
// 生产路径不调用，但保持零副作用（不改任何状态）。
bool gpuSmoothDebugPlane(int which, float* out, int capacity, int* pw, int* ph) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_ready || !out || which < 0 || which >= PL_COUNT) return false;
    if (g_pw <= 0 || g_ph <= 0) return false;
    if (g_allocPw < g_pw || g_allocPh < g_ph) return false;
    if ((long long)capacity < (long long)g_pw * g_ph * 4) return false;

    // staging 必须和平面纹理一样大（CopyResource 要求尺寸完全一致），
    // 但只把左上角真实 pw×ph 拷给调用方 —— 桶外的填充像素不是逻辑数据。
    D3D11_TEXTURE2D_DESC sd{};
    sd.Width = (UINT)g_allocPw; sd.Height = (UINT)g_allocPh;
    sd.MipLevels = 1; sd.ArraySize = 1; sd.SampleDesc.Count = 1;
    sd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ID3D11Texture2D* stg = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&sd, nullptr, &stg))) return false;
    g_ctx->CopyResource(stg, g_plane[which]);

    D3D11_MAPPED_SUBRESOURCE m{};
    bool ok = false;
    if (SUCCEEDED(g_ctx->Map(stg, 0, D3D11_MAP_READ, 0, &m))) {
        for (int y = 0; y < g_ph; ++y) {
            std::memcpy(out + (size_t)y * g_pw * 4,
                        (const unsigned char*)m.pData + (size_t)y * m.RowPitch,
                        (size_t)g_pw * 16);
        }
        g_ctx->Unmap(stg, 0);
        ok = true;
    }
    stg->Release();
    if (ok) { if (pw) *pw = g_pw; if (ph) *ph = g_ph; }
    return ok;
}
