// beauty_hwprobe.cpp —— 硬件能力探测与渲染路径自动定档（P1 实现）
//
// 设计约束：
//   1) 不依赖任何 OFX 头文件 —— 本文件同时被插件与离线基准工具使用
//   2) 不抛异常、不 abort —— 探测失败一律降级为 BEAUTY_GPU_NONE
//   3) 探测结果只写一次，之后全是只读访问（多实例并发安全）
//
// 阈值推导（依据 P1 基准实测，见 _p1bench/d3d11_transfer_bench.cpp 运行结果）：
//   CPU 侧：1080p 8 线程 24.6ms → 11.9 ns/px；4K 127ms → 15.3 ns/px
//           （4K 超线性，因为内存带宽压力，故分两段取）
//   GPU 侧：4K 上传 14.32ms（9.3GB/s）、下载 7.90ms（16.8GB/s）、
//           21 趟全分辨率 load/store 25.85ms
//           真实负载是 18 趟半分辨率 + 3 趟全分辨率 ≈ 7.5 趟全分辨率等效
//           → 计算约 25.85 * 7.5/21 ≈ 9.2ms
//   双缓冲异步后帧时间 ≈ max(上传 + 计算, 下载)
//     → 4K  ≈ max(14.3 + 9.2, 7.9) = 23.5ms   vs CPU 127ms
//     → 1080p ≈ max(4.75 + 2.3, 2.3) = 7.1ms  vs CPU 24.6ms
//   结论：只要 GPU 可用且分辨率不是极小，GPU 都显著更快；阈值只需挡住
//   「小预览帧」以免频繁设备交互，并按实测带宽动态调整。

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <string>
#include <mutex>
#include <vector>

#include "beauty_hwprobe.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "advapi32.lib")   // RegOpenKeyEx: 读 CPU 基准频率

// ---------------------------------------------------------------------------
// 内部状态
// ---------------------------------------------------------------------------
static BeautyHwInfo g_info;
static std::once_flag g_probeOnce;
static std::mutex     g_calibMutex;

static void finalizeVerdict(BeautyHwInfo& info);   // 前置声明：校准后需重算结论

// 上传带宽下限（GB/s）。低于它就按它算，理由见 beautyHwCalibrate 内的说明。
static const double kBwFloorGBs = 5.0;

// ---------------------------------------------------------------------------
// 配置文件（beauty.ini）
//
// 为什么要有它：2026-11-06 排错时管道代码本身是好的，但那次 Resolve 是从桌面
// 图标启动的，launch.bat 里设的 BEAUTY_GPU_PIPE 压根继承不到，于是「日志里
// 一行管道都没有」被当成管道故障，白跑一个来回。开关必须落在磁盘上，不能挂在
// 「你从哪个图标启动」上。
//
// 三级优先级：环境变量 > beauty.ini > 代码内自动默认。
// ---------------------------------------------------------------------------
static const char kIniTemplate[] =
    "; DaVinci Beauty 运行配置 v3.3.0\r\n"
    "; 改完保存后需要重启 DaVinci Resolve 才生效。\r\n"
    "; 同名环境变量优先级更高：BEAUTY_PATH / BEAUTY_GPU_PIPE / BEAUTY_GPU_SMOOTH / BEAUTY_GPU_MINPIX\r\n"
    "\r\n"
    "; path: auto = 按硬件探测自动定档 | gpu = 强制 GPU | cpu = 强制 CPU\r\n"
    "path = auto\r\n"
    "\r\n"
    "; gpu_pipe: GPU 渲染管道\r\n"
    ";   auto = 首帧自检一次（日志会写一行 GPU 管道），之后不再跑，零额外开销\r\n"
    ";   on   = 每帧都过一遍（验证用，多花 3~4ms）\r\n"
    ";   off  = 完全关闭\r\n"
    "gpu_pipe = auto\r\n"
    "\r\n"
    "; gpu_smooth: 磨皮改走 GPU（D3D11 Compute）\r\n"
    ";   auto = 走 GPU，任何一步失败（无卡 / 特性等级不足 / 显存不足）当场回落 CPU（1.0.9.0 起默认）\r\n"
    ";   off  = 走 CPU。行为与 1.0.8.0 及更早版本完全一致\r\n"
    ";   on   = 同 auto，表示已目视确认过\r\n"
    "; 2026-11-07 已在 1080x1920 真实素材上目视确认：无色块/断层、发丝与衣领无糊、亮度无偏，\r\n"
    "; 累计 4928 帧零回落，均合计 4.55ms。选卡只排除 WARP，AMD / Intel 核显同样会走 GPU。\r\n"
    "; 日志里会有「GPU 磨皮[auto] ... 合计=x.xx ms」与一行适配器型号，可用来核对。\r\n"
    "gpu_smooth = auto\r\n"
    "\r\n"
    "; gpu_minpix: 低于此像素数的画面走 CPU（0 = 按实测带宽自动推导）\r\n"
    "gpu_minpix = 0\r\n";

// 去掉首尾空白（含 \r \n \t 空格）。
static std::string trimStr(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::string& cfgPathRef() {
    static std::string p = [] {
        const char* pd = getenv("ProgramData");
        std::string d = (pd && *pd) ? pd : "C:\\ProgramData";
        d += "\\DaVinciBeauty\\beauty.ini";
        return d;
    }();
    return p;
}

// 首次运行生成模板。带 UTF-8 BOM：没有 BOM 时中文注释在部分 Windows 记事本里
// 会显示成乱码，用户就不知道该把 auto 改成什么，配置项等于白给。
static void cfgWriteTemplate(const std::string& path) {
    std::string dir = path.substr(0, path.find_last_of("\\/"));
    if (!dir.empty()) CreateDirectoryA(dir.c_str(), nullptr);

    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
    fwrite(bom, 1, sizeof(bom), f);
    fwrite(kIniTemplate, 1, strlen(kIniTemplate), f);
    fclose(f);
}

static void cfgLoad(std::vector<std::pair<std::string, std::string>>& kv) {
    const std::string& path = cfgPathRef();
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { cfgWriteTemplate(path); return; }   // 模板内容全是默认值，等同未配

    std::string text;
    char buf[4096];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);

    size_t i = 0;
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) i = 3;

    while (i < text.size()) {
        size_t e = text.find('\n', i);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(i, e - i);
        i = e + 1;

        size_t cm = line.find_first_of("#;");
        if (cm != std::string::npos) line.erase(cm);
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;    // 空行 / [section] / 纯注释

        std::string k = trimStr(line.substr(0, eq));
        std::string v = trimStr(line.substr(eq + 1));
        if (k.empty()) continue;
        for (char& c : k) c = (char)tolower((unsigned char)c);
        kv.emplace_back(std::move(k), std::move(v));
    }
}

// 找不到返回 nullptr。同一键出现多次时取最后一次（便于在文件末尾追加覆盖）。
static const std::string* cfgFind(const char* key) {
    static const std::vector<std::pair<std::string, std::string>> kv = [] {
        std::vector<std::pair<std::string, std::string>> v;
        cfgLoad(v);
        return v;
    }();
    if (!key) return nullptr;
    std::string k(key);
    for (char& c : k) c = (char)tolower((unsigned char)c);
    for (auto it = kv.rbegin(); it != kv.rend(); ++it)
        if (it->first == k) return &it->second;
    return nullptr;
}

const char* beautyCfgStr(const char* key, const char* def) {
    const std::string* v = cfgFind(key);
    return (v && !v->empty()) ? v->c_str() : def;
}

long long beautyCfgInt(const char* key, long long def) {
    const std::string* v = cfgFind(key);
    if (!v || v->empty()) return def;
    char* end = nullptr;
    long long n = _strtoi64(v->c_str(), &end, 10);
    // "auto" / "on" / "off" 不是数字，交回调用方按字符串处理
    return (end && end != v->c_str()) ? n : def;
}

const char* beautyCfgPath() { return cfgPathRef().c_str(); }

// ---------------------------------------------------------------------------
// CPU / 内存
// ---------------------------------------------------------------------------
static void probeCpu(BeautyHwInfo& info) {
    info.cpuLogical = (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (info.cpuLogical <= 0) {
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        info.cpuLogical = (int)si.dwNumberOfProcessors;
    }

    // 物理核：按 RelationProcessorCore 统计（每个 core 条目可能含多组 SMT）
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (len > 0) {
        std::vector<unsigned char> buf(len);
        if (GetLogicalProcessorInformationEx(RelationProcessorCore,
                (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf.data(), &len)) {
            int cores = 0;
            DWORD off = 0;
            while (off < len) {
                auto* e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf.data() + off);
                if (e->Relationship == RelationProcessorCore) ++cores;
                if (e->Size == 0) break;
                off += e->Size;
            }
            info.cpuPhysical = cores > 0 ? cores : info.cpuLogical;
        }
    }
    if (info.cpuPhysical <= 0) info.cpuPhysical = info.cpuLogical;

    HKEY k = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                      0, KEY_READ, &k) == ERROR_SUCCESS) {
        DWORD mhz = 0, sz = sizeof(mhz), type = 0;
        if (RegQueryValueExA(k, "~MHz", nullptr, &type, (LPBYTE)&mhz, &sz) == ERROR_SUCCESS)
            info.cpuMaxMhz = (int)mhz;
        RegCloseKey(k);
    }

    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms))
        info.memTotalMB = (unsigned long long)(ms.ullTotalPhys / (1024ull * 1024ull));
}

// ---------------------------------------------------------------------------
// GPU 探测
// ---------------------------------------------------------------------------
static bool checkFormatForUav(ID3D11Device* dev, DXGI_FORMAT f, bool& typedUavRead) {
    UINT sup = 0;
    if (FAILED(dev->CheckFormatSupport(f, &sup))) return false;
    typedUavRead = (sup & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW) != 0;
    return (sup & D3D11_FORMAT_SUPPORT_TEXTURE2D) != 0;
}

static void probeGpu(BeautyHwInfo& info) {
    IDXGIFactory1* fac = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&fac))) return;

    IDXGIAdapter1* best = nullptr;
    double bestScore = -1.0;

    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* a = nullptr;
        if (fac->EnumAdapters1(i, &a) != S_OK) break;
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        ++info.gpuAdapterCount;

        bool soft = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        bool intel = (d.VendorId == 0x8086);
        double vramGB = (double)d.DedicatedVideoMemory / (1024.0 * 1024.0 * 1024.0);
        // 独显优先；同为独显/核显时显存大者优先；软件渲染器直接排除
        double score = soft ? -1.0 : (vramGB + (intel ? 0.0 : 1000.0));

        if (score > bestScore) {
            bestScore = score;
            if (best) best->Release();
            best = a;
        } else {
            a->Release();
        }
    }

    if (!best) { fac->Release(); return; }

    DXGI_ADAPTER_DESC1 bd{};
    best->GetDesc1(&bd);
    WideCharToMultiByte(CP_UTF8, 0, bd.Description, -1,
                        info.gpuName, (int)sizeof(info.gpuName), nullptr, nullptr);
    info.gpuVendorId = bd.VendorId;
    info.gpuVramMB   = (unsigned long long)(bd.DedicatedVideoMemory / (1024ull * 1024ull));
    info.gpuIsDiscrete = (bd.VendorId != 0x8086);

    D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got{};
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDevice(best, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                                   fl, _countof(fl), D3D11_SDK_VERSION,
                                   &dev, &got, &ctx);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(best, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                               &fl[1], 1, D3D11_SDK_VERSION, &dev, &got, &ctx);
    }
    best->Release();
    fac->Release();

    if (FAILED(hr) || !dev) { if (ctx) ctx->Release(); return; }
    info.gpuFeatureLevel = (int)got;

    // CS 5.0：实际编译一个最小 compute shader，比查 flag 可靠
    {
        static const char* kMin = "RWTexture2D<float4> o : register(u0);\n"
                                  "[numthreads(1,1,1)] void CSMain(uint3 i:SV_DispatchThreadID){o[i.xy]=1;}\n";
        ID3DBlob* code = nullptr;
        HRESULT h = D3DCompile(kMin, strlen(kMin), "min.hlsl", nullptr, nullptr, "CSMain",
                               "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, nullptr);
        if (SUCCEEDED(h) && code) {
            ID3D11ComputeShader* cs = nullptr;
            info.gpuCs50Ok = SUCCEEDED(dev->CreateComputeShader(
                code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs)) && cs != nullptr;
            if (cs) cs->Release();
            code->Release();
        }
    }

    // 16F / 32F 的 typed UAV 读取支持（OBS 在该卡上踩过 32F 渲染目标崩溃）
    bool t16 = false, t32 = false;
    checkFormatForUav(dev, DXGI_FORMAT_R16G16B16A16_FLOAT, t16);
    checkFormatForUav(dev, DXGI_FORMAT_R32G32B32A32_FLOAT, t32);
    info.gpuTypedUav16F = t16;
    info.gpuTypedUav32F = t32;

    // STAGING 读回能力
    {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = 64; d.Height = 64; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* t = nullptr;
        info.gpuStagingOk = SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &t)) && t != nullptr;
        if (t) t->Release();
    }

    // 定档
    if (!info.gpuCs50Ok || !info.gpuStagingOk) {
        info.gpuTier = BEAUTY_GPU_NONE;
    } else if (!info.gpuIsDiscrete || info.gpuVramMB < 2048) {
        info.gpuTier = BEAUTY_GPU_LOW;
    } else if (info.gpuVramMB < 6144) {
        info.gpuTier = BEAUTY_GPU_MID;
    } else {
        info.gpuTier = BEAUTY_GPU_HIGH;
    }

    ctx->Release();
    dev->Release();
}

// ---------------------------------------------------------------------------
// 微型带宽校准
// ---------------------------------------------------------------------------
static double nowMsQpc() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
}

struct CalibDev {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
};

static bool openBestDevice(CalibDev& out) {
    IDXGIFactory1* fac = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&fac))) return false;

    IDXGIAdapter1* best = nullptr;
    double bestScore = -1.0;
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* a = nullptr;
        if (fac->EnumAdapters1(i, &a) != S_OK) break;
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        bool soft = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        double vramGB = (double)d.DedicatedVideoMemory / (1024.0 * 1024.0 * 1024.0);
        double score = soft ? -1.0 : (vramGB + ((d.VendorId == 0x8086) ? 0.0 : 1000.0));
        if (score > bestScore) { bestScore = score; if (best) best->Release(); best = a; }
        else a->Release();
    }
    bool ok = false;
    if (best) {
        D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        D3D_FEATURE_LEVEL got{};
        HRESULT hr = D3D11CreateDevice(best, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                                       fl, _countof(fl), D3D11_SDK_VERSION,
                                       &out.dev, &got, &out.ctx);
        if (FAILED(hr))
            hr = D3D11CreateDevice(best, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                                   &fl[1], 1, D3D11_SDK_VERSION, &out.dev, &got, &out.ctx);
        ok = SUCCEEDED(hr) && out.dev && out.ctx;
        best->Release();
    }
    fac->Release();
    return ok;
}

static bool measureAt(CalibDev& d, int W, int H, double& upGBs, double& downGBs, double& copyMs) {
    const size_t rowBytes = (size_t)W * 16;
    std::vector<unsigned char> host((size_t)W * H * 16, 0x3C);

    // 关键：上传纹理必须轮转，否则 Map(WRITE_DISCARD) 会等上一帧的
    // CopyResource 读完才能返回，测出来是「含等待」的值（实测会把 9.3GB/s
    // 压成 3.0GB/s）。4 张轮转足以让 GPU 有 3 帧的时间消化。
    const int kRot = 4;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    ID3D11Texture2D* texUp[kRot] = {};
    for (int i = 0; i < kRot; ++i) {
        if (FAILED(d.dev->CreateTexture2D(&td, nullptr, &texUp[i]))) {
            for (int j = 0; j < i; ++j) texUp[j]->Release();
            return false;
        }
    }

    D3D11_TEXTURE2D_DESC sd = td;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* texStage = nullptr;
    if (FAILED(d.dev->CreateTexture2D(&sd, nullptr, &texStage))) {
        for (int i = 0; i < kRot; ++i) texUp[i]->Release();
        return false;
    }

    // DEFAULT 中间层不可省：DYNAMIC → STAGING 的 CopyResource 走驱动慢路径
    // （实测把下载压到 2.2GB/s），必须经 DEFAULT 中转才回到 16GB/s 级
    D3D11_TEXTURE2D_DESC gd = td;
    gd.Usage = D3D11_USAGE_DEFAULT;
    gd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    gd.CPUAccessFlags = 0;
    ID3D11Texture2D* texGpu = nullptr;
    if (FAILED(d.dev->CreateTexture2D(&gd, nullptr, &texGpu))) {
        for (int i = 0; i < kRot; ++i) texUp[i]->Release();
        texStage->Release();
        return false;
    }

    const int kFrames = 5;
    double tUp = 0, tDown = 0;

    // 显式同步点：把「上一帧 CopyResource 的 GPU 执行」挡在计时之外，
    // 否则它会顺延到 Map 里被算成下载时间（实测把 16GB/s 压成 2.8GB/s）
    D3D11_QUERY_DESC qd{};
    qd.Query = D3D11_QUERY_EVENT;
    ID3D11Query* syncQ = nullptr;
    d.dev->CreateQuery(&qd, &syncQ);

    auto drainGpu = [&]() {
        if (!syncQ) return;
        d.ctx->End(syncQ);
        d.ctx->Flush();
        while (d.ctx->GetData(syncQ, nullptr, 0, 0) == S_FALSE) YieldProcessor();
    };

    for (int f = 0; f <= kFrames; ++f) {
        ID3D11Texture2D* up = texUp[f % kRot];

        double t0 = nowMsQpc();
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(d.ctx->Map(up, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) break;
        for (int y = 0; y < H; ++y)
            memcpy((BYTE*)m.pData + (size_t)y * m.RowPitch,
                   host.data() + (size_t)y * rowBytes, rowBytes);
        d.ctx->Unmap(up, 0);
        double t1 = nowMsQpc();

        d.ctx->CopyResource(texGpu, up);
        drainGpu();                        // 不计时：纯 GPU 搬运

        double t2 = nowMsQpc();
        d.ctx->CopyResource(texStage, texGpu);
        D3D11_MAPPED_SUBRESOURCE md{};
        if (FAILED(d.ctx->Map(texStage, 0, D3D11_MAP_READ, 0, &md))) break;
        volatile unsigned char sink = 0;
        for (int y = 0; y < H; y += 128) sink ^= ((unsigned char*)md.pData)[(size_t)y * W * 16];
        (void)sink;
        d.ctx->Unmap(texStage, 0);
        double t3 = nowMsQpc();

        if (f > 0) {   // 丢掉第一帧（纹理首次触碰 + 驱动预热）
            tUp   += (t1 - t0);
            tDown += (t3 - t2);
        }
    }

    if (syncQ) syncQ->Release();
    for (int i = 0; i < kRot; ++i) texUp[i]->Release();
    texGpu->Release();
    texStage->Release();

    double bytes = (double)W * H * 16.0;
    upGBs   = bytes / (tUp / kFrames * 1e6);
    downGBs = bytes / (tDown / kFrames * 1e6);
    copyMs  = tDown / kFrames;   // 含显存内拷贝 + PCIe 回读，作端到端参考
    return upGBs > 0.01 && downGBs > 0.01;
}

bool beautyHwCalibrate() {
    std::lock_guard<std::mutex> lk(g_calibMutex);
    beautyHwProbe();                 // 确保静态探测已跑
    BeautyHwInfo& info = g_info;     // 同翻译单元内直接访问，避免 const_cast
    if (info.calibrated) return true;
    if (info.gpuTier == BEAUTY_GPU_NONE) return false;

    CalibDev d;
    if (!openBestDevice(d)) return false;

    // 只用 1080p 起步：PCIe 带宽与分辨率基本线性，1080p 足以定标，
    // 而 4K 校准要创建 4 张 133MB 纹理、耗时约 600ms —— 放在 onLoad 里
    // 会让打开工程明显变慢。1080p 单档约 80ms。
    //
    // 3.3.0：1080p 跑两轮取最大值。原因是实测同一台机器、同一份代码，
    // 三次启动量到 4.0 / 7.0 / 5.7 GB/s（离线无损环境下稳定 9.3）——
    // Resolve 启动期正在初始化 Fusion/CUDA，GPU 与内存带宽都被抢，单轮
    // 极容易撞在谷底。带宽是硬件的上限属性，多轮取最大值才逼近真值。
    struct Sz { int w, h; };
    const Sz sizes[] = { {1920, 1080}, {1280, 720}, {854, 480} };
    bool ok = false;
    double bestUp = 0, bestDown = 0, bestCpy = 0;
    int    bestW = 0, bestH = 0;

    for (const auto& s : sizes) {
        const int rounds = (s.w == 1920) ? 2 : 1;
        bool got = false;
        for (int r = 0; r < rounds; ++r) {
            double up = 0, down = 0, cpy = 0;
            if (!measureAt(d, s.w, s.h, up, down, cpy)) continue;
            got = true;
            if (up > bestUp) {
                bestUp = up; bestDown = down; bestCpy = cpy;
                bestW = s.w; bestH = s.h;
            }
        }
        if (got) { ok = true; break; }   // 该档可用就别再往小档试了
    }

    d.ctx->Release();
    d.dev->Release();

    if (ok) {
        info.uploadGBsRaw = bestUp;

        // 下限兜底：实测低于 5 GB/s 时按 5 算。
        //
        // 为什么需要：阈值公式是 300000 × 10 / upload，带宽每低 1 GB/s，阈值
        // 就抬高一截。实测 4.0 GB/s 会推出 74.8 万像素的门槛 —— 那意味着
        // 1080p 勉强过线、1440p 以下的中等尺寸素材全被误判到 CPU 慢路径，
        // 而 207 万像素的画面在 GPU 上实测只有 0.94ms 搬运。
        //
        // 5.0 的依据：离线基准本机 9.3、真机启动期 4.0~7.0。取下限 5 得到
        // k=2.0、阈值 60 万像素（约 1035×580），既挡住小预览帧，又不会把
        // 正常素材推回 CPU。它是个保守的地板，不是最优解 —— P2 会改成用
        // 首帧的真实 ROI 面积与实测帧时间做反馈修正，届时这个常量退休。
        if (bestUp < kBwFloorGBs) {
            info.uploadFloored = true;
            info.uploadGBs     = kBwFloorGBs;
        } else {
            info.uploadFloored = false;
            info.uploadGBs     = bestUp;
        }
        info.downloadGBs = bestDown;
        // 归一到 4K 单趟 copy 成本，供上层按面积估算算力
        info.gpuCopyMs4k = bestCpy * ((double)(3840 * 2160) /
                                      ((double)bestW * bestH)) * 0.5;
        info.calibrated  = true;
    }

    if (ok) {
        // 动态阈值：带宽越差，越倾向于让小画面走 CPU。
        // 本机实测 upload 8.6GB/s → k≈1.16 → 阈值约 0.35MP（≈770x450）。
        // 基数取 300000 而非 200000，是为了让 640x394（252160）稳稳落在 CPU 侧：
        // 它与临界值只差 8% 时会出现「同一素材不同缩放比反复切路径」的抖动，
        // 这类抖动比那 1~2ms 的收益更伤体验。
        double k = (info.uploadGBs > 1.0) ? (10.0 / info.uploadGBs) : 4.0;
        unsigned long long px = (unsigned long long)(300000.0 * k);
        if (px < 100000ull)  px = 100000ull;
        if (px > 1800000ull) px = 1800000ull;
        info.gpuMinPixels = px;
    }
    // 校准改变了带宽与阈值，必须重跑结论，否则 verdict 与 minPix 不一致
    finalizeVerdict(info);
    return ok;
}

// ---------------------------------------------------------------------------
// 定档结论
// ---------------------------------------------------------------------------
static void finalizeVerdict(BeautyHwInfo& info) {
    // CPU 线程：留 1 核给 Resolve 主线程与解码，上限 12（实测 8→16 还有增益，
    // 但超过物理核太多会被宿主抢调度，反而抖动）
    int t = info.cpuPhysical - 1;
    if (t < 1) t = 1;
    if (t > 12) t = 12;
    if (info.memTotalMB && info.memTotalMB < 8192ull) t = (t > 4) ? 4 : t;
    info.recommendedThreads = t;

    // 走哪条路：环境变量 > beauty.ini > 自动。
    // 环境变量优先是为了临时排查（改文件要重启，改环境变量也要重启，但前者
    // 会留下状态、后者忘了清就会一直生效，所以仍以配置文件为日常入口）。
    const char* pathStr = getenv("BEAUTY_PATH");
    if (!pathStr || !*pathStr) pathStr = beautyCfgStr("path", nullptr);
    int forced = BEAUTY_PATH_AUTO;
    if (pathStr) {
        if (!_stricmp(pathStr, "gpu")) forced = BEAUTY_PATH_GPU;
        else if (!_stricmp(pathStr, "cpu")) forced = BEAUTY_PATH_CPU;
    }

    if (info.gpuTier == BEAUTY_GPU_NONE) info.recommendedPath = BEAUTY_PATH_CPU;
    else if (forced != BEAUTY_PATH_AUTO)  info.recommendedPath = forced;
    else                                  info.recommendedPath = BEAUTY_PATH_GPU;

    // 像素阈值：环境变量 > beauty.ini > 校准推导值。
    // 注意 0 在两个来源里都表示「不覆盖」，因为 0 像素本来就没有意义。
    long long minPixOverride = -1;
    if (const char* e = getenv("BEAUTY_GPU_MINPIX")) {
        minPixOverride = _atoi64(e);
    } else {
        minPixOverride = beautyCfgInt("gpu_minpix", -1);
    }
    unsigned long long px = 0;
    if (minPixOverride > 0) px = (unsigned long long)minPixOverride;
    if (px == 0) px = info.gpuMinPixels ? info.gpuMinPixels : 200000ull;
    info.gpuMinPixels = px;

    const char* tierName = "NONE";
    switch (info.gpuTier) {
        case BEAUTY_GPU_LOW:  tierName = "LOW";  break;
        case BEAUTY_GPU_MID:  tierName = "MID";  break;
        case BEAUTY_GPU_HIGH: tierName = "HIGH"; break;
        default: break;
    }

    // 带宽摘要：兜底生效时把原始值和地板都写出来，否则下次又会对着一个
    // 「看起来很正常」的 5.0 找问题，而真实测量其实只有 4.0。
    char bwStr[96];
    if (!info.calibrated) {
        snprintf(bwStr, sizeof(bwStr), "assumed");
    } else if (info.uploadFloored) {
        snprintf(bwStr, sizeof(bwStr), "%.1f->%.1f GB/s(floor)",
                 info.uploadGBsRaw, info.uploadGBs);
    } else {
        snprintf(bwStr, sizeof(bwStr), "%.1f GB/s", info.uploadGBs);
    }

    snprintf(info.verdict, sizeof(info.verdict),
             "cpu=%dC/%dT %dMHz mem=%lluMB | gpu=%s tier=%s vram=%lluMB fl=0x%X cs5=%d uav16=%d uav32=%d | "
             "bw=%s path=%s threads=%d minPix=%llu",
             info.cpuPhysical, info.cpuLogical, info.cpuMaxMhz, info.memTotalMB,
             info.gpuName[0] ? info.gpuName : "(none)", tierName, info.gpuVramMB,
             info.gpuFeatureLevel, (int)info.gpuCs50Ok, (int)info.gpuTypedUav16F,
             (int)info.gpuTypedUav32F,
             bwStr,
             info.recommendedPath == BEAUTY_PATH_GPU ? "GPU" : "CPU",
             info.recommendedThreads, info.gpuMinPixels);
}

// ---------------------------------------------------------------------------
// 公开接口
// ---------------------------------------------------------------------------
const BeautyHwInfo& beautyHwProbe(bool forceRefresh) {
    if (forceRefresh) {
        g_info = BeautyHwInfo{};
        probeCpu(g_info);
        probeGpu(g_info);
        finalizeVerdict(g_info);
        return g_info;
    }
    std::call_once(g_probeOnce, [] {
        probeCpu(g_info);
        probeGpu(g_info);
        finalizeVerdict(g_info);
    });
    return g_info;
}

int beautyHwDescribe(char* out, int cap) {
    if (!out || cap <= 0) return 0;
    const BeautyHwInfo& i = beautyHwProbe();

    // 兜底生效时必须把「实测多少 / 按多少算」两个数都摆出来。只写 5.0 会让
    // 下一个人以为机器真有 5.0，再去质疑阈值公式，方向就跑偏了。
    char calibStr[192];
    if (!i.calibrated) {
        snprintf(calibStr, sizeof(calibStr), "assumed（未测到，按经验值）");
    } else if (i.uploadFloored) {
        snprintf(calibStr, sizeof(calibStr),
                 "upload=%.1f GB/s（实测 %.1f，触发下限 %.1f） download=%.1f GB/s",
                 i.uploadGBs, i.uploadGBsRaw, kBwFloorGBs, i.downloadGBs);
    } else {
        snprintf(calibStr, sizeof(calibStr),
                 "upload=%.1f GB/s download=%.1f GB/s", i.uploadGBs, i.downloadGBs);
    }

    int n = snprintf(out, (size_t)cap,
        "--- hardware probe ---\n"
        "CPU   : %d physical / %d logical cores, %d MHz, RAM %llu MB\n"
        "GPU   : %s (vendor=0x%04X, vram=%llu MB, FL=0x%X, discrete=%d, adapters=%d)\n"
        "GPUcap: cs5=%d staging=%d typedUAV16F=%d typedUAV32F=%d tier=%d\n"
        "Calib : %s\n"
        "Decide: path=%s threads=%d gpuMinPixels=%llu\n"
        "Config: %s\n",
        i.cpuPhysical, i.cpuLogical, i.cpuMaxMhz, i.memTotalMB,
        i.gpuName[0] ? i.gpuName : "(none)", i.gpuVendorId, i.gpuVramMB,
        i.gpuFeatureLevel, (int)i.gpuIsDiscrete, i.gpuAdapterCount,
        (int)i.gpuCs50Ok, (int)i.gpuStagingOk, (int)i.gpuTypedUav16F, (int)i.gpuTypedUav32F,
        i.gpuTier,
        calibStr,
        i.recommendedPath == BEAUTY_PATH_GPU ? "GPU" : "CPU",
        i.recommendedThreads, i.gpuMinPixels,
        beautyCfgPath());
    if (n < 0) n = 0;
    return (n < cap) ? n : cap - 1;
}

void beautyHwOneLine(char* out, int cap) {
    if (!out || cap <= 0) return;
    const BeautyHwInfo& i = beautyHwProbe();
    snprintf(out, (size_t)cap, "%s", i.verdict);
}

BeautyPath beautyChoosePath(int w, int h, int userSetting) {
    if (userSetting == BEAUTY_PATH_GPU || userSetting == BEAUTY_PATH_CPU)
        return (BeautyPath)userSetting;

    const BeautyHwInfo& i = beautyHwProbe();
    if (i.gpuTier == BEAUTY_GPU_NONE) return BEAUTY_PATH_CPU;

    // 能力许可 + 尺寸门槛：小预览帧留在 CPU，省掉设备交互与同步开销
    unsigned long long px = (unsigned long long)w * (unsigned long long)h;
    if (px < i.gpuMinPixels) return BEAUTY_PATH_CPU;
    return BEAUTY_PATH_GPU;
}

int beautyHwCpuThreads() {
    return beautyHwProbe().recommendedThreads;
}
