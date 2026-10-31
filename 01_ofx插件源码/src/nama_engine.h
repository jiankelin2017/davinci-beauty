#pragma once
// NamaEngine —— DaVinci Resolve OFX 美颜插件的引擎适配层。
//
// v3.0.0 起：本层不再加载 FaceUnity NamaSDK，而是把请求转给 OBS 自研的纯 CPU
// 美颜引擎（beauty_core 里的 CoreEngine，对应 OBS 3.1.39）。人脸检测用 ONNX YuNet，
// 在引擎内部**自动检测 GPU**（仅 Windows DirectML）：只要把 onnxruntime.dll 换成
// onnxruntime-directml 构建版，人脸推理就自动走 GPU；否则自动退回 CPU。本文件
// 只做「NamaSettings → 原始参数 → 转调 CoreEngine」（经 core_glue 桥接层），
// 不持有任何渲染资源、不依赖任何闭源 DLL。
//
// 公开接口（ofx_plugin.cpp 依赖）完全保留，宿主一行都不用改。
//
// 面板参数枚举（P_*）沿用本文件下方定义，与 CoreEngine 的 BeautySettings::panel[]
// 下标逐位一致（美型三项索引 11/12/13 在两边含义相同：瘦脸 / 大眼 / 下巴），
// 因此参数可逐字段直通，无需任何映射表。

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// 构建版本号 —— 唯一出处
// ---------------------------------------------------------------------------
// NamaVersion() 只编码 MAJOR/MINOR（对外 ABI 语义不变）；完整显示版本
// （含 PATCH 位）看 NAMA_BUILD_STR，日志里的 "init OK (v2.1.2 ...)" 用它。
#define NAMA_BUILD_MAJOR 3
#define NAMA_BUILD_MINOR 0
#define NAMA_BUILD_PATCH 4
#define NAMA_BUILD_STR "3.0.4"

// 面板参数顺序（滤镜属性页滑块的顺序）
enum {
    P_BLUR = 0,      // blur_level              磨皮   sdk_max 6.0
    P_CLARITY,       // clarity                 清晰   sdk_max 1.0
    P_WHITEN,        // color_level             美白   sdk_max 2.0
    P_RED,           // red_level               红润   sdk_max 1.0
    P_SHARPEN,       // sharpen                 锐化   sdk_max 1.0
                     //   底层接线保留：磨皮=0 时它确实有效，旧工程的非零值不能丢。
    P_GLOBAL_SHARP,  // 无 SDK 项                 全局锐化 —— 把与「锐化」同一条
                     //   曲线的细节回填扩展到皮肤掩码**之外**。0 = 脸外逐字节
                     //   不变（默认，保持原「只动脸」硬指标）。
    P_THREED,        // face_threed             五官立体 sdk_max 1.0
    P_EYEBRIGHT,     // eye_bright              亮眼   sdk_max 1.0
    P_POUCH,         // remove_pouch_strength   去黑眼圈 sdk_max 1.0
    P_NASOLABIAL,    // remove_nasolabial_folds_strength 去法令纹 sdk_max 1.0
    P_DELSPOT,       // delspot_level           祛斑痘 sdk_max 1.0
                     //   3.0.16 新增。实测 SDK 侧确实生效（脸内效果量 +9.4%），
                     //   但帧间闪动也会上升约 56%（1.626 -> 2.533），所以默认 0
                     //   由用户按机器性能与观感自行决定开多大。
                     //   （secret，面板不显示。）
    // ---- 2.1.0 新增：美型三项（几何形变，SDK 侧）----------------------------
    // 只挑「正交维度」的三项：脸宽 / 眼睛大小 / 下庭长度。刻意不接同一族里的
    // cheek_narrow / cheek_v / cheek_small / cheek_short —— 它们和 cheek_thinning
    // 抢同一块像素，同时推会互相打架（对方插件只暴露「瘦脸/大眼」两项也是这个原因）。
    // 形变是像素位移，与掩码类效果有两条额外的约束，见 render() 里的 shapeOn 分支。
    //
    // ⚠️ v2.2.3 起这三项在面板里**沉底**（kParams 数组同样换序，两者必须逐一
    // 对应 —— readParams 按下标写 panel[]）。原因：Resolve 的 OFX 面板不支持
    // 任何折叠分组（Page / 页内 Group / root Group 三种均实测无效），折叠走不通，
    // 退而求其次把低频项排到列表最末，常用项保持在顶部。
    P_CHEEK,         // cheek_thinning          瘦脸   sdk_max 1.0
    P_EYE,           // eye_enlarging           大眼   sdk_max 1.0
    P_CHIN,          // intensity_chin          下巴   sdk_max 1.0
    P_COUNT
};

struct NamaSettings {
    double panel[P_COUNT];   // 面板刻度 0..100
    int blur_type;           // 1=双边滤波（实测最强，推荐）0=高斯 2=弱磨皮
    int skin_detect;         // 0/1 只磨皮肤区域（默认 1：不要把背景一起磨糊）
    // 3.0.17：磨皮模式（纯 CPU 侧，不是 SDK 参数）。
    //   0 人像磨皮 = 3.0.16 行为，逐位不变
    //   1 清晰磨皮 = 平坦皮肤少回填、边缘轮廓多回填（推荐）
    //   2 精细磨皮 = 整体收敛，只留关键轮廓
    //   3 朦胧磨皮 = 整体压低细频
    int smooth_mode;

    NamaSettings() : blur_type(1), skin_detect(1), smooth_mode(0) {
        // paramGetValue 失败时的兜底默认：磨皮 50 / 美白 30 / 红润 30，其余十一项 0。
        // 必须与 ofx_plugin.cpp 的 kParams[].dflt 保持一致（那边是宿主真正下发的值）。
        // 62 的来历见 kParams 上方的标定说明：本版磨皮是线性曲线，20 档的实际强度
        // 只有 1.x 的约 1/5（几乎不可见）；62 是物理摄像头 GT 下与同类产品默认档
        // （blur=80 → 皮肤噪声 81.4%）对齐的档位。
        // 美型三项（2.1.0）必须默认 0：形变是不可逆的几何改动，默认开了等于
        // 擅自改用户的脸。
        // v2.2.3 用户决策：默认「磨皮 50 / 清晰 0 / 美白 30 / 红润 30」，其余十项 0。
        // 顺序必须同时看 ofx_plugin.cpp 的 kParams[] 与这里的 P_* 枚举（三者对应）。
        // 注：v2.0.3~v2.2.2 这里是 62，依据物理摄像头 GT 插值（见 kParams 注释），
        // 本次是用户侧观感取舍覆盖了那条 GT 结论，回退只需改这三个数。
        const double d[P_COUNT] = {50, 0, 30, 30, 0, 0, 0, 0, 0, 0, 0,
                                   0, 0, 0};
        for (int i = 0; i < P_COUNT; i++) panel[i] = d[i];
    }

    // 十项效果全 0 —— 此时 SDK 输出与输入一致，可以直接透传，不必进 SDK。
    bool allZero() const {
        for (int i = 0; i < P_COUNT; i++)
            if (panel[i] != 0.0) return false;
        return true;
    }
};

class NamaEngine {
public:
    static NamaEngine& inst();

    // 资源目录（<OBS>/obs-plugins/64bit/obs-nama-beauty）里必须含自研 CPU 引擎
    // 的运行时资产：onnxruntime.dll（ONNX Runtime，普通 CPU 版即可；如需 GPU
    // 推理可换成 onnxruntime-directml 构建版）与 yunet.onnx（YuNet 人脸检测模型）。
    // 成功返回 true，失败时 err 描述原因。首次调用耗时主要在 ONNX 会话构建与
    // 人脸模型加载，所以调用方应当在「模块加载 / 滤镜创建」时预热，而不是等
    // 第一次渲染。线程安全：内部加锁并复检状态。
    bool ensureReady(std::string& err);
    // 滤镜创建时专用：加锁判断「是否需要预热」，并把「本次是否真的做了初始化」
    // 通过 didInit 报回（仅用于日志）。
    bool preheat(std::string& err, bool& didInit);
    bool ready() const { return ready_; }

    // SDK 是否抛过访问违例。一旦为真，本进程内不再调用 SDK（避免反复崩）。
    bool poisoned() const { return poisoned_; }
    // 初始化失败次数；达到上限后 ensureReady 直接失败，不再重付那 2 秒。
    int failCount() const { return failCount_; }
    // 创建 GL 上下文的线程 id（诊断用）
    unsigned long ownerThread() const { return ownerThread_; }

    // 逐帧处理。src / dst 都是 RGBA8，长度 w*h*4。允许 src == dst。
    bool process(const unsigned char* src, unsigned char* dst,
                 int w, int h, const NamaSettings& s);

    // ---- 每实例一个 SDK 道具（slot）----
    //
    // 引擎内部的道具与参数缓存以前是**进程级单例**：两个滤镜（磨皮 30 / 90）交替
    // 渲染时会互相覆盖参数，而且 applied_ 缓存每帧被冲掉，每帧白付 10 次 setter。
    // 现在每个滤镜实例独占一个 slot。
    //
    // acquireSlot 成功返回 >=0 的槽位号，失败返回 -1（err 说明原因）。
    // 归还后槽位进空闲池复用，不销毁 SDK 道具（池大小 = 并发实例峰值）。
    int  acquireSlot(std::string& err);
    void releaseSlot(int slot);
    // 进程级检测选项（fuSetDefaultRotationMode / 关键点质量 / 最大人脸数）。
    // SDK 这三个 setter 是进程全局的，多个滤镜实例共用同一份状态，最后一次
    // 调用生效；内部加锁 + SEH，只能在渲染线程调用。
    void setDetectOptions(int rotate, int landmarkQuality, int maxFaces);
    // OBS 的视频渲染是严格顺序的，不需要 advanceFrame 开关（保留参数只为
    // 与 OFX 版保持同一份实现，少一处 diff 少一处坑）。
    bool process(int slot, const unsigned char* src, unsigned char* dst,
                 int w, int h, const NamaSettings& s, bool advanceFrame = true);

    int lastFaces() const { return faces_; }
    // v3.0.1：本帧人脸检测的最高候选置信度（不受阈值影响），供日志判断
    // 「模型没看出脸」还是「看出了没到阈值」。
    float lastFaceScore() const { return faceScore_; }
    int itemId() const;     // 实现见 nama_engine.cpp（经 coreBeauty 桥接）
    const char* version() const;  // 实现见 nama_engine.cpp（含 GPU provider）
    // 上一次失败的原因（成功时为空串）。供插件层把原因报给宿主。
    const char* lastErr() const { return failReason_.c_str(); }
    // 上一次 process 内 SDK 自身的耗时（毫秒，不含调用方的像素格式转换）
    double lastSdkMs() const { return lastSdkMs_; }

    // ---- 语义区域（自研皮肤分割）状态，供日志读取 ----
    // 不可用（人脸模型未加载 / 区域推理失败）时全部效果退回 v1.4 的颜色启发式，
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
    float faceScore_ = 0.0f;   // v3.0.1：本帧最高候选置信度
    const char* ver_ = "";
    double lastSdkMs_ = 0.0;

    std::string dir_;          // 插件 DLL 所在目录（.../obs-plugins/64bit）
    std::string assetDir_;     // 数据目录（.../obs-plugins/64bit/obs-nama-beauty）
    void* hGL_ = nullptr;      // 隐藏窗口
    void* hDC_ = nullptr;
    void* hRC_ = nullptr;
    int item_ = 0;             // 第一个道具（诊断用；= slots_[0].item）
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
    // CPU 侧 unsharp mask 的中间缓冲（横/纵模糊的积分值），按帧大小增长后复用。
    // 3.0.2 起多一组 7-tap 的横向和（中频带回填用）与一列滑窗累加器。
    // 两个横向和缓冲用 unsigned short：3-tap 最大 765、7-tap 最大 1785，
    // 都装得下 —— 1080p 下每帧少读写 8MB 内存，实测省 ~1.5ms。
    std::vector<unsigned short> sharpenTmp_;
    std::vector<unsigned short> sharpenTmp7_;
    std::vector<int> sharpenCol7_;
    bool ensureRegion();

    // 加载进内存的 face_beautification.bundle，acquireSlot 新建道具时复用
    std::vector<unsigned char> beautyPkg_;

    // 每实例一个 SDK 道具 + 它自己「上一次真正写进 SDK 的参数」。
    // 参数未变化时跳过重复 setter（每帧省 10 次查表），且不同实例互不干扰。
    // 3.0.1 起还带自己的细节层 EMA 历史（见 applySharpen）：也是每实例一份，
    // 两个滤镜交替渲染时互不污染。
    struct Slot {
        int item = 0;
        NamaSettings applied;
        bool hasApplied = false;
        bool inUse = false;
        // 3.0.1：细节层时序 EMA 的上一帧高频层（回填用的 hv，范围
        // [-255,255]，短整型足够）。按帧大小增长后复用。
        std::vector<short> detailPrev;
        // 3.0.2：中频带（box3-box7）的上一帧值，同样过 EMA、同样每槽位一份。
        std::vector<short> midPrev;
        bool hasDetailPrev = false;
        // 3.0.10：磨皮强度混合的「时间平滑原图」srcT（全帧 RGBA，每槽位一份）。
        // 见 bgraToRgbaBlend 的长注释 —— 3.0.8 的混合按 (1-t) 把相机噪声注回脸上，
        // 这里把混合的原图端换成 srcT，只削时间白噪、不动静态纹理。
        std::vector<unsigned char> blendPrev;
        // srcT 失效标记：t=0/t=1 走短路分支时不更新 srcT，下一帧要整帧对齐一次，
        // 否则运动后会用上很久以前的历史，表现为拖影。
        bool blendSnap = true;
        // 3.0.10：faces 门控的滞回（见 kFaceHoldFrames / kFaceGateStep）。
        // 用户实测「整脸突然暗一下再回来」就是 faces 单帧抖 0 让美白整档归零。
        int faceHold = 0;
        double faceGate = 1.0;
        // 2.1.2：这里原本有两个全帧缓冲（shapeGeo / shapeBase），是 2.1.0/2.1.1
        // 「形变在前、美颜在后」那套顺序的产物 —— 形变过一次之后，掩码、混合的
        // (1-t) 端、细节回填的高频来源、贴回基准，全都得换成形变后的版本，
        // 于是每个用途各存一份。
        // 美型挪到**最后**之后，这些补偿一个都不需要了：美颜那一遍作用在原始
        // 几何上，与 2.0.x 逐字节一致，缓冲随之删掉。
    };
    std::vector<Slot> slots_;
};

// ---------------------------------------------------------------------------
// 自测入口：不经 OBS，直接跑通 加载→初始化→渲染→落盘 全链路。
// 输入是裸 RGBA8（宽×高×4 字节），输出同尺寸的裸 RGBA8（便于像素比对）。
// 返回 0 表示成功。故意用 C ABI，方便用 ctypes / LoadLibrary 调用
// （blur_test 下的探针脚本用的就是它）。
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
