// gpu_beauty.cpp —— D3D11 GPU 后端（P1：ROI 优先的传输管道）
//
// 设计理由见 gpu_beauty.h 文件头。这里只记实现层面必须知道的几个坑：
//
// 坑 1：DYNAMIC → STAGING 的 CopyResource 走驱动慢路径（P1 校准里实测把下载
//       测成 2.2 GB/s，真值 16.8 GB/s）。下载一律经 DEFAULT 中转。
//
// 坑 2：不能把 DYNAMIC 纹理直接当 SRV 让 CS 读。部分驱动对「可锁纹理被着色器
//       读取」会退化成每次 dispatch 前内部影子拷贝，实测把 1.84MP 的恒等拷贝
//       从 1ms 拖到 15ms。先 CopyResource 到 DEFAULT 再读。
//
// 坑 3：显式 fence 极贵。本机 End(QUERY)+Flush+GetData 自旋固定 11~15ms，
//       与帧大小无关 —— 那是 Optimus 的同步代价，不是 GPU 在算。
//       管道里只留 Map(READ) 一次自然阻塞。
//
// 坑 4：帧缓冲是 RGBA8，不是 float。按 float 读会 4 倍越界。
//       P1 第一版就这么崩过（access violation），根因是只看 process() 的
//       `unsigned char*` 参数就假定它是 float 位模式。

#include "gpu_beauty.h"
#include "beauty_hwprobe.h"
#include "beauty_log.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace {

// 引擎帧缓冲的像素格式。R8G8B8A8_UINT 而不是 _UNORM：UINT 读出来就是
// 0..255 的原始整数，不经过任何归一化，恒等拷贝天然逐字节精确。
const DXGI_FORMAT kFmt = DXGI_FORMAT_R8G8B8A8_UINT;

// ---------------------------------------------------------------- 全局状态

std::mutex           g_mtx;
ID3D11Device*        g_dev  = nullptr;
ID3D11DeviceContext* g_ctx  = nullptr;
ID3D11ComputeShader* g_cs[GPU_BEAUTY_STAGE_COUNT] = {nullptr, nullptr};

ID3D11Texture2D*           g_texPack = nullptr;  // DYNAMIC，Map 写入（仅上传）
ID3D11Texture2D*           g_texA    = nullptr;  // DEFAULT，CS 输入
ID3D11Texture2D*           g_texB    = nullptr;  // DEFAULT，CS 输出
ID3D11Texture2D*           g_stage   = nullptr;  // STAGING，下载
ID3D11ShaderResourceView*  g_srvA    = nullptr;
ID3D11UnorderedAccessView* g_uavB    = nullptr;
int                        g_texW    = 0;
int                        g_texH    = 0;

bool g_tried    = false;
bool g_ready    = false;
char g_err[512] = {0};

long long g_frames       = 0;
double    g_sumUpload    = 0.0;
double    g_sumSubmit    = 0.0;
double    g_sumDownload  = 0.0;
int       g_lastMismatch = -1;
int       g_lastMaxDiff  = 0;

// ---------------------------------------------------------------- 工具函数

double nowMsQpc() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f;
    }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
}

void setErr(const char* what, HRESULT hr) {
    std::snprintf(g_err, sizeof(g_err), "%s (hr=0x%08lX)", what, (unsigned long)hr);
}

void releaseTextures() {
    if (g_srvA)    { g_srvA->Release();    g_srvA    = nullptr; }
    if (g_uavB)    { g_uavB->Release();    g_uavB    = nullptr; }
    if (g_texPack) { g_texPack->Release(); g_texPack = nullptr; }
    if (g_texA)    { g_texA->Release();    g_texA    = nullptr; }
    if (g_texB)    { g_texB->Release();    g_texB    = nullptr; }
    if (g_stage)   { g_stage->Release();   g_stage   = nullptr; }
    g_texW = g_texH = 0;
}

// 拆设备（调用方必须持 g_mtx）。
//
// P1 修复（2026-11-06）：原先只有 gpuBeautyShutdown 里有一份内联的拆卸代码，
// 而 createDevice 在「设备建好了、着色器没编译过」时会直接 return false —— 
// 此时 g_dev/g_ctx 非空但 g_ready=false，g_tried 又已经置位，于是这个设备
// 永远不会被任何人释放，一直挂到进程退出。抽出来之后两条路径共用同一份逻辑。
void releaseDevice() {
    releaseTextures();
    for (int i = 0; i < GPU_BEAUTY_STAGE_COUNT; ++i) {
        if (g_cs[i]) { g_cs[i]->Release(); g_cs[i] = nullptr; }
    }
    if (g_ctx) { g_ctx->ClearState(); g_ctx->Flush(); g_ctx->Release(); g_ctx = nullptr; }
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
    g_texW = g_texH = 0;
}

// ---------------------------------------------------------------- 着色器

// 恒等拷贝。读 SRV 得 uint4(0..255)，写 UAV 存 uint4(0..255)，中间不做任何
// 归一化 —— 所以「逐字节一致」是构造保证，不是容差内相等。
//
// 注意 RWTexture2D 的下标必须写成 tid.xy（operator[] 只接受一个 uint2，
// 写成 [x, y] 会编译失败 —— P1 的基准里踩过）。
const char* kCsIdentity = R"HLSL(
Texture2D<uint4>    srcTex : register(t0);
RWTexture2D<uint4>  dstTex : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID) {
    uint w, h;
    dstTex.GetDimensions(w, h);
    if (tid.x >= w || tid.y >= h) return;
    dstTex[tid.xy] = srcTex.Load(int3((int)tid.x, (int)tid.y, 0));
}
)HLSL";

// 交换 R/B。用途有两个：
//   1) 证明 dispatch 真的执行了（恒等全过也可能是 CS 根本没跑）；
//   2) 证明校验函数抓得住差异（若比对永远返回「一致」，P2 的对拍就毫无意义）。
const char* kCsSwapRb = R"HLSL(
Texture2D<uint4>    srcTex : register(t0);
RWTexture2D<uint4>  dstTex : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID) {
    uint w, h;
    dstTex.GetDimensions(w, h);
    if (tid.x >= w || tid.y >= h) return;
    uint4 c = srcTex.Load(int3((int)tid.x, (int)tid.y, 0));
    dstTex[tid.xy] = uint4(c.z, c.y, c.x, c.w);
}
)HLSL";

bool compileCs(const char* src, const char* name, ID3D11ComputeShader** out) {
    ID3DBlob* code = nullptr;
    ID3DBlob* err  = nullptr;
    UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS;
    HRESULT hr = D3DCompile(src, std::strlen(src), name, nullptr, nullptr,
                            "CSMain", "cs_5_0", flags, 0, &code, &err);
    if (FAILED(hr)) {
        std::snprintf(g_err, sizeof(g_err), "编译 %s 失败: %s", name,
                      err ? (const char*)err->GetBufferPointer() : "(无信息)");
        if (err)  err->Release();
        if (code) code->Release();
        return false;
    }
    hr = g_dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(),
                                    nullptr, out);
    code->Release();
    if (err) err->Release();
    if (FAILED(hr)) { setErr("CreateComputeShader", hr); return false; }
    return true;
}

// ---------------------------------------------------------------- 设备创建

// 与 beauty_hwprobe 的选法一致：排除软件渲染器（Basic Render Driver 也能建
// 设备，但比 CPU 还慢），独显优先，其次显存大者。本机是 RTX 5060 Laptop +
// Intel 核显双显卡，选错的后果是 4K 纹理在核显共享显存上分配失败。
IDXGIAdapter1* pickAdapter() {
    IDXGIFactory1* fac = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&fac)) || !fac)
        return nullptr;

    IDXGIAdapter1* best = nullptr;
    unsigned long long bestScore = 0;
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* ad = nullptr;
        if (fac->EnumAdapters1(i, &ad) != S_OK || !ad) break;
        DXGI_ADAPTER_DESC1 d{};
        if (FAILED(ad->GetDesc1(&d))) { ad->Release(); continue; }
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) { ad->Release(); continue; }

        unsigned long long score = d.DedicatedVideoMemory / (1024ull * 1024ull) + 1;
        if (d.VendorId != 0x8086) score += 100000ull;   // 独显优先
        if (score > bestScore) {
            if (best) best->Release();
            best = ad;
            bestScore = score;
        } else {
            ad->Release();
        }
    }
    fac->Release();
    return best;
}

bool createDevice() {
    IDXGIAdapter1* ad = pickAdapter();
    if (!ad) { std::snprintf(g_err, sizeof(g_err), "没有可用 D3D11 适配器"); return false; }

    DXGI_ADAPTER_DESC1 d{};
    ad->GetDesc1(&d);

    static const D3D_FEATURE_LEVEL want[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0
    };
    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(ad, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                                   want, 2, D3D11_SDK_VERSION,
                                   &g_dev, &got, &g_ctx);
    if (hr == E_INVALIDARG) {
        hr = D3D11CreateDevice(ad, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                               want + 1, 1, D3D11_SDK_VERSION,
                               &g_dev, &got, &g_ctx);
    }
    ad->Release();
    if (FAILED(hr) || !g_dev || !g_ctx) {
        setErr("D3D11CreateDevice", hr);
        releaseDevice();      // 半成功也要拆干净，见 releaseDevice 的注释
        return false;
    }

    if (!compileCs(kCsIdentity, "gpu_beauty_identity", &g_cs[GPU_BEAUTY_IDENTITY])) {
        releaseDevice();
        return false;
    }
    if (!compileCs(kCsSwapRb,   "gpu_beauty_swaprb",   &g_cs[GPU_BEAUTY_SWAP_RB])) {
        releaseDevice();
        return false;
    }

    NALOG(BEAUTY_LOG_INFO,
          "GPU 后端已就绪：适配器=%ls 显存=%lluMB CS5.0 编译通过",
          d.Description, (unsigned long long)(d.DedicatedVideoMemory / (1024 * 1024)));
    return true;
}

// ---------------------------------------------------------------- 纹理池

bool ensureTextures(int w, int h) {
    if (g_texPack && g_texA && g_texB && g_stage && g_texW == w && g_texH == h) return true;
    releaseTextures();
    if (w <= 0 || h <= 0) { std::snprintf(g_err, sizeof(g_err), "ROI 尺寸非法"); return false; }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)w; td.Height = (UINT)h;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = kFmt;
    td.SampleDesc.Count = 1;

    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    HRESULT hr = g_dev->CreateTexture2D(&td, nullptr, &g_texPack);
    if (FAILED(hr)) { setErr("CreateTexture2D(upload)", hr); return false; }

    // 两张 DEFAULT 做 ping-pong：P2 的 21 趟磨皮每趟换方向，P1 只用 A→B 一趟，
    // 但结构先摆好，P2 不用改分配。
    D3D11_TEXTURE2D_DESC ad = td;
    ad.Usage = D3D11_USAGE_DEFAULT;
    ad.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    ad.CPUAccessFlags = 0;
    hr = g_dev->CreateTexture2D(&ad, nullptr, &g_texA);
    if (FAILED(hr)) { setErr("CreateTexture2D(A)", hr); releaseTextures(); return false; }
    hr = g_dev->CreateTexture2D(&ad, nullptr, &g_texB);
    if (FAILED(hr)) { setErr("CreateTexture2D(B)", hr); releaseTextures(); return false; }

    D3D11_TEXTURE2D_DESC sd = td;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = g_dev->CreateTexture2D(&sd, nullptr, &g_stage);
    if (FAILED(hr)) { setErr("CreateTexture2D(staging)", hr); releaseTextures(); return false; }

    hr = g_dev->CreateShaderResourceView(g_texA, nullptr, &g_srvA);
    if (FAILED(hr)) { setErr("CreateSRV", hr); releaseTextures(); return false; }
    hr = g_dev->CreateUnorderedAccessView(g_texB, nullptr, &g_uavB);
    if (FAILED(hr)) { setErr("CreateUAV", hr); releaseTextures(); return false; }

    g_texW = w; g_texH = h;
    return true;
}

// ROI 向外对齐到偶数：P2 的磨皮第一趟是 2×2 整数抽样下采样，
// 奇数宽高会让半分辨率层的边界与 CPU 参考版对不上。
GpuBeautyRoi alignRoi(GpuBeautyRoi r, int w, int h) {
    if (r.x0 < 0) r.x0 = 0;
    if (r.y0 < 0) r.y0 = 0;
    if (r.x1 > w) r.x1 = w;
    if (r.y1 > h) r.y1 = h;
    r.x0 &= ~1;
    r.y0 &= ~1;
    if (r.x1 < w) r.x1 = (r.x1 + 1) & ~1;
    if (r.y1 < h) r.y1 = (r.y1 + 1) & ~1;
    return r;
}

}  // namespace

// ---------------------------------------------------------------- 公开接口

bool gpuBeautyInit() {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_tried) return g_ready;
    g_tried = true;

    const BeautyHwInfo& hw = beautyHwProbe();
    if (hw.gpuTier == BEAUTY_GPU_NONE || !hw.gpuCs50Ok) {
        std::snprintf(g_err, sizeof(g_err), "硬件探测判定无可用 GPU（tier=%d cs5=%d）",
                      hw.gpuTier, hw.gpuCs50Ok ? 1 : 0);
        g_ready = false;
        return false;
    }
    g_ready = createDevice();
    if (!g_ready) NALOG(BEAUTY_LOG_WARNING, "GPU 后端初始化失败：%s（回退 CPU）", g_err);
    return g_ready;
}

bool gpuBeautyReady() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_ready;
}

void gpuBeautyShutdown() {
    std::lock_guard<std::mutex> lk(g_mtx);
    // 2026-11-06：这个函数以前是死代码（零调用点），设备与纹理留到进程退出。
    // 现在由 ofx_plugin.cpp 的 onUnload 调用；g_tried 复位后，重载插件还能重建。
    releaseDevice();
    g_ready = false;
    g_tried = false;
    g_frames = 0;
    g_sumUpload = g_sumSubmit = g_sumDownload = 0.0;
}

bool gpuBeautyApplyRoi(const unsigned char* src, unsigned char* dst,
                       int w, int h, int rowPixels,
                       GpuBeautyRoi roi, int stage, GpuBeautyTiming* timing) {
    if (!src || !dst || w <= 0 || h <= 0 || rowPixels < w) return false;
    if (stage < 0 || stage >= GPU_BEAUTY_STAGE_COUNT) return false;
    roi = alignRoi(roi, w, h);
    if (roi.empty()) return false;

    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_ready) return false;
    const int rw = roi.x1 - roi.x0, rh = roi.y1 - roi.y0;
    if (!ensureTextures(rw, rh)) return false;

    const size_t rowBytes = (size_t)rw * 4;

    // ---- 1. 上传：只搬 ROI 行 ----
    double t0 = nowMsQpc();
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g_ctx->Map(g_texPack, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        std::snprintf(g_err, sizeof(g_err), "Map(upload) 失败");
        return false;
    }
    for (int y = 0; y < rh; ++y) {
        const unsigned char* s = src + ((size_t)(roi.y0 + y) * rowPixels + roi.x0) * 4;
        std::memcpy((unsigned char*)m.pData + (size_t)y * m.RowPitch, s, rowBytes);
    }
    g_ctx->Unmap(g_texPack, 0);
    // 坑 2：DYNAMIC 不直接当 SRV，先设备内拷到 DEFAULT。
    g_ctx->CopyResource(g_texA, g_texPack);
    double t1 = nowMsQpc();

    // ---- 2. 计算 ----
    ID3D11ShaderResourceView*  srv = g_srvA;
    ID3D11UnorderedAccessView* uav = g_uavB;
    g_ctx->CSSetShader(g_cs[stage], nullptr, 0);
    g_ctx->CSSetShaderResources(0, 1, &srv);
    g_ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    g_ctx->Dispatch((UINT)((rw + 7) / 8), (UINT)((rh + 7) / 8), 1);

    ID3D11ShaderResourceView*  nullSrv = nullptr;
    ID3D11UnorderedAccessView* nullUav = nullptr;
    g_ctx->CSSetShaderResources(0, 1, &nullSrv);
    g_ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    g_ctx->CSSetShader(nullptr, nullptr, 0);

    // 坑 1：下载经 DEFAULT 中转。坑 3：不插 fence，靠 Map(READ) 自然阻塞。
    g_ctx->CopyResource(g_stage, g_texB);
    double t2 = nowMsQpc();

    // ---- 3. 下载：只写回 ROI 行 ----
    D3D11_MAPPED_SUBRESOURCE md{};
    if (FAILED(g_ctx->Map(g_stage, 0, D3D11_MAP_READ, 0, &md))) {
        std::snprintf(g_err, sizeof(g_err), "Map(download) 失败");
        return false;
    }
    for (int y = 0; y < rh; ++y) {
        unsigned char* d = dst + ((size_t)(roi.y0 + y) * rowPixels + roi.x0) * 4;
        std::memcpy(d, (const unsigned char*)md.pData + (size_t)y * md.RowPitch, rowBytes);
    }
    g_ctx->Unmap(g_stage, 0);
    double t3 = nowMsQpc();

    if (timing) {
        timing->uploadMs   = t1 - t0;
        timing->submitMs   = t2 - t1;
        timing->downloadMs = t3 - t2;   // 含 GPU 执行
        timing->totalMs    = t3 - t0;
        timing->roiW = rw;
        timing->roiH = rh;
    }
    g_frames++;
    g_sumUpload   += (t1 - t0);
    g_sumSubmit   += (t2 - t1);
    g_sumDownload += (t3 - t2);
    return true;
}

bool gpuBeautyVerifyRoi(const unsigned char* src, int w, int h, int rowPixels,
                        GpuBeautyRoi roi, int stage,
                        GpuBeautyTiming* timing,
                        int* outMismatch, int* outMaxDiff) {
    if (outMismatch) *outMismatch = -1;
    if (outMaxDiff)  *outMaxDiff = 0;

    GpuBeautyRoi r = alignRoi(roi, w, h);
    if (r.empty()) return false;
    const int rw = r.x1 - r.x0, rh = r.y1 - r.y0;

    // ROI 大小的临时缓冲：4K 全帧 8.3MP 的 RGBA8 是 33MB，按人脸 ROI 只要 7.4MB。
    // thread_local 而不是 static：Resolve 可能多线程并发 render 多个实例。
    const size_t n = (size_t)rw * (size_t)rh * 4;
    static thread_local std::vector<unsigned char> tmp;
    if (tmp.size() < n) tmp.resize(n);

    for (int y = 0; y < rh; ++y) {
        const unsigned char* s = src + ((size_t)(r.y0 + y) * rowPixels + r.x0) * 4;
        std::memcpy(&tmp[(size_t)y * rw * 4], s, (size_t)rw * 4);
    }

    GpuBeautyRoi local{0, 0, rw, rh};
    if (!gpuBeautyApplyRoi(&tmp[0], &tmp[0], rw, rh, rw, local, stage, timing))
        return false;

    int mism = 0, maxd = 0;
    for (int y = 0; y < rh; ++y) {
        const unsigned char* a = src + ((size_t)(r.y0 + y) * rowPixels + r.x0) * 4;
        const unsigned char* b = &tmp[(size_t)y * rw * 4];
        const size_t len = (size_t)rw * 4;
        if (std::memcmp(a, b, len) != 0) {
            for (size_t i = 0; i < len; ++i) {
                const int d = (int)a[i] - (int)b[i];
                if (d != 0) {
                    ++mism;
                    const int ad = (d < 0) ? -d : d;
                    if (ad > maxd) maxd = ad;
                }
            }
        }
    }
    if (outMismatch) *outMismatch = mism;
    if (outMaxDiff)  *outMaxDiff = maxd;

    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_lastMismatch = mism;
        g_lastMaxDiff  = maxd;
    }
    return mism == 0;
}

void gpuBeautyStats(long long* frames, double* avgUpload, double* avgSubmit,
                    double* avgDownload, int* lastMismatch, int* lastMaxDiff) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (frames)       *frames       = g_frames;
    if (avgUpload)    *avgUpload    = g_frames ? g_sumUpload / g_frames : 0.0;
    if (avgSubmit)    *avgSubmit    = g_frames ? g_sumSubmit / g_frames : 0.0;
    if (avgDownload)  *avgDownload  = g_frames ? g_sumDownload / g_frames : 0.0;
    if (lastMismatch) *lastMismatch = g_lastMismatch;
    if (lastMaxDiff)  *lastMaxDiff  = g_lastMaxDiff;
}

const char* gpuBeautyLastError() { return g_err; }
