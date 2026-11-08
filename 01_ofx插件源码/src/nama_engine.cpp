// nama_engine.cpp —— DaVinciBeauty 引擎适配层（v3.0.0）
//
// 本层不再加载旧版闭源 SDK。它把 ofx_plugin.cpp 的全部请求转给自研的
// 纯 CPU 美颜引擎（CoreEngine，对应上一代引擎 3.1.39），人脸检测（YuNet / ONNX）在引擎内部
// 自动检测并优先使用 GPU（DirectML），无 GPU 则自动退回 CPU。
// 所有渲染 / 检测逻辑都在 CoreEngine 里；这里只做参数搬运 + 转调（经 core_glue 桥接层）。
//
// 公开接口与旧版（闭源 SDK 版）完全一致，ofx_plugin.cpp 一行都不用改。

#include "nama_engine.h"
#include "core_glue.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

// core_glue 的 coreBeautyProcess 固定按 14 个 double 转发；P_COUNT 一旦增长，
// `double panel[14]` + 循环到 P_COUNT 就会写穿栈。这里把契约钉死在编译期。
static_assert(P_COUNT == 14, "core_glue.h / coreBeautyProcess 固定 14 项，改枚举必须同步改桥接层");

std::mutex g_mux;   // 保护 ensureReady / preheat 的「只初始化一次」语义

// UTF-8 窄字符 → 宽字符。NamaSelfTest 的路径参数是 UTF-8（探针脚本用 Python
// 的 .encode("utf-8") 传），而 _wfopen 要宽字符。以前这里直接当 ANSI 用，
// 路径含中文时打不开文件。
std::wstring utf8ToWide(const char* s) {
    if (!s || !*s) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    w.resize((size_t)n - 1);
    return w;
}

// 与 ofx_plugin.cpp 的 kParams 逐位对齐（acceptance.py 的「参数」组静态校验用）。
// 自研引擎不再有 SDK key 概念，这里只保留名字表，供「面板名 ↔ key 名」对齐检查；
// 不用于任何运行时逻辑。名字必须与 kParams[i].name 完全一致。
const char* keys[P_COUNT] = {
    "blur_level", "clarity", "color_level", "red_level",
    "sharpen", "global_sharpen", "face_threed", "eye_bright",
    "remove_pouch_strength", "remove_nasolabial_folds_strength",
    "delspot_level", "cheek_thinning", "eye_enlarging", "intensity_chin"
};

}  // namespace

// ---------------------------------------------------------------------------
// 单例
// ---------------------------------------------------------------------------
NamaEngine& NamaEngine::inst() {
    static NamaEngine s;
    return s;
}

NamaEngine::NamaEngine() {}
// 析构函数为头文件内联（~NamaEngine() {}），此处不再定义。

// ---------------------------------------------------------------------------
// 初始化
// ---------------------------------------------------------------------------
bool NamaEngine::ensureReady(std::string& err) {
    std::lock_guard<std::mutex> lk(g_mux);
    if (ready_) return true;
    char buf[512];
    bool ok = coreBeautyEnsureReady(nullptr, buf, (int)sizeof(buf));
    ready_ = ok;
    if (ok) {
        ownerThread_  = GetCurrentThreadId();
        regionActive_ = coreBeautyRegionActive();
        failReason_.clear();
    } else {
        failReason_ = buf;
        err = buf;
        failCount_++;
    }
    return ok;
}

bool NamaEngine::preheat(std::string& err, bool& didInit) {
    std::lock_guard<std::mutex> lk(g_mux);
    didInit = false;
    if (ready_) return true;
    char buf[512];
    int di = 0;
    bool ok = coreBeautyPreheat(nullptr, buf, (int)sizeof(buf), &di);
    ready_ = ok;
    didInit = (di != 0);
    if (ok) {
        ownerThread_  = GetCurrentThreadId();
        regionActive_ = coreBeautyRegionActive();
        failReason_.clear();
    } else {
        failReason_ = buf;
        err = buf;
        failCount_++;
    }
    return ok;
}

// ready()/poisoned()/failCount()/ownerThread()/regionActive() 为头文件内联方法。

void NamaEngine::release() {
    coreBeautyRelease();
    ready_ = false;
}

// ---------------------------------------------------------------------------
// 每实例一个槽位
// ---------------------------------------------------------------------------
int NamaEngine::acquireSlot(std::string& err) {
    char buf[512];
    int s = coreBeautyAcquireSlot(buf, (int)sizeof(buf));
    if (s < 0) {
        failReason_ = buf;
        err = buf;
    }
    return s;
}

void NamaEngine::releaseSlot(int slot) {
    coreBeautyReleaseSlot(slot);
}

void NamaEngine::setDetectOptions(int rotate, int landmarkQuality, int maxFaces) {
    coreBeautySetDetectOptions(rotate, landmarkQuality, maxFaces);
}

// ---------------------------------------------------------------------------
// 逐帧处理
// ---------------------------------------------------------------------------
bool NamaEngine::process(const unsigned char* src, unsigned char* dst,
                        int w, int h, const NamaSettings& s) {
    char buf[512];
    int slot = coreBeautyAcquireSlot(buf, (int)sizeof(buf));
    if (slot < 0) {
        failReason_ = buf;
        return false;
    }
    bool ok = process(slot, src, dst, w, h, s, true);
    coreBeautyReleaseSlot(slot);
    return ok;
}

bool NamaEngine::process(int slot, const unsigned char* src, unsigned char* dst,
                         int w, int h, const NamaSettings& s, bool advanceFrame) {
    if (!ready_) {
        std::string e;
        if (!ensureReady(e)) return false;
    }
    // NamaSettings 与 BeautySettings 的内存布局逐字段相同：
    //   double panel[14] + int blur_type + int skin_detect + int smooth_mode
    // 美型三项下标（11/12/13 = 瘦脸/大眼/下巴）在两边含义一致，可直接直通。
    double panel[14];
    for (int i = 0; i < P_COUNT; i++) panel[i] = s.panel[i];

    bool ok = coreBeautyProcess(slot, src, dst, w, h, panel,
                               s.blur_type, s.skin_detect, s.smooth_mode, advanceFrame);
    faces_       = coreBeautyLastFaces();
    faceScore_   = coreBeautyLastFaceScore();   // v3.0.1
    failReason_  = coreBeautyLastErr();
    lastSdkMs_   = coreBeautyLastCoreMs();
    return ok;
}

// ---------------------------------------------------------------------------
// 只读诊断（供日志 / UI 使用）
// ---------------------------------------------------------------------------
int NamaEngine::itemId() const    { return coreBeautyItemId(); }

const char* NamaEngine::version() const {
    // 只在不一致时改写。调用方常见写法是 `version() ? version() : "?"` ——
    // 两次调用之间若发生重新赋值，第一个指针就可能悬空（UB）。内容在引擎
    // 初始化后不再变化，因此这里实际上只写一次。
    static std::mutex m;
    static std::string v;
    std::string cur = std::string(coreBeautyVersion()) + " / " + coreBeautyGpuProvider();
    std::lock_guard<std::mutex> lk(m);
    if (cur != v) v = std::move(cur);
    return v.c_str();
}

int   NamaEngine::regionFaces() const     { return coreBeautyLastFaces(); }
double NamaEngine::regionCoverage() const { return 0.0; }   // CoreEngine 不暴露该统计
double NamaEngine::regionInferMs() const  { return 0.0; }    // CoreEngine 不暴露该统计
long   NamaEngine::regionInferCount() const { return 0; }    // CoreEngine 不暴露该统计
const char* NamaEngine::regionErr() const {
    return regionErr_.empty() ? "" : regionErr_.c_str();
}

// 诊断（BUG4）：本帧是否发生「整帧换帧」。见 beauty_core.cpp 的
// g_beautyDiagFrameSubst 注释与 PROGRESS「BUG4 多实例串台」第 7 节。
bool NamaEngine::frameSubstituted() const { return coreBeautyDiagFrameSubst() != 0; }

// ---------------------------------------------------------------------------
// 自测入口（C ABI，供 blur_test 探针 / acceptance 调用）
// ---------------------------------------------------------------------------
extern "C" {

int __stdcall NamaSelfTest(const char* engineDir, const char* rawRgbaIn,
                           const char* rawRgbaOut, int width, int height,
                           double blur, double whiten, char* msgBuf, int msgCap) {
    char buf[512];
    auto setMsg = [&](const char* s) {
        if (msgBuf && msgCap > 0) { strncpy(msgBuf, s, (size_t)msgCap - 1); msgBuf[msgCap - 1] = '\0'; }
    };
    if (!rawRgbaIn || !rawRgbaOut || width <= 0 || height <= 0) { setMsg("参数不合法"); return 2; }

    // ---- 读输入帧 ----
    // v3.0.3 修正：rawRgbaIn/rawRgbaOut 是**文件路径**，不是内存缓冲。
    // v2.0 移植到 CoreEngine 时把这里写成了「直接把入参当图像指针用」，
    // 结果整条自测链路喂进去的是路径字符串（如某个 mp4 的完整路径），
    // 检测必然 0 脸、输出也写不出来（还会把 Python 的 bytes 对象写崩）。
    // 这导致 v3.0.x 期间所有基于自测得出的「引擎 0 检出」结论都是假象。
    const size_t need = (size_t)width * (size_t)height * 4;
    std::vector<unsigned char> in(need), out(need);
    {
        const std::wstring w = utf8ToWide(rawRgbaIn);
        FILE* f = nullptr;
        if (w.empty() || _wfopen_s(&f, w.c_str(), L"rb") != 0 || !f) {
            setMsg("打不开输入文件");
            return 3;
        }
        const size_t got = fread(&in[0], 1, need, f);
        fclose(f);
        if (got != need) { setMsg("输入字节不足"); return 4; }
    }

    if (!coreBeautyEnsureReady(engineDir, buf, (int)sizeof(buf))) {
        setMsg(buf);
        return 1;   // 引擎初始化失败
    }

    // v3.0.4：行序。自测喂的是文件里的 top-down RGBA（探针/夹具都是按
    // PIL/numpy 的行序存的），而引擎默认是 OFX 的 bottom-up，所以这里显式
    // 声明 top-down。要拿真机抓下来的 bottom-up 帧（C:\beauty_dump\src.raw）
    // 回归检测链路时，设 BEAUTY_IMG_BOTTOMUP=1 即可，不用改代码。
    {
        const char* bu = getenv("BEAUTY_IMG_BOTTOMUP");
        coreBeautySetImageBottomUp(bu && *bu ? (bu[0] != '0') : 0);
    }

    // v3.0.4：自测必须**确定性**。异步检测下「本帧用哪份掩码」取决于 worker
    // 线程调度，同一份输入连跑两次会得到不同输出（实测同进程连跑 blur62 出现
    // 两个 md5）—— 那正是 04_验收工具/baseline.py 的基线一直不可复现的原因。
    // 这里关掉异步，让检测在调用线程上同步完成。生产路径（OFX render）不受影响。
    coreBeautySetAsyncDetect(0);

    NamaSettings s;                 // 默认构造 = 全 0
    s.panel[P_BLUR]   = blur;       // P_BLUR / P_WHITEN 来自本文件头部枚举，与引擎下标一致
    s.panel[P_WHITEN] = whiten;

    // v3.0.4：恢复 NAMA_ST_* 自测钩子（v2.x 有，v3.0 重写时丢了）。
    //
    // 为什么必须有：04_验收工具/baseline.py 的 15 个逐字节用例里，有 8 个靠这些
    // 变量切换清晰/锐化/全局锐化/美型/磨皮模式。钩子丢掉之后，那 8 个用例的
    // 输出与 blur62_wh30 **完全相同**（实测同一个 md5），回归网看着有 15 格，
    // 实际只覆盖了 4 种参数组合 —— 参数接线断了也测不出来。
    //
    // 只影响 C ABI 自测路径，不碰宿主 render。
    {
        auto stv = [](const char* name, double& dst) {
            const char* v = getenv(name);
            if (v && *v) dst = atof(v);
        };
        stv("NAMA_ST_CLARITY", s.panel[P_CLARITY]);
        stv("NAMA_ST_SHARPEN", s.panel[P_SHARPEN]);
        stv("NAMA_ST_GSHARP",  s.panel[P_GLOBAL_SHARP]);
        stv("NAMA_ST_CHEEK",   s.panel[P_THIN_FACE]);
        stv("NAMA_ST_EYE",     s.panel[P_BIG_EYE]);
        stv("NAMA_ST_CHIN",    s.panel[P_CHIN]);
        stv("NAMA_ST_DELSPOT", s.panel[P_DELSPOT]);
        stv("NAMA_ST_NASO",    s.panel[P_NASOLABIAL]);
        double sm = (double)s.smooth_mode;
        stv("NAMA_ST_SMOOTH", sm);
        s.smooth_mode = (int)sm;
        // v3.0.5：磨皮算法三档（0 双边 / 1 高斯 / 2 弱磨皮）。基线要能覆盖
        // 「换了算法档」的输出差异，否则接线断了也测不出来 —— 与 NAMA_ST_*
        // 其余钩子同一个理由。
        double alg = (double)s.blur_type;
        stv("NAMA_ST_ALG", alg);
        s.blur_type = (int)alg;
    }

    char ebuf[512];
    int slot = coreBeautyAcquireSlot(ebuf, (int)sizeof(ebuf));
    if (slot < 0) {
        setMsg(ebuf);
        coreBeautyRelease();
        return 2;   // 槽位申请失败
    }

    double panel[14];
    for (int i = 0; i < P_COUNT; i++) panel[i] = s.panel[i];
    bool ok = coreBeautyProcess(slot, &in[0], &out[0], width, height,
                               panel, s.blur_type, s.skin_detect, s.smooth_mode, true);
    // 检测结果要在释放槽位之前取，释放后会清零。
    const int   faces = coreBeautyLastFaces();
    const double best = coreBeautyLastFaceScore();
    coreBeautyReleaseSlot(slot);
    coreBeautyRelease();

    if (!ok) {
        setMsg("coreBeautyProcess 失败");
        return 3;   // 渲染失败
    }

    {
        const std::wstring w = utf8ToWide(rawRgbaOut);
        FILE* f = nullptr;
        if (w.empty() || _wfopen_s(&f, w.c_str(), L"wb") != 0 || !f) {
            setMsg("写不了输出文件");
            return 7;
        }
        fwrite(&out[0], 1, need, f);
        fclose(f);
    }

    // 把检测结果回传，探针脚本可以直接断言「有没有检出脸」，
    // 不用再去翻 BEAUTY_DEBUG_DETECT 的日志。
    snprintf(buf, sizeof(buf), "faces=%d best=%.4f", faces, best);
    setMsg(buf);
    return 0;       // 成功
}

unsigned long __stdcall NamaVersion(void) {
    return ((unsigned long)NAMA_BUILD_MAJOR << 16) | (unsigned long)NAMA_BUILD_MINOR;
}

// beauty_build.h 的实现：版本串的唯一出口。数值只由上面的宏决定，
// 这里不写任何字面量副本（写死过一次 "1.4.0"，日志里就和 build= 打架）。
const char* namaBuildStr(void) {
    return NAMA_BUILD_STR;
}

}  // extern "C"
