#pragma once
// ---------------------------------------------------------------------------
// beauty_core —— 美颜引擎抽象层
//
// 目的：把「谁来做美颜」从 OBS 胶水层里解耦出去。
//
//   obs-beauty.cpp  ──>  IBeautyEngine  ──>  CoreEngine        （默认：自研）
//                                       └─>  LegacyAdapter       （回退：参考实现驱动，仅离线基准）
//                                       └─>  PassthroughEngine （验证用直通）
//
// 接口严格按 LegacyEngine 的现有用法设计（RGBA 进 / RGBA 出 + BeautySettings +
// 每实例 slot），所以替换实现时 OBS 侧不需要懂任何引擎内部细节。
//
// 选择方式（进程内只解析一次）：
//   * 不设 / 未知值 → CoreEngine（3.1.7 起的默认）
//   * BEAUTY_ENGINE_IMPL=legacy       → LegacyAdapter（仅离线基准 DLL 编译）
//   * BEAUTY_ENGINE_IMPL=core         → CoreEngine（显式写出来，便于脚本自证）
//   * BEAUTY_ENGINE_IMPL=passthrough  → 直通，用于验证开关与降级
//
// 为什么要留一个直通引擎：它是「开关真的生效」的最小可验证证据 —— 装上之后
// 若画面完全没变化，说明开关没接上；若画面变成原图，说明链路走通了。它同时
// 也是「新引擎出错时的安全降级目标」。
// ---------------------------------------------------------------------------

#include <string>
#include <cstdint>
#include "beauty_params.h"   // BeautySettings / P_* 的唯一出处，不自造一份

// ---------------------------------------------------------------------------
// 引擎接口
//
// 方法语义与 LegacyEngine 的同名方法一一对应，注释见 legacy_engine.h。
// 这里只说明接口层面的差异与约定：
//   * process() 允许 src == dst；实现方必须自己准备输出暂存缓冲。
//   * acquireSlot()/releaseSlot() 保持「每实例一个上下文」的模型。自研引擎
//     若不需要槽位，返回一个恒定的合法值即可（PassthroughEngine 返回 0）。
//   * 所有方法都可能被渲染线程调用；线程安全由实现方负责。
// ---------------------------------------------------------------------------
class IBeautyEngine {
public:
    virtual ~IBeautyEngine() {}

    // 短标识，用于日志与 A/B 对照（"legacy" / "passthrough" / "core"）。
    virtual const char* id() const = 0;
    virtual const char* version() const = 0;

    virtual bool ensureReady(std::string& err) = 0;
    virtual bool preheat(std::string& err, bool& didInit) = 0;
    virtual bool ready() const = 0;

    virtual int  acquireSlot(std::string& err) = 0;
    virtual void releaseSlot(int slot) = 0;
    virtual void setDetectOptions(int rotate, int landmarkQuality, int maxFaces) = 0;

    // advanceFrame：是否推进引擎内部的帧序号（人脸跟踪的时序状态）。
    // 默认 true（正常渲染）；测试里要「同一帧喂两次、比对两条实现路径」时
    // 传 false，否则第二次调用会看到已被前一次更新过的跟踪状态。
    virtual bool process(int slot, const unsigned char* src, unsigned char* dst,
                         int w, int h, const BeautySettings& s,
                         bool advanceFrame = true) = 0;

    // ---- 只读诊断，供日志使用（可以返回保守值，不影响画面）----
    virtual int          lastFaces() const = 0;
    // 本帧人脸检测的最高候选置信度（不受 conf 阈值 / NMS 影响）。
    // 用于日志判断「模型没看出脸」还是「看出了没到阈值」。v3.0.1 新增。
    virtual float         lastFaceScore() const = 0;
    virtual bool         regionActive() const = 0;
    // GPU 执行提供器名（自动检测到的，例如 "DirectML" / "CPU"），供日志与版本串使用。
    virtual const char*  gpuProvider() const { return "CPU"; }
    virtual double       lastCoreMs() const = 0;
    virtual const char*  lastErr() const = 0;
    virtual int          itemId() const = 0;
    virtual unsigned long ownerThread() const = 0;

    // 帧行序声明：true = 源缓冲第 0 行是画面最下面一行（bottom-up）。
    //
    // OFX 宿主（Resolve）给的就是 bottom-up，所以 CoreEngine 默认 true；
    // NamaSelfTest / 离线夹具喂的是 top-down 的 raw 文件，必须显式设 false，
    // 否则人脸检测会看到上下颠倒的画面（v3.0.4 之前一直是这个状态）。
    // 老引擎（legacy/passthrough）不做人脸检测，默认空实现即可。
    virtual void setImageBottomUp(bool) {}

    // 异步人脸检测开关（默认开）。自测 / 离线逐字节对拍必须关掉：
    // 异步下「本帧用哪份掩码」取决于 worker 线程调度，同一份输入连跑两次
    // 会得到不同的输出（实测同进程连跑 blur62 出现两个 md5）—— 那样基线
    // 根本不可复现。生产路径保持开，检测不压渲染线程。
    virtual void setAsyncDetect(bool) {}

    // 宿主卸载插件时调用；之后本进程内不再使用。
    virtual void release() = 0;
};

// ---------------------------------------------------------------------------
// 自研核参数（仅供离线对拍 / 调参使用）
//
// 生产路径不调用它：默认值就是离线对拍得出的最佳组合（见
// 05_文档/S2_磨皮核对拍结论.md）。离线扫描参数时用这个入口改，避免为了调参
// 去改源码重编译。
// ---------------------------------------------------------------------------
void beautyCoreSetSmoothParams(int radius, float sigmaSpace,
                               float sigmaColor, int down);

// 性能开关（仅供离线对照实验）
//   threads <= 0 自动（硬件并发数，上限 8）；1 = 强制单线程
//   useRoi  0 = 关掉 ROI 裁剪，退回全帧磨皮
//
// 生产路径不调用它。关 ROI 只用于测量「全帧成本/ROI 节省了多少」，
// 生产必须保持开启 —— 关掉后 93% 的算力会花在随后被丢弃的像素上。
void beautyCoreSetPerf(int threads, int useRoi);

// Sobel 边缘门控灵敏度（仅供离线调参）。0 = 关闭。
// 灵敏度系数取自 wuxiwuhen/obs-beauty-filter（MIT）的 PSComposite，原值 ×10.0。
// 门控会在「亮度梯度大」的地方（眉毛、眼镜框、发际线）取消磨皮。
void beautyCoreSetEdgeK(int k);

// 五官保护强度 0..100（仅供离线调参）。0 = 关闭。
// 眼睛和嘴巴上的磨皮会乘 (1 - protect × k/100)，做法同 lumina 的 uSmoothProtect。
void beautyCoreSetProtectK(int k);

// 阈值软膝（立体感）0..255，0 = 关闭。|src-smoothed| 超过它会按比例减少磨皮，
// 从而保住鼻梁侧、下颌线这类大幅度的明暗过渡。做法同 lumina 的 uSmoothThreshold。
void beautyCoreSetLimitThr(int thr);

// 磨皮算法切换（离线 A/B 用）：
//   mode  1 = 高低频分离（默认），0 = 旧的「双边滤波 + 线性混合」
//   sigma 低频尺度（高斯 sigma，半径自动取 3σ）
//   magLo/magHi  幅度门控区间（0..255 尺度）：小于 lo 全磨，大于 hi 不动
void beautyCoreSetFreq(int mode, float sigma, float magLo, float magHi);

// 颗粒回填量 0..1（离线调参）。抑制双边迭代的阶梯感。
void beautyCoreSetFreqGrain(float g);

// 低频提取方式（离线 A/B）：mode 0 = 双边迭代，1 = 引导滤波；radius/eps 只对后者生效。
void beautyCoreSetGuide(int mode, int radius, float eps);

// 低频层路线（离线 A/B）：path 0 = 旧 uint8 双边迭代，1 = 浮点双边，
// 2 = 浮点双边 + 低频参照（默认）。refRadius <= 0 表示不做参照。
void beautyCoreSetLowPath(int path, int refRadius);

// 清晰（clarity）离线标定：面板 100 档映射到 0..maxGain；radius 是盒式半径
// （12 → 25px 窗口）；kneeLo/kneeHi 是软膝区间（防边缘光晕）。
void beautyCoreSetClarity(float maxGain, int radius, float kneeLo, float kneeHi);

// ---------------------------------------------------------------------------
// 异步掩码提交周期的当前值（帧，0 = 每帧）。**只用于日志**，不影响行为。
// 为什么要暴露：3.1.8 起生产默认每帧检测，离线台架仍是每 4 帧（同步路径），
// 两者数字不可直接比。日志里打出实际节奏，省得下次又对不上账。
int beautyCoreMaskCadence();

// 累计提交给检测 worker 的帧数（诊断用）。配合帧数就能算出实际节奏：
//   提交数 / 帧数 ≈ 1/(周期+1)。周期 0 → ≈1.0，周期 4 → ≈0.2。
// 这是「每帧检测」唯一能直接测到的证据 —— 它不体现在渲染线程的毫秒数上。
uint64_t beautyCoreMaskSubmits();

// 掩码↔帧对齐计数（3.1.10，诊断用）：used = 用了对齐帧处理的帧数。
// ratio = used / total（total 由 beautyCoreAlignCount 的出参给）。
// 30fps 下 ≈1 说明 worker 每帧都到货、掩码与画面逐像素同源；
// 明显 < 1 说明检测追不上渲染，拖影会按比例回来。
uint64_t beautyCoreAlignUsed();

// 3.1.10 探针：出参带回 (对齐帧数, 处理总帧数)。
int beautyCoreAlignStat(uint64_t* used, uint64_t* total);

// 掩码导出（仅供离线对拍）
//
// 为什么要这个：自研引擎只磨「肤色 ∩ 人脸框」那 5% 左右，而 参考实现的改动区有
// 13%（含脖子/肩）。拿参考实现改动区当分母去算细节保留率，会把「只磨了脸」读成
// 「几乎没磨」（保留率 99%）。对拍的分母必须是**自研掩码本身**。
//
// src   RGBA8（w*h*4），会被强制重算一次掩码（不吃缓存）
// outMask w*h 字节，0 或 255
// 返回 false 表示本帧没有可用掩码（无脸 / 模型不可用），此时 outMask 全 0。
// 只对 core 引擎有效；其它实现返回 false。
// ---------------------------------------------------------------------------
// 皮肤判据快照（C20，只用于日志）
//
// 为什么要暴露：真机上「磨皮没效果」的头号原因是肤色掩码被白平衡压低，
// 而这个值光看画面看不出来。日志里打一行，一眼能确认自适应有没有开、
// 当前椭圆中心漂到哪了。返回指向静态缓冲的指针，非线程安全但只在启动时读。
// ---------------------------------------------------------------------------
const char* beautyCoreSkinInfo();

// 「肤色自适应估计首次收效」的一次性通知（C20 真机自查）。引擎 worker 在
// 第 2 帧以后第一次把估计出的中心写进全局时置位；取走返回 true 并清零。
// 渲染线程拿到 true 后紧跟一行 beautyCoreSkinInfo() —— init 时那行只有
// (center=default, not yet estimated)，这里才是自适应真正生效的证据。
// 返回的指针指向静态缓冲，只在本线程打日志用。
bool beautyCoreSkinAnnounce();

bool beautyCoreComputeMask(const unsigned char* src, int w, int h,
                           unsigned char* outMask);
//
//   legacy      LegacyAdapter（仅离线基准 DLL） —— 与本次改动前逐位相同的行为
//   passthrough          直通，用于验证开关与降级
//   core                 自研引擎（T2 的落点，已实现磨皮）
//
// 环境变量只在第一次调用时解析一次（与 skipGlLock 等既有开关同一套写法）。
// 未设置时行为与改动前完全一致：不读环境、不打印、零开销。
// ---------------------------------------------------------------------------
IBeautyEngine& beautyEngine();
const char* beautyEngineId();
