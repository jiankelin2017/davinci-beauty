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
//   t1 uint    mask（R8_UINT，0=不磨皮 / 255=全量）
//   t2 uint    protect（R8_UINT，保护强度 0~255）
//   t3 float4  平面 A：box 的输入 / amp 的 C / compose 的 B1
//   t4 float4  平面 B：amp 的 B2 / compose 的 B2
//   t5 float4  平面 S：compose 的 amp
//   u0 float4  平面输出
//   u1 uint4   dst
// 1.0.7.0 之前 t1/t2 是同一张 uint4 的 r/g 两个通道，拆开是为了让上传退化成
// 整行 memcpy（见 gpuSmoothRgbBand 上传段的注释）。
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
// 1.0.7.0：掩码与五官保护拆成两张 R8_UINT。
// 此前是一张 R8G8B8A8_UINT，只为塞两个字节，上传却要写 4 字节/像素、
// 而且是逐字节 store —— 台架实测 4K 15.6MB 花 2.73ms，比同样体积的
// 整行 memcpy 慢 2.4 倍。拆成单通道后上传退化成纯 memcpy，体积还只剩 1/4。
Texture2D<uint>     g_maskTex  : register(t1);
Texture2D<uint>     g_protTex  : register(t2);
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
    int m = (int)g_maskTex.Load(int3((int)id.x, (int)id.y, 0));
    if (m == 0)
    {
        g_dstTex[id.xy] = s;
        return;
    }

    int x = g_imgX0 + (int)id.x;
    int y = g_imgY0 + (int)id.y;

    precise float skin = (float)m * (1.0f / 255.0f);
    precise float eye = (float)g_protTex.Load(int3((int)id.x, (int)id.y, 0))
                        * (1.0f / 255.0f) * g_eyeK;
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
// 初始化结果只播报一次。1.0.9.0 把这条日志写在 gpuSmoothInit() 里，而生产路径从不调用
// 它（走 gpuSmoothRgbBand 内的懒加载），整个函数连字符串一起被链接器当死代码丢掉，
// 日志里永远看不到适配器型号——1.0.10.0 把播报移到懒加载处，两处共用本标志避免重复。
bool  g_initLogged = false;
char  g_err[512] = {0};

ID3D11Device*        g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
ID3D11ComputeShader* g_cs[K_COUNT] = {nullptr};
ID3D11Buffer*        g_cb = nullptr;

// 源图（RGBA8）与掩码/保护（各一张 R8）。每类三件套都按「DYNAMIC 上传 +
// DEFAULT 供 CS 读 + SRV」配：DYNAMIC 不能直接当 SRV（见上方的性能注释）。
ID3D11Texture2D*          g_packSrc  = nullptr;
ID3D11Texture2D*          g_texSrc   = nullptr;
ID3D11ShaderResourceView* g_srvSrc   = nullptr;

ID3D11Texture2D*          g_packMask = nullptr;
ID3D11Texture2D*          g_texMask  = nullptr;
ID3D11ShaderResourceView* g_srvMask  = nullptr;

ID3D11Texture2D*          g_packProt = nullptr;
ID3D11Texture2D*          g_texProt  = nullptr;
ID3D11ShaderResourceView* g_srvProt  = nullptr;

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

// 适配器身份（1.0.9.0 加）。为什么值得单独存一份并打进日志：
// 本模块选卡只排除 DXGI_ADAPTER_FLAG_SOFTWARE（WARP 软件渲染），**不按厂商过滤**，
// 所以 AMD / Intel 核显同样会走 GPU 路径。而这两家的浮点末位与 Map 读回性能都没有实测过。
// 一旦用户报「GPU 磨皮画面不对」或「比 CPU 还慢」，日志里有型号与厂商，
// 才能一眼分清是硬件差异还是插件缺陷 —— 否则只能远程猜。
char g_adapterInfo[256] = {0};

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
    rel(g_packSrc); rel(g_texSrc); rel(g_srvSrc);
    rel(g_packMask); rel(g_texMask); rel(g_srvMask);
    rel(g_packProt); rel(g_texProt); rel(g_srvProt);
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
    // 设备重建后重新播报一次适配器（1.0.10.0）：驱动重置往往伴随换卡或换驱动版本，
    // 日志里留两行比只留首行更有诊断价值。
    g_initLogged = false;
}

bool createDevice() {
    // 已经建过就先拆干净：直接覆盖 g_dev 会泄漏旧设备，且旧设备建的纹理还在，
    // 新上下文一碰就返回 DEVICE_REMOVED。这条在 mock_host 的多实例序列里必现。
    if (g_dev) teardownDevice();

    IDXGIAdapter1* ad = pickAdapter();
    if (!ad) { std::snprintf(g_err, sizeof(g_err), "找不到硬件适配器"); return false; }

    // 在 Release 之前把身份记下来 —— 这里是全流程唯一还持有 ad 的地方。
    DXGI_ADAPTER_DESC1 adDesc{};
    if (SUCCEEDED(ad->GetDesc1(&adDesc))) {
        char nameUtf8[192] = {0};
        WideCharToMultiByte(CP_UTF8, 0, adDesc.Description, -1,
                            nameUtf8, (int)sizeof(nameUtf8) - 1, nullptr, nullptr);
        std::snprintf(g_adapterInfo, sizeof(g_adapterInfo),
                      "%s [厂商 0x%04X，显存 %.1f GB]",
                      nameUtf8[0] ? nameUtf8 : "型号未知", adDesc.VendorId,
                      (double)adDesc.DedicatedVideoMemory / 1073741824.0);
    } else {
        std::snprintf(g_adapterInfo, sizeof(g_adapterInfo), "型号未知");
    }

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

    // 特性等级必须显式挡（1.0.9.0 加）。pFeatureLevels 传 nullptr 时，D3D11 的默认
    // 列表里含 10.0 / 9.x，DX10 老卡会「创建设备成功、直到编译算着色器才失败」。
    // 两条路最终都回落 CPU，但后者报的是 CreateComputeShader 失败，极易被误判成驱动坏了。
    // 这里只释放刚建的两个对象：shader / 纹理都还没建，不必走 teardownDevice
    // （它会把 g_tried 清回 false，导致每帧重试创建）。
    if (fl < D3D_FEATURE_LEVEL_11_0) {
        std::snprintf(g_err, sizeof(g_err),
                      "显卡特性等级 %.1f 低于 11.0，不支持 Compute Shader 5.0：%s",
                      (double)fl / 4096.0, g_adapterInfo);
        if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
        if (g_dev) { g_dev->Release(); g_dev = nullptr; }
        return false;
    }
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

    D3D11_TEXTURE2D_DESC dv = td;
    dv.Usage = D3D11_USAGE_DEFAULT;
    dv.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(hr = g_dev->CreateTexture2D(&dv, nullptr, &g_texSrc))) { setErr("texSrc", hr); return false; }
    if (FAILED(hr = g_dev->CreateShaderResourceView(g_texSrc, nullptr, &g_srvSrc))) { setErr("srvSrc", hr); return false; }

    // 掩码 / 保护：单通道 R8_UINT。用 UINT 而不是 UNORM 是为了逐字节精确 ——
    // UNORM 读出来是 value/255 的浮点，再乘回 255 不保证回到原整数。
    D3D11_TEXTURE2D_DESC u8 = td;
    u8.Format = DXGI_FORMAT_R8_UINT;
    D3D11_TEXTURE2D_DESC u8pack = u8, u8def = u8;
    u8pack.Usage = D3D11_USAGE_DYNAMIC;
    u8pack.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    u8pack.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    u8def.Usage = D3D11_USAGE_DEFAULT;
    u8def.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    struct Mono { ID3D11Texture2D** pack; ID3D11Texture2D** tex;
                  ID3D11ShaderResourceView** srv; const char* name; };
    const Mono monos[2] = {
        { &g_packMask, &g_texMask, &g_srvMask, "mask" },
        { &g_packProt, &g_texProt, &g_srvProt, "prot" },
    };
    for (int i = 0; i < 2; ++i) {
        if (FAILED(hr = g_dev->CreateTexture2D(&u8pack, nullptr, monos[i].pack))) {
            setErr(monos[i].name, hr); return false;
        }
        if (FAILED(hr = g_dev->CreateTexture2D(&u8def, nullptr, monos[i].tex))) {
            setErr(monos[i].name, hr); return false;
        }
        if (FAILED(hr = g_dev->CreateShaderResourceView(*monos[i].tex, nullptr, monos[i].srv))) {
            setErr(monos[i].name, hr); return false;
        }
    }

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
    // 播报与首帧懒加载共用 g_initLogged：台架先调 gpuSmoothInit()、再进 RgbBand 时，
    // 只会在前者打一行，不会重复。（1.0.10.0）
    if (!g_initLogged) {
        g_initLogged = true;
        if (!g_ready)
            NALOG(BEAUTY_LOG_WARNING, "GPU 磨皮初始化失败：%s（回退 CPU，适配器 %s）",
                  g_err, g_adapterInfo[0] ? g_adapterInfo : "未选定");
        else
            NALOG(BEAUTY_LOG_INFO, "GPU 磨皮后端已就绪：%s", g_adapterInfo);
    }
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
        if (!createDevice()) {
            // 失败只播报一次：这里每帧都会被重试（设备可能因驱动重置而恢复），
            // 但日志不能每帧刷一行。
            if (!g_initLogged) {
                g_initLogged = true;
                NALOG(BEAUTY_LOG_WARNING, "GPU 磨皮初始化失败：%s（回退 CPU，适配器 %s）",
                      g_err, g_adapterInfo[0] ? g_adapterInfo : "未选定");
            }
            return false;
        }
        g_ready = true;
        // 生产路径的适配器播报点（1.0.10.0 起）。台架走 gpuSmoothInit()，两处共用标志。
        if (!g_initLogged) {
            g_initLogged = true;
            NALOG(BEAUTY_LOG_INFO, "GPU 磨皮后端已就绪：%s", g_adapterInfo);
        }
    }
    if (!ensureResources(imgRW, imgRH, pw, ph)) return false;

    const double t0 = nowMsQpc();
    // 跨块使用的分步时间戳（块内声明会超出作用域）
    double ta = t0, tb = t0, td = t0;
    // 上传段三次 Map(WRITE_DISCARD) 的累计耗时。块内声明就传不出去，
    // 而它正是「uploadMs 为什么 2.9ms 却只搬 2.3MB」这个问题的答案所在。
    double upMapMs = 0.0;

    // ---- 1. 上传 ----
    {
        D3D11_MAPPED_SUBRESOURCE m{};
        double tm = nowMsQpc();
        if (FAILED(g_ctx->Map(g_packSrc, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            std::snprintf(g_err, sizeof(g_err), "Map(packSrc) 失败");
            return false;
        }
        upMapMs += nowMsQpc() - tm;
        for (int y = 0; y < imgRH; ++y) {
            const unsigned char* s = src + ((size_t)(imgY0 + y) * (size_t)w + (size_t)imgX0) * 4;
            std::memcpy((unsigned char*)m.pData + (size_t)y * m.RowPitch, s, (size_t)imgRW * 4);
        }
        g_ctx->Unmap(g_packSrc, 0);
        ta = nowMsQpc();                // 源图搬运结束（掩码打包开始）

        // 掩码 / 保护：各 1 字节/像素，纯整行 memcpy。
        // 旧实现是一张 RGBA8 里逐像素写 4 个字节（其中 2 个是常量），
        // 台架实测同体积比整行 memcpy 慢 2.4 倍，且多传 2 倍数据。
        for (int k = 0; k < 2; ++k) {
            ID3D11Texture2D* tex = (k == 0) ? g_packMask : g_packProt;
            const unsigned char* srcPlane = (k == 0) ? mask : protect;
            const unsigned char fill = (k == 0) ? 255 : 0;   // 与旧写法同义
            double tm2 = nowMsQpc();
            if (FAILED(g_ctx->Map(tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                std::snprintf(g_err, sizeof(g_err), "Map(%s) 失败", (k == 0) ? "mask" : "prot");
                return false;
            }
            upMapMs += nowMsQpc() - tm2;
            for (int y = 0; y < imgRH; ++y) {
                unsigned char* d = (unsigned char*)m.pData + (size_t)y * m.RowPitch;
                if (srcPlane) {
                    const unsigned char* s = srcPlane
                        + (size_t)(imgY0 + y) * (size_t)w + (size_t)imgX0;
                    std::memcpy(d, s, (size_t)imgRW);
                } else {
                    std::memset(d, fill, (size_t)imgRW);
                }
            }
            g_ctx->Unmap(tex, 0);
        }
        tb = nowMsQpc();                // 掩码打包结束（GPU 内部拷贝开始）

        // DYNAMIC 不能直当 SRV：拷进 DEFAULT 再用（P1 实测直用会从 1ms 变 15ms）
        g_ctx->CopyResource(g_texSrc,  g_packSrc);
        g_ctx->CopyResource(g_texMask, g_packMask);
        g_ctx->CopyResource(g_texProt, g_packProt);
    }
    const double t1 = nowMsQpc();
    const double tc = t1;               // DYNAMIC→DEFAULT 两次内部拷贝结束

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
            ID3D11ShaderResourceView* s[6] = { g_srvSrc, g_srvMask, g_srvProt,
                                               g_planeSrv[PL_B1], g_planeSrv[PL_B2], g_planeSrv[PL_AMP] };
            dispatchOne(K_COMPOSE, s, 6, nullptr, g_uavDst,
                        (UINT)((imgRW + 7) / 8), (UINT)((imgRH + 7) / 8));
        }
    }
    unbindAll();
    g_ctx->Flush();
    const double t2 = nowMsQpc();

    // ---- 3. 提交读回 ----
    // 声明在块外：Unmap 之后的时间戳要参与 t3 与 timing 的拆分。
    double timingMapOut = t2;
    double timingMapWaitOut = 0.0;   // Map(READ) 调用本身的耗时
    double timingMapCopyOut = 0.0;   // 拿到 pData 之后逐行 memcpy 的耗时
    double timingFillOut = 0.0;      // ROI 外补齐（1.0.8.0 起挪到 Map 之前与 GPU 重叠）
    {
        // 只把**输出矩形**拷进 staging，而不是整块桶化尺寸的纹理。
        // 两者能差一倍以上：4K 典型人脸时输出 1353×1753（9.1MB），
        // 上传矩形是 1918×2130（15.6MB）—— 多拷的 6.5MB 一个像素都用不上。
        // staging 仍是上传矩形那么大（CopySubresourceRegion 允许目标更小区域），
        // 所以 RowPitch 与纹理尺寸不变，读回时按 (y - oy0) 取行即可。
        D3D11_BOX box;
        box.left   = (UINT)(ox0 - imgX0);
        box.top    = (UINT)(oy0 - imgY0);
        box.front  = 0;
        box.right  = (UINT)(ox1 - imgX0);
        box.bottom = (UINT)(oy1 - imgY0);
        box.back   = 1;
        g_ctx->CopySubresourceRegion(g_stage, 0, 0, 0, 0, g_texDst, 0, &box);
        td = nowMsQpc();                // GPU→staging 拷贝已提交

        // ---- 3b. ROI 外补齐：塞进「GPU 还在算 + 正拷回」这段等待里 ----
        // 这一段此前排在 Map 之后，纯属排队：它写的是 dst 的 ROI **之外**，
        // 读回写的是 ROI **之内**，两者不重叠、彼此无依赖。
        // 台架实测（4K 典型人脸）：Map 等 GPU 5.81ms、补齐 2.78ms。
        // 挪到 Map 前面之后，这 2.78ms 落在等待里跑，基本是白拿的。
        //
        // 位置很关键：必须在 CopySubresourceRegion **之后**。子区域拷贝提交得越早，
        // GPU 越不会在算完之后空等命令；放在提交之前会让这条拷贝晚 2~3ms 才进队列，
        // 反而把整帧拖长 —— 省下来的一头又赔回去。
        //
        // Map 失败时这里已经把 ROI 外写过了：无害。失败后本函数 return false，
        // 调用方（beauty_core）改走 surface3RgbBand 整帧重算，dst 会被全量覆盖。
        const double tf0 = nowMsQpc();
        if (dst != src) {
            // 大块上下两条 + 逐行左右两条。试过把大块换成 _mm_stream_si128
            // 非时序写（省掉 read-for-ownership），三对交替实测 fill 反而稳定慢
            // 3~4%：条带两端不满 cache line，NT 仍要读改写；而 mapCopy 也没因
            // 「不污染 L3」受益到能被量出来。故保留普通 memcpy。
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
        timingFillOut = nowMsQpc() - tf0;

        D3D11_MAPPED_SUBRESOURCE m{};
        const double tm0 = nowMsQpc();  // Map 之前
        const HRESULT hrMap = g_ctx->Map(g_stage, 0, D3D11_MAP_READ, 0, &m);
        timingMapWaitOut = nowMsQpc() - tm0;   // 只算 Map() 本身：等同步点 + PCIe 往返
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
            // staging 的 (0,0) 就是输出矩形的左上角（见上面的 box），
            // 所以行取 (y - oy0)、列从 0 开始 —— 这两处都不能再减 imgX0/imgY0。
            // 1.0.7.0 第一版就是这里照抄了整块拷贝的偏移，横向整体错位，
            // 台架立刻报 maxdiff=255（差异集中在 ROI，正好是错位的量）。
            const unsigned char* s = (const unsigned char*)m.pData
                + (size_t)(y - oy0) * m.RowPitch;
            unsigned char* d = dst + (size_t)y * (size_t)w * 4;
            std::memcpy(d + (size_t)ox0 * 4, s, (size_t)(ox1 - ox0) * 4);
        }
        g_ctx->Unmap(g_stage, 0);
        timingMapOut = nowMsQpc();
        timingMapCopyOut = (timingMapOut - tm0) - timingMapWaitOut;  // 逐行 memcpy 的耗时
    }
    const double t3 = timingMapOut;

    // ---- 4. ROI 外补齐 ----
    // 已在 3b 处完成（挪到 Map 之前，与 GPU 重叠）。这里留一段说明，免得下次
    // 有人照着 CPU 版 surface3RgbBand 的语义顺序以为漏了。
    //
    // CPU 版是「整帧逐像素写 dst」：mask=0 处写 dst=src，mask>0 处写磨皮结果。
    // 所以本函数必须保证调用返回后 dst 全帧有效 —— 生产调用点（beauty_core）
    // 进来之前**不保证** dst 已拷过 src，那里只有两条早退分支会 memcpy。
    // 这一条是本函数能被当作 surface3RgbBand 直接替换的前提，不能指望调用方。
    //
    // 用四条带而不是整帧 memcpy：被替换掉的正是 ROI 那一块（4K 实测占 34%），
    // 上下两条是整行连续拷贝，左右两条在大矩形边上各拷一小段。
    // dst == src（原地处理）时整段跳过：矩形外本来就已经是 src 的内容。

    // t4 = 帧尾。补齐已并入读回前的等待，所以帧尾就是 Unmap 返回那一刻。
    const double t4 = t3;

    if (timing) {
        timing->uploadMs   = t1 - t0;
        timing->computeMs  = t2 - t1;
        // downloadMs 只算「读回」本身（提交拷贝 + 等 GPU + 读 staging），
        // 要能直接对上台架量的读回口径；补齐虽然挪到了读回之前，但它不是读回。
        timing->downloadMs = (t3 - t2) - timingFillOut;
        timing->totalMs    = t4 - t0;
        timing->packMs     = ta - t0;
        timing->maskMs     = tb - ta;
        timing->copyUpMs   = tc - tb;
        timing->copyDnMs   = td - t2;
        timing->mapMs      = timingMapOut - td - timingFillOut;
        timing->mapWaitMs  = timingMapWaitOut;
        timing->mapCopyMs  = timingMapCopyOut;
        // 上传侧的同一套拆分：upMapMs 是三次 Map 本身，剩下的是纯 memcpy/memset。
        // upMemMs 用 tb-t0 减而不是另取时间戳，这样两者相加恒等于「源图+掩码」段，
        // 不会因为多插几对 QPC 调用把测量本身算成开销。
        timing->upMapMs    = upMapMs;
        timing->upMemMs    = (tb - t0) - upMapMs;
        timing->fillMs     = timingFillOut;
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
