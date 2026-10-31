#pragma once
// NamaEngine —— FaceUnity NamaSDK 的离屏驱动层（被 OFX 插件调用）。
//
// 这一层刻意做得很薄：只依赖 Win32 + opengl32 + gdi32，不依赖 OpenFX。
// 这样既能在 OFX 插件里用，也能被下面的自测导出函数直接调用，
// 便于在没有图形界面的时候验证整条渲染管线是通的。
//
// 关键约束（都有出处，不是猜测）：
//   * NamaSDK 需要一个 current 的 OpenGL 上下文才能工作，所以这里自建一个
//     隐藏窗口 + wgl context（与 obs-nama-beauty.cpp 的 nama_gl_init 一致）。
//   * 上下文是线程独占的：调用前必须 wglMakeCurrent 到自己的上下文，
//     调用完必须还原调用线程原本的上下文，否则会把宿主的 GL 状态踩坏。
//     obs-nama-beauty.cpp 的注释记录过一次真实事故（UI 线程与渲染线程互踩
//     导致画面花掉 / 卡死），这里用一个全局互斥 + __declspec(thread) 还原规避。
//   * 全部 SDK 调用串行化：SDK 本身不是可重入的，OFX 的 render 可以是多线程。
//   * 每次 SDK 调用都走 SEH 兜底（见 nama_engine.cpp 的 seh* 系列）。SDK 是逆向
//     对接的闭源 DLL，一旦在宿主进程里抛访问违例，宿主会整体崩掉；包一层
//     __try/__except 后最坏只是这一帧透传。

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// 构建版本号 —— 唯一出处
//
// 以前 "1.3" 这个字符串散在 ofx_plugin.cpp 的日志里和 nama_engine.cpp 的
// NamaVersion() 里，改版本要记得改好几处，漏一处就会让日志和实际二进制对不上。
// 现在统一从这里派生。
// ---------------------------------------------------------------------------
#define NAMA_BUILD_MAJOR 1
#define NAMA_BUILD_MINOR 51
#define NAMA_BUILD_STR "1.51"

// 面板参数顺序（与 obs-nama-beauty.cpp 的 g_params[] 一一对应）
enum {
    P_BLUR = 0,      // blur_level              磨皮   sdk_max 6.0
    P_CLARITY,       // clarity                 清晰   sdk_max 1.0
    P_WHITEN,        // color_level             美白   sdk_max 2.0
    P_RED,           // red_level               红润   sdk_max 1.0
    P_SHARPEN,       // sharpen                 锐化   sdk_max 1.0
                     //   已在界面上隐藏（OfxParamPropSecret），原因见
                     //   02_测试夹具beauty_probe/锐化无效排查报告.md。
                     //   底层接线保留：磨皮=0 时它确实有效，旧工程的非零值不能丢。
    P_THREED,        // face_threed             五官立体 sdk_max 1.0
    P_EYEBRIGHT,     // eye_bright              亮眼   sdk_max 1.0
    P_POUCH,         // remove_pouch_strength   去黑眼圈 sdk_max 1.0
    P_COUNT
};

struct NamaSettings {
    double panel[P_COUNT];   // 面板刻度 0..100
    int blur_type;           // 1=双边滤波（实测最强，推荐）0=高斯 2=弱磨皮
    int skin_detect;         // 0/1 只磨皮肤区域（默认 1：不要把背景一起磨糊）

    NamaSettings() : blur_type(1), skin_detect(1) {
        // 首次拖到片段上的默认值：只开「磨皮 20」，其余七项 0。
        // 与 ofx_plugin.cpp 的 kParams[].dflt 必须保持一致 —— 这里是
        // paramGetValue 失败时的兜底，实际面板值由宿主按 kParams 的 Default 给出。
        // 磨皮 70 / 美白 15 / 红润 30 的旧默认已按产品要求下调。
        const double d[P_COUNT] = {20, 0, 0, 0, 0, 0, 0, 0};
        for (int i = 0; i < P_COUNT; i++) panel[i] = d[i];
    }

    // 八项效果全 0 —— 此时 SDK 输出与输入一致，可以直接透传，不必进 SDK。
    bool allZero() const {
        for (int i = 0; i < P_COUNT; i++)
            if (panel[i] != 0.0) return false;
        return true;
    }
};

class NamaEngine {
public:
    static NamaEngine& inst();

    // 资源目录里必须含 CNamaSDK.dll / fuai.dll / auth_package.bin
    // 及 model/、graphics/ 两个子目录。成功返回 true，失败时 err 描述原因。
    // 首次调用耗时约 1.7~2.0 秒（fuSetup + 人脸模型 + 美颜道具），所以调用方
    // 应当在「实例创建」时预热，而不是等第一次渲染。
    // 线程安全：内部加锁并复检状态。首次成功约 1.7~2.0 秒。
    bool ensureReady(std::string& err);
    // createInstance 专用：加锁判断「是否需要预热」，并把「本次是否真的做了初始化」
    // 通过 didInit 报回（仅用于日志）。这样就不会出现「无锁读 ready_/poisoned_」
    // 的 TOCTOU（旧 CODE_REVIEW 的 B4）。
    bool preheat(std::string& err, bool& didInit);
    bool ready() const { return ready_; }

    // SDK 是否抛过访问违例。一旦为真，本进程内不再调用 SDK（避免反复崩）。
    bool poisoned() const { return poisoned_; }
    // 初始化失败次数；达到上限后 ensureReady 直接失败，不再重付那 2 秒。
    int failCount() const { return failCount_; }
    // 创建 GL 上下文的线程 id（诊断用：OFX 的 render 可能来自别的线程）
    unsigned long ownerThread() const { return ownerThread_; }

    // 逐帧处理。src / dst 都是 RGBA8，长度 w*h*4。允许 src == dst。
    //
    // 旧接口（自测用）：内部自动拿一个默认 slot。
    bool process(const unsigned char* src, unsigned char* dst,
                 int w, int h, const NamaSettings& s);

    // ---- 每实例一个 SDK 道具（slot）----
    //
    // 引擎内部的道具与参数缓存以前是**进程级单例**：两个实例（磨皮 30 / 90）交替
    // 渲染时会互相覆盖参数，而且 applied_ 缓存每帧被冲掉，每帧白付 10 次 setter
    //（旧 CODE_REVIEW 的 B5）。现在每个 OFX 实例独占一个 slot。
    //
    // acquireSlot 成功返回 >=0 的槽位号，失败返回 -1（err 说明原因）。
    // 归还后槽位进空闲池复用，不销毁 SDK 道具（池大小 = 并发实例峰值）。
    int  acquireSlot(std::string& err);
    void releaseSlot(int slot);
    // advanceFrame=false 表示宿主当前不是顺序渲染（随机访问 / 生成单帧），
    // 这时不推进内部人脸跟踪帧序，避免时序状态漂移。
    bool process(int slot, const unsigned char* src, unsigned char* dst,
                 int w, int h, const NamaSettings& s, bool advanceFrame = true);

    int lastFaces() const { return faces_; }
    int itemId() const { return item_; }
    const char* version() const { return ver_; }
    // 上一次失败的原因（成功时为空串）。供插件层把原因报给宿主。
    const char* lastErr() const { return failReason_.c_str(); }
    // 上一次 process 内 SDK 自身的耗时（毫秒，不含调用方的像素格式转换）
    double lastSdkMs() const { return lastSdkMs_; }

    // ---- 语义区域（fuai 皮肤分割）状态，供日志与夹具读取 ----
    // 不可用（缺 fuai.dll / 明文 bundle）时全部效果退回 v1.4 的颜色启发式，
    // 不会报错也不会崩溃。
    bool   regionActive() const { return regionActive_; }
    int    regionFaces() const;
    double regionCoverage() const;
    double regionInferMs() const;
    long   regionInferCount() const;
    const char* regionErr() const;

    // 释放 SDK 与 GL 资源。仅在宿主明确卸载插件时调用。
    void release();

private:
    NamaEngine();
    ~NamaEngine() {}
    NamaEngine(const NamaEngine&) = delete;
    NamaEngine& operator=(const NamaEngine&) = delete;

    bool ensurePath(std::string& err);
    bool ensureGL(std::string& err);
    bool ensureSDK(std::string& err);
    // 加锁版本：调用方必须**未**持有 mutex_。didInit 可为空。
    bool ensureReadyLocked(std::string& err, bool* didInit);
    // 创建一个新的美颜道具并写入模块级默认参数。调用前 GL 上下文必须已 current。
    bool createBeautyItemGL(int* outItem);
    bool enter();     // true = 已切到本引擎的 GL 上下文；false = 切换失败
    void leave();

    bool ready_ = false;
    bool pathDone_ = false;
    bool glDone_ = false;
    bool poisoned_ = false;
    int  failCount_ = 0;
    unsigned long ownerThread_ = 0;
    std::string failReason_;
    int faces_ = 0;
    const char* ver_ = "";
    double lastSdkMs_ = 0.0;

    std::string dir_;          // 引擎 DLL 所在目录
    void* hModule_ = nullptr;  // CNamaSDK.dll
    void* hGL_ = nullptr;      // 隐藏窗口
    void* hDC_ = nullptr;
    void* hRC_ = nullptr;
    int item_ = 0;             // 第一个道具（诊断 / 自测显示用；= slots_[0].item）
    int frameId_ = 0;
    void* mutex_ = nullptr;    // CRITICAL_SECTION，避免依赖 MSVC 版本的 std::mutex 布局
    std::string work_;         // 渲染临时缓冲（输出侧，已换成 BGRA）
    std::string bgScratch_;    // 输入侧 BGRA 副本（src 是调用方的，不能原地改）
    // 美白肤色掩码（1/4 分辨率）与羽化中间结果，跨帧复用，避免每帧堆分配。
    // 仅在语义区域不可用时（退回颜色启发式）才会用到。
    std::vector<unsigned char> whitenMask_;
    std::vector<unsigned char> whitenTmp_;

    // ---- 语义区域融合用的映射表 ----
    // 把 192×256 的皮肤掩码双线性采样到 w×h：每个 x/y 只算一次下标与权重，
    // 逐像素只剩查表与乘加。表很小（w+h 项），每帧重建的代价可以忽略。
    bool regionActive_ = false;      // 已成功初始化（不代表每帧都有掩码）
    bool regionInitDone_ = false;    // 已尝试过初始化（避免每帧重试）
    std::string regionErr_;          // 初始化失败原因（regionActive_=false 时有效）
    std::vector<int> mapX0_, mapX1_, mapWX_, mapY0_, mapY1_, mapWY_;
    bool ensureRegion();

    // 加载进内存的 face_beautification.bundle，acquireSlot 新建道具时复用
    std::vector<unsigned char> beautyPkg_;

    // 每实例一个 SDk 道具 + 它自己「上一次真正写进 SDK 的参数」。
    // 参数未变化时跳过重复 setter（每帧省 10 次查表），且不同实例互不干扰。
    struct Slot {
        int item = 0;
        NamaSettings applied;
        bool hasApplied = false;
        bool inUse = false;
    };
    std::vector<Slot> slots_;
    int defaultSlot_ = -1;     // 自测用：process(src,...) 自动占用的槽位
};

// ---------------------------------------------------------------------------
// 自测入口：不经 OpenFX 协议，直接跑通 加载→初始化→渲染→落盘 全链路。
// 输入是裸 RGBA8（宽×高×4 字节），输出同尺寸的裸 RGBA8（便于像素比对）。
// 返回 0 表示成功。故意用 C ABI，方便用 ctypes / LoadLibrary 调用。
// ---------------------------------------------------------------------------
#ifdef __cplusplus
extern "C" {
#endif
__declspec(dllexport) int __stdcall NamaSelfTest(const char* engineDir,
                                                 const char* rawRgbaIn,
                                                 const char* rawRgbaOut,
                                                 int width, int height,
                                                 double blur, double whiten,
                                                 char* msgBuf, int msgCap);
__declspec(dllexport) unsigned long __stdcall NamaVersion(void);
#ifdef __cplusplus
}
#endif
