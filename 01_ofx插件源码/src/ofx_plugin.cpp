// ofx_plugin.cpp —— DaVinci Beauty 的 OpenFX 插件层。
//
// 结构照官方 Examples/Invert/invert.cpp 的最小骨架，参数部分照
// Examples/Basic/basic.cpp。所有 OpenFX 幂等常量与 suite 签名都来自官方头文件，
// 没有手抄 magic number。
//
// 分工：这一层只负责「跟 Resolve 对话」，真正的美颜在 nama_engine.cpp 里。
// 两者不共享任何 OpenFX 类型，所以引擎层可以被自测独立驱动。
//
// 三件事在这一层特别讲究：
//   1. 自带日志（ofxLog）。插件跑在宿主进程里，出问题时没有日志就只能靠猜。
//   2. 每帧零堆分配。像素缓冲按线程复用，否则 1080p 每帧要重分配 16MB。
//   3. 像素格式转换走 AVX2。这是整条链路上唯一由我们完全掌控、也最耗时的一段。

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <immintrin.h>

#include "ofxImageEffect.h"
#include "ofxMemory.h"
#include "ofxMessage.h"
#include "ofxMultiThread.h"
#include "ofxParam.h"
#include "ofxPixels.h"
#include "nama_engine.h"

// ---------------------------------------------------------------------------
// 宿主席位
// ---------------------------------------------------------------------------
static OfxHost* gHost = nullptr;
static OfxImageEffectSuiteV1* gEffectHost = nullptr;
static OfxPropertySuiteV1* gPropHost = nullptr;
static OfxParameterSuiteV1* gParamHost = nullptr;
static OfxMessageSuiteV1* gMsgHost = nullptr;

// ---------------------------------------------------------------------------
// 自带日志
//
// 落在 %ProgramData%\DaVinciBeauty\logs\ofx.log。设 NAMA_OFX_LOG 可改路径，
// 设成 0 可整体关闭。
//
// 只对前若干帧做详细记录，之后按间隔抽样，避免正常播放时把磁盘写满。
// ---------------------------------------------------------------------------
#define NAMA_LOG_DEFAULT "C:\\ProgramData\\DaVinciBeauty\\logs\\ofx.log"
#define NAMA_LOG_MAX_BYTES (4 * 1024 * 1024)

// ---------------------------------------------------------------------------
// 手写的 clip 属性名前缀
//
// 规范（ofxImageEffect.h 第 381–386 行）：下面这一类属性都是
//   「special props all have the clip name postpended after "_"」，
// 即 <前缀><clip 名>，但头文件里只给了注释、**没有 #define**。
// 所以只能手抄。这些字符串绝不能改 —— 改一个字母宿主就收不到。
// ---------------------------------------------------------------------------
#define NAMA_CLIP_PROP_DEPTH_PREFIX "OfxImageClipPropDepth_"
#define NAMA_CLIP_PROP_COMPONENTS_PREFIX "OfxImageClipPropComponents_"
#define NAMA_CLIP_PROP_PAR_PREFIX "OfxImageClipPropPAR_"

static CRITICAL_SECTION g_logCs;
static INIT_ONCE g_logCsOnce = INIT_ONCE_STATIC_INIT;
static volatile LONG g_renderCount = 0;

// 初始化回调：只在 g_logCsOnce 第一次被触发时执行一次。
static BOOL CALLBACK ofxLogInitCs(PINIT_ONCE /*once*/, PVOID /*param*/, PVOID* /*ctx*/) {
    InitializeCriticalSectionAndSpinCount(&g_logCs, 0x400);
    return TRUE;
}

// 旧实现用「CAS 置位后再 InitializeCriticalSection」，在「已置位、未初始化」的窗口里
// 另一个线程会 EnterCriticalSection 一个尚未初始化的临界区 → 未定义行为（旧报告的 A3）。
// 改成 INIT_ONCE：保证初始化完整完成后，别的线程才会继续往下走。
static void ofxLogEnsureCs() {
    InitOnceExecuteOnce(&g_logCsOnce, ofxLogInitCs, nullptr, nullptr);
}

// 逐级创建目录。
//
// 【v2.0.1 修的真 bug】旧实现只有一句 CreateDirectoryA(父目录)，而它**只建一级**。
// 装机后 C:\ProgramData\DaVinciBeauty 本身就不存在，于是 C:\ProgramData\DaVinciBeauty\logs
// 永远建不出来，fopen 失败，每一条日志都被静默丢掉 —— Resolve 里跑了一整天，
// 磁盘上一个 ofx.log 都没有，而这期间所有「出错就看日志」的排查其实都是空的。
// 更糟的是旧代码里 `static bool dirOk` 无论成败都置 true，等于第一次失败就永久放弃。
//
// 现在按分隔符逐段建，并且只在**确认目录真的存在**后才置位缓存。
static void ensureDirRecursive(const std::string& dir) {
    if (dir.empty()) return;
    std::string cur;
    cur.reserve(dir.size());
    for (size_t i = 0; i < dir.size(); i++) {
        const char c = dir[i];
        const bool sep = (c == '\\' || c == '/');
        cur.push_back(c);
        if (!sep && i + 1 != dir.size()) continue;
        std::string p = cur;
        while (!p.empty() && (p.back() == '\\' || p.back() == '/')) p.pop_back();
        // "C:" 这种纯驱动器前缀不是目录，建不了也没必要建
        const bool driveOnly = (p.size() == 2 && p[1] == ':');
        if (!driveOnly && !p.empty()) CreateDirectoryA(p.c_str(), nullptr);
    }
}

static void ofxLog(const char* fmt, ...) {
    const char* env = getenv("NAMA_OFX_LOG");
    if (env && env[0] == '0' && env[1] == 0) return;

    ofxLogEnsureCs();
    EnterCriticalSection(&g_logCs);

    std::string path = (env && *env) ? std::string(env) : std::string(NAMA_LOG_DEFAULT);
    size_t slash = path.find_last_of("\\/");
    if (slash != std::string::npos) {
        static std::string dirDone;
        const std::string dir = path.substr(0, slash);
        if (dir != dirDone) {
            ensureDirRecursive(dir);
            // 确认建成才缓存，否则下次继续试（旧代码在这里无条件置位）
            const DWORD at = GetFileAttributesA(dir.c_str());
            if (at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY))
                dirDone = dir;
        }
    }

    // 超过上限就整file重来，不做复杂的轮转
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fad)) {
        LARGE_INTEGER sz;
        sz.LowPart = fad.nFileSizeLow;
        sz.HighPart = (LONG)fad.nFileSizeHigh;
        if (sz.QuadPart > NAMA_LOG_MAX_BYTES) DeleteFileA(path.c_str());
    }

    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "a") == 0 && f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "%02d:%02d:%02d.%03d [T%05lu] ", st.wHour, st.wMinute, st.wSecond,
                st.wMilliseconds, (unsigned long)GetCurrentThreadId());
        va_list a;
        va_start(a, fmt);
        vfprintf(f, fmt, a);
        va_end(a);
        fputc('\n', f);
        fclose(f);
    }

    LeaveCriticalSection(&g_logCs);
}

// ---------------------------------------------------------------------------
// 参数元数据：(内部名, 中文标签, 默认值, 提示)
// ---------------------------------------------------------------------------
struct ParamDef {
    const char* name;
    const char* label;
    double dflt;
    const char* hint;
    // true → 设 OfxParamPropSecret，宿主界面不显示，但参数照常存在、照常读写。
    // 用来下线「实测无效但接线完好」的滑块：保留底层，随时一个字段改回来。
    bool secret;
    // 所属参数页。"Main" = 主「美颜」页；"Shape" = v2.2.0 拆出来的「美型」页。
    // nullptr 视同 "Main"。
    //
    // 为什么要拆页（用户反馈「UI 选项太多」）：面板一度是 14 个滑块平铺，
    // 而美型三项是几何形变，与「磨皮/美白」这类像素级效果性质完全不同 ——
    // 它会改变五官位置、承担额外的第二遍 SDK 渲染开销。混在一起既难找，
    // 也容易让人手滑推一下就把脸拉变形。
    const char* page;
};

// 首次拖到片段上的默认值（v2.0.3 起）：只开「磨皮 62」，其余十项 0。
// v2.2.3 起改为「磨皮 50 / 美白 30 / 红润 30」，其余十一项 0 —— 用户侧决策。
// 以前是 70 / 美白 15 / 红润 30，用户一拖上去就是重妆感，还得先逐项归零。
//
// v2.0 把磨皮默认值从 20 提到 50：v1.51 的滑块在 10 档就接近饱和（实测
// mad 10 档 0.167 / 100 档 0.207，只差 1.24 倍），而 v2.0 换成了线性曲线
// （10 档 0.014 / 100 档 0.208）。同一个「20」在两版里的实际强度差约 5 倍，
// 沿用 20 会让用户第一眼以为插件没生效。50 档对应 mad 0.087，是肉眼可辨
// 的轻磨皮，也是 OBS 侧标定的常用档（纹理保留约 86%）。
//
// ⚠ 顺序必须与 nama_engine.h 的 P_* 枚举逐一对应：readParams() 按下标写进
// NamaSettings::panel[]，顺序错一位就会「拖 A 滑块、B 效果变」。编译期只能保证
// 元素个数不多不少，保证不了顺序 —— 改这里务必同时看枚举。
static const ParamDef kParams[P_COUNT] = {
    // v2.0.3 曾把默认值从 50 提到 62，依据是物理摄像头 GT：第三方默认 blur=80
    //   对应皮肤区噪声 81.4%，我方 50 是 85.9%、75 是 76.9%，插值到 81.4% 落在
    //   62.5 档，取 62 —— 那是当时唯一有 GT 支撑的默认值调整，且不改引擎曲线。
    //
    // v2.2.3 用户明确要求改回「磨皮 50 / 美白 30 / 红润 30」（产品侧的观感取舍：
    //   62 的磨皮单独看偏重，配合美白/红润一起给，整组下来的观感由这三个值共同
    //   决定）。注意：这是**覆盖 GT 结论的用户决策**，不是新测出来的数据。
    //   回退方式：改回 62 / 0 / 0 即可，引擎曲线未动，GT 依据仍记录在案的上面几行。
    {"blur_level", "磨皮", 50, "磨皮强度。默认 50（轻磨皮，用户侧标定档）。"
     "本版档位是线性的：1.x 的 20 约相当于本版的 70~80，想要更强请往 80~95 推。", false, "Main"},
    {"clarity", "清晰", 0, "清晰度。结构增强：只放大 3~7px 的中频轮廓，不放大毛孔噪点。"
     "v2.0.2 起独立生效（此前必须先把「锐化」推起来它才有反应）。", false, "Main"},
    {"color_level", "美白", 30, "美白。只提亮肤色，无脸帧逐字节不动。默认 30。", false, "Main"},
    {"red_level", "红润", 30, "红润。实测满量程改动 58% 像素。默认 30。", false, "Main"},
    // 已隐藏（secret）。两个独立原因，详见 02_测试夹具beauty_probe/锐化无效排查报告.md：
    //   1) SDK 自身行为：磨皮 > 0 时锐化对输出的贡献恒为 0（字节级确证 0/252160）；
    //   2) 1.51 起语义掩码把锐化改动撤掉 93%（锐化增强的是眼睛/眉毛/嘴唇/发丝，
    //      恰好在皮肤掩码外）。想要锐利感请用「清晰」——它不受磨皮影响。
    {"sharpen", "锐化", 0, "已停用：磨皮大于 0 时完全无效；需要锐利感请用「清晰」。", true, "Main"},
    // v2.0 新增：把「锐化」同一条细节回填曲线扩展到皮肤掩码之外。0 = 脸外逐字节不变，
    // 这是 OBS 版 3.0.13 为「只动脸」这条硬指标做的设计，默认必须保持 0。
    {"global_sharpen", "全局锐化", 0,
     "把细节回填扩展到皮肤区域之外。0 表示脸外逐字节不变（默认）；"
     "调大后背景也会变锐，适合需要整体质感时使用。", false, "Main"},
    {"face_threed", "五官立体", 0, "五官立体。实测满量程改动 16% 像素。", false, "Main"},
    {"eye_bright", "亮眼", 0, "亮眼。实测很弱，满量程只改动 0.65% 像素，需放大才看得出。", false, "Main"},
    {"remove_pouch_strength", "去黑眼圈", 0, "去黑眼圈。实测很弱，满量程只改动 0.93% 像素。", false, "Main"},
    // v2.0 新增两项：OBS 版实测 SDK 侧确实生效的参数，v1.51 从未接过。
    {"remove_nasolabial_folds_strength", "去法令纹", 0,
     "去法令纹。SDK 侧生效；强度过大时鼻翼两侧可能出现涂抹感，建议 0~40。", false, "Main"},
    // 隐藏（secret）：v2.0 回移后实测在「只开磨皮」的调用链上**逐字节无变化**
    // （blur=0 与 blur=50 两种条件、delspot=50/100 都试过，改动像素 0）。
    // OBS 侧 3.0.16 记录的效果量 +9.4% 是在「同时带 sharpen=4」的那一组测出来的，
    // 而磨皮开启时本项目根本不下发 sharpen ⇒ 那个增益很可能来自 sharpen 而非它。
    // 因此不删接线（万一特定素材/组合下有效），但从面板隐藏，避免给用户一个
    // 拖了没反应的滑块。这与 v1.51 处理「锐化」的方式一致。
    {"delspot_level", "祛斑痘", 0,
     "已停用：实测在当前调用链上无效果（改动像素为 0），详见源码注释。", true, "Main"},
    // ---- 2.1.0 美型三项（几何形变）----------------------------------------
    // 数值是「改动像素比例」，来自 OBS 3.0.19 侧对 166 个 SDK 参数的逐项实测
    // （见 05_文档/一级入口说明.md 第十六节）。只挑正交的三项：脸宽、眼大小、
    // 下庭。同族的窄脸 38.9% / V脸 28.3% / 小脸 29.6% / 短脸 18.0% 都作用于
    // 同一块下半脸像素，同时推会互相打架，所以不做成滑块。
    //
    // ⚠️ 形变是像素位移，与本插件「只在皮肤掩码内动手、掩码外逐字节不动」的
    // 承诺有冲突，引擎侧为此加了两处处理（见 nama_engine.cpp 的 shapeOn 分支）：
    //   1) 磨皮强度不再用 CPU 混合（那会把形变稀释成 t 倍并留双线）；
    //   2) 掩码与「不动基准」都改用形变后的帧，否则掩码错位 + 鬼影。
    {"cheek_thinning", "瘦脸", 0,
     "脸宽收窄。实测满量程改动 36.5% 像素，是三项里最明显的。"
     "开启后磨皮强度改由 SDK 承担，档位手感与关闭美型时略有不同。", false, "Shape"},
    {"eye_enlarging", "大眼", 0,
     "眼睛放大。实测满量程改动 2.6% 像素（眼睛本身占画面小），"
     "效果局部但准确；数值越大越明显，建议 30~70。", false, "Shape"},
    {"intensity_chin", "下巴", 0,
     "下巴长度/形状微调。实测满量程改动 2.7% 像素。"
     "与「瘦脸」作用在不同区域，可以同时使用。", false, "Shape"},
};

static const char* kBlurTypeChoices[] = {"双边滤波", "高斯", "弱磨皮"};

// v2.0 新增：磨皮模式四档。这一项**不是 SDK 参数**（OBS 侧做过 14 组实验，
// heavy_blur / *_smooth 系列输出逐位相同，已证伪），而是引擎自己的 CPU 后处理：
// 按结构自适应决定平坦皮肤与边缘轮廓各回填多少细节。
// 索引直接对应 NamaSettings::smooth_mode，不要改动顺序。
static const char* kSmoothModeChoices[] = {
    "人像磨皮",
    "清晰磨皮",
    "精细磨皮",
    // 索引 3 在 OBS 侧叫「朦胧磨皮（细频整体压低）」，v2.0.1 改名。
    // 原因：这个名字与实际行为方向相反。kTexModes[3] = {140,0,0} 的含义是
    // 「按面板锐化档位的 55% 回填细频」，不是「压低压低」——本引擎没有负回填
    // （hf 取自原图，减它只会把边缘挖穿），所以它做不到「比纯磨皮更糊」。
    // 实测（640x394 人像帧，磨皮 50，相对原图）：四档细频平坦区
    //   模式0 92.3% / 模式1 94.3% / 模式2 96.8% / 模式3 96.7%
    // 即模式 3 是「细节留得最多」的一档，故改名。
    "轻磨皮",
};

// 每个 instance 缓存的参数句柄
struct InstanceData {
    OfxParamHandle p[P_COUNT];
    OfxParamHandle blurType;
    OfxParamHandle skinDetect;
    // v2.0：磨皮模式四档（0 人像 / 1 清晰 / 2 精细 / 3 朦胧）
    OfxParamHandle smoothMode;
    // 本实例在引擎里独占的 SDK 道具槽位（>=0 有效）。见 NamaEngine::acquireSlot。
    int slot = -1;
    // 本实例是否已经把「引擎不可用」的报错抛给宿主了。
    // 以前这是函数级 static，跨实例共享且永不重置 —— 第一个实例报过一次之后，
    // 后面所有实例（哪怕在不同片段上）都静默失败，用户完全看不到提示。
    bool warned = false;
    // v2.0.1：输入像素编码自检只在首帧做一次，结果进日志（见 probeInputEncoding）
    bool probedInput = false;
};

// ---------------------------------------------------------------------------
// 取/存 instance 私有数据
// ---------------------------------------------------------------------------
static InstanceData* getData(OfxImageEffectHandle instance) {
    OfxPropertySetHandle props = nullptr;
    gEffectHost->getPropertySet(instance, &props);
    if (!props) return nullptr;
    void* p = nullptr;
    gPropHost->propGetPointer(props, kOfxPropInstanceData, 0, &p);
    return (InstanceData*)p;
}

// ---------------------------------------------------------------------------
// 参数读取
// ---------------------------------------------------------------------------
static void readParams(InstanceData* d, NamaSettings& s) {
    for (int i = 0; i < P_COUNT; i++) {
        // v3.0.3：句柄取不到时不能默默给 0。
        // 以前这里 `v` 初值 0、句柄为空就跳过 —— 结果是「面板参数全 0」，
        // 引擎走「逐字节透传」分支（sdk 耗时 ~1ms），用户看到的现象是
        // 「插件完全没效果」，而日志里看不出任何异常。改成回落到声明默认值，
        // 至少保持与面板显示一致的观感。
        double v = kParams[i].dflt;
        if (d && d->p[i] && gParamHost->paramGetValue(d->p[i], &v) != kOfxStatOK)
            v = kParams[i].dflt;
        s.panel[i] = v;
    }
    if (d && d->blurType) {
        int c = 0;
        gParamHost->paramGetValue(d->blurType, &c);
        // Choice 索引对应昵称；SDK 的 blur_type：1=双边滤波，0=高斯，2=弱磨皮
        static const int map[3] = {1, 0, 2};
        s.blur_type = (c >= 0 && c < 3) ? map[c] : 1;
    }
    if (d && d->skinDetect) {
        int b = 0;
        gParamHost->paramGetValue(d->skinDetect, &b);
        s.skin_detect = b ? 1 : 0;
    }
    // v2.0：磨皮模式。取不到句柄时保持 NamaSettings 的默认 0（人像磨皮），
    // 这样老工程（1.x 存的、没有这个参数）打开后观感不变。
    if (d && d->smoothMode) {
        int m = 0;
        gParamHost->paramGetValue(d->smoothMode, &m);
        s.smooth_mode = (m >= 0 && m <= 3) ? m : 0;
    }
}

// ---------------------------------------------------------------------------
// 像素格式转换
//
// NamaSDK 只吃 RGBA8。宿主给 float32 时，每帧要在两种格式间搬 2×8.3M 个浮点
// （1080p）。这段代码是整条链路上唯一完全由我们掌控的耗时点，所以用 AVX2 写。
//
// 注意：这些函数体里不能出现需要析构的 C++ 对象，否则 MSVC 的 __try 会报 C2712。
// ---------------------------------------------------------------------------
static inline bool isFloatDepth(const char* d) {
    return d && strcmp(d, kOfxBitDepthFloat) == 0;
}

// ---------------------------------------------------------------------------
// 输入像素自检（v2.0.1）
//
// 为什么需要：引擎把输入一律当成「8bit 非线性编码像素」——f32_to_u8_row 是纯
// v*255 缩放，**不做伽马解码**。这在 DaVinci YRGB（非 Color Managed）+ Rec.709
// 时间线（2026-10-26 实测的配置）下是正确的。
//
// 但如果宿主改送 scene-linear（Color Managed / ACES 项目里 OFX 常收到线性
// float），v*255 会把 18% 灰映射成 46/255 而不是 105/255：画面整体压暗、人脸
// 检测漏检、美白提亮比例失真 —— 而且**不报任何错**，用户只看到「效果变弱了」。
//
// 这里只做观测：每实例首帧采一次亮度分布并写日志。中位 < 55/255 基本可以断定
// 是线性域（非线性编码的画面极少这么暗）。不做自动补偿 —— 补偿需要确切的传递
// 函数，猜错比不补偿更糟。
// ---------------------------------------------------------------------------
static void probeInputEncoding(const void* src, int rowBytes, int w, int h, bool isFloat) {
    if (!src || w < 8 || h < 8 || rowBytes <= 0) return;
    int hist[256] = {0};
    long long total = 0;
    for (int y = 0; y < h; y += 8) {
        const unsigned char* row =
            (const unsigned char*)src + (size_t)y * (size_t)rowBytes;
        for (int x = 0; x < w; x += 16) {
            int r, g, b;
            if (isFloat) {
                const float* p = (const float*)row + (size_t)x * 4;
                r = (int)(p[2] * 255.0f + 0.5f);
                g = (int)(p[1] * 255.0f + 0.5f);
                b = (int)(p[0] * 255.0f + 0.5f);
                r = r < 0 ? 0 : (r > 255 ? 255 : r);
                g = g < 0 ? 0 : (g > 255 ? 255 : g);
                b = b < 0 ? 0 : (b > 255 ? 255 : b);
            } else {
                const unsigned char* p = row + (size_t)x * 4;
                b = p[0]; g = p[1]; r = p[2];
            }
            hist[(77 * r + 150 * g + 29 * b) >> 8]++;
            total++;
        }
    }
    if (total <= 0) return;
    // 排除纯黑像素再求中位。
    // 第一版直接用全帧中位，结果第一张测试帧就假阳性：那张图 53.9% 是黑遮幅，
    // 中位恒为 0，而它的非黑像素中位是 120（完全正常的 Rec.709 画面）。
    // 黑边、遮幅、黑底字幕这些在真实素材里很常见，必须排除。
    long long nonzero = 0;
    for (int i = 8; i < 256; i++) nonzero += hist[i];
    if (nonzero * 100 < total * 10) {
        ofxLog("inputProbe 有效像素不足 10%%（画面几乎全黑），跳过编码自检");
        return;
    }
    const long long k1 = total / 100, k50 = nonzero / 2, k99 = total * 99 / 100;
    int median = -1, lo1 = -1, hi99 = -1;
    long long acc = 0, accNz = 0;
    for (int i = 0; i < 256; i++) {
        acc += hist[i];
        if (i >= 8) accNz += hist[i];
        if (lo1 < 0 && acc >= k1) lo1 = i;
        if (median < 0 && accNz >= k50) median = i;
        if (acc >= k99) { hi99 = i; break; }
    }
    if (median < 55)
        ofxLog("inputProbe 中位=%d/255 黑1%%=%d 亮99%%=%d —— WARN 疑似 scene-linear 输入。"
               "本引擎按非线性编码像素处理，线性输入会导致人脸检测漏检、美白比例失真。"
               "请把时间线色彩空间设为 Rec.709/sRGB，或使用非 Color Managed 的 DaVinci YRGB",
               median, lo1, hi99);
    else
        ofxLog("inputProbe 中位=%d/255 黑1%%=%d 亮99%%=%d（非线性编码，正常范围）",
               median, lo1, hi99);
}

// float32 → uint8：乘 255、加 0.5、夹到 [0,255]、截断。
//
// NaN 处理：AVX2 分支靠 maxps/minps「任一操作数为 NaN 时返回第二操作数」的语义，
// 让 NaN 落到 0。标量分支以前依赖「(int)NaN 在 MSVC 上得 INT_MIN」这一**未定义
// 行为**，结果虽然凑巧也是 0，但换编译器/换优化档就可能变。这里改成显式判断，
// 与 AVX2 分支的结果严格对齐。
static inline void f32_to_u8_row(const float* src, unsigned char* dst, int n) {
    int i = 0;
#if defined(__AVX2__)
    const __m256 mul = _mm256_set1_ps(255.0f);
    const __m256 half = _mm256_set1_ps(0.5f);
    const __m256 lo = _mm256_setzero_ps();
    const __m256 hi = _mm256_set1_ps(255.0f);
    for (; i + 8 <= n; i += 8) {
        __m256 v = _mm256_loadu_ps(src + i);
        v = _mm256_mul_ps(v, mul);
        v = _mm256_add_ps(v, half);
        v = _mm256_max_ps(v, lo);
        v = _mm256_min_ps(v, hi);
        __m256i iv = _mm256_cvttps_epi32(v);
        __m128i p16 = _mm_packus_epi32(_mm256_castsi256_si128(iv),
                                       _mm256_extracti128_si256(iv, 1));
        __m128i p8 = _mm_packus_epi16(p16, p16);
        _mm_storel_epi64((__m128i*)(dst + i), p8);
    }
#endif
    for (; i < n; i++) {
        float v = src[i] * 255.0f + 0.5f;
        // 显式挡住 NaN 与负数（NaN 的所有比较都为 false，所以先判 !(v >= 0)）。
        if (!(v >= 0.0f)) v = 0.0f;
        else if (v > 255.0f) v = 255.0f;
        dst[i] = (unsigned char)(int)v;
    }
}

// uint8 → float32
static inline void u8_to_f32_row(const unsigned char* src, float* dst, int n) {
    int i = 0;
#if defined(__AVX2__)
    const __m256 inv = _mm256_set1_ps(1.0f / 255.0f);
    for (; i + 8 <= n; i += 8) {
        __m128i b = _mm_loadl_epi64((const __m128i*)(src + i));
        __m256 f = _mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(b));
        _mm256_storeu_ps(dst + i, _mm256_mul_ps(f, inv));
    }
#endif
    for (; i < n; i++) dst[i] = (float)src[i] / 255.0f;
}

// 行列拷贝/转换：源与目标各自按自己的 rowBytes 走，互不假设对方布局。
static void blitFrame(const char* srcDepth, const void* srcPtr, int srcRowBytes,
                      const char* dstDepth, void* dstPtr, int dstRowBytes,
                      int w, int h) {
    const bool sf = isFloatDepth(srcDepth);
    const bool df = isFloatDepth(dstDepth);
    const int perPx = sf ? 16 : 4;   // RGBA float32 = 16 字节/像素
    const int rowBytesNeed = w * perPx;
    const int dstPerPx = df ? 16 : 4;
    const int dstRowBytesNeed = w * dstPerPx;

    if (sf == df) {
        if (srcRowBytes == rowBytesNeed && dstRowBytes == dstRowBytesNeed) {
            memcpy(dstPtr, srcPtr, (size_t)rowBytesNeed * (size_t)h);
            return;
        }
        for (int y = 0; y < h; y++)
            memcpy((unsigned char*)dstPtr + (size_t)y * dstRowBytes,
                   (const unsigned char*)srcPtr + (size_t)y * srcRowBytes,
                   (size_t)rowBytesNeed);
        return;
    }

    if (sf && !df) {   // float → byte
        for (int y = 0; y < h; y++)
            f32_to_u8_row((const float*)((const unsigned char*)srcPtr + (size_t)y * srcRowBytes),
                          (unsigned char*)dstPtr + (size_t)y * dstRowBytes, w * 4);
    } else {           // byte → float
        for (int y = 0; y < h; y++)
            u8_to_f32_row((const unsigned char*)srcPtr + (size_t)y * srcRowBytes,
                          (float*)((unsigned char*)dstPtr + (size_t)y * dstRowBytes), w * 4);
    }
}

// 清掉目标图像里「主处理区域之外」的剩余部分，杜绝未初始化显存造成的边缘花屏。
//
// 宿主给的 src / dst bounds 在正常情况下一致；一旦不一致，主处理路径只会写
// min(w,dw) × min(h,dh) 那一块，目标剩余区域就保留旧内存（旧报告的 B8）。
// 这里用「复制相邻已处理像素」的方式补满：先补右侧多出的列，再补下方多出的行，
// 保证每一个目标像素都有确定的值（而不是垃圾）。
static void edgeFillDst(const char* dstDepth, void* dstPtr, int dstRowBytes,
                        int dw, int dh, int w, int h) {
    if (!dstPtr || dstRowBytes <= 0) return;
    if (w <= 0 || h <= 0) return;
    if (dw <= w && dh <= h) return;          // 没有剩余区域

    const bool df = isFloatDepth(dstDepth);
    const int perPx = df ? 16 : 4;
    unsigned char* base = (unsigned char*)dstPtr;

    // 1) 已处理的 h 行里，把第 w 列起的像素用第 w-1 列复制
    if (dw > w) {
        for (int y = 0; y < h; y++) {
            unsigned char* row = base + (size_t)y * dstRowBytes;
            if (df) {
                const float* srcpx = (const float*)(row + (size_t)(w - 1) * 16);
                for (int x = w; x < dw; x++) {
                    float* d = (float*)(row + (size_t)x * 16);
                    d[0] = srcpx[0]; d[1] = srcpx[1]; d[2] = srcpx[2]; d[3] = srcpx[3];
                }
            } else {
                const unsigned char* srcpx = row + (size_t)(w - 1) * 4;
                for (int x = w; x < dw; x++) {
                    unsigned char* d = row + (size_t)x * 4;
                    d[0] = srcpx[0]; d[1] = srcpx[1]; d[2] = srcpx[2]; d[3] = srcpx[3];
                }
            }
        }
    }

    // 2) 第 h 行起，整行用第 h-1 行复制
    if (dh > h) {
        const unsigned char* srcRow = base + (size_t)(h - 1) * dstRowBytes;
        const size_t rowBytes = (size_t)dw * perPx;
        for (int y = h; y < dh; y++)
            memcpy(base + (size_t)y * dstRowBytes, srcRow, rowBytes);
    }
}


//
// 这里刻意不用 `thread_local std::vector`：那是「带析构函数的线程局部对象」，
// 一旦宿主 FreeLibrary 之后还有线程退出，DLL 里的析构函数指针就悬空了。
// 改成 __declspec(thread) 存一个裸指针、指向刻意不释放的分配 —— 进程退出时
// 由操作系统统一回收，代价是每个渲染线程常驻约 16MB（1080p）。
struct Scratch {
    unsigned char* in = nullptr;
    unsigned char* out = nullptr;
    size_t cap = 0;

    void ensure(size_t n) {
        if (cap >= n) return;
        unsigned char* a = (unsigned char*)malloc(n);
        unsigned char* b = (unsigned char*)malloc(n);
        if (!a || !b) { free(a); free(b); return; }   // 保持旧缓冲，下帧再试
        free(in);
        free(out);
        in = a;
        out = b;
        cap = n;
    }
};

__declspec(thread) Scratch* t_scratch = nullptr;

static Scratch* scratch() {
    if (!t_scratch) t_scratch = new Scratch();   // 故意泄漏，不与 DLL 生命周期赛跑
    return t_scratch;
}

// ---------------------------------------------------------------------------
// describe：告诉宿主这个插件是什么
// ---------------------------------------------------------------------------
static OfxStatus describeMain(OfxImageEffectHandle effect) {
    OfxPropertySetHandle props = nullptr;
    gEffectHost->getPropertySet(effect, &props);
    if (!props) return kOfxStatFailed;

    gPropHost->propSetString(props, kOfxPropLabel, 0, "DaVinci Beauty 美颜");
    gPropHost->propSetString(props, kOfxImageEffectPluginPropGrouping, 0, "DaVinci Beauty");
    gPropHost->propSetString(props, kOfxPropShortLabel, 0, "DaVinci Beauty");
    gPropHost->propSetString(props, kOfxPropLongLabel, 0, "DaVinci Beauty");
    gPropHost->propSetString(props, kOfxImageEffectPluginPropSingleInstance, 0, "false");

    // 能被当作滤镜（Edit / Color 页面）使用
    gPropHost->propSetString(props, kOfxImageEffectPropSupportedContexts, 0,
                             kOfxImageEffectContextFilter);
    gPropHost->propSetString(props, kOfxImageEffectPropSupportedContexts, 1,
                             kOfxImageEffectContextGeneral);

    // 同时接受 8 位与浮点：NamaSDK 内部按 8 位 RGBA 处理，浮点会先降采样
    gPropHost->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthByte);
    gPropHost->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 1, kOfxBitDepthFloat);

    // 人脸检测需要看整帧画面，所以明确不支持分块渲染
    gPropHost->propSetInt(props, kOfxImageEffectPropSupportsTiles, 0, 0);
    gPropHost->propSetInt(props, kOfxImageEffectPropSupportsMultiResolution, 0, 0);

    // 支持「输入与输出 clip 各自是不同深度」—— 这是 blitFrame 本来就具备的能力
    // （float→byte / byte→float 双向转换都实现了），也是 GetClipPreferences 里
    // 让输出深度跟随输入的前提。
    //
    // 规范依据 ofxImageEffect.h 第 803–816 行：只有该属性为 1，宿主才会允许插件
    // 在 kOfxImageEffectActionGetClipPreferences 里显式设置输出 clip 的像素深度。
    //
    // ⚠️ 命名坑：宏名是 kOfxImageEffectPropSupportsMultipleClipDepths，但它在头文件
    // 里展开成的字符串是 "OfxImageEffectPropMultipleClipDepths"（少了 Supports）——
    // 这是 OFX 官方历史遗留的不一致，千万别「顺手改成对的」。
    gPropHost->propSetInt(props, kOfxImageEffectPropSupportsMultipleClipDepths, 0, 1);

    // 与「多深度」平行的另一把钥匙：只有先声明支持多 PAR，宿主才会允许插件在
    // GetClipPreferences 里显式设置输出 clip 的 PAR（ofxImageEffect.h 第 826-830 行）。
    // 当前 getClipPreferences 已经在写 OfxImageClipPropPAR_Output，缺了这一行那条
    // 设置很可能被合规宿主忽略（旧报告的 A6）。
    gPropHost->propSetInt(props, kOfxImageEffectPropSupportsMultipleClipPARs, 0, 1);

    // 本插件依赖空间信息（人脸检测 + 磨皮），声明「无法在无空间感知下渲染」。
    // 这样宿主在需要无空间感知渲染（例如生成 LUT）时会直接绕过本插件，而不是
    // 让它在缺少空间上下文的情况下出图（ofxImageEffect.h 第 1396-1413 行）。
    gPropHost->propSetString(props, kOfxImageEffectPropNoSpatialAwareness, 0, "false");

    // 渲染线程安全档位 —— 仍然是 Unsafe。
    //
    // 规范里三档的含义（ofxImageEffect.h）：Unsafe = 同一时刻只能有一个 render；
    // InstanceSafe = 不同实例可以并行；FullySafe = 同一实例也可以并行。
    //
    // 现在每个实例已经独占自己的 SDK 道具与参数缓存（NamaEngine 的 slot 池），
    // 所以「实例之间互相覆盖参数」的问题没有了。真正让它无法升到 InstanceSafe 的
    // 是引擎的**物理约束**：NamaSDK 非可重入，全部调用必须走单一 GL 上下文，引擎
    // 内部还有一把全局互斥在串行化。既然宿主串行化与这把锁效果相同（零额外损失），
    // 就老老实实声明 Unsafe，让宿主替我们排好队。
    gPropHost->propSetString(props, kOfxImageEffectPluginRenderThreadSafety, 0,
                             kOfxImageEffectRenderUnsafe);

    ofxLog("describe            label=DaVinci Beauty 美颜 grouping=DaVinci Beauty contexts=Filter,General "
           "depths=Byte,Float par=1 noSpatial=false tiles=0 threadsafe=Unsafe build=" NAMA_BUILD_STR);
    return kOfxStatOK;
}

// ---------------------------------------------------------------------------
// getRegionOfDefinition：输出范围 = 输入源的范围
// ---------------------------------------------------------------------------
static OfxStatus getRegionOfDefinition(OfxImageEffectHandle instance,
                                       OfxPropertySetHandle /*inArgs*/,
                                       OfxPropertySetHandle outArgs) {
    if (!outArgs) return kOfxStatReplyDefault;

    OfxImageClipHandle src = nullptr;
    gEffectHost->clipGetHandle(instance, kOfxImageEffectSimpleSourceClipName, &src, nullptr);

    OfxRectI rod;
    memset(&rod, 0, sizeof(rod));
    bool haveRod = false;
    if (src) {
        OfxPropertySetHandle cp = nullptr;
        gEffectHost->clipGetPropertySet(src, &cp);
        if (cp &&
            gPropHost->propGetIntN(cp, kOfxImageEffectPropRegionOfDefinition, 4, &rod.x1) ==
                kOfxStatOK &&
            rod.x2 > rod.x1 && rod.y2 > rod.y1) {
            haveRod = true;
        }
    }

    // 拿不到合法 RoD（宿主还没算过 / 是零面积）时返回 ReplyDefault，让宿主自己决定。
    // 旧代码在这时会回一个「0,0,0,0 的 RoD」+ kOfxStatOK —— 那等于明确告诉宿主
    // 「输出范围是空的」，某些宿主会把这一段判成黑帧/空帧（旧报告的 A4）。
    if (!haveRod) return kOfxStatReplyDefault;

    gPropHost->propSetIntN(outArgs, kOfxImageEffectPropRegionOfDefinition, 4, &rod.x1);
    return kOfxStatOK;
}

// ---------------------------------------------------------------------------
// describeInContext：声明输入输出 clip 与全部参数
// ---------------------------------------------------------------------------
static OfxStatus describeInContext(OfxImageEffectHandle effect, OfxPropertySetHandle /*inArgs*/) {
    OfxPropertySetHandle props = nullptr;

    // 输出
    gEffectHost->clipDefine(effect, kOfxImageEffectOutputClipName, &props);
    gPropHost->propSetString(props, kOfxImageEffectPropSupportedComponents, 0,
                             kOfxImageComponentRGBA);

    // 输入
    gEffectHost->clipDefine(effect, kOfxImageEffectSimpleSourceClipName, &props);
    gPropHost->propSetString(props, kOfxImageEffectPropSupportedComponents, 0,
                             kOfxImageComponentRGBA);

    OfxParamSetHandle paramSet = nullptr;
    gEffectHost->getParamSet(effect, &paramSet);
    if (!paramSet) return kOfxStatFailed;

    OfxPropertySetHandle pp = nullptr;

    // ---- 参数页必须先定义 ----
    // 以前是把 Main 页放在所有参数**之后**定义、再用 kOfxParamPropPageChild 逐个
    // 追加归类。规范没保证「后置归类」一定生效，部分宿主（含 Resolve）要求页面
    // 先于其子参数存在。这里改成：先建页，然后每个参数在 paramDefine 之后立刻用
    // kOfxParamPropParent 挂到页上 —— 不依赖定义顺序，更可靠。
    OfxParamHandle pageHandle = nullptr;
    const OfxStatus stPage = gParamHost->paramDefine(paramSet, kOfxParamTypePage, "Main", &pp);
    if (stPage == kOfxStatOK) {
        gPropHost->propSetString(pp, kOfxPropLabel, 0, "美颜");
    }
    // v3.0.4：paramDefine 失败不代表页面不可用 —— 有的宿主在 describe 被调两次、
    // 或实例已存在同名页时返回失败码。这里一律再试一次 paramGetHandle 把句柄取回来，
    // 拿到就照常归类（以前 define 失败就整页放弃，参数全散到根级）。
    if (gParamHost->paramGetHandle(paramSet, "Main", &pageHandle, nullptr) != kOfxStatOK)
        pageHandle = nullptr;
    // 页面拿不到时，参数会散落到根级（仍然可用，只是归类失效）。以前这里是静默
    // 降级 —— 出问题时只能靠猜。现在明确记一条日志（旧报告 A8）。
    if (!pageHandle)
        ofxLog("describeInContext WARN 参数页 Main 不可用（define=%d），参数将挂在根级",
               (int)stPage);

    // ---- v2.2.2：独立的「美型」分组（可折叠）---------------------------------
    // 用户诉求：美型功能要能用，只是面板太杂 —— 要「折叠」，不要「隐藏」。
    //
    // v2.2.0 用第二个 OFX Page：Resolve 不渲染 Page，拍平成一列，无效。
    // v2.2.1 改用 Group，但仍把 Group 挂在 Main 页下 —— 嵌套在页内的分组同样不呈现。
    // v2.2.2 的关键修正：**Group 必须留在 root 层级**（不设 kOfxParamPropParent）。
    //   Resolve 确实支持 OFX 分组（第三方商业插件在 inspector 里就有可折叠小节，
    //   并能双击标题栏收起），但只对 root 层级的 Group 生效；页内嵌套的会被降级。
    //   三项美型仍用 Parent 挂到 Shape 之下。
    //   兜底：分组建不出来时三项退回 Main —— 宁可不分组，也不能丢参数。
    OfxParamHandle shapeHandle = nullptr;
    const OfxStatus stShape = gParamHost->paramDefine(paramSet, kOfxParamTypeGroup, "Shape", &pp);
    if (stShape == kOfxStatOK) {
        gPropHost->propSetString(pp, kOfxPropLabel, 0, "美型（几何形变）");
        gPropHost->propSetString(pp, kOfxParamPropHint, 0,
                                 "几何形变类：瘦脸 / 大眼 / 下巴。三项默认均为 0（关闭）。"
                                 "任意一项非 0 时会额外增加一遍 SDK 渲染（1080p 约 +7ms、"
                                 "4K 约 +19ms），且磨皮力度改由 SDK 接管，手感与关闭时略有不同。"
                                 "形变会改变五官位置，建议从小值起步。");
        // 默认收起 —— 用户明确反馈「UI 选项太多」，展开状态只会添乱。
        // ⚠️ 不设 kOfxParamPropParent：Group 必须在 root 层级，Resolve 才渲染。
        //    （v2.2.1 把它挂在 Main 页下，结果是分组被忽略、三项照样平铺。）
        gPropHost->propSetInt(pp, kOfxParamPropGroupOpen, 0, 0);
    }
    // v3.0.4：同 Main 页 —— define 失败也再试一次取句柄，避免「分组没了」。
    if (gParamHost->paramGetHandle(paramSet, "Shape", &shapeHandle, nullptr) != kOfxStatOK)
        shapeHandle = nullptr;
    if (!shapeHandle)
        ofxLog("describeInContext WARN 美型分组 Shape 不可用（define=%d），三项美型将退回主「美颜」页",
               (int)stShape);

    // ---- P_COUNT 个 0..100 的效果滑块（2.1.0 起 14 个）----
    for (int i = 0; i < P_COUNT; i++) {
        if (gParamHost->paramDefine(paramSet, kOfxParamTypeDouble, kParams[i].name, &pp) !=
            kOfxStatOK)
            continue;
        gPropHost->propSetString(pp, kOfxPropLabel, 0, kParams[i].label);
        gPropHost->propSetString(pp, kOfxParamPropHint, 0, kParams[i].hint);
        gPropHost->propSetString(pp, kOfxParamPropScriptName, 0, kParams[i].name);
        gPropHost->propSetString(pp, kOfxParamPropDoubleType, 0, kOfxParamDoubleTypeScale);
        gPropHost->propSetDouble(pp, kOfxParamPropDefault, 0, kParams[i].dflt);
        gPropHost->propSetDouble(pp, kOfxParamPropMin, 0, 0.0);
        gPropHost->propSetDouble(pp, kOfxParamPropMax, 0, 100.0);
        gPropHost->propSetDouble(pp, kOfxParamPropDisplayMin, 0, 0.0);
        gPropHost->propSetDouble(pp, kOfxParamPropDisplayMax, 0, 100.0);
        // v2.2.0：按 ParamDef::page 归类。Shape 页不可用时退回 Main，
        // 保证参数一定存在、一定可读 —— 宁可不分组，也不能丢参数。
        const char* pg = "Main";
        if (kParams[i].page && kParams[i].page[0] &&
            strcmp(kParams[i].page, "Shape") == 0 && shapeHandle)
            pg = "Shape";
        if (pageHandle) gPropHost->propSetString(pp, kOfxParamPropParent, 0, pg);
        // 下线滑块：secret 让宿主完全不显示，但参数仍然存在、仍然可读写，
        // 旧工程里存的非零值也不会丢（Sharpen 在磨皮=0 时确实是有效的）。
        if (kParams[i].secret)
            gPropHost->propSetInt(pp, kOfxParamPropSecret, 0, 1);
    }

    // ---- 磨皮模式（v2.0 新增，四档）----
    // 这一项是引擎的 CPU 后处理，不是 SDK 参数；默认档与 v1.51 观感最接近。
    // 放在「磨皮算法」之前，因为它决定细节回填策略，比滤波算法更影响观感。
    if (gParamHost->paramDefine(paramSet, kOfxParamTypeChoice, "smooth_mode", &pp) == kOfxStatOK) {
        gPropHost->propSetString(pp, kOfxPropLabel, 0, "磨皮模式");
        gPropHost->propSetString(pp, kOfxParamPropScriptName, 0, "smooth_mode");
        gPropHost->propSetString(pp, kOfxParamPropHint, 0,
                                 "四档只在「细节回填策略」上有区别，任何一档都不会把磨皮变成锐化。"
                                 "人像=不额外回填（SDK 原始磨皮质感）；清晰=只回填轮廓、皮肤最干净；"
                                 "精细=轮廓小幅回填且平坦区压得更狠；轻磨皮=均匀回填、最接近原图质感。"
                                 "需要锐度请调「锐化」或「清晰」滑块。");
        for (int i = 0; i < 4; i++)
            gPropHost->propSetString(pp, kOfxParamPropChoiceOption, i, kSmoothModeChoices[i]);
        gPropHost->propSetInt(pp, kOfxParamPropDefault, 0, 0);  // 0 = 人像磨皮
        if (pageHandle) gPropHost->propSetString(pp, kOfxParamPropParent, 0, "Main");
    }

    // ---- 磨皮算法 ----
    if (gParamHost->paramDefine(paramSet, kOfxParamTypeChoice, "blur_type", &pp) == kOfxStatOK) {
        gPropHost->propSetString(pp, kOfxPropLabel, 0, "磨皮算法");
        gPropHost->propSetString(pp, kOfxParamPropScriptName, 0, "blur_type");
        gPropHost->propSetString(pp, kOfxParamPropHint, 0,
                                 "实测细节能量（越小越平滑）：双边滤波 2.358、高斯 2.719、弱磨皮 3.762、不磨皮 4.180。");
        for (int i = 0; i < 3; i++)
            gPropHost->propSetString(pp, kOfxParamPropChoiceOption, i, kBlurTypeChoices[i]);
        gPropHost->propSetInt(pp, kOfxParamPropDefault, 0, 0);  // 0 = 双边滤波
        if (pageHandle) gPropHost->propSetString(pp, kOfxParamPropParent, 0, "Main");
    }

    // ---- 只磨皮肤区域 ----
    // 默认打开：全局磨皮会把人脸以外的背景、衣服、字幕一起磨糊，用户体感就是
    // "画面整体不对劲"。开启后 SDK 只处理检测到的皮肤区域，背景逐字节不动。
    // 实测 44 张真实照片：开启后磨皮影响面积平均减少 20.8 个百分点
    // （-30.6pp ~ -4.3pp），整帧平均亮度最大漂移 0.46/255（无全局亮度问题）。
    // 注意：依赖人脸检测， faces=0 时该开关无效（此时应转去调磨皮算法/强度）。
    if (gParamHost->paramDefine(paramSet, kOfxParamTypeBoolean, "skin_detect", &pp) == kOfxStatOK) {
        gPropHost->propSetString(pp, kOfxPropLabel, 0, "只磨皮肤区域");
        gPropHost->propSetString(pp, kOfxParamPropScriptName, 0, "skin_detect");
        gPropHost->propSetString(pp, kOfxParamPropHint, 0,
                                 "默认开启。开启后只处理检测到的皮肤区域，背景和衣服不动；"
                                 "画面里检测不到人脸时此开关不生效。");
        gPropHost->propSetInt(pp, kOfxParamPropDefault, 0, 1);
        if (pageHandle) gPropHost->propSetString(pp, kOfxParamPropParent, 0, "Main");
    }

    // ---- 版本号（v2.2.3 起，面板底部只读展示）-------------------------------
    // 用户要求：面板上要能直接看到版本号，且**随升级自动改变**。
    //
    // 为什么放在这里 / 为什么是 String 而不是内置 label：
    //   OFX 无法指定「右下角 / 左下角」这类绝对位置 —— Resolve 严格按 paramDefine
    //   的顺序纵向渲染，所以「最后一个定义」就是最靠近面板底部的稳定落点。
    //
    // 为什么它能自动同步：default 直接引用 NAMA_BUILD_STR —— 那是版本号唯一出处
    // （nama_engine.h）。以后每次升级只改宏，这里跟着变，不存在"忘了手动同步"。
    //
    // kOfxParamPropEnabled=0 让它灰显不可编辑，只是个标签。
    if (gParamHost->paramDefine(paramSet, kOfxParamTypeString, "build_version", &pp) == kOfxStatOK) {
        gPropHost->propSetString(pp, kOfxPropLabel, 0, "版本");
        gPropHost->propSetString(pp, kOfxParamPropScriptName, 0, "build_version");
        gPropHost->propSetString(pp, kOfxParamPropHint, 0,
                                 "插件版本号（只读）。随每次升级自动变化，"
                                 "排障/反馈时请一并告知这个值。");
        gPropHost->propSetString(pp, kOfxParamPropDefault, 0, NAMA_BUILD_STR);
        gPropHost->propSetInt(pp, kOfxParamPropEnabled, 0, 0);
        if (pageHandle) gPropHost->propSetString(pp, kOfxParamPropParent, 0, "Main");
    }

    ofxLog("describeInContext   clips=Output,Source params=%d(+page)", P_COUNT + 4);
    return kOfxStatOK;
}

// ---------------------------------------------------------------------------
// GetClipPreferences：让输出 clip 的深度 / 组件 / PAR 跟随输入
//
// 为什么必须实现（ofxImageEffect.h 第 315–325 行）：宿主会在实例创建后、每次输入
// clip 变化时、以及被 ClipPreferencesSlaveParam 指名的参数变化时调用它。不实现，
// 宿主只能按自己的默认策略决定输出深度 —— 而这可能和输入不一致，白白多一次
// float↔byte 往返（实测单程 3.5 ms，来回 7 ms）。
//
// ⚠️ 陷阱一：属性名要手写拼接。规范（第 381–386 行）说这些是
//   「special props all have the clip name postpended after "_"」，
//   即 OfxImageClipPropDepth_<clipname>，但头文件里**没有对应 #define**。
//   所以 "OfxImageClipPropDepth_" 这个前缀只能手写 —— 这是本文件唯一一处
//   手抄的 magic string，写了就绝不能改。
//
// ⚠️ 陷阱二：kOfxImageEffectFrameVarying 必须为 true。我们的引擎内部维护
//   递增的 frameId_，且人脸跟踪带时序状态（人脸模型依赖前后帧），输出第 N 帧
//   依赖之前处理过的帧。声明 false 会让宿主复用缓存输出、跳过 render。
//
// inArgs 在该 action 里是 NULL（规范第 328 行），不要读它。
// 返回值：改了至少一个属性 → kOfxStatOK；什么都没改 → kOfxStatReplyDefault。
// ---------------------------------------------------------------------------
static OfxStatus getClipPreferences(OfxImageEffectHandle instance,
                                    OfxPropertySetHandle /*inArgs*/,
                                    OfxPropertySetHandle outArgs) {
    if (!outArgs) return kOfxStatReplyDefault;

    OfxImageClipHandle src = nullptr;
    gEffectHost->clipGetHandle(instance, kOfxImageEffectSimpleSourceClipName, &src, nullptr);
    if (!src) return kOfxStatReplyDefault;

    OfxPropertySetHandle cp = nullptr;
    gEffectHost->clipGetPropertySet(src, &cp);
    if (!cp) return kOfxStatReplyDefault;

    bool changed = false;

    // 深度：输入是什么，输出就跟着是什么（byte→byte / float→float）。
    // 这正是宿主默认行为之外的「显式表态」，让 Resolve 不必自作主张。
    char* depth = nullptr;
    if (gPropHost->propGetString(cp, kOfxImageEffectPropPixelDepth, 0, &depth) == kOfxStatOK &&
        depth) {
        std::string dp = std::string(NAMA_CLIP_PROP_DEPTH_PREFIX) + kOfxImageEffectOutputClipName;
        if (gPropHost->propSetString(outArgs, dp.c_str(), 0, depth) == kOfxStatOK) changed = true;
    }

    // 组件：输入是 RGBA，输出也保持 RGBA。
    char* comps = nullptr;
    if (gPropHost->propGetString(cp, kOfxImageEffectPropComponents, 0, &comps) == kOfxStatOK &&
        comps) {
        std::string cp2 = std::string(NAMA_CLIP_PROP_COMPONENTS_PREFIX) +
                          kOfxImageEffectOutputClipName;
        if (gPropHost->propSetString(outArgs, cp2.c_str(), 0, comps) == kOfxStatOK) changed = true;
    }

    // 像素宽高比：跟随输入。规范要求必须是正的非零 double。
    // 注意属性名是 kOfxImagePropPixelAspectRatio（不是 ...Effect... 那个，
    // 后者是 host 侧的 kOfxImageEffectPropProjectPixelAspectRatio）。
    double par = 0.0;
    if (gPropHost->propGetDouble(cp, kOfxImagePropPixelAspectRatio, 0, &par) == kOfxStatOK &&
        par > 0.0) {
        std::string pp = std::string(NAMA_CLIP_PROP_PAR_PREFIX) + kOfxImageEffectOutputClipName;
        if (gPropHost->propSetDouble(outArgs, pp.c_str(), 0, par) == kOfxStatOK) changed = true;
    }

    // 时序：输出随帧变化（引擎有内部帧计数与人脸跟踪状态），必须声明为 true。
    if (gPropHost->propSetInt(outArgs, kOfxImageEffectFrameVarying, 0, 1) == kOfxStatOK)
        changed = true;

    // 只在**第一次**应答时记一行。Resolve 在每次 clip 变化 / 开始播放时都会问
    // 一遍，逐次打印会把 ofx.log 冲成同一条消息的复读机，反而盖住真正有用的
    // render 行（v3.0.4）。
    if (changed) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            ofxLog("getClipPreferences 输出深度/组件/PAR 跟随输入，FrameVarying=true（后续不再重复记录）");
        }
    }
    return changed ? kOfxStatOK : kOfxStatReplyDefault;
}

// ---------------------------------------------------------------------------
// instance 生命周期
// ---------------------------------------------------------------------------
static OfxStatus createInstance(OfxImageEffectHandle instance) {
    OfxPropertySetHandle props = nullptr;
    gEffectHost->getPropertySet(instance, &props);
    OfxParamSetHandle paramSet = nullptr;
    gEffectHost->getParamSet(instance, &paramSet);

    InstanceData* d = new InstanceData();
    memset(d->p, 0, sizeof(d->p));
    d->blurType = nullptr;
    d->skinDetect = nullptr;
    d->smoothMode = nullptr;

    // v3.0.3：句柄取不到要显式报出来。
    // 静默失败的后果极隐蔽：readParams 读到全 0 → 引擎逐字节透传 → 用户以为
    // 插件坏了（实测日志里就是 sdk=1.0ms / faces=0 / score=0.00）。
    int hFail = 0;
    char hFailNames[320] = {0};
    for (int i = 0; i < P_COUNT; i++) {
        if (gParamHost->paramGetHandle(paramSet, kParams[i].name, &d->p[i], nullptr) != kOfxStatOK ||
            !d->p[i]) {
            d->p[i] = nullptr;
            hFail++;
            if (strlen(hFailNames) + strlen(kParams[i].name) + 2 < sizeof(hFailNames)) {
                if (hFailNames[0]) strcat_s(hFailNames, sizeof(hFailNames), ",");
                strcat_s(hFailNames, sizeof(hFailNames), kParams[i].name);
            }
        }
    }
    if (hFail > 0)
        ofxLog("createInstance WARN %d/%d 个参数句柄取不到：%s（这些参数按声明默认值兜底）",
               hFail, P_COUNT, hFailNames);
    gParamHost->paramGetHandle(paramSet, "blur_type", &d->blurType, nullptr);
    gParamHost->paramGetHandle(paramSet, "skin_detect", &d->skinDetect, nullptr);
    gParamHost->paramGetHandle(paramSet, "smooth_mode", &d->smoothMode, nullptr);

    gPropHost->propSetPointer(props, kOfxPropInstanceData, 0, (void*)d);

    // 预热引擎 + 为实例申请一个独占槽位。加锁判断在 preheat 内部完成，避免
    // 「无锁读 ready_/poisoned_」的 TOCTOU（旧报告 B4）；槽位让多实例各用各的
    // SDK 道具与参数缓存（旧报告 B5）。
    NamaEngine& engine = NamaEngine::inst();
    std::string err;
    bool didInit = false;
    double t0 = (double)GetTickCount64();
    bool ok = engine.preheat(err, didInit);
    double ms = (double)GetTickCount64() - t0;

    if (didInit) {
        const char* ng = getenv("NAMA_ENGINE_NO_GLLOCK");
        const char* glTxt = (ng && ng[0] == '1') ? "off" : "on";
        ofxLog("createInstance 预热引擎 %s 耗时=%.0fms build=%s ver=%s item=%d ownerTid=%lu gllock=%s %s",
               ok ? "成功" : "失败", ms, NAMA_BUILD_STR,
               engine.version() ? engine.version() : "?",
               engine.itemId(), engine.ownerThread(), glTxt, ok ? "" : err.c_str());
        // 语义区域（自研皮肤分割）单独打一行。它是 v1.5 的核心，出问题时的
        // 第一个问题永远是「区域到底有没有生效」，而这行就是答案：
        //   region=on    → 已加载人脸模型并完成区域推理，效果被限制在皮肤内
        //   region=off   → 退回 v1.4 颜色启发式（原因见 err）
        // faces/cov 是**上一次推理**的快照，刚初始化完还没有掩码，所以可能为 0。
        ofxLog("createInstance 语义区域 region=%s segfaces=%d cov=%.1f%% infer=%.1fms n=%ld %s",
               engine.regionActive() ? "on" : "off",
               engine.regionFaces(), 100.0 * engine.regionCoverage(),
               engine.regionInferMs(), engine.regionInferCount(),
               engine.regionActive() ? "" : engine.regionErr());
        // 预热失败以前只写日志，用户在界面上完全看不到 —— 表现就是「拖上去了但没效果」。
        // 这里补一次宿主报错，让问题可见。
        if (!ok && gMsgHost) {
            d->warned = true;
            gMsgHost->message(instance, kOfxMessageError, "", "%s",
                              err.empty() ? "DaVinci Beauty 引擎预热失败，美颜不会生效。"
                                          : err.c_str());
        }
    } else {
        ofxLog("createInstance 复用已就绪引擎 ok=%d ver=%s %s",
               (int)ok, engine.version() ? engine.version() : "?", err.c_str());
    }

    if (ok) {
        std::string serr;
        d->slot = engine.acquireSlot(serr);
        if (d->slot < 0) {
            ofxLog("createInstance 申请引擎槽位失败：%s", serr.c_str());
            if (gMsgHost && !d->warned) {
                d->warned = true;
                gMsgHost->message(instance, kOfxMessageError, "", "%s",
                                  serr.empty() ? "DaVinci Beauty 引擎不可用，美颜不会生效。"
                                               : serr.c_str());
            }
        } else {
            ofxLog("createInstance 实例槽位 slot=%d", d->slot);
        }
    }
    return kOfxStatOK;
}

static OfxStatus destroyInstance(OfxImageEffectHandle instance) {
    InstanceData* d = getData(instance);
    if (d) {
        if (d->slot >= 0) {
            NamaEngine::inst().releaseSlot(d->slot);
            d->slot = -1;
        }
        delete d;
        OfxPropertySetHandle props = nullptr;
        gEffectHost->getPropertySet(instance, &props);
        if (props) gPropHost->propSetPointer(props, kOfxPropInstanceData, 0, nullptr);
    }
    ofxLog("destroyInstance");
    return kOfxStatOK;
}

// ---------------------------------------------------------------------------
// 代理/缩略图判定
//
// 真实 Resolve 日志（ofx.log）里出现过两类「不值得跑 SDK」的渲染：
//   render #3 time=2884.000 161x80 scale=0.15 faces=0 sdk=17.7ms
//   render #7 time=0.000   504x284 scale=0.47 faces=0 sdk=13.7ms
// 161x80 只有全尺寸的 0.6% 像素，SDK 却比全尺寸还慢，而且这种尺寸下人脸检测
// 必然失败（faces=0）。这些帧来自缩略图 / 代理预览 / 调色台小监看，用户根本
// 看不出美颜效果，白白付出十几毫秒。
//
// 所以低于阈值时直接透传。阈值取得很保守（面积 1/16、线性缩放 1/4），宁可漏放
// 也不误杀 —— 半分辨率预览（scale=0.5，面积 1/4）仍在阈值之上，照常走美颜。
// ---------------------------------------------------------------------------
static const double kProxyScaleMin = 0.25;      // 线性缩放低于此值视为代理帧
static const double kProxyAreaFrac = 0.0625;    // 或面积低于真实 RoD 的 1/16

// 超大帧保护：单个 RGBA8 缓冲 = w*h*4 字节，处理链路上同时存在 3 份（每线程 in/out
// + 引擎 work_）。超过这个像素数就直接透传，避免异常尺寸把宿主内存吃爆。
// 8K（7680×4320 ≈ 33M 像素）仍在此限内，正常工程不受影响。
static const double kMaxProcessPixels = 64.0 * 1024.0 * 1024.0;   // 64M 像素

// 返回 true 表示「这一帧不值得跑 SDK，直接透传」。仅做标量判断，不碰 SDK。
//
// w/h 是本帧实际处理尺寸；refW/refH 是源 clip 的真实 RoD 尺寸（拿不到就传 0）。
static bool isThrowawayRender(OfxPropertySetHandle inArgs, int w, int h,
                              int refW, int refH) {
    // 规范 1.5.1 起提供的显式信号：宿主自认这是 UI 缩略图渲染。
    // ofxImageEffect.h 里明确建议插件可以「turning itself off in IsIdentity」。
    if (inArgs) {
        int thumb = 0;
        if (gPropHost->propGetInt(inArgs, kOfxImageEffectPropThumbnailRender, 0, &thumb) ==
                kOfxStatOK &&
            thumb != 0)
            return true;
    }

    // 线性缩放：宿主明确给了 RenderScale 时以它为准。**一旦宿主给了 scale，就绝不再
    // 叠加面积兜底** —— 否则 160×120 这类正常小画幅（面积 < 32400px）会被误杀，
    // 用户看到「插件挂上去了但完全没效果」（旧报告的 A1）。
    bool haveScale = false;
    if (inArgs) {
        double scale[2] = {1.0, 1.0};
        if (gPropHost->propGetDoubleN(inArgs, kOfxImageEffectPropRenderScale, 2, scale) ==
            kOfxStatOK) {
            haveScale = true;
            if (scale[0] < kProxyScaleMin || scale[1] < kProxyScaleMin) return true;
        }
    }

    // 兜底：只有宿主**没给** scale 时才看面积，且参考面积必须是源 clip 的真实 RoD
    // （不能再写死 960×540 —— 那样 4K/8K 工程的代理帧判定会整体偏移）。
    // 参考面积拿不到（refW/refH<=0）就放弃兜底：宁可多跑一次 SDK，也绝不误杀。
    if (!haveScale && w > 0 && h > 0 && refW > 0 && refH > 0) {
        double frac = ((double)w * (double)h) / ((double)refW * (double)refH);
        if (frac < kProxyAreaFrac) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// IsIdentity：告诉宿主「直接取源 clip 的图像，不用调我渲染」
//
// 满足任一条件即返回恒等：
//   1. 八项全 0 —— 实测 SDK 在八项全 0 且 filter_level 归零时输出与输入差 0.000；
//   2. 这是缩略图 / 代理帧 —— 用户看不出效果，跑 SDK 纯属浪费（见上）。
//
// 规范（ofxImageEffect.h 第 389–448 行）要求：在 outArgs 上设 kOfxPropName
// （用哪个 clip 当输出）与 kOfxPropTime，并返回 kOfxStatOK；不是恒等则返回
// kOfxStatReplyDefault 走正常渲染。
//
// 注意 IsIdentity 的 inArgs 里没有 render window（规范第 438–443 行列出的只有
// OfxPropTime / FieldToRender / RenderWindow / RenderScale / ThumbnailRender），
// 所以这里只按 ThumbnailRender 与 RenderScale 判断，不做面积兜底。
// ---------------------------------------------------------------------------
static OfxStatus isIdentity(OfxImageEffectHandle instance,
                            OfxPropertySetHandle inArgs,
                            OfxPropertySetHandle outArgs) {
    if (!outArgs) return kOfxStatReplyDefault;

    double time = 0.0;
    if (inArgs) gPropHost->propGetDouble(inArgs, kOfxPropTime, 0, &time);

    // 缩略图 / 代理帧：无条件透传，宿主连 render 都不会调用（w=h=0 跳过面积判断）。
    if (isThrowawayRender(inArgs, 0, 0, 0, 0)) {
        gPropHost->propSetString(outArgs, kOfxPropName, 0, kOfxImageEffectSimpleSourceClipName);
        gPropHost->propSetDouble(outArgs, kOfxPropTime, 0, time);
        return kOfxStatOK;
    }

    InstanceData* d = getData(instance);
    NamaSettings s;
    readParams(d, s);
    if (!s.allZero()) return kOfxStatReplyDefault;

    gPropHost->propSetString(outArgs, kOfxPropName, 0, kOfxImageEffectSimpleSourceClipName);
    gPropHost->propSetDouble(outArgs, kOfxPropTime, 0, time);
    return kOfxStatOK;
}

// ---------------------------------------------------------------------------
// render
// ---------------------------------------------------------------------------
static OfxStatus renderMain(OfxImageEffectHandle instance,
                            OfxPropertySetHandle inArgs,
                            OfxPropertySetHandle /*outArgs*/) {
    LONG seq = InterlockedIncrement(&g_renderCount);
    // 默认只详细记录前若干帧，之后抽样；NAMA_OFX_LOG_ALL=1 则逐帧记录。
    // 抽样阈值写成常量，免得正常播放时把 4MB 的日志上限刷爆。
    const char* logAll = getenv("NAMA_OFX_LOG_ALL");
    const bool verbose = (seq <= 8) || (seq % 120 == 0) ||
                         (logAll && logAll[0] == '1' && logAll[1] == 0);

    InstanceData* d = getData(instance);
    OfxStatus status = kOfxStatOK;
    bool didLog = false;          // 本帧是否打了详细日志（放图后补打整动作耗时）
    double tActBeforeRel = 0.0;

    // 整个动作的墙钟占用。插件自己那几段耗时（转格式 / SDK / 转回）只是其中一部分，
    // 宿主在 clipGetImage 里把帧从显存搬进内存的开销不在这几段里。不单独量这个数，
    // 就分不清「慢在美颜」还是「慢在数据往返」。
    LARGE_INTEGER qpf, qpc0;
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&qpc0);
    auto sinceEntry = [&](void) {
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        return (double)(c.QuadPart - qpc0.QuadPart) * 1000.0 / (double)qpf.QuadPart;
    };

    // 宿主给的渲染时间。**必须原样传给 clipGetImage** —— 那个参数是「取第几帧」，
    // 不是可有可无的装饰。以前这里写死 0.0，等于时间线播放到第 N 帧、我们却每帧都
    // 向宿主索要第 0 帧的画面：检视器上画面就定格在第一帧不动（声音照常走），
    // 而且宿主每帧都要为「另一个时间点」重新准备输入，缓存反复失效。
    // 规范依据：ofxImageEffect.h 里 clipGetImage 的 \arg time = "time to fetch the
    // image at"；render 动作的 inArgs 明确带 OfxPropTime。
    double time = 0.0;
    if (inArgs) gPropHost->propGetDouble(inArgs, kOfxPropTime, 0, &time);

    // 渲染缩放：宿主在代理/半分辨率播放时会小于 1，仅用于诊断
    double scale[2] = {1.0, 1.0};
    if (inArgs)
        gPropHost->propGetDoubleN(inArgs, kOfxImageEffectPropRenderScale, 2, scale);

    OfxImageClipHandle outputClip = nullptr, sourceClip = nullptr;
    OfxPropertySetHandle outputImg = nullptr, sourceImg = nullptr;

    gEffectHost->clipGetHandle(instance, kOfxImageEffectOutputClipName, &outputClip, nullptr);
    gEffectHost->clipGetHandle(instance, kOfxImageEffectSimpleSourceClipName, &sourceClip, nullptr);
    if (!outputClip || !sourceClip) return kOfxStatFailed;

    // 源 clip 的真实 RoD：既是渲染窗口的兜底，也是「面积兜底判定代理帧」的参考面积
    // 来源（旧代码把参考面积写死成 960×540，4K/8K 工程的判定会整体偏移）。
    OfxRectI srcRod;
    memset(&srcRod, 0, sizeof(srcRod));
    bool haveRod = false;
    {
        OfxPropertySetHandle cp = nullptr;
        gEffectHost->clipGetPropertySet(sourceClip, &cp);
        if (cp &&
            gPropHost->propGetIntN(cp, kOfxImageEffectPropRegionOfDefinition, 4, &srcRod.x1) ==
                kOfxStatOK &&
            srcRod.x2 > srcRod.x1 && srcRod.y2 > srcRod.y1)
            haveRod = true;
    }
    const int rodW = haveRod ? (srcRod.x2 - srcRod.x1) : 0;
    const int rodH = haveRod ? (srcRod.y2 - srcRod.y1) : 0;

    // 宿主信号（旧报告 A10）：草稿 / 交互 / 顺序渲染 / 无空间感知。
    // 其中「顺序渲染」用来决定是否推进引擎内部的人脸跟踪帧序；
    // 「无空间感知」直接透传（本插件本身依赖空间信息）。
    bool draft = false, interactive = false, sequential = true, noSpatial = false;
    if (inArgs) {
        int v = 0;
        if (gPropHost->propGetInt(inArgs, kOfxImageEffectPropRenderQualityDraft, 0, &v) ==
            kOfxStatOK) draft = (v != 0);
        v = 0;
        if (gPropHost->propGetInt(inArgs, kOfxImageEffectPropInteractiveRenderStatus, 0, &v) ==
            kOfxStatOK) interactive = (v != 0);
        v = 0;
        if (gPropHost->propGetInt(inArgs, kOfxImageEffectPropSequentialRenderStatus, 0, &v) ==
            kOfxStatOK) sequential = (v != 0);
        char* ns = nullptr;
        if (gPropHost->propGetString(inArgs, kOfxImageEffectPropNoSpatialAwareness, 0, &ns) ==
                kOfxStatOK && ns)
            noSpatial = (strcmp(ns, "true") == 0);
    }

    // 渲染窗口：规范里宿主通过 inArgs 传入，取整帧时按源 RoD 兜底
    OfxRectI win;
    memset(&win, 0, sizeof(win));
    bool haveWin = false;
    if (inArgs &&
        gPropHost->propGetIntN(inArgs, kOfxImageEffectPropRenderWindow, 4, &win.x1) == kOfxStatOK)
        haveWin = (win.x2 > win.x1) && (win.y2 > win.y1);
    if (!haveWin) {
        if (haveRod) win = srcRod;
        if (win.x2 <= win.x1 || win.y2 <= win.y1) {
            if (verbose) ofxLog("render #%ld 空渲染范围，跳过", seq);
            return kOfxStatOK;
        }
    }

    OfxRectD region;
    region.x1 = (double)win.x1;
    region.y1 = (double)win.y1;
    region.x2 = (double)win.x2;
    region.y2 = (double)win.y2;

    if (gEffectHost->clipGetImage(outputClip, time, &region, &outputImg) != kOfxStatOK) {
        ofxLog("render #%ld 取输出图失败 time=%.3f", seq, time);
        return kOfxStatFailed;
    }
    if (gEffectHost->clipGetImage(sourceClip, time, &region, &sourceImg) != kOfxStatOK) {
        ofxLog("render #%ld 取源图失败 time=%.3f", seq, time);
        gEffectHost->clipReleaseImage(outputImg);
        return kOfxStatFailed;
    }
    double tGetImg = sinceEntry();

    // 处理区域尺寸与「是否真的写了目标图」在 do 块外声明，供收尾统一补边（B8）。
    int w = 0, h = 0, dw = 0, dh = 0;
    int dstRowBytes = 0;
    void* dstPtr = nullptr;
    char* dstDepth = nullptr;
    bool wroteDst = false;

    do {
        int srcRowBytes = 0;
        OfxRectI dstRect, srcRect;
        void* srcPtr = nullptr;
        char* srcDepth = nullptr;

        gPropHost->propGetInt(outputImg, kOfxImagePropRowBytes, 0, &dstRowBytes);
        gPropHost->propGetIntN(outputImg, kOfxImagePropBounds, 4, &dstRect.x1);
        gPropHost->propGetPointer(outputImg, kOfxImagePropData, 0, &dstPtr);
        gPropHost->propGetString(outputImg, kOfxImageEffectPropPixelDepth, 0, &dstDepth);

        gPropHost->propGetInt(sourceImg, kOfxImagePropRowBytes, 0, &srcRowBytes);
        gPropHost->propGetIntN(sourceImg, kOfxImagePropBounds, 4, &srcRect.x1);
        gPropHost->propGetPointer(sourceImg, kOfxImagePropData, 0, &srcPtr);
        gPropHost->propGetString(sourceImg, kOfxImageEffectPropPixelDepth, 0, &srcDepth);

        // 以下任一不满足都**不能**装作没事：目标图一个字节都没写，若仍回 kOfxStatOK，
        // 宿主拿到的是未初始化显存 → 花屏 / 上一帧残影，而且用户完全看不到报错
        //（旧报告的 A5）。这里统一置失败状态。
        if (!dstPtr || !srcPtr) {
            ofxLog("render #%ld 图像数据指针为空 dst=%p src=%p", seq, dstPtr, srcPtr);
            status = kOfxStatFailed;
            break;
        }
        if (srcRowBytes <= 0 || dstRowBytes <= 0) {
            ofxLog("render #%ld rowBytes 非法 src=%d dst=%d", seq, srcRowBytes, dstRowBytes);
            status = kOfxStatFailed;
            break;
        }

        // src / dst 各自的 bounds 都要看。以前这里只用 srcRect 算宽高、却写进
        // dstPtr —— 宿主两侧范围不一致时就会写错位，是个真 bug。
        int sw = srcRect.x2 - srcRect.x1, sh = srcRect.y2 - srcRect.y1;
        dw = dstRect.x2 - dstRect.x1;
        dh = dstRect.y2 - dstRect.y1;
        if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) {
            ofxLog("render #%ld bounds 非法 src=%dx%d dst=%dx%d", seq, sw, sh, dw, dh);
            status = kOfxStatFailed;
            break;
        }

        w = sw < dw ? sw : dw;
        h = sh < dh ? sh : dh;
        if (sw != dw || sh != dh)
            ofxLog("render #%ld WARN bounds 不一致 src=%dx%d dst=%dx%d，按 %dx%d 处理并补边",
                   seq, sw, sh, dw, dh, w, h);

        NamaSettings s;
        readParams(d, s);

        // v3.0.3：面板参数变化时记一行。
        // 「插件没效果」有两种完全不同的原因，日志上必须能一眼分开：
        //   · 参数全 0 → 引擎走逐字节透传（sdk≈1ms、faces=0、score=0.00）；
        //   · 参数正常但检不出脸 → sdk≥15ms、faces=0、score 是个小正数。
        // 只在参数真的变化时打，稳态零开销。
        {
            static double lastSig = -1e30;
            double sig = 0.0;
            for (int i = 0; i < P_COUNT; i++) sig = sig * 1.618 + s.panel[i];
            sig = sig * 1.618 + s.blur_type * 3.1 + s.skin_detect * 5.7 + s.smooth_mode * 9.3;
            if (sig != lastSig) {
                lastSig = sig;
                char pl[512];
                int off = 0;
                for (int i = 0; i < P_COUNT && off < (int)sizeof(pl); i++)
                    off += snprintf(pl + off, sizeof(pl) - (size_t)off, "%s%.0f",
                                    i ? "/" : "", s.panel[i]);
                ofxLog("面板参数 磨皮/清晰/美白/红润/锐化/全局锐化/立体/亮眼/去眼袋/去法令纹/祛斑/瘦脸/大眼/下巴 = %s"
                       " | blurType=%d skinDetect=%d smoothMode=%d",
                       pl, s.blur_type, s.skin_detect, s.smooth_mode);
            }
        }

        const char* depthTxt = isFloatDepth(srcDepth) ? "float" : "byte";

        // ---- 输入编码自检：每实例只采一次（v2.0.1）----
        // 放在代理/全 0 分支之前，保证「第一次真正出图」就能采到。
        // 采样本身约 3 万次循环，对首帧的耗时影响在 0.1ms 量级。
        if (!d->probedInput) {
            probeInputEncoding(srcPtr, srcRowBytes, w, h, isFloatDepth(srcDepth));
            d->probedInput = true;
        }

        // ---- 无空间感知渲染（例如生成 LUT）：必须透传（旧报告 A10）----
        // 本插件依赖整帧空间信息（人脸检测 + 磨皮），宿主声明这种渲染时不能出图。
        if (noSpatial) {
            blitFrame(srcDepth, srcPtr, srcRowBytes, dstDepth, dstPtr, dstRowBytes, w, h);
            wroteDst = true;
            if (verbose) ofxLog("render #%ld 无空间感知渲染 → 透传 %dx%d (无需 SDK)", seq, w, h);
            break;
        }

        // ---- 代理 / 缩略图帧：跳过整条 SDK 链路，直接按深度搬运 ----
        // 宿主有时不走 IsIdentity 而直接调 render（例如它已经为别的理由准备好了
        // 输出图），所以这里必须再判一次。日志实测这类帧要让 SDK 白跑 13～18 ms。
        if (isThrowawayRender(inArgs, w, h, rodW, rodH)) {
            blitFrame(srcDepth, srcPtr, srcRowBytes, dstDepth, dstPtr, dstRowBytes, w, h);
            wroteDst = true;
            if (verbose)
                ofxLog("render #%ld 代理/缩略图帧 → 透传 %dx%d scale=%.2f (无需 SDK)",
                       seq, w, h, scale[0]);
            break;
        }

        // ---- 全 0：跳过整条 SDK 链路，直接按深度搬运 ----
        if (s.allZero()) {
            blitFrame(srcDepth, srcPtr, srcRowBytes, dstDepth, dstPtr, dstRowBytes, w, h);
            wroteDst = true;
            if (verbose)
                ofxLog("render #%ld 参数全 0 → 透传 %dx%d in=%s out=%s (无需 SDK)",
                       seq, w, h, depthTxt, isFloatDepth(dstDepth) ? "float" : "byte");
            break;
        }

        // ---- 超大帧保护：异常尺寸直接透传，避免把宿主内存吃爆（P2）----
        if ((double)w * (double)h > kMaxProcessPixels) {
            blitFrame(srcDepth, srcPtr, srcRowBytes, dstDepth, dstPtr, dstRowBytes, w, h);
            wroteDst = true;
            ofxLog("render #%ld 尺寸过大 %dx%d，改为透传（超过 %.0f 像素上限）",
                   seq, w, h, kMaxProcessPixels);
            break;
        }

        LARGE_INTEGER f, c0;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&c0);
        // 时钟用 QPC：GetTickCount64 只有 15.6ms 精度，量不出 0.x ms 的那几段
        auto ms = [&](void) {
            LARGE_INTEGER c;
            QueryPerformanceCounter(&c);
            return (double)(c.QuadPart - c0.QuadPart) * 1000.0 / (double)f.QuadPart;
        };

        // ---- 转成一整块连续 RGBA8（按线程复用缓冲，不再每帧分配 16MB）----
        size_t need = (size_t)w * (size_t)h * 4;
        Scratch* scr = scratch();
        scr->ensure(need);
        if (scr->cap < need) {
            status = kOfxStatErrMemory;
            break;
        }
        unsigned char* pin = scr->in;
        unsigned char* pout = scr->out;
        blitFrame(srcDepth, srcPtr, srcRowBytes, kOfxBitDepthByte, pin, w * 4, w, h);
        double tIn = ms();

        // ---- 美颜 ----
        // 每个实例独占一个引擎槽位；createInstance 时没拿到就再试一次，
        // 仍失败则走透传 + 报错（旧报告 B5）。
        NamaEngine& engine = NamaEngine::inst();
        int slot = d ? d->slot : -1;
        if (slot < 0) {
            std::string serr;
            slot = engine.acquireSlot(serr);
            if (slot >= 0 && d) d->slot = slot;
        }
        // 非顺序渲染（随机访问 / 单帧）时不让引擎推进人脸跟踪帧序。
        bool ok = (slot >= 0) && engine.process(slot, pin, pout, w, h, s, sequential);
        double tSdk = ms();

        if (!ok) {
            // 引擎不可用时把源画面原样搬过去，避免 Resolve 里出现黑帧
            memcpy(pout, pin, need);
            // 报错状态 per-instance，避免「第一个实例报过之后全静默」。
            // d 理论上不会为空（createInstance 成功过才会走到 render），
            // 为空时退化到一个静态标志，不再重复刷屏。
            static bool fallbackWarned = false;
            bool& warned = d ? d->warned : fallbackWarned;
            if (!warned) {
                warned = true;
                const char* why = engine.lastErr();
                ofxLog("render #%ld 引擎失败，改为透传：%s", seq, why && *why ? why : "(无原因)");
                if (gMsgHost) {
                    // message() 是 printf 风格，引擎原因里可能带 % 字符，必须走 %s
                    gMsgHost->message(instance, kOfxMessageError, "", "%s",
                                      why && *why ? why : "DaVinci Beauty 引擎不可用，画面按原样输出。");
                }
            }
        }

        // ---- 写回目标深度 ----
        blitFrame(kOfxBitDepthByte, pout, w * 4, dstDepth, dstPtr, dstRowBytes, w, h);
        wroteDst = true;
        double tOut = ms();
        double tTotal = tOut;   // 与 ms() 共用同一个 QPC 起点，它本身就是总耗时
        tActBeforeRel = sinceEntry();

        if (verbose) {
            didLog = true;
            ofxLog("render #%ld time=%.3f %dx%d scale=%.2f in=%s out=%s rowBytes=%d/%d faces=%d score=%.2f "
                   "取图=%.1fms 转in=%.1fms sdk=%.1fms 转out=%.1fms 合计=%.1fms "
                   "slot=%d draft=%d interactive=%d seq=%d region=%s segn=%ld",
                   seq, time, w, h, scale[0], depthTxt,
                   isFloatDepth(dstDepth) ? "float" : "byte",
                   srcRowBytes, dstRowBytes, engine.lastFaces(), engine.lastFaceScore(),
                   tGetImg, tIn, tSdk - tIn, tOut - tSdk, tTotal,
                   slot, (int)draft, (int)interactive, (int)sequential,
                   engine.regionActive() ? "on" : "off", engine.regionInferCount());
        }
    } while (0);

    // bounds 不一致时，把目标图主区域之外的剩余部分补满，杜绝边缘花屏（B8）。
    if (wroteDst && (dw > w || dh > h))
        edgeFillDst(dstDepth, dstPtr, dstRowBytes, dw, dh, w, h);

    gEffectHost->clipReleaseImage(sourceImg);
    gEffectHost->clipReleaseImage(outputImg);

    // 整动作墙钟。减去上面那几段，剩下的就是宿主自己花在我们动作里的时间
    // （取图/放图，通常是显存↔内存的往返）——这才是判断「该优化谁」的依据。
    if (didLog) {
        double tAct = sinceEntry();
        ofxLog("render #%ld 动作总=%.1fms（宿主取放图 %.1fms）",
               seq, tAct, tAct - tActBeforeRel);
    }
    return status;
}

// ---------------------------------------------------------------------------
// 加载：取宿主席位
// ---------------------------------------------------------------------------
static OfxStatus onLoad(void) {
    if (!gHost) {
        ofxLog("onLoad 失败：宿主未提供 OfxHost");
        return kOfxStatErrMissingHostFeature;
    }
    gEffectHost = (OfxImageEffectSuiteV1*)gHost->fetchSuite(gHost->host, kOfxImageEffectSuite, 1);
    gPropHost = (OfxPropertySuiteV1*)gHost->fetchSuite(gHost->host, kOfxPropertySuite, 1);
    gParamHost = (OfxParameterSuiteV1*)gHost->fetchSuite(gHost->host, kOfxParameterSuite, 1);
    gMsgHost = (OfxMessageSuiteV1*)gHost->fetchSuite(gHost->host, kOfxMessageSuite, 1);
    if (!gEffectHost || !gPropHost) {
        ofxLog("onLoad 失败：缺少必要 suite effect=%d prop=%d",
               gEffectHost ? 1 : 0, gPropHost ? 1 : 0);
        return kOfxStatErrMissingHostFeature;
    }

    char* hostName = nullptr;
    if (gPropHost->propGetString((OfxPropertySetHandle)gHost->host, kOfxPropName, 0,
                                 &hostName) != kOfxStatOK)
        hostName = nullptr;
    ofxLog("onLoad 成功 宿主=%s paramSuite=%d msgSuite=%d build=" NAMA_BUILD_STR,
           hostName ? hostName : "(未提供)", gParamHost ? 1 : 0, gMsgHost ? 1 : 0);
    return kOfxStatOK;
}

static OfxStatus onUnload(void) {
    // v3.0.0 起不再有 NamaSDK（旧文案「卸载 SDK」会误导排错方向）。
    ofxLog("unload：释放引擎（CoreEngine + ONNX 会话）");
    NamaEngine::inst().release();
    return kOfxStatOK;
}

// ---------------------------------------------------------------------------
// 主分发
// ---------------------------------------------------------------------------
static OfxStatus pluginMain(const char* action, const void* handle,
                            OfxPropertySetHandle inArgs, OfxPropertySetHandle outArgs) {
    try {
        OfxImageEffectHandle effect = (OfxImageEffectHandle)handle;
        if (strcmp(action, kOfxActionLoad) == 0) return onLoad();
        if (strcmp(action, kOfxActionUnload) == 0) return onUnload();
        if (strcmp(action, kOfxActionDescribe) == 0) return describeMain(effect);
        if (strcmp(action, kOfxImageEffectActionDescribeInContext) == 0)
            return describeInContext(effect, inArgs);
        if (strcmp(action, kOfxImageEffectActionGetRegionOfDefinition) == 0)
            return getRegionOfDefinition(effect, inArgs, outArgs);
        if (strcmp(action, kOfxImageEffectActionGetClipPreferences) == 0)
            return getClipPreferences(effect, inArgs, outArgs);
        if (strcmp(action, kOfxActionCreateInstance) == 0) return createInstance(effect);
        if (strcmp(action, kOfxActionDestroyInstance) == 0) return destroyInstance(effect);
        if (strcmp(action, kOfxImageEffectActionIsIdentity) == 0)
            return isIdentity(effect, inArgs, outArgs);
        if (strcmp(action, kOfxImageEffectActionRender) == 0)
            return renderMain(effect, inArgs, outArgs);
    } catch (...) {
        ofxLog("pluginMain(%s) 抛出 C++ 异常", action ? action : "?");
        return kOfxStatErrMemory;
    }
    return kOfxStatReplyDefault;
}

static void setHostFunc(OfxHost* host) {
    gHost = host;
}

static OfxPlugin nab = {kOfxImageEffectPluginApi,
                        1,
                        "com.nama.davinci.beauty",
                        NAMA_BUILD_MAJOR,
                        NAMA_BUILD_MINOR,
                        setHostFunc,
                        pluginMain};

static OfxPlugin* pluginList[] = {&nab, nullptr};

// ---------------------------------------------------------------------------
// 强制导出符号（避免名字修饰）
// ---------------------------------------------------------------------------
extern "C" {
__declspec(dllexport) int OfxGetNumberOfPlugins(void) { return 1; }

__declspec(dllexport) OfxPlugin* OfxGetPlugin(int nth) {
    if (nth < 0 || nth >= (int)(sizeof(pluginList) / sizeof(pluginList[0]) - 1)) return nullptr;
    return pluginList[nth];
}
}  // extern "C"
