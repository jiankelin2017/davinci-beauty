// nama_engine.cpp —— NamaEngine 实现。
//
// 所有 SDK 函数都是 cdecl；签名来自 BeautyCore.cs 与 obs-nama-beauty.cpp，
// 两者对同一个 DLL 的用法一致，可以交叉印证。

#include "nama_engine.h"
#include "nama_region.h"

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

// ---------------------------------------------------------------------------
// 关于第三个参数：它是**精确枚举值，不是位掩码**（2026-10-24 实测，别再试）
//
// 曾经想用「把 FUAITYPE_FACEPROCESSOR_SKINSEGMENT 之类按位或进去」的办法，
// 让 CNamaSDK 顺便把皮肤分割模型也加载起来。实测结论是行不通：
//
//   aitype=0x100  -> 预热 859ms, ofx.log faces=1, sdk=5.0ms   （正常）
//   aitype=0x101  -> 预热 313ms, ofx.log faces=0, sdk=2.2ms   （人脸模型根本没加载）
//
// 1<<0 … 1<<31 逐位 + 6 组组合，**全部**表现为后者。所以 OR 进任何额外位都只是
// 让这个值变成 SDK 不认识的类型，结果是人脸检测整体失效 —— 而且画面看起来
// 仍然「有磨皮」，因为磨皮的肤色掩码是逐像素颜色判据、不依赖人脸检测
//（见 PHASE0_SDK区域探测报告 表 2），所以从像素差异上几乎看不出来，极易误判成
// 「参数无效」。判据必须看 ofx.log 的 faces= 与预热耗时，不能看画面。
//
// 另外，皮肤分割模型**本来就在跑**：初始化日志里有
// `fuai/face/face_processor.h:200 Perform SetUseSkinSeg`。缺的从来不是模型，
// 而是「美颜道具去消费分割结果」的开关，那个开关只存在于 bundle 的 JS 控制器里
//（face_beautification/files/script.js），我们的集成路径没走到那一层。
// 详见 02_测试夹具beauty_probe\PHASE1_区域精准化探测报告.md 第 5.4 节。
// ---------------------------------------------------------------------------
// FU_FORMAT_RGBA_BUFFER —— probe_nama.py 里用 0 作为 in/out format 实测通过。
// ⚠️ 注意：常量值 0 本身没问题，但这个 SDK 版本按 **BGRA byte order** 解释
// format 0 的缓冲区。实测（照片 A7300538，640×394，同一帧同一次初始化）：
//     R/G/B 顺序输入 → faces=0（检不出人脸）
//     B/G/R 顺序输入 → faces=1（正常检出）
// 43 张真实照片里 39 张原先 faces=0，把通道换成 BGRA 后全部正常检出。
// 也就是说以前「检测不到人脸 → skin_detect 不生效 → 变相全局磨皮」的根因
// 有一半在这里：不是人脸小，是通道序错了，AI 模型看到的是一张颜色错乱的脸。
// 转换在 SDK 边界上做（rgbaToBgra / bgraToRgba），不进插件其它部分。
const int FU_FORMAT_RGBA_BUFFER = 0;

// ---------------------------------------------------------------------------
// 全局滤镜：默认关闭
//
// face_beautification 道具里挂着一个作用于**整帧**的色调滤镜（filter_name +
// filter_level）。早期实现照抄 obs-nama-beauty.cpp，把 filter_name 设成 "ziran2"、
// filter_level 设成 0.4，并且「只要有任意一项效果不为 0 就把滤镜打开」。
//
// 这不是一个中性的开关，实测（640×394 纯灰 128 输入，无人脸）：
//   filter_level=0.0 → 输出 128,128,128（逐字节不变）
//   filter_level=0.1 → +4,+3,+3
//   filter_level=0.2 → +8,+7,+7
//   filter_level=0.4 → +16,+14,+14
// 灰阶 ramp 上是一条抬暗部的曲线：输入 25 → 34、128 → 144、229 → 230，
// 即**整帧（含背景、衣服、字幕）被提亮**，而不是只处理人脸。
// 用户把「磨皮」从 0 拉到 1 时，画面整体会突然亮一截 —— 这正是要修的 bug。
//
// 因此滤镜永久关闭：filter_name 用 "origin"（等价于无滤镜），filter_level 恒 0。
// 早期 Python 脚本版（nama_worker.py:327-328）本来就是这么做的，OFX 版偏离了它。
//
// 代价：不再有原插件那种全局色调美化。八项效果本身不受影响 —— 磨皮、清晰、
// 美白、红润在滤镜关闭时依然生效（已用棋盘格 / 灰阶图逐项验证）。
// ---------------------------------------------------------------------------
const char* const kFilterName = "origin";
const double kFilterLevel = 0.0;

// ---------------------------------------------------------------------------
// 通道序转换：宿主侧一律 RGBA，SDK 侧按 BGRA 解释（见 FU_FORMAT_RGBA_BUFFER 的说明）
//
// 只在 SDK 边界上交换 R/B：进 SDK 前 rgbaToBgra，出 SDK 后 bgraToRgba。
// 写成一个循环是为了让编译器自动向量化；每帧两遍全帧扫描，1080p 约 0.2ms。
// 刻意不做「检测到输入已是 BGRA 就跳过」的优化 —— 没有可靠的判据，
// 而一次多余的交换只是 0.2ms，猜错的代价是整帧人脸检不出。
// ---------------------------------------------------------------------------
inline void rgbaToBgra(unsigned char* p, size_t n) {
    for (size_t i = 0; i < n; i += 4) {
        unsigned char t = p[i];
        p[i] = p[i + 2];
        p[i + 2] = t;
    }
}

inline void bgraToRgba(unsigned char* p, size_t n) {
    for (size_t i = 0; i < n; i += 4) {
        unsigned char t = p[i];
        p[i] = p[i + 2];
        p[i + 2] = t;
    }
}

// 初始化连续失败几次之后就不再重试。每次重试都要付约 2 秒（fuSetup + 模型 + 道具），
// 让它跟着每一帧反复发生会把宿主拖死。
const int kMaxInitFail = 3;

// ---------------------------------------------------------------------------
// 诊断注入口：NAMA_EXTRA_PARAMS / NAMA_EXTRA_PARAMS_FILE
//
// 形如 "use_new_whiten=1;color_level_v2=0.5"（分号分隔，值为 double）。
// 这些参数在 SDK 里存在但**没有出现在任何公开文档里**（是从 CNamaSDK.dll 的
// 字符串池里挖出来的），要判断「哪一组开关能让美白只作用于人脸」只能逐个试。
//
// 两个来源：
//   NAMA_EXTRA_PARAMS      环境变量，进程内只解析一次（诊断用）
//   NAMA_EXTRA_PARAMS_FILE 文件，**每次下发前重新读**。测量脚本可以在同一个
//                          进程里换文件内容来扫参数组合，不用反复重启
//                          插件/重建 SDK 实例（初始化要 2 秒一扫太慢）。
//
// 语义：
//   * 创建美颜道具时下发一次；
//   * 每次面板参数变化时再下发一次（保证与其它参数同批生效）；
//   * 走文件来源时**每帧都下发**，这样只改文件不改面板参数也能生效。
// 不设置这两个环境变量时行为与以前**完全一致**（不解析、不下发、零开销）。
//
// 这是诊断用途的开关，不在 OFX 面板上暴露；正式方案确定后应改成常量并删掉它。
// ---------------------------------------------------------------------------
struct ExtraParam {
    std::string key;
    double value;
};

// 解析 "k=v;k2=v2"。空白会被去掉；无法解析的片段直接跳过。
static void parseExtraParamText(const std::string& s, std::vector<ExtraParam>& out) {
    size_t pos = 0;
    while (pos < s.size()) {
        size_t sc = s.find(';', pos);
        if (sc == std::string::npos) sc = s.size();
        std::string one = s.substr(pos, sc - pos);
        pos = sc + 1;
        size_t eq = one.find('=');
        if (eq == std::string::npos) continue;
        std::string k = one.substr(0, eq);
        std::string v = one.substr(eq + 1);
        // 去掉首尾空白
        size_t b = k.find_first_not_of(" \t");
        size_t e = k.find_last_not_of(" \t");
        if (b == std::string::npos) continue;
        k = k.substr(b, e - b + 1);
        b = v.find_first_not_of(" \t");
        e = v.find_last_not_of(" \t");
        if (b == std::string::npos) continue;
        v = v.substr(b, e - b + 1);
        ExtraParam p;
        p.key = k;
        p.value = atof(v.c_str());
        out.push_back(p);
    }
}

// 诊断参数当前来源。文件优先于环境变量。
static const char* extraParamsFile() {
    static int cached = -1;
    static std::string path;
    if (cached < 0) {
        cached = 0;
        const char* p = getenv("NAMA_EXTRA_PARAMS_FILE");
        if (p && *p) {
            path = p;
            cached = 1;
        }
    }
    return cached ? path.c_str() : nullptr;
}

// 返回当前生效的诊断参数。
//   * 走文件来源：每次重新读文件（外部可以随时换内容）
//   * 走环境变量：进程内只解析一次
static const std::vector<ExtraParam>& extraParams() {
    static std::vector<ExtraParam> cached;
    static bool inited = false;
    const char* file = extraParamsFile();
    if (file) {
        std::vector<ExtraParam> now;
        FILE* f = nullptr;
        errno_t e = fopen_s(&f, file, "rb");
        if (e == 0 && f) {
            char buf[4096];
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            fclose(f);
            buf[n] = 0;
            parseExtraParamText(std::string(buf), now);
        }
        cached.swap(now);
        return cached;
    }
    if (!inited) {
        inited = true;
        const char* env = getenv("NAMA_EXTRA_PARAMS");
        if (env && *env) {
            parseExtraParamText(std::string(env), cached);
        }
    }
    return cached;
}

// 逐帧是否还要切换 GL 上下文。//
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

// 把诊断参数下发到 SDK 道具。
// SEH 兜底函数内部不能出现需要析构的 C++ 对象，所以这里只传裸指针和标量。
//
// 走文件来源时**每帧都下发**：这样外部只改文件、不动面板参数，也能让参数立刻
// 生效（测量脚本扫参数组合时可以一个进程跑完，不必反复重启插件）。
// 走环境变量来源时只在参数变化时下发（与其它参数同批），保持零额外开销。
static void applyExtraParams(int item, bool force) {
    const std::vector<ExtraParam>& v = extraParams();
    if (!force && v.empty()) return;
    for (size_t i = 0; i < v.size(); i++) {
        const char* k = v[i].key.c_str();
        double val = v[i].value;
        sehSetParamd(item, k, val);
        fprintf(stderr, "[nama] 诊断参数 %s = %g\n", k, val);
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

int sehLoadFaceModel(int* outRet, const unsigned char* pkg, int len) {
    __try {
        // 第三个参数是精确枚举，只能传 FUAITYPE_FACEPROCESSOR，不能按位或。
        *outRet = g_fuLoadAIModelFromPackage(pkg, len, FUAITYPE_FACEPROCESSOR);
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

// ---------------------------------------------------------------------------
// 美白：按肤色掩码在 CPU 侧提亮（只动皮肤/人脸，不动背景）
//
// 为什么不用 SDK 自带的美白通道（color_level / color_level_v2）：
// 实测 5 张真实照片（640×394，A/B 各热身 3 帧、逐像素差分），两条通道都是
// **整帧**作用，没有任何参数能把它限到人脸——
//   color_level：横幅照片 99% 像素被改，人脸∩内容 11.25 vs 非人脸∩内容 11.17
//                （几乎无差别），连没人脸的照片也 96% 被改；
//   color_level_v2 + use_new_whiten：只在竖幅照片上看着像"限定人脸列"，
//                其实那是 letterbox 黑边的假象——内容区照样 99% 被改；
//   use_new_whiten/whiten_v2/beauty_mask_v/skin_detect/lower_quality/
//   manual_scale：全部无效或本身惰性。V≥0.65 还会退化成连黑边都改的全局。
//
// 为什么不用 fuFaceProcessorGetResultHeadMask（官方 CNamaSDK.h 有这个导出）：
//   插件曾按它实现「只提亮头部掩码」，但实测本 SDK 版本在 fuRenderItemsEx
//   之后对 index=0 调用**恒返回 nullptr**（mw=mh=0），即使同一帧 faces=1。
//   可能还需要未文档化的前置开关，风险太大，所以退回 CPU 肤色掩码。
//
// 于是改走插件自己这条线：渲染前把 SDK 美白恒关（见 process 里的
// sehSetParamd(item,"color_level",0)），渲染完成后按肤色掩码提亮：
//   * 有脸 → 只动皮肤（掩码羽化过，边界不硬）；
//   * 无脸 → 直接跳过（顺带修掉无人脸照片的整幅提亮）。
//
// 性能是硬约束：全分辨率掩码（逐像素判定 + 3×3 膨胀 + 5×5 羽化）在 1080p
// 实测 **38 ms/帧**，把 70fps 直接拖到 16fps。因此：
//   * 掩码在 1/4 分辨率上算（每 4×4 块取一个样本），3×3 均值羽化，
//     应用时双线性放大回全分辨率；
//   * 掩码缓冲由引擎复用，稳定后零堆分配；
//   * 提亮量按掩码值预生成 256 项整数表，逐像素只有乘加，无浮点。
// 精度取舍：4px 的掩码粒度对「整张脸 / 整片皮肤」足够，羽化后没有硬边。
// 已知局限：掩码是肤色启发式，画面里大面积的肤色物体（木桌、米色墙）
// 也会被轻微提亮；没有可靠的人脸框之前这是可接受的折中。
//
// 公式 c += (255-c) · k · m · (c/255)：按亮度加权——皮肤提得动，
// 头发/纯黑背景几乎不动（c=0 时恒为 0），m 是双线性采样的肤色掩码。
// ---------------------------------------------------------------------------
static const int kWhitenMaskDown = 4;   // 掩码降采样倍率（1080p → 约 13 万点）
static void applyWhiten(unsigned char* rgba, int w, int h,
                        double strength, int nFaces,
                        std::vector<unsigned char>& mask,
                        std::vector<unsigned char>& tmp) {
    if (strength <= 0.0 || nFaces <= 0)
        return;   // 没人脸就不动：既不全局提亮，也不误伤无人脸画面
    const double kMax = 0.40;   // 面板美白=100 → k=0.40（实测皮肤提亮 ~+25）
    double k = strength / 100.0 * kMax;
    if (k <= 0.0) return;

    const int mw = (w + kWhitenMaskDown - 1) / kWhitenMaskDown;
    const int mh = (h + kWhitenMaskDown - 1) / kWhitenMaskDown;
    const size_t mn = (size_t)mw * (size_t)mh;
    if (mask.size() < mn) mask.resize(mn);
    if (tmp.size() < mn) tmp.resize(mn);

    // ---- 第一 pass：低分辨率肤色掩码（每 4×4 块取一个样本）------------
    int hits = 0;
    for (int y = 0; y < mh; y++) {
        const unsigned char* row = rgba + (size_t)(y * kWhitenMaskDown) * (size_t)w * 4;
        unsigned char* m = &mask[(size_t)y * (size_t)mw];
        for (int x = 0; x < mw; x++) {
            const unsigned char* p = row + (size_t)(x * kWhitenMaskDown) * 4;
            int R = p[0], G = p[1], B = p[2];
            int Y = (66 * R + 129 * G + 25 * B + 128) >> 8;
            int Cb = ((-38 * R - 74 * G + 112 * B + 128) >> 8) + 128;
            int Cr = ((112 * R - 94 * G - 18 * B + 128) >> 8) + 128;
            bool skin = (Cr >= 133 && Cr <= 173 && Cb >= 77 && Cb <= 127 &&
                         R > G && G > B && R > 80 && Y > 60 && Y < 235);
            m[x] = skin ? 255 : 0;
            if (skin) hits++;
        }
    }
    if (hits == 0) return;   // 没有肤色像素：整帧一个字节都不动

    // ---- 第二 pass：3×3 均值羽化（可分离，边界复制）-------------------
    for (int y = 0; y < mh; y++) {          // 水平
        const unsigned char* s = &mask[(size_t)y * (size_t)mw];
        unsigned char* t = &tmp[(size_t)y * (size_t)mw];
        for (int x = 0; x < mw; x++) {
            int x0 = x > 0 ? x - 1 : 0;
            int x1 = x + 1 < mw ? x + 1 : mw - 1;
            t[x] = (unsigned char)((s[x0] + s[x] + s[x1]) / 3);
        }
    }
    for (int y = 0; y < mh; y++) {          // 垂直（结果写回 mask）
        int y0 = y > 0 ? y - 1 : 0;
        int y1 = y + 1 < mh ? y + 1 : mh - 1;
        const unsigned char* a = &tmp[(size_t)y0 * (size_t)mw];
        const unsigned char* b = &tmp[(size_t)y * (size_t)mw];
        const unsigned char* c = &tmp[(size_t)y1 * (size_t)mw];
        unsigned char* m = &mask[(size_t)y * (size_t)mw];
        for (int x = 0; x < mw; x++)
            m[x] = (unsigned char)((a[x] + b[x] + c[x]) / 3);
    }

    // ---- 第三 pass：把 k 与掩码值合成整数表 ---------------------------
    // c' = c + c·(255-c)·k·m/255，m = 掩码值/255
    // 令 t = k·m·2^24/255 = k·mask·2^24/255²
    // t 最大 26214，c·(255-c) 最大 16256，乘积 < 2^31，int 足够。
    int tLut[256];
    for (int i = 0; i < 256; i++)
        tLut[i] = (int)(k * (double)i * 16777216.0 / 65025.0);

    const char* dbgEnv = getenv("NAMA_MASK_DEBUG");
    const bool dbg = (dbgEnv && dbgEnv[0] == '1');
    size_t cov = 0;

    // ---- 第四 pass：双线性放大掩码 + 提亮 -----------------------------
    for (int y = 0; y < h; y++) {
        const int qy = y >> 2, ry = y & 3;
        const unsigned char* r0 = &mask[(size_t)qy * (size_t)mw];
        const unsigned char* r1 = &mask[(size_t)(qy + 1 < mh ? qy + 1 : qy) * (size_t)mw];
        unsigned char* px = rgba + (size_t)y * (size_t)w * 4;
        for (int x = 0; x < w; x++, px += 4) {
            const int qx = x >> 2, rx = x & 3;
            const int x1 = qx + 1 < mw ? qx + 1 : qx;
            const int top = r0[qx] * (4 - rx) + r0[x1] * rx;
            const int bot = r1[qx] * (4 - rx) + r1[x1] * rx;
            const int mv = (top * (4 - ry) + bot * ry) >> 4;
            const int t = tLut[mv];
            if (t == 0) continue;
            if (mv > 127) cov++;
            for (int c = 0; c < 3; c++) {
                int v = px[c];
                px[c] = (unsigned char)(v + ((v * (255 - v) * t) >> 24));
            }
        }
    }

    if (dbg)
        fprintf(stderr, "[nama] 美白掩码 低分辨率=%dx%d 覆盖=%.1f%% k=%.3f\n",
                mw, mh, 100.0 * (double)cov / (double)((size_t)w * (size_t)h), k);
}

// 语义掩码的推理降频：每 kRegionEvery 帧真正跑一次 fuai，其余帧沿用上次掩码。
//
// 为什么是 3：fuai 单次推理实测 15.6 ms（1080×1920，detect_mode=0），而且
// **与分辨率基本无关**（模型输入尺寸固定，360×640 也是 20 ms），所以降频是
// 唯一的降本手段。N=3 摊到约 5.2 ms/帧，正好抵掉被删掉的 4.5 ms 颜色掩码。
// 相邻帧人脸位置几乎不动，3 帧的滞后在实拍素材上看不出来；但**快速摇镜 /
// 主体突然入画**时会滞后 2 帧。若以后要更稳，把这里改小即可（N=2 → 7.8 ms）。
const int kRegionEvery = 3;

// ---------------------------------------------------------------------------
// 语义区域融合（v1.5）：一趟遍历同时做两件事
//
//   1) 按皮肤掩码把 SDK 的效果收回去 —— m=0 的像素逐字节还原成输入，
//      于是「背景、衣服、木桌」完全不动（这是 v1.4 做不到的：SDK 双边档下
//      非肤色区仍有 93% 像素被改）。
//   2) 按同一掩码做美白提亮 —— 掩码来自 fuai 的语义分割，不再是 Cr/Cb 颜色判据，
//      所以白衣服、米色墙不会被提亮。
//
// 掩码是 fuai 的 192×256 皮肤概率图（整图各向异性拉伸，不是人脸局部裁剪），
// 双线性采样到全分辨率。每个 x/y 的下标与权重预先算好，逐像素只剩查表与乘加。
// ---------------------------------------------------------------------------
static void buildMaskMap(int w, int h, int mw, int mh,
                         std::vector<int>& x0, std::vector<int>& x1,
                         std::vector<int>& wx, std::vector<int>& y0,
                         std::vector<int>& y1, std::vector<int>& wy) {
    x0.resize((size_t)w); x1.resize((size_t)w); wx.resize((size_t)w);
    y0.resize((size_t)h); y1.resize((size_t)h); wy.resize((size_t)h);
    for (int x = 0; x < w; x++) {
        int i0 = 0, f = 0;
        if (mw > 1 && w > 1) {
            const long long q = ((long long)x * (mw - 1) * 65536) / (w - 1);
            i0 = (int)(q >> 16);
            f = (int)(((q & 0xFFFF) * 256) / 65536);
            if (i0 >= mw - 1) { i0 = mw - 1; f = 0; }
        }
        x0[x] = i0;
        x1[x] = (i0 + 1 < mw) ? i0 + 1 : i0;
        wx[x] = f;
    }
    for (int y = 0; y < h; y++) {
        int i0 = 0, f = 0;
        if (mh > 1 && h > 1) {
            const long long q = ((long long)y * (mh - 1) * 65536) / (h - 1);
            i0 = (int)(q >> 16);
            f = (int)(((q & 0xFFFF) * 256) / 65536);
            if (i0 >= mh - 1) { i0 = mh - 1; f = 0; }
        }
        y0[y] = i0;
        y1[y] = (i0 + 1 < mh) ? i0 + 1 : i0;
        wy[y] = f;
    }
}

static void applyRegionEffects(unsigned char* out, const unsigned char* src,
                               int w, int h, double whitenStrength,
                               bool limitToSkin,
                               const unsigned char* mask, int mw, int mh,
                               std::vector<int>& x0, std::vector<int>& x1,
                               std::vector<int>& wx, std::vector<int>& y0,
                               std::vector<int>& y1, std::vector<int>& wy) {
    if (!mask || mw <= 0 || mh <= 0) return;
    if (!limitToSkin && whitenStrength <= 0.0) return;

    // 与颜色启发式那条路同一个标定：面板 100 → k=0.40（实测皮肤提亮约 +25）
    const double kMax = 0.40;
    const double k = whitenStrength <= 0.0 ? 0.0 : whitenStrength / 100.0 * kMax;
    int tLut[256];
    for (int i = 0; i < 256; i++)
        tLut[i] = (int)(k * (double)i * 16777216.0 / 65025.0);

    buildMaskMap(w, h, mw, mh, x0, x1, wx, y0, y1, wy);

    for (int y = 0; y < h; y++) {
        unsigned char* row = out + (size_t)y * w * 4;
        const unsigned char* srow = src + (size_t)y * w * 4;
        const unsigned char* m0 = mask + (size_t)y0[y] * mw;
        const unsigned char* m1 = mask + (size_t)y1[y] * mw;
        const int fy = wy[y];
        for (int x = 0; x < w; x++, row += 4, srow += 4) {
            const int fx = wx[x];
            const int a0 = m0[x0[x]], a1 = m0[x1[x]];
            const int b0 = m1[x0[x]], b1 = m1[x1[x]];
            const int top = a0 * (256 - fx) + a1 * fx;
            const int bot = b0 * (256 - fx) + b1 * fx;
            const int mv = (top * (256 - fy) + bot * fy) >> 16;   // 0..255

            if (limitToSkin) {
                // m256：0→0、255→256。保证满掩码时逐字节等于 SDK 输出，
                // 也保证零掩码时逐字节等于输入（背景不动）。
                const int m256 = mv + (mv >> 7);
                if (m256 == 0) {
                    row[0] = srow[0]; row[1] = srow[1]; row[2] = srow[2];
                } else {
                    for (int c = 0; c < 3; c++) {
                        const int d = (int)row[c] - (int)srow[c];
                        row[c] = (unsigned char)((int)srow[c] + ((d * m256) >> 8));
                    }
                }
            }
            if (k > 0.0) {
                const int t = tLut[mv];
                if (t != 0) {
                    for (int c = 0; c < 3; c++) {
                        const int v = row[c];
                        row[c] = (unsigned char)(v + ((v * (255 - v) * t) >> 24));
                    }
                }
            }
        }
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

    int lmRet = 0;
    if (!sehLoadFaceModel(&lmRet, faceModel.empty() ? nullptr : &faceModel[0],
                          (int)faceModel.size()) ||
        !sehFaceSetup()) {
        InterlockedExchange(&g_sehHit, 1);
        leave();
        err = "人脸模型加载抛访问违例";
        return false;
    }
    // 返回值以前被直接忽略：模型加载失败时 SDK 会静默降级（faces 恒 0），
    // 表现成「参数怎么调都没效果」。这里至少留一条痕迹。
    if (lmRet == 0)
        fprintf(stderr, "[nama] 警告 fuLoadAIModelFromPackage 返回 0，人脸模型未加载\n");

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

    // 模块级默认，与 obs-nama-beauty.cpp 的 nama_init 基本一致，
    // 两处刻意偏离：filter_name/level 见 kFilterName；skin_detect 默认开。
    sehSetParamd(*outItem, "is_opengl_es", 0);  // 桌面 GL，否则 SDK 渲染出全黑
    sehSetParamd(*outItem, "use_landmark", 1);
    sehSetParamd(*outItem, "skin_detect", 1);   // 只磨皮肤，别把背景一起磨糊
    sehSetParamd(*outItem, "face_shape", 4);
    sehSetParamd(*outItem, "face_shape_level", 0.0);
    sehSetParams(*outItem, "filter_name", kFilterName);
    sehSetParamd(*outItem, "filter_level", kFilterLevel);
    sehSetParamd(*outItem, "color_level", 0.0);   // SDK 美白恒关（整帧），改走 applyWhiten
    sehSetParamd(*outItem, "heavy_blur", 0);
    // 诊断注入口（默认空 → 什么都不做）
    applyExtraParams(*outItem, true);
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
        ensureRegion();          // 语义区域：失败只降级，不影响引擎就绪
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
// 语义区域（fuai 皮肤分割）
//
// 只在引擎就绪时试一次。失败不设 err、不计数、不影响 ready_ —— 缺 fuai.dll 或
// 缺明文 bundle 是完全正常的情况（安装包里只有加密件），此时全部效果退回
// v1.4 的颜色启发式，行为与 1.4 逐字节一致。
//
// 耗时约 600 ms（读 23 MB bundle + 建 tflite 解释器），与 SDK 的 1.7 s 初始化
// 叠在一起，所以务必只在 createInstance 的预热路径里跑（ensureReadyLocked
// 只会真正初始化一次）。
// ---------------------------------------------------------------------------
bool NamaEngine::ensureRegion() {
    if (regionInitDone_) return regionActive_;
    regionInitDone_ = true;

    // 诊断开关：NAMA_REGION_OFF=1 强制退回 v1.4 行为，用于 A/B 对比同一段素材。
    const char* off = getenv("NAMA_REGION_OFF");
    if (off && off[0] == '1') {
        regionErr_ = "被 NAMA_REGION_OFF=1 关闭";
        regionActive_ = false;
        fprintf(stderr, "[nama] 语义区域被 NAMA_REGION_OFF=1 关闭\n");
        return false;
    }

    NamaRegion& r = NamaRegion::inst();
    std::string err;
    if (!r.ensureReady(dir_, err)) {
        regionErr_ = err;
        regionActive_ = false;
        fprintf(stderr, "[nama] 语义区域不可用，退回颜色启发式：%s\n", err.c_str());
        return false;
    }
    regionActive_ = true;
    fprintf(stderr, "[nama] 语义区域就绪（fuai 皮肤分割，每 %d 帧推理一次）\n",
            kRegionEvery);
    return true;
}

// 这几个只被 ofx.log 和测试夹具读取，都是无锁的只读快照 —— 读到上一帧的值
// 完全够用，不值得为它加锁。
int NamaEngine::regionFaces() const { return NamaRegion::inst().lastFaces(); }
double NamaEngine::regionCoverage() const { return NamaRegion::inst().coverage(); }
double NamaEngine::regionInferMs() const { return NamaRegion::inst().lastInferMs(); }
long NamaEngine::regionInferCount() const { return NamaRegion::inst().inferCount(); }
const char* NamaEngine::regionErr() const { return regionErr_.c_str(); }

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
    //
    // 注意这里**不要**预先交换 out。它的用途有两个：
    //   1) SDK 没写满时的兜底内容（此时它应该是 RGBA，因为后面要原样交给 dst）；
    //   2) SDK 的输出缓冲（此时 SDK 会按 BGRA 写它）。
    // 所以交换只做一次，放在 SDK 写完之后（见本函数末尾的 bgraToRgba）。
    // 以前在这里先 rgbaToBgra、末尾又 bgraToRgba，等于交换两次，输出颜色是反的。
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

    if (changed) {
        sehSetParamd(item, "blur_type", (double)s.blur_type);
        sehSetParamd(item, "skin_detect", (double)s.skin_detect);
        for (int i = 0; i < P_COUNT; i++) {
            if (i == P_WHITEN) continue;   // 美白不下发 SDK，见 applyWhiten
            double v = s.panel[i];
            if (v < 0) v = 0;
            if (v > 100) v = 100;
            sehSetParamd(item, keys[i], v / 100.0 * sdkMax[i]);
        }
        // SDK 自带美白恒关：实测 color_level 是整帧提亮（人脸 11.25 /
        // 背景 11.17 无差别，没人脸的照片也 96% 被改），没有任何参数能
        // 把它限到人脸。美白改由 applyWhiten 按肤色掩码在 CPU 侧完成。
        sehSetParamd(item, "color_level", 0.0);
        // 全局滤镜恒关（见 kFilterName / kFilterLevel 的说明）。这里以前是
        //   filter_level = (任意一项 > 0) ? 0.4 : 0.0
        // 于是「磨皮从 0 拉到 1」会让整帧突然提亮 +16/+14/+14 —— 用户报的亮度 bug。
        sehSetParams(item, "filter_name", kFilterName);
        sehSetParamd(item, "filter_level", kFilterLevel);
        // 诊断注入口：与其它参数同批下发（默认空 → 什么都不做）
        applyExtraParams(item, false);
        sl.applied = s;
        sl.hasApplied = true;
    } else {
        // 走文件来源的诊断参数：每帧重新读一次文件并下发，
        // 这样外部只换文件内容就能换参数组合（测量脚本用）。
        if (extraParamsFile()) applyExtraParams(item, true);
    }

    // 非顺序渲染（随机访问 / 单帧生成）时不推进帧序，避免人脸跟踪的时序状态漂移。
    const int useFrame = advanceFrame ? frameId_ : 0;

    int items[1] = {item};
    int ret = 0;
    // 输入换成 BGRA 再交给 SDK：src 是宿主给的 RGBA，且不能原地改（调用方可能
    // 复用同一块缓冲），所以复制一份到 bgScratch_ 再换。
    // 不换的话人脸检测基本失效 —— 实测 43 张真实照片里 39 张 faces=0，
    // 换序后 11 张检出（含多脸帧）。
    if (bgScratch_.size() < need) bgScratch_.resize(need);
    unsigned char* inBgra = (unsigned char*)&bgScratch_[0];
    memcpy(inBgra, src, need);
    rgbaToBgra(inBgra, need);

    if (!sehRender(&ret, FU_FORMAT_RGBA_BUFFER, out,
                   FU_FORMAT_RGBA_BUFFER, (void*)inBgra,
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

    if (ret == 0) {
        LeaveCriticalSection((CRITICAL_SECTION*)mutex_);
        char t[128];
        sprintf_s(t, "render 返回 0 (err=%d)", g_fuGetSystemError ? g_fuGetSystemError() : -1);
        failReason_ = t;
        return false;
    }
    // 输出也是 BGRA，换回 RGBA 再交给调用方
    bgraToRgba(out, need);

    // ---- 语义掩码 ----------------------------------------------------------
    // 放在 leave() 之后：fuai 是我们不认识的第三方 DLL，**不让它碰我们的 GL
    // 上下文**。enter()/leave() 存在的意义就是把宿主/SDK 的 GL 状态隔离开，
    // 让一个闭源 AI 库在这个窗口里插手是自找麻烦。
    //
    // 这里有个坑值得记下来（2026-10-24）：把这段夹在 sehRender 与 leave()
    // 之间、让上下文保持 current，也能跑通，结果完全一样。真正导致「第一次
    // render 就整进程静默消失」的不是上下文，而是 sehGetSkin 里那个
    // `int extra[4]` —— fuai 实际往里写 6 个 int，栈上越界踩坏 /GS cookie，
    // 触发 __fastfail(0xC0000409)，SEH 和 VEH 都兜不住。详见 nama_region.cpp。
    const unsigned char* mask = nullptr;
    int mw = 0, mh = 0;
    double cov = 0.0;
    if (regionActive_) {
        NamaRegion& r = NamaRegion::inst();
        mask = r.update(src, w, h, kRegionEvery);
        if (mask) {
            mw = r.maskW();
            mh = r.maskH();
            cov = r.coverage();
        }
    }

    // ---- 区域融合 ----------------------------------------------------------
    // 三种情况**不能混为一谈**（2026-10-24 实测踩过）：
    //
    //  1) 有掩码、覆盖度正常 → 按掩码合成。
    //  2) 有掩码、但覆盖度 ≈0 → fuai 在说「这一帧没有皮肤」。这时候必须
    //     **什么都不做**，绝不能退回全帧磨皮：素材 A7300560（无脸、无人像）
    //     就撞在这里 —— 掩码全 0，而 SDK 自己的颜色掩码认为 54% 的像素是皮肤，
    //     退回老路等于把整张图磨糊。拿全 0 掩码走一遍 applyRegionEffects 即可：
    //     m=0 的像素逐字节还原成输入，结果就是整帧不动。
    //     例外：SDK 明明检出了人脸、fuai 却说没皮肤 —— 两边判断冲突，更像
    //     fuai 偶发失败。这时退回颜色启发式，至少不会让人脸突然没效果。
    //  3) 拿不到掩码（fuai 不可用 / 连续推理都失败）→ 退回 v1.4 颜色启发式，
    //     也就是 NAMA_REGION_OFF=1 时的行为。
    //
    // 注意 src 是 RGBA，正好是 fuai 要的格式（kFmtRGBA），不用像 SDK 那样换序。
    // 掩码缓冲与映射表都是引擎成员，因此只能在锁内用；宿主声明了 Serial/Unsafe，
    // render 本就串行。
    const bool haveMask = (mask && mw > 0 && mh > 0);
    const bool noSkin = haveMask && cov < 0.001;

    if (haveMask && !(noSkin && faces > 0)) {
        // 美白沿用 v1.4 的门槛：**没检出人脸就不提亮**。
        //
        // 语义掩码本身是整帧的（没有脸也会给出皮肤色区域），如果不加这个门槛，
        // 把镜头对着米色墙 / 皮沙发也会被提亮 —— 那正是这一轮要修掉的毛病，
        // 只是换了个触发条件。掩码负责「在皮肤内」，faces 负责「画面里确实有人」。
        const double whiten = (faces > 0) ? s.panel[P_WHITEN] : 0.0;
        applyRegionEffects(out, src, w, h, whiten,
                           s.skin_detect != 0,
                           mask, mw, mh,
                           mapX0_, mapX1_, mapWX_, mapY0_, mapY1_, mapWY_);
    } else {
        // v1.4 老路径：颜色启发式美白（无脸则什么都不做）
        applyWhiten(out, w, h, s.panel[P_WHITEN], faces, whitenMask_, whitenTmp_);
    }

    if (dst != out) memcpy(dst, out, need);
    LeaveCriticalSection((CRITICAL_SECTION*)mutex_);
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
    // 语义区域：清掉掩码与状态，下次 ensureReady 会重新初始化。
    // 不这么做的话 regionActive_ 仍为 true 但 update() 恒返回 nullptr，
    // 会静默退化成「只有颜色启发式美白、磨皮不再限区域」。
    NamaRegion::inst().release();
    regionActive_ = false;
    regionInitDone_ = false;
    mapX0_.clear(); mapX1_.clear(); mapWX_.clear();
    mapY0_.clear(); mapY1_.clear(); mapWY_.clear();
    LeaveCriticalSection((CRITICAL_SECTION*)mutex_);
}

// ---------------------------------------------------------------------------
// 自测入参路径是 UTF-8（ctypes / Python 的惯例），但引擎内部走 ANSI 窄路径
// （fopen_s / LoadLibraryExA）。源码放在中文目录时 LoadLibrary 会 err=126。
// 这里在入口统一转成 ACP，自测在中文路径下也能跑。
// ---------------------------------------------------------------------------
static std::string utf8ToAcp(const char* s) {
    if (!s || !*s) return std::string();
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 0) return std::string(s);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    int m = WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (m <= 0) return std::string(s);
    std::string a((size_t)m, '\0');
    WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, &a[0], m, nullptr, nullptr);
    a.resize((size_t)m - 1);   // 去掉结尾 '\0'
    return a;
}

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
    const std::string engineDirA = utf8ToAcp(engineDir);
    const std::string rawInA = utf8ToAcp(rawIn);
    const std::string rawOutA = utf8ToAcp(rawOut);
    if (!engineDirA.empty()) {
        _putenv_s("NAMA_ENGINE_DIR", engineDirA.c_str());
    }

    FILE* f = nullptr;
    if (fopen_s(&f, rawInA.c_str(), "rb") != 0 || !f) {
        setLastErr(msgBuf, msgCap, "打不开输入文件：%s", rawInA.c_str());
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

    if (fopen_s(&f, rawOutA.c_str(), "wb") != 0 || !f) {
        setLastErr(msgBuf, msgCap, "写不了输出文件：%s", rawOutA.c_str());
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
