// nama_region.cpp —— fuai.dll 驱动实现。
//
// 函数签名来自 05_解密模型数据\REVERSE_REPORT.md（Ghidra + ctypes 双重验证），
// 调用顺序照 _tools\extract_face_assets.py 的 FuAI 封装，两者交叉印证。

#include "nama_region.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

namespace {

// ---------------------------------------------------------------------------
// fuai 导出（__cdecl）
// ---------------------------------------------------------------------------
typedef void* (*fn_newProc)(const char*, int);
typedef void* (*fn_newResult)(void);
typedef void* (*fn_newView)(void);
typedef void  (*fn_initView)(void*, int, int, int, int,
                             void*, int, void*, int, void*, int);
typedef void  (*fn_setViewData)(void*, int, void*);
typedef void  (*fn_process)(void*, void*, void*);
typedef int   (*fn_getNum)(void*);
typedef float* (*fn_getSkinSeg)(void*, void*, int*, int*, int*);
typedef void  (*fn_setInt)(void*, int);

fn_newProc     g_newProc = nullptr;
fn_newResult   g_newResult = nullptr;
fn_newView     g_newView = nullptr;
fn_initView    g_initView = nullptr;
fn_setViewData g_setViewData = nullptr;
fn_process     g_process = nullptr;
fn_getNum      g_getNum = nullptr;
fn_getSkinSeg  g_getSkinSeg = nullptr;
fn_setInt      g_setMaxFaces = nullptr;
fn_setInt      g_setDetectMode = nullptr;
fn_setInt      g_setUseLandmark = nullptr;
fn_setInt      g_setUseHead = nullptr;
fn_setInt      g_setUseHair = nullptr;
fn_setInt      g_setUseSkin = nullptr;

// 图像格式枚举（FUAI_ImageViewInit 的第 2 个参数）
// 0=UNKOWN 1=RGB 2=BGR 3=RGBA 4=BGRA 5=GRAY ...
const int kFmtRGBA = 3;

double nowMsLocal() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
}

std::string utf8ToAcpLocal(const std::string& s) {
    if (s.empty()) return s;
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return s;
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    int m = WideCharToMultiByte(CP_ACP, 0, w.c_str(), n, nullptr, 0, nullptr, nullptr);
    if (m <= 0) return s;
    std::string a((size_t)m, '\0');
    WideCharToMultiByte(CP_ACP, 0, w.c_str(), n, &a[0], m, nullptr, nullptr);
    return a;
}

bool readAll(const std::string& path, std::vector<unsigned char>& out) {
    FILE* f = nullptr;
    if (fopen_s(&f, utf8ToAcpLocal(path).c_str(), "rb") != 0 || !f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return false; }
    out.resize((size_t)n);
    size_t got = fread(&out[0], 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { out.clear(); return false; }
    return true;
}

// ---- 逐调用追踪 ----------------------------------------------------------
// NAMA_REGION_TRACE=1 时把每一步写进 stderr 并立刻 fflush。
// 用来定位「进程直接消失」这类问题 —— SEH 只能兜住本线程的访问违例，
// fuai 内部起的 worker 线程崩了是兜不住的，只能靠最后一条日志判断死在哪。
bool traceOn() {
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("NAMA_REGION_TRACE");
        on = (e && e[0] == '1') ? 1 : 0;
    }
    return on == 1;
}

void trace(const char* fmt, ...) {
    if (!traceOn()) return;
    va_list ap;
    va_start(ap, fmt);
    fputs("[region] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    fflush(stderr);
}

// ---- 致命异常探针 --------------------------------------------------------
// NAMA_REGION_TRACE=1 时装一个 VEH。用途：fuai 有些崩溃 SEH 兜不住（另一个
// 线程里炸、/GS cookie 被踩、栈溢出），进程会静默消失、连 WER 事件都没有。
// VEH 在所有 SEH 之前被调用，能把异常码和出错地址打出来，至少知道死因。
LONG CALLBACK regionVeh(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    void* addr = ep->ExceptionRecord->ExceptionAddress;
    char mod[MAX_PATH] = {0};
    HMODULE hm = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &hm) && hm)
        GetModuleFileNameA(hm, mod, MAX_PATH);
    fprintf(stderr, "[region][VEH] 异常 code=0x%08X addr=%p module=%s\n",
            (unsigned)code, addr, mod[0] ? mod : "?");
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

void installVehOnce() {
    static bool done = false;
    if (done || !traceOn()) return;
    done = true;
    AddVectoredExceptionHandler(1, regionVeh);
    trace("VEH 已安装");
}

// ---- SEH 薄封装 -----------------------------------------------------------
// __try/__except 不能和需要栈展开的 C++ 对象共存，所以每个都做成裸函数。

int sehProcess(void* proc, void* view, void* result) {
    __try {
        g_process(proc, view, result);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int sehGetNum(void* result, int* out) {
    __try {
        *out = g_getNum(result);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// 返回 1 成功；0 抛异常；2 返回空指针或尺寸非法
int sehGetSkin(void* proc, void* view, float** ptr, int* w, int* h) {
    __try {
        // -------------------------------------------------------------------
        // 这个 extra 缓冲是踩过坑的地方，别改小（2026-10-24）
        //
        // 05_解密模型数据\_tools\extract_face_assets.py 里是
        //     extra = (ctypes.c_int * 4)()
        // 那是**堆**对象。fuai 往这个指针写的东西明显不止 4 个 int —— 堆上越界
        // 只是悄悄踩坏相邻内存，看不出来；放到栈上就会踩坏 /GS 栈 cookie，
        // 函数返回时 __security_check_cookie 直接 __fastfail(0xC0000409)，
        // 这个终止**绕过 VEH 也绕过 SEH**，表现为「进程静默消失、连 WER 事件
        // 都没有」。这就是为什么同样一段调用序列在 Python 里好好的，
        // 搬到 C++ 里第一次 render 就整进程没了。
        //
        // 现在给 1 KB 余量。若以后又出现 0xC0000409，先把这里再放大。
        // -------------------------------------------------------------------
        int extra[256] = {0};
        // 追踪模式下用哨兵量出 fuai 实际写了多少个 int（只诊断用，不影响生产）
        const int kProbe = 256;
        if (traceOn())
            for (int i = 0; i < kProbe; i++) extra[i] = 0x7F7F7F7F;
        *ptr = g_getSkinSeg(proc, view, w, h, extra);
        if (traceOn()) {
            int last = -1;
            for (int i = 0; i < kProbe; i++)
                if (extra[i] != 0x7F7F7F7F) last = i;
            trace("extra 被写到下标 %d（%d 个 int，缓冲 %d）", last, last + 1, kProbe);
        }
        if (!*ptr || *w <= 0 || *h <= 0) return 2;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int sehInitView(void* view, int w, int h, void* buf, int stride) {
    __try {
        // 契约照 extract_face_assets.FuAI.process：先 InitView 只给 stride（缓冲指针留空），
        // 再用 SetViewData 把第 0 平面指过去。stride 传 0 会让 SDK 按错的行距读。
        g_initView(view, kFmtRGBA, w, h, 0, nullptr, stride, nullptr, 0, nullptr, 0);
        g_setViewData(view, 0, buf);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

}  // namespace

NamaRegion& NamaRegion::inst() {
    static NamaRegion s;
    return s;
}

std::string NamaRegion::findBundle() const {
    // 1) 显式指定
    const char* env = getenv("NAMA_REGION_BUNDLE");
    if (env && *env) return std::string(env);
    // 2) 引擎目录旁（推荐：把明文 bundle 拷到 fuai.dll 同目录）
    const char* names[] = {
        "\\ai_face_processor_pc_plain.bundle",
        "\\model\\ai_face_processor_pc_plain.bundle",
        "\\ai_face_processor_pc.bundle",
        "\\model\\ai_face_processor_pc.bundle",
    };
    for (int i = 0; i < 4; i++) {
        std::string p = dir_ + names[i];
        DWORD a = GetFileAttributesA(utf8ToAcpLocal(p).c_str());
        if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) return p;
    }
    return std::string();
}

bool NamaRegion::loadDll(std::string& err) {
    // ---------------------------------------------------------------------
    // 直接加载 fuai.dll，和 CNamaSDK 共用同一份模块实例。
    //
    // 曾经担心过冲突：CNamaSDK.dll 的导入表里就有 fuai.dll（PE 导入表核对过），
    // 我们 LoadLibrary("fuai.dll") 拿回来的是同一个实例，两边各自建一个
    // FaceProcessor，共用 fuai 的全局状态（auth / GLLoader / tflite 池）。
    //
    // 实测结论：**不冲突**。曾经试过复制一份改名 fuai_region.dll 做模块隔离，
    // 崩得一模一样 —— 说明当时那个「第一次 render 就整进程静默消失」跟共享
    // 状态无关，真凶是 sehGetSkin 里 `int extra[4]` 的栈越界（见那个函数的
    // 注释）。修掉之后共享实例完全正常，所以这 29 MB 的副本不需要了。
    // ---------------------------------------------------------------------
    std::string p = dir_ + "\\fuai.dll";
    std::wstring wp;
    int n = MultiByteToWideChar(CP_UTF8, 0, p.c_str(), (int)p.size(), nullptr, 0);
    if (n > 0) {
        wp.resize((size_t)n);
        MultiByteToWideChar(CP_UTF8, 0, p.c_str(), (int)p.size(), &wp[0], n);
    }
    HMODULE h = wp.empty()
                    ? nullptr
                    : LoadLibraryExW(wp.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) {
        char b[256];
        sprintf_s(b, "LoadLibrary fuai.dll 失败 (err=%lu)", GetLastError());
        err = b;
        return false;
    }
    hDll_ = h;
    trace("fuai.dll 已加载 (h=%p)", h);
    g_newProc     = (fn_newProc)GetProcAddress(h, "FUAI_NewFaceProcessorFromBundle");
    g_newResult   = (fn_newResult)GetProcAddress(h, "FUAI_NewFaceProcessorResult");
    g_newView     = (fn_newView)GetProcAddress(h, "FUAI_NewImageView");
    g_initView    = (fn_initView)GetProcAddress(h, "FUAI_ImageViewInit");
    g_setViewData = (fn_setViewData)GetProcAddress(h, "FUAI_ImageViewSetData");
    g_process     = (fn_process)GetProcAddress(h, "FUAI_FaceProcessorProcess2Result");
    g_getNum      = (fn_getNum)GetProcAddress(h, "FUAI_FaceProcessorGetNumFromResult");
    g_getSkinSeg  = (fn_getSkinSeg)GetProcAddress(h, "FUAI_FaceProcessorGetSkinSegResult");
    g_setMaxFaces = (fn_setInt)GetProcAddress(h, "FUAI_FaceProcessorSetMaxFaces");
    g_setDetectMode = (fn_setInt)GetProcAddress(h, "FUAI_FaceProcessorSetDetectMode");
    g_setUseLandmark = (fn_setInt)GetProcAddress(h, "FUAI_FaceProcessorSetUseFaceLandmark");
    g_setUseHead  = (fn_setInt)GetProcAddress(h, "FUAI_FaceProcessorSetUseHeadSegmenter");
    g_setUseHair  = (fn_setInt)GetProcAddress(h, "FUAI_FaceProcessorSetUseHairSegmenter");
    g_setUseSkin  = (fn_setInt)GetProcAddress(h, "FUAI_FaceProcessorSetUseSkinSeg");
    if (!g_newProc || !g_newResult || !g_newView || !g_initView || !g_setViewData ||
        !g_process || !g_getNum || !g_getSkinSeg) {
        err = "fuai.dll 缺少必需导出";
        return false;
    }
    return true;
}

bool NamaRegion::loadBundle(std::string& err) {
    std::string p = findBundle();
    if (p.empty()) {
        err = "找不到明文 bundle（放到 fuai.dll 同目录并命名为 "
              "ai_face_processor_pc_plain.bundle，或用 NAMA_REGION_BUNDLE 指定）";
        return false;
    }
    if (!readAll(p, bundle_)) { err = "读取 bundle 失败: " + p; return false; }
    // 加密件以 CNamaSDK 的头开头，明文一定是 Zip
    if (bundle_.size() < 4 || bundle_[0] != 'P' || bundle_[1] != 'K') {
        err = "bundle 不是明文 Zip（随包分发的是加密件，需先导出明文）: " + p;
        return false;
    }
    return true;
}

bool NamaRegion::ensureReady(const std::string& engineDir, std::string& err) {
    if (ready_) return true;
    if (failed_) { err = err_; return false; }
    dir_ = engineDir;
    trace("ensureReady dir=%s", dir_.c_str());

    if (!loadDll(err)) { trace("loadDll 失败: %s", err.c_str()); failed_ = true; err_ = err; return false; }
    if (!loadBundle(err)) { trace("loadBundle 失败: %s", err.c_str()); failed_ = true; err_ = err; return false; }
    trace("bundle 已加载 %d 字节", (int)bundle_.size());

    trace("-> FUAI_NewFaceProcessorFromBundle");
    proc_ = g_newProc((const char*)&bundle_[0], (int)bundle_.size());
    trace("<- FUAI_NewFaceProcessorFromBundle proc=%p", proc_);
    if (!proc_) {
        failed_ = true;
        err_ = "FUAI_NewFaceProcessorFromBundle 返回 NULL";
        err = err_;
        return false;
    }
    trace("-> FUAI_NewFaceProcessorResult / NewImageView");
    result_ = g_newResult();
    view_ = g_newView();
    trace("<- result=%p view=%p", result_, view_);
    if (!result_ || !view_) {
        failed_ = true;
        err_ = "FUAI result/view 分配失败";
        err = err_;
        return false;
    }

    // 只开需要的分支。关键点关不掉（关掉直接访问违例），头部/头发掩码用不上，
    // 关掉可省约 5 ms/次（实测 +head 17.2 ms vs 不带 15.6 ms，+hair 再多 5 ms）。
    trace("-> SetMaxFaces(%d)", 2);
    if (g_setMaxFaces) g_setMaxFaces(proc_, 2);
    trace("-> SetDetectMode(0)");
    if (g_setDetectMode) g_setDetectMode(proc_, 0);   // 0 = 图片模式，逐帧全检，最稳
    trace("-> SetUseFaceLandmark(1)");
    if (g_setUseLandmark) g_setUseLandmark(proc_, 1);
    trace("-> SetUseHeadSegmenter(0)");
    if (g_setUseHead) g_setUseHead(proc_, 0);
    trace("-> SetUseHairSegmenter(0)");
    if (g_setUseHair) g_setUseHair(proc_, 0);
    trace("-> SetUseSkinSeg(1)");
    if (g_setUseSkin) g_setUseSkin(proc_, 1);

    ready_ = true;
    trace("ensureReady 完成");

    // -----------------------------------------------------------------------
    // 诊断自检：NAMA_REGION_SELFTEST=1 时立刻用一张合成渐变图跑一次推理。
    //
    // 用来切分故障区间：这时 CNamaSDK 已经 fuSetup + 建好美颜道具，但**还没跑过
    // 任何一帧 fuRenderItemsEx**。
    //   * 自检就崩  → 冲突发生在「加载/建道具」阶段，与渲染无关
    //   * 自检正常、render 后崩 → 冲突发生在 SDK 真正跑过 AI 之后
    // -----------------------------------------------------------------------
    const char* st = getenv("NAMA_REGION_SELFTEST");
    if (st && st[0] == '1') {
        // 两个尺寸都跑一遍：用来确认 fuai 往 extra 写多少 int 与分辨率无关
        const int sizes[2][2] = {{640, 394}, {1920, 1080}};
        for (int k = 0; k < 2; k++) {
            const int W = sizes[k][0], H = sizes[k][1];
            std::vector<unsigned char> buf((size_t)W * H * 4, 0);
            for (int y = 0; y < H; y++) {
                for (int x = 0; x < W; x++) {
                    unsigned char* p = &buf[((size_t)y * W + (size_t)x) * 4];
                    p[0] = (unsigned char)(x * 255 / W);
                    p[1] = (unsigned char)(y * 255 / H);
                    p[2] = 160;
                    p[3] = 255;
                }
            }
            trace("SELFTEST %dx%d 开始", W, H);
            const unsigned char* m = update(&buf[0], W, H, 1);
            trace("SELFTEST %dx%d 结束 mask=%p %dx%d faces=%d cov=%.3f",
                  W, H, m, maskW(), maskH(), lastFaces(), coverage());
        }
    }
    return true;
}

bool NamaRegion::inferOnce(const unsigned char* rgba, int w, int h) {
    double t0 = nowMsLocal();
    installVehOnce();

    trace("-> ImageViewInit(%dx%d stride=%d buf=%p)", w, h, w * 4, rgba);
    if (!sehInitView(view_, w, h, (void*)rgba, w * 4)) {
        failed_ = true;
        err_ = "FUAI_ImageViewInit 抛访问违例";
        trace("ImageViewInit 抛访问违例");
        return false;
    }
    trace("-> FaceProcessorProcess2Result");
    if (!sehProcess(proc_, view_, result_)) {
        failed_ = true;
        err_ = "FUAI_FaceProcessorProcess2Result 抛访问违例";
        trace("Process2Result 抛访问违例");
        return false;
    }
    trace("<- FaceProcessorProcess2Result");

    int n = 0;
    trace("-> FaceProcessorGetNumFromResult");
    if (!sehGetNum(result_, &n)) {
        failed_ = true;
        err_ = "FUAI_FaceProcessorGetNumFromResult 抛访问违例";
        trace("GetNumFromResult 抛访问违例");
        return false;
    }
    faces_ = n < 0 ? 0 : n;
    trace("<- faces=%d", faces_);

    float* p = nullptr;
    int mw = 0, mh = 0;
    trace("-> FaceProcessorGetSkinSegResult");
    int r = sehGetSkin(proc_, view_, &p, &mw, &mh);
    trace("<- GetSkinSegResult r=%d ptr=%p %dx%d", r, p, mw, mh);
    if (r == 0) {
        failed_ = true;
        err_ = "FUAI_FaceProcessorGetSkinSegResult 抛访问违例";
        return false;
    }
    if (r == 2 || !p || mw <= 0 || mh <= 0) {
        // 没人脸时这里可能是空指针 —— 不是错误，只是本次没有掩码
        hasMask_ = false;
        coverage_ = 0.0;
        inferMs_ = nowMsLocal() - t0;
        inferCount_++;
        return true;
    }

    // float 概率 → uint8，顺带算覆盖度（>=0.5 的像素占比）
    const size_t npx = (size_t)mw * (size_t)mh;
    if (mask_.size() < npx) mask_.resize(npx);
    mw_ = mw;
    mh_ = mh;
    size_t hit = 0;
    const float* src = p;
    unsigned char* dst = &mask_[0];
    for (size_t i = 0; i < npx; i++) {
        float v = src[i];
        int q = (int)(v * 255.0f + 0.5f);
        if (q < 0) q = 0;
        if (q > 255) q = 255;
        dst[i] = (unsigned char)q;
        if (q >= 128) hit++;
    }
    coverage_ = (double)hit / (double)npx;
    hasMask_ = true;
    inferMs_ = nowMsLocal() - t0;
    inferCount_++;
    return true;
}

const unsigned char* NamaRegion::update(const unsigned char* rgba, int w, int h, int every) {
    if (!ready_ || failed_ || !rgba || w <= 0 || h <= 0) return nullptr;
    if (every < 1) every = 1;

    // 每 every 帧才真推理；其余帧沿用上次掩码（人脸位置在相邻帧里几乎不动）
    const bool due = (!hasMask_) || (frameSeq_ % (long)every == 0);
    frameSeq_++;
    if (due) {
        if (!inferOnce(rgba, w, h)) return nullptr;
    }
    return hasMask_ ? &mask_[0] : nullptr;
}

void NamaRegion::release() {
    // 刻意不 FreeLibrary：宿主可能还有别的线程在用，进程退出交给 OS 回收。
    ready_ = false;
    proc_ = nullptr;
    result_ = nullptr;
    view_ = nullptr;
    mask_.clear();
    bundle_.clear();
}
