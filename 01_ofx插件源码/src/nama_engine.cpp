// nama_engine.cpp —— NamaEngine 实现。
//
// 所有 SDK 函数都是 cdecl；签名来自 BeautyCore.cs 与 obs-nama-beauty.cpp，
// 两者对同一个 DLL 的用法一致，可以交叉印证。

#include "nama_engine.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#ifndef LOAD_WITH_ALTERED_SEARCH_PATH
#define LOAD_WITH_ALTERED_SEARCH_PATH 0x8
#endif

// ---------------------------------------------------------------------------
// NamaSDK 函数指针
// ---------------------------------------------------------------------------
typedef int (*fn_fuSetup)(void*, int, void*, void*, int);
typedef const char* (*fn_fuGetVersion)(void);
typedef int (*fn_fuIsLibraryInit)(void);
typedef int (*fn_fuGetSystemError)(void);
typedef const char* (*fn_fuGetSystemErrorString)(int);
typedef void (*fn_fuSetLogLevel)(int);
typedef int (*fn_fuLoadAIModelFromPackage)(const unsigned char*, int, int);
typedef void (*fn_fuSetFaceProcessorDetectMode)(int);
typedef void (*fn_fuFaceProcessorSetFaceLandmarkQuality)(int);
typedef void (*fn_fuSetDefaultRotationMode)(int);
typedef int (*fn_fuSetMaxFaces)(int);
typedef int (*fn_fuFaceProcessorGetNumResults)(void);
typedef int (*fn_fuCreateItemFromPackage)(const unsigned char*, int);
typedef void (*fn_fuDestroyAllItems)(void);
typedef int (*fn_fuItemSetParamd)(int, const char*, double);
typedef int (*fn_fuItemSetParams)(int, const char*, const char*);
typedef int (*fn_fuRenderItemsEx)(int, void*, int, void*, int, int, int, const int*, int);
typedef void (*fn_fuOnDeviceLost)(void);
typedef void (*fn_fuDestroyLibData)(void);

namespace {
fn_fuSetup g_fuSetup = nullptr;
fn_fuGetVersion g_fuGetVersion = nullptr;
fn_fuIsLibraryInit g_fuIsLibraryInit = nullptr;
fn_fuGetSystemError g_fuGetSystemError = nullptr;
fn_fuGetSystemErrorString g_fuGetSystemErrorString = nullptr;
fn_fuSetLogLevel g_fuSetLogLevel = nullptr;
fn_fuLoadAIModelFromPackage g_fuLoadAIModelFromPackage = nullptr;
fn_fuSetFaceProcessorDetectMode g_fuSetFaceProcessorDetectMode = nullptr;
fn_fuFaceProcessorSetFaceLandmarkQuality g_fuFaceProcessorSetFaceLandmarkQuality = nullptr;
fn_fuSetDefaultRotationMode g_fuSetDefaultRotationMode = nullptr;
fn_fuSetMaxFaces g_fuSetMaxFaces = nullptr;
fn_fuFaceProcessorGetNumResults g_fuFaceProcessorGetNumResults = nullptr;
fn_fuCreateItemFromPackage g_fuCreateItemFromPackage = nullptr;
fn_fuDestroyAllItems g_fuDestroyAllItems = nullptr;
fn_fuItemSetParamd g_fuItemSetParamd = nullptr;
fn_fuItemSetParams g_fuItemSetParams = nullptr;
fn_fuRenderItemsEx g_fuRenderItemsEx = nullptr;
fn_fuOnDeviceLost g_fuOnDeviceLost = nullptr;
fn_fuDestroyLibData g_fuDestroyLibData = nullptr;

const int FUAITYPE_FACEPROCESSOR = 0x100;
// FU_FORMAT_RGBA_BUFFER —— probe_nama.py 里用 0 作为 in/out format 实测通过
const int FU_FORMAT_RGBA_BUFFER = 0;

// 初始化连续失败几次之后就不再重试。每次重试都要付约 2 秒（fuSetup + 模型 + 道具），
// 让它跟着每一帧反复发生会把宿主拖死。
const int kMaxInitFail = 3;

// 逐帧是否还要切换 GL 上下文。
//
// fuRenderItemsEx 走的是 FU_FORMAT_RGBA_BUFFER —— 输入输出都是 CPU 内存指针，
// 原理上渲染阶段不需要 GL 上下文（上下文只在 fuSetup / 模型加载时需要）。
// 但 SDK 内部有可能顺手创建 GL 纹理，所以默认保持「每帧切换」的保守做法。
//
// 设 NAMA_ENGINE_NO_GLLOCK=1 可跳过逐帧切换。这个开关存在的意义是把
// 「每帧两次 wglMakeCurrent 到底值不值」变成可测的：宿主自己的渲染线程上
// 通常已经有它自己的 GL 上下文，我们切走再切回会带来两次隐式 flush。
static bool skipGlLock() {
    static int cached = -1;
    if (cached < 0) {
        const char* v = getenv("NAMA_ENGINE_NO_GLLOCK");
        cached = (v && v[0] == '1' && v[1] == 0) ? 1 : 0;
    }
    return cached == 1;
}

// 线程局部：保存调用线程原本的 GL 上下文，用完必须还原
__declspec(thread) HDC t_savedDC = nullptr;
__declspec(thread) HGLRC t_savedRC = nullptr;
__declspec(thread) bool t_enterOk = false;

// ---------------------------------------------------------------------------
// SEH 兜底
//
// NamaSDK 是逆向对接的闭源 DLL。它一旦在宿主进程里抛访问违例（0xC0000005），
// 整个 DaVinci Resolve 都会跟着死。所以每个 SDK 入口都包一层 __try/__except，
// 代价是这一帧透传，而不是宿主崩掉。
//
// 注意：下面这些函数体内**不能出现需要析构的 C++ 对象**，否则 MSVC 报 C2712。
// 所以它们只收裸指针和标量，参数名也刻意用 a/b/c 这种不依赖外部的形式。
// ---------------------------------------------------------------------------
volatile LONG g_sehHit = 0;   // 最近一次调用是否被 SEH 兜住

// 返回 1 表示正常执行完（*outRet 有效），0 表示被 SEH 兜住
int sehSetup(int* outRet, void* auth, int authLen) {
    __try {
        *outRet = g_fuSetup(nullptr, 0, nullptr, auth, authLen);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int sehCreateItem(int* outItem, void* pkg, int len) {
    __try {
        *outItem = g_fuCreateItemFromPackage((const unsigned char*)pkg, len);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int sehSetParamd(int item, const char* key, double v) {
    __try {
        g_fuItemSetParamd(item, key, v);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int sehSetParams(int item, const char* key, const char* val) {
    __try {
        g_fuItemSetParams(item, key, val);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int sehRender(int* outRet, int fmtOut, void* pOut, int fmtIn, void* pIn,
              int w, int h, int frameId, int* items, int n) {
    __try {
        *outRet = g_fuRenderItemsEx(fmtOut, pOut, fmtIn, pIn, w, h, frameId, items, n);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int sehLoadFaceModel(const unsigned char* pkg, int len) {
    __try {
        g_fuLoadAIModelFromPackage(pkg, len, FUAITYPE_FACEPROCESSOR);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int sehFaceSetup(void) {
    __try {
        g_fuSetFaceProcessorDetectMode(0);
        g_fuFaceProcessorSetFaceLandmarkQuality(1);
        g_fuSetMaxFaces(4);
        g_fuSetDefaultRotationMode(0);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int sehGetNumFaces(int* outN) {
    __try {
        *outN = g_fuFaceProcessorGetNumResults();
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int sehDestroy(void) {
    __try {
        g_fuDestroyAllItems();
        g_fuOnDeviceLost();
        g_fuDestroyLibData();
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// 毫秒级单调时钟（GetTickCount64 只有 15.6ms 精度，量 4.5ms 的渲染不够用）
double nowMs() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
}

bool readFileBinary(const std::string& path, std::vector<unsigned char>& out) {
    FILE* f = nullptr;
    errno_t e = fopen_s(&f, path.c_str(), "rb");
    if (e != 0 || !f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return false; }
    out.resize((size_t)n);
    size_t got = fread(&out[0], 1, (size_t)n, f);
    fclose(f);
    return got == (size_t)n;
}

void setLastErr(char* buf, int cap, const char* fmt, ...) {
    if (!buf || cap <= 0) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf_s(buf, (size_t)cap, _TRUNCATE, fmt, args);
    va_end(args);
}
}  // namespace

// ---------------------------------------------------------------------------
// 构造 / 互斥
// ---------------------------------------------------------------------------
NamaEngine::NamaEngine() {
    CRITICAL_SECTION* cs = new CRITICAL_SECTION;
    InitializeCriticalSectionAndSpinCount(cs, 0x400);
    mutex_ = cs;
}

NamaEngine& NamaEngine::inst() {
    // C++11 魔法静态：**指针**的动态初始化由编译器保证只执行一次且线程安全
    // （MSVC 2015+ 默认 /Zc:threadSafeInit）。
    //
    // 旧写法 `static NamaEngine* s = nullptr; if (!s) s = new NamaEngine();`
    // 里那句 if 赋值没有任何保护：createInstance（宿主线程）与 process（渲染线程）
    // 首次并发时会各 new 一个引擎，进程里于是有两个 GL 上下文 / 两套 item /
    // 两份参数缓存互相踩，且其中一个永久泄漏（旧报告的 A2）。
    //
    // 这里把 new 放进初始化式，既拿到魔法静态的线程安全，又保持「对象刻意不析构」
    // 的行为（避免宿主 FreeLibrary 时静态析构与渲染线程赛跑）。
    static NamaEngine* s = new NamaEngine();
    return *s;
}

// ---------------------------------------------------------------------------
// 资源目录定位：以本 DLL 自身位置为准，这样 bundle 拷到哪都能找到引擎
// ---------------------------------------------------------------------------
bool NamaEngine::ensurePath(std::string& err) {
    if (pathDone_) return true;

    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&NamaVersion, &self);
    if (self) {
        char path[MAX_PATH] = {0};
        GetModuleFileNameA(self, path, MAX_PATH);
        std::string s(path);
        size_t p = s.find_last_of("\\/");
        dir_ = (p == std::string::npos) ? "." : s.substr(0, p);
    } else {
        dir_ = ".";
    }

    // 允许用环境变量覆盖，方便在没装到 OFX 目录时做测试
    const char* override = getenv("NAMA_ENGINE_DIR");
    if (override && *override) dir_ = override;

    pathDone_ = true;
    if (dir_.empty()) {
        err = "定位引擎目录失败";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 隐藏窗口 + WGL 上下文（复刻 obs-nama-beauty.cpp 的 nama_gl_init）
// ---------------------------------------------------------------------------
bool NamaEngine::ensureGL(std::string& err) {
    if (glDone_) return true;

    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&NamaVersion, &self);

    WNDCLASSA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = (HINSTANCE)(self ? self : GetModuleHandleA(nullptr));
    wc.lpszClassName = "NamaBeautyHiddenGL";
    RegisterClassA(&wc);  // 重复注册会失败，忽略即可

    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "", 0, 0, 0, 1, 1,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { err = "CreateWindowExA 失败"; return false; }
    HDC dc = GetDC(hwnd);
    if (!dc) { err = "GetDC 失败"; return false; }

    PIXELFORMATDESCRIPTOR pfd;
    ZeroMemory(&pfd, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER | PFD_DRAW_TO_WINDOW;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;

    int fmt = ChoosePixelFormat(dc, &pfd);
    if (!fmt) { err = "ChoosePixelFormat 失败"; return false; }
    if (!SetPixelFormat(dc, fmt, &pfd)) { err = "SetPixelFormat 失败"; return false; }
    HGLRC rc = wglCreateContext(dc);
    if (!rc) { err = "wglCreateContext 失败"; return false; }

    hGL_ = hwnd;
    hDC_ = dc;
    hRC_ = rc;
    glDone_ = true;
    // 记下是谁创建的上下文。OFX 的 render 可能来自别的线程（我们声明了
    // FullySafe），这个值只用于诊断：WGL 允许上下文在非并发的情况下换线程。
    ownerThread_ = (unsigned long)GetCurrentThreadId();
    return true;
}

bool NamaEngine::enter() {
    t_savedDC = wglGetCurrentDC();
    t_savedRC = wglGetCurrentContext();
    // 切换失败时原本的上下文仍然 current，此时绝不能再去 leave()，
    // 否则等于用一对旧值做了一次无意义的 wglMakeCurrent。
    t_enterOk = (wglMakeCurrent((HDC)hDC_, (HGLRC)hRC_) != FALSE);
    return t_enterOk;
}

void NamaEngine::leave() {
    if (!t_enterOk) return;
    wglMakeCurrent(t_savedDC, t_savedRC);
    t_enterOk = false;
}

// ---------------------------------------------------------------------------
// 加载 SDK / 授权 / 人模型 / 美颜 bundle
// ---------------------------------------------------------------------------
bool NamaEngine::ensureSDK(std::string& err) {
    char tmp[MAX_PATH + 128] = {0};

    std::string dll = dir_ + "\\CNamaSDK.dll";
    // LOAD_WITH_ALTERED_SEARCH_PATH：让 CNamaSDK.dll 能在自己旁边找到 fuai.dll
    HMODULE h = LoadLibraryExA(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) {
        sprintf_s(tmp, "LoadLibrary 失败: %s (err=%lu)", dll.c_str(), GetLastError());
        err = tmp;
        return false;
    }
    hModule_ = h;

#define LOAD(f)                                            \
    g_##f = (fn_##f)GetProcAddress(h, #f);                 \
    if (!g_##f) { err = "缺少导出函数 " #f; return false; }

    LOAD(fuSetup)
    LOAD(fuGetVersion)
    LOAD(fuIsLibraryInit)
    LOAD(fuGetSystemError)
    LOAD(fuGetSystemErrorString)
    LOAD(fuSetLogLevel)
    LOAD(fuLoadAIModelFromPackage)
    LOAD(fuSetFaceProcessorDetectMode)
    LOAD(fuFaceProcessorSetFaceLandmarkQuality)
    LOAD(fuSetDefaultRotationMode)
    LOAD(fuSetMaxFaces)
    LOAD(fuFaceProcessorGetNumResults)
    LOAD(fuCreateItemFromPackage)
    LOAD(fuDestroyAllItems)
    LOAD(fuItemSetParamd)
    LOAD(fuItemSetParams)
    LOAD(fuRenderItemsEx)
    LOAD(fuOnDeviceLost)
    LOAD(fuDestroyLibData)
#undef LOAD

    std::vector<unsigned char> auth, faceModel;
    if (!readFileBinary(dir_ + "\\auth_package.bin", auth) ||
        !readFileBinary(dir_ + "\\model\\ai_face_processor_pc.bundle", faceModel) ||
        !readFileBinary(dir_ + "\\graphics\\face_beautification.bundle", beautyPkg_)) {
        err = "资源文件缺失（需要 auth_package.bin / model/ai_face_processor_pc.bundle / "
              "graphics/face_beautification.bundle）";
        return false;
    }

    if (!enter()) {
        err = "wglMakeCurrent 失败（无法切到本引擎的 GL 上下文）";
        return false;
    }

    g_fuSetLogLevel(2);

    int r = 0;
    if (!sehSetup(&r, auth.empty() ? nullptr : &auth[0], (int)auth.size())) {
        InterlockedExchange(&g_sehHit, 1);
        leave();
        err = "fuSetup 抛访问违例（SDK 与本机不兼容或授权异常）";
        return false;
    }
    if (r == 0) {
        leave();
        sprintf_s(tmp, "fuSetup 失败 (err=%d)", g_fuGetSystemError());
        err = tmp;
        return false;
    }
    ver_ = g_fuGetVersion() ? g_fuGetVersion() : "";

    if (!sehLoadFaceModel(faceModel.empty() ? nullptr : &faceModel[0],
                          (int)faceModel.size()) ||
        !sehFaceSetup()) {
        InterlockedExchange(&g_sehHit, 1);
        leave();
        err = "人脸模型加载抛访问违例";
        return false;
    }

    // 创建第一个道具（同时放进 slot 池，供第一个 OFX 实例使用）。
    // 模块级默认参数写在这里，与 obs-nama-beauty.cpp 的 nama_init 完全一致。
    int first = 0;
    if (!createBeautyItemGL(&first)) {
        InterlockedExchange(&g_sehHit, 1);
        leave();
        sprintf_s(tmp, "fuCreateItemFromPackage 失败 item=%d err=%d", first,
                  g_fuGetSystemError());
        err = tmp;
        return false;
    }
    slots_.clear();
    Slot s0;
    s0.item = first;
    slots_.push_back(s0);
    item_ = first;
    leave();

    return true;
}

bool NamaEngine::createBeautyItemGL(int* outItem) {
    *outItem = 0;
    if (!sehCreateItem(outItem, beautyPkg_.empty() ? nullptr : &beautyPkg_[0],
                       (int)beautyPkg_.size())) {
        InterlockedExchange(&g_sehHit, 1);
        return false;
    }
    if (*outItem <= 0) return false;

    // 模块级默认，与 obs-nama-beauty.cpp 的 nama_init 完全一致
    sehSetParamd(*outItem, "is_opengl_es", 0);  // 桌面 GL，否则 SDK 渲染出全黑
    sehSetParamd(*outItem, "use_landmark", 1);
    sehSetParamd(*outItem, "skin_detect", 0);
    sehSetParamd(*outItem, "face_shape", 4);
    sehSetParamd(*outItem, "face_shape_level", 0.0);
    sehSetParams(*outItem, "filter_name", "ziran2");
    sehSetParamd(*outItem, "filter_level", 0.4);  // 与原插件默认一致
    sehSetParamd(*outItem, "heavy_blur", 0);
    return true;
}

bool NamaEngine::ensureReady(std::string& err) {
    return ensureReadyLocked(err, nullptr);
}

bool NamaEngine::preheat(std::string& err, bool& didInit) {
    didInit = false;
    return ensureReadyLocked(err, &didInit);
}

bool NamaEngine::ensureReadyLocked(std::string& err, bool* didInit) {
    if (didInit) *didInit = false;

    EnterCriticalSection((CRITICAL_SECTION*)mutex_);
    bool ok = false;
    do {
        if (ready_) { ok = true; break; }
        if (poisoned_) {
            err = "SDK 已因访问违例停用，本进程内不再调用";
            break;
        }
        if (failCount_ >= kMaxInitFail) {
            err = failReason_.empty() ? "引擎初始化已放弃" : failReason_;
            break;
        }
        // 到这里才是真的「需要且允许」做一次初始化
        if (didInit) *didInit = true;
        err.clear();
        if (!ensurePath(err)) break;
        if (!ensureGL(err)) break;
        if (!ensureSDK(err)) break;
        ok = true;
    } while (0);

    if (ok) {
        ready_ = true;
        failCount_ = 0;
    } else if (didInit && *didInit) {
        // 只有真的尝试过初始化才累计失败次数（poisoned / 已放弃 不算）
        failCount_++;
        failReason_ = err;
    }
    LeaveCriticalSection((CRITICAL_SECTION*)mutex_);
    return ready_;
}

// ---------------------------------------------------------------------------
// 每实例槽位
// ---------------------------------------------------------------------------
int NamaEngine::acquireSlot(std::string& err) {
    // 先确保引擎就绪（内部加锁并复检；已就绪时只是两次入/出锁，代价极低）
    if (!ensureReadyLocked(err, nullptr)) return -1;

    EnterCriticalSection((CRITICAL_SECTION*)mutex_);
    int idx = -1;
    for (size_t i = 0; i < slots_.size(); i++) {
        if (!slots_[i].inUse) { idx = (int)i; break; }
    }
    if (idx < 0) {
        // 空闲池空了：新建一个道具。GL 上下文只在创建时需要。
        if (!enter()) {
            LeaveCriticalSection((CRITICAL_SECTION*)mutex_);
            err = "wglMakeCurrent 失败（无法为实例创建道具）";
            return -1;
        }
        int it = 0;
        bool cok = createBeautyItemGL(&it);
        leave();
        if (!cok) {
            LeaveCriticalSection((CRITICAL_SECTION*)mutex_);
            err = "创建美颜道具失败";
            return -1;
        }
        Slot s;
        s.item = it;
        slots_.push_back(s);
        idx = (int)slots_.size() - 1;
    }
    slots_[idx].inUse = true;
    // 归还过的槽位可能残留上一个实例的参数指纹，标为「未应用」强制重下发
    slots_[idx].hasApplied = false;
    LeaveCriticalSection((CRITICAL_SECTION*)mutex_);
    return idx;
}

void NamaEngine::releaseSlot(int slot) {
    EnterCriticalSection((CRITICAL_SECTION*)mutex_);
    if (slot >= 0 && slot < (int)slots_.size()) {
        slots_[slot].inUse = false;
        slots_[slot].hasApplied = false;
    }
    LeaveCriticalSection((CRITICAL_SECTION*)mutex_);
}

// ---------------------------------------------------------------------------
// 逐帧处理
// ---------------------------------------------------------------------------
bool NamaEngine::process(const unsigned char* src, unsigned char* dst,
                         int w, int h, const NamaSettings& s) {
    // 自测旧接口：自动占用一个默认槽位。
    if (defaultSlot_ < 0) {
        std::string e;
        int sl = acquireSlot(e);
        if (sl < 0) {
            failReason_ = e;
            return false;
        }
        defaultSlot_ = sl;
    }
    return process(defaultSlot_, src, dst, w, h, s, true);
}

bool NamaEngine::process(int slot, const unsigned char* src, unsigned char* dst,
                         int w, int h, const NamaSettings& s, bool advanceFrame) {
    std::string err;
    if (!ensureReady(err)) {
        failReason_ = err;
        return false;
    }
    if (w <= 0 || h <= 0 || !src || !dst) {
        failReason_ = "尺寸或指针不合法";
        return false;
    }

    EnterCriticalSection((CRITICAL_SECTION*)mutex_);

    if (poisoned_) {
        LeaveCriticalSection((CRITICAL_SECTION*)mutex_);
        failReason_ = "SDK 已因访问违例停用";
        return false;
    }
    if (slot < 0 || slot >= (int)slots_.size() || slots_[slot].item <= 0) {
        LeaveCriticalSection((CRITICAL_SECTION*)mutex_);
        failReason_ = "无效的引擎槽位";
        return false;
    }
    Slot& sl = slots_[slot];
    const int item = sl.item;

    // 帧级临时缓冲：SDK 要求输出缓冲独立，不能原地覆盖。
    // memcpy 还有一个作用：SDK 若没有写满全缓冲（例如无人脸时），剩下的像素
    // 保持为输入值而不是垃圾 —— 这是刻意的，不是多余拷贝，别删。
    size_t need = (size_t)w * (size_t)h * 4;
    if (work_.size() < need) work_.resize(need);
    unsigned char* out = (unsigned char*)&work_[0];
    memcpy(out, src, need);

    // 面板值 → SDK 值：value = panel / 100 * sdk_max
    // sdk_max 表来自 obs-nama-beauty.cpp 的 g_params[]
    static const char* keys[P_COUNT] = {
        "blur_level", "clarity",  "color_level", "red_level",
        "sharpen",    "face_threed", "eye_bright", "remove_pouch_strength"
    };
    static const double sdkMax[P_COUNT] = {6.0, 1.0, 2.0, 1.0, 1.0, 1.0, 1.0, 1.0};

    // 逐帧的 GL 上下文切换可以被 NAMA_ENGINE_NO_GLLOCK=1 关掉。
    // 关掉时 leave() 会自动变成空操作（t_enterOk 保持 false），无需额外判断。
    if (!skipGlLock() && !enter()) {
        LeaveCriticalSection((CRITICAL_SECTION*)mutex_);
        failReason_ = "wglMakeCurrent 失败";
        return false;
    }

    double t0 = nowMs();

    // 参数没变就不重复下发 —— 每帧 10 次 setter 都是 SDK 内部的字符串查表。
    // 缓存是**每槽位**的，两个实例交替渲染也不会互相冲掉（旧报告的 B5）。
    bool changed = !sl.hasApplied || sl.applied.blur_type != s.blur_type ||
                   sl.applied.skin_detect != s.skin_detect;
    if (!changed) {
        for (int i = 0; i < P_COUNT; i++)
            if (sl.applied.panel[i] != s.panel[i]) { changed = true; break; }
    }

    bool anyEffect = false;
    if (changed) {
        sehSetParamd(item, "blur_type", (double)s.blur_type);
        sehSetParamd(item, "skin_detect", (double)s.skin_detect);
        for (int i = 0; i < P_COUNT; i++) {
            double v = s.panel[i];
            if (v < 0) v = 0;
            if (v > 100) v = 100;
            if (v > 0) anyEffect = true;
            sehSetParamd(item, keys[i], v / 100.0 * sdkMax[i]);
        }
        // ziran2 是全局滤镜，开着会把整帧都改掉。8 项效果全为 0 时用户期待的是
        // 原图，所以这里把滤镜一并关掉，保证「全 0 == 透传」。
        sehSetParamd(item, "filter_level", anyEffect ? 0.4 : 0.0);
        sl.applied = s;
        sl.hasApplied = true;
    }

    // 非顺序渲染（随机访问 / 单帧生成）时不推进帧序，避免人脸跟踪的时序状态漂移。
    const int useFrame = advanceFrame ? frameId_ : 0;

    int items[1] = {item};
    int ret = 0;
    if (!sehRender(&ret, FU_FORMAT_RGBA_BUFFER, out,
                   FU_FORMAT_RGBA_BUFFER, (void*)src,
                   w, h, useFrame, items, 1)) {
        // SDK 抛访问违例：把引擎标成毒化状态，本进程内不再调用 SDK。
        // 不这么做的话，后续每一帧都会再崩一次，Resolve 会被反复骚扰。
        poisoned_ = true;
        leave();
        LeaveCriticalSection((CRITICAL_SECTION*)mutex_);
        failReason_ = "fuRenderItemsEx 抛访问违例，已停用 SDK";
        return false;
    }
    if (advanceFrame) frameId_++;
    lastSdkMs_ = nowMs() - t0;

    int faces = 0;
    if (!sehGetNumFaces(&faces)) faces = 0;
    faces_ = faces < 0 ? 0 : faces;
    leave();

    LeaveCriticalSection((CRITICAL_SECTION*)mutex_);

    if (ret == 0) {
        char t[128];
        sprintf_s(t, "render 返回 0 (err=%d)", g_fuGetSystemError ? g_fuGetSystemError() : -1);
        failReason_ = t;
        return false;
    }
    if (dst != out) memcpy(dst, out, need);
    return true;
}

void NamaEngine::release() {
    if (!ready_) return;
    EnterCriticalSection((CRITICAL_SECTION*)mutex_);
    if (enter()) {
        sehDestroy();
        leave();
    }
    if (hRC_) { wglDeleteContext((HGLRC)hRC_); hRC_ = nullptr; }
    if (hDC_ && hGL_) { ReleaseDC((HWND)hGL_, (HDC)hDC_); hDC_ = nullptr; }
    if (hGL_) { DestroyWindow((HWND)hGL_); hGL_ = nullptr; }
    if (hModule_) { FreeLibrary((HMODULE)hModule_); hModule_ = nullptr; }
    ready_ = false;
    glDone_ = false;
    pathDone_ = false;
    // fuDestroyAllItems 已经把池里所有道具销毁，槽位全部作废
    slots_.clear();
    defaultSlot_ = -1;
    beautyPkg_.clear();
    LeaveCriticalSection((CRITICAL_SECTION*)mutex_);
}

// ---------------------------------------------------------------------------
// 自测导出
// ---------------------------------------------------------------------------
extern "C" {

unsigned long __stdcall NamaVersion(void) {
    return (unsigned long)((NAMA_BUILD_MAJOR << 16) | NAMA_BUILD_MINOR);
}

int __stdcall NamaSelfTest(const char* engineDir, const char* rawIn, const char* rawOut,
                           int width, int height, double blur, double whiten,
                           char* msgBuf, int msgCap) {
    if (msgBuf && msgCap > 0) msgBuf[0] = 0;
    if (!rawIn || !rawOut || width <= 0 || height <= 0) {
        setLastErr(msgBuf, msgCap, "参数不合法");
        return 2;
    }
    if (engineDir && *engineDir) {
        _putenv_s("NAMA_ENGINE_DIR", engineDir);
    }

    FILE* f = nullptr;
    if (fopen_s(&f, rawIn, "rb") != 0 || !f) {
        setLastErr(msgBuf, msgCap, "打不开输入文件：%s", rawIn);
        return 3;
    }
    size_t need = (size_t)width * (size_t)height * 4;
    std::vector<unsigned char> buf(need);
    size_t got = fread(&buf[0], 1, need, f);
    fclose(f);
    if (got != need) {
        setLastErr(msgBuf, msgCap, "输入字节不足：需要 %zu，实际 %zu", need, got);
        return 4;
    }

    NamaSettings s;
    for (int i = 0; i < P_COUNT; i++) s.panel[i] = 0;  // 自测只验证被指定的两项
    s.panel[P_BLUR] = blur;
    s.panel[P_WHITEN] = whiten;
    s.blur_type = 1;

    double t0 = nowMs();
    std::string err;
    NamaEngine& e = NamaEngine::inst();
    if (!e.ensureReady(err)) {
        setLastErr(msgBuf, msgCap, "初始化失败：%s", err.c_str());
        return 5;
    }
    double initMs = nowMs() - t0;

    std::vector<unsigned char> out(need);
    t0 = nowMs();
    bool ok = e.process(&buf[0], &out[0], width, height, s);
    double procMs = nowMs() - t0;
    if (!ok) {
        setLastErr(msgBuf, msgCap, "render 失败：%s", e.lastErr());
        return 6;
    }

    if (fopen_s(&f, rawOut, "wb") != 0 || !f) {
        setLastErr(msgBuf, msgCap, "写不了输出文件：%s", rawOut);
        return 7;
    }
    fwrite(&out[0], 1, need, f);
    fclose(f);

    setLastErr(msgBuf, msgCap,
               "OK ver=%s item=%d faces=%d init=%.0fms process=%.1fms sdk=%.1fms",
               e.version() ? e.version() : "?", e.itemId(), e.lastFaces(),
               initMs, procMs, e.lastSdkMs());
    return 0;
}

}  // extern "C"
