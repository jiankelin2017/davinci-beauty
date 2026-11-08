// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Jian
// beauty_core.cpp —— 引擎抽象层 + 三个实现的容器
//
//   1) PassthroughEngine：直通，用来证明「开关真的生效」；
//   2) CoreEngine：自研引擎，3.1.7 起是唯一的实现；
//   3) beautyEngine()：返回选定的实现（默认 core）。
//
// 3.1.21：旧的参考版驱动适配层**已从插件里移除**。它只在离线基准 DLL 里保留
// —— 那份 DLL 的作用是「量出参考版的输出」，不随插件发布。插件的编译单元里
// 不再有任何参考版二进制名。

#include "beauty_core.h"
#include "beauty_face.h"
#include "beauty_freq.h"
#include "beauty_guide.h"
#include "beauty_onnx.h"
#include "beauty_pool.h"
#include "beauty_region.h"
#include "beauty_smooth.h"
#include "beauty_log.h"
#include "beauty_warp.h"   /* 3.1.27：美形三项的几何变形 */
#include "gpu_beauty.h"    /* P1：D3D11 ROI 传输管道（beauty.ini 的 gpu_pipe 控制） */
#include "beauty_build.h"  /* 版本串：转出 BEAUTY_BUILD_STR，见该头注释（不能直接包含 beauty_engine.h） */
#include "gpu_smooth.h"    /* P2：磨皮生产路径的 D3D11 Compute 版（beauty.ini 的 gpu_smooth 控制） */
#include "beauty_hwprobe.h" /* 3.3.0：beauty.ini 配置读取（gpu_pipe 三态） */

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifdef BEAUTY_WITH_LEGACY_ENGINE
#include "legacy_engine.h"
#endif

// ---------------------------------------------------------------------------
// 诊断出口（BUG4 多实例塌陷）
//
// 「掩码↔帧对齐」那条优化会在检测结果到货的那一帧把 src 整体换成缓存的
// heldFrame_（见下面 process() 里 3.1.10 那段）。单实例时这是对的；两个实例
// 交替调用时，heldFrame_ 可能属于**另一个**实例 —— 一旦如此，本实例的输出就是
// 对方的画面，表现为「两层合成塌成一层」。
//
// 这里只暴露一个「本帧是否发生了整帧替换」的标志给 OFX 层的诊断日志，用来把
// 「我们自己换了帧」和「宿主给的就是同一张图」在日志上分开。不改任何渲染行为。
// ---------------------------------------------------------------------------
// 2026-11-06 复核（P1）：这个标志必须**每线程一份**。
//
// 它回答的是「本线程上一次 process 有没有换整帧」，而多个实例的 process 可能
// 同时跑在不同渲染线程上。做成进程级 int 有两个后果：
//   ① A 线程写 1、B 线程随后读到 —— 日志把「我方换了帧」记到对方头上，
//      这条诊断的可信度就没了，而它存在的唯一意义就是判读串台；
//   ② 无同步的跨线程写读按标准是 UB（编译器可以把它缓存进寄存器）。
// thread_local 把两个问题一起解决，代价只是一次 TLS 访问。
thread_local int g_beautyDiagFrameSubst = 0;

namespace {

// ---------------------------------------------------------------------------
// 上一次 process() 的只读快照（供 OFX 层打日志 / 报错）
//
// BUG4 把实例状态做成了「换进换出」的共享成员，于是 process() 一退出，成员里
// 装的就是**别的实例**的东西了。所以退出前必须留一份快照。
//
// 快照必须是 thread_local，不能用进程级成员：两个实例并发渲染时，对方的
// process 会把快照覆盖掉。lastErr() 尤其致命 —— 它把 c_str() 交出去，对方
// 只要往同一个 std::string 里赋一次值（可能触发重分配），调用方手里的指针
// 就悬垂了，而 ofx_plugin.cpp 拿到它之后还要先过一遍 ofxLog 和宿主的 message()。
// thread_local 之后，快照的写与读天然在同一个渲染线程上，顺序也天然成立。
// ---------------------------------------------------------------------------
thread_local int         t_lastFacesOut = 0;
thread_local float       t_lastScoreOut = 0.0f;
thread_local std::string t_lastErrOut;

#ifdef BEAUTY_WITH_LEGACY_ENGINE
// ---------------------------------------------------------------------------
// 旧版适配层 —— 旧链路适配器（仅离线基准 DLL 编译，插件不编译这段）
// ---------------------------------------------------------------------------
class LegacyAdapter : public IBeautyEngine {
public:
    const char* id() const override { return "legacy"; }
    const char* version() const override { return LegacyEngine::inst().version(); }

    bool ensureReady(std::string& err) override { return LegacyEngine::inst().ensureReady(err); }
    bool preheat(std::string& err, bool& didInit) override {
        return LegacyEngine::inst().preheat(err, didInit);
    }
    bool ready() const override { return LegacyEngine::inst().ready(); }

    int acquireSlot(std::string& err) override { return LegacyEngine::inst().acquireSlot(err); }
    void releaseSlot(int slot) override { LegacyEngine::inst().releaseSlot(slot); }
    void setDetectOptions(int rotate, int landmarkQuality, int maxFaces) override {
        LegacyEngine::inst().setDetectOptions(rotate, landmarkQuality, maxFaces);
    }

    bool process(int slot, const unsigned char* src, unsigned char* dst,
                 int w, int h, const BeautySettings& s,
                 bool advanceFrame = true) override {
        return LegacyEngine::inst().process(slot, src, dst, w, h, s, advanceFrame);
    }

    int lastFaces() const override { return LegacyEngine::inst().lastFaces(); }
    float lastFaceScore() const override { return 0.0f; }   // LegacyEngine 不暴露，v3.0.1
    bool regionActive() const override { return LegacyEngine::inst().regionActive(); }
    double lastCoreMs() const override { return LegacyEngine::inst().lastCoreMs(); }
    const char* lastErr() const override { return LegacyEngine::inst().lastErr(); }
    int itemId() const override { return LegacyEngine::inst().itemId(); }
    unsigned long ownerThread() const override { return LegacyEngine::inst().ownerThread(); }

    void release() override { LegacyEngine::inst().release(); }
};

#endif  // BEAUTY_WITH_LEGACY_ENGINE

// ---------------------------------------------------------------------------
// PassthroughEngine —— 直通
//
// 用途有两个：
//   * 证明 A/B 开关确实接上了（切过去画面应当变成完全没美颜的原图）；
//   * 作为自研引擎出错时的安全降级目标（比「滤镜整体直通」更安全：旧宿主侧的
//     尺寸/纹理/自检逻辑仍然照常跑一遍，不会因为降级而悄悄失效）。
//
// 刻意**不**做任何图像处理：一旦它产出与输入不一致的像素，就说明骨架接错了。
// ---------------------------------------------------------------------------
class PassthroughEngine : public IBeautyEngine {
public:
    const char* id() const override { return "passthrough"; }
    const char* version() const override { return "0"; }

    bool ensureReady(std::string& err) override { err.clear(); return true; }
    bool preheat(std::string& err, bool& didInit) override {
        err.clear();
        didInit = false;
        return true;
    }
    bool ready() const override { return true; }

    int acquireSlot(std::string& err) override { err.clear(); return 0; }
    void releaseSlot(int) override {}
    void setDetectOptions(int, int, int) override {}

    bool process(int, const unsigned char* src, unsigned char* dst,
                 int w, int h, const BeautySettings&,
                 bool = true) override {
        if (!src || !dst || w <= 0 || h <= 0) return false;
        if (dst != src) memcpy(dst, src, (size_t)w * (size_t)h * 4);
        return true;
    }

    int lastFaces() const override { return 0; }
    float lastFaceScore() const override { return 0.0f; }
    bool regionActive() const override { return false; }
    double lastCoreMs() const override { return 0.0; }
    const char* lastErr() const override { return ""; }
    int itemId() const override { return 0; }
    unsigned long ownerThread() const override { return (unsigned long)GetCurrentThreadId(); }

    void release() override {}
};

// smoothstep 的 C++ 版（旧宿主的 effect 里没有内置，这里自己写）
inline float smoothStep(float a, float b, float x) {
    if (b <= a) return (x < a) ? 0.0f : 1.0f;
    float t = (x - a) / (b - a);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

// 约束到 [0,1]。面板档位是 0..100 的 double，面板外（离线脚本）可能给越界值。
inline float clampi01(double v) {
    const float f = (float)v;
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

// 五锚点查表（锚点对应面板 0/25/50/75/100），中间线性插值。
// 用它而不是幂函数：产品曲线在低档几乎是硬拐点（25 档 ≈ 满档的 0.5%），
// 任何单项幂函数都拟合不出「0.005 / 0.211 / 0.588 / 1.0」这条形状。
inline float lvLerp5(const float tbl[5], float t) {
    if (t <= 0.0f) return tbl[0];
    if (t >= 1.0f) return tbl[4];
    const float u = t * 4.0f;
    const int i = (int)u;
    const float f = u - (float)i;
    if (i >= 4) return tbl[4];
    return tbl[i] + (tbl[i + 1] - tbl[i]) * f;
}

// "a,b,c,d,e" → 5 个 float。缺项保持原值，便于只覆盖其中几个锚点。
inline void parse5(const char* s, float out[5]) {
    if (!s || !*s) return;
    const char* p = s;
    for (int i = 0; i < 5 && p && *p; i++) {
        out[i] = (float)atof(p);
        const char* q = strchr(p, ',');
        p = q ? (q + 1) : nullptr;
    }
}

// 自研核参数：CoreEngine 与离线调参入口共用。默认值 = 离线对拍最佳组合。
struct SmoothCfg {
    int radius = beautycore::kSmoothRadius;
    float sigmaSpace = beautycore::kSmoothSigmaSpace;
    float sigmaColor = beautycore::kSmoothSigmaColor;
    int down = 1;
    // 0 = 自动（按硬件并发数，上限 8）；1 = 强制单线程（调参复现用）
    int threads = 0;
    // 只在掩码包围盒内做滤波。关掉它只能用于「测量全帧成本」这类对照实验，
    // 生产路径必须保持开启：关掉后 93% 的算力会花在随后被丢弃的像素上。
    bool roi = true;
    // Sobel 边缘门控的灵敏度。0 = 关闭（退回纯掩码混合）。
    // 灵敏度口径来自一份 MIT 许可的公开实现（PSComposite）：
    //   edge = saturate(|∇luma| * k),  k 默认 10（原实现用 ×10.0）
    //   出处与许可条款见 THIRD-PARTY-NOTICES.md 第 4 节
    // 实测：k=10 时高梯度区磨皮量降到 81.8%，低梯度区 100.0% 不变。
    // 调参见 05_文档/C2_边缘门控与保边核.md
    int edgeK = 10;
    // 五官保护强度 0..100。磨皮权重会乘 (1 - protect × protectK/100)，
    // 0 = 不保护，100 = 五官区域完全不磨。做法同该实现的 uSmoothProtect。
    //
    // 80 → 40 是 C 方案的一部分：原来唇细纹理 96.7%（参考版89.3%），保护压太死。
    // 40 的取舍依据是**眼区**：protectK 0/20/40/60/80 对应眼区
    // 90.3/90.9/91.7/92.6/93.5，而参考版是 91.3 —— 40 恰好落在参考版线上，
    // 再往下眼睛就被磨得比参考版还狠了（见 C15 §3）。
    int protectK = 40;
    // 阈值软膝：|src - smoothed| 超过它就开始减少磨皮，到 2.5 倍时完全不磨。
    //
    // **默认关闭（0）**：实测在我们的实现里几乎没有效果。原因是我们用的是
    // r=2 的双边滤波，本身就很保边 —— 全帧平均 |src-sm| 只有 0.064/255，
    // 连 15 的阈值都够不到，所以 thr=15/30/50 时高差值区的磨皮量都在 99.8%
    // 以上，等于没动。这条在 那份实现里有意义，是因为它用的是大半径高斯，
    // |src-sm| 本来就大。
    //
    // 保留这个能力是为「以后换成更大半径的核」留口子，生产不开。
    int limitThr = 0;

    // ---- 高低频磨皮（频率分离）----
    // 1 = 用高低频（默认），0 = 退回旧的「双边滤波 + 线性混合」。
    // 旧方式的问题：dst = src·(1-t) + blur·t 会把**所有频段**一起削弱 t 倍，
    // 包括脸部的低频结构（明暗层次、骨骼起伏）→ 强度一大就"糊"。
    // 高低频只压「幅度小的高频」（毛孔/痘印），低频从不动。
    int freqMode = 1;
    // 低频尺度 = radius × iters。迭代比放大半径便宜：r=2 迭代 3 次 ≈ 半径 6 的
    // 保边模糊，成本却只有 r=6 的约 1/9（面积比）。
    // lowPath=3（盒式）时这一项不生效，尺度由 boxRadius/boxIters 决定。
    int freqIters = 2;
    // 幅度门控。magHi 从 30 抬到 60 是 C 方案的一部分：换成不保边的盒式低通后，
    // 高频层的幅度整体变大，30 会把痘印/鼻翼阴影一起当成「结构」放过去 ——
    // 实测 magHi 30→60 让额头 90.4%→81.0%、鼻翼 93.4%→85.4%（见 C15）。
    // 眉毛/唇线这些真边缘的幅度远大于 60，仍然被拦住（眼区实测 92.3%，参考版 91.3%）。
    float freqMagLo = 6.0f;  // 幅度小于它：全按强度磨
    float freqMagHi = 60.0f; // 幅度大于它：完全不动（眉毛、鼻梁边、嘴唇轮廓）
    // 颗粒回填：把 4 邻域尺度的极细颗粒扣回一部分。
    // 0.5 → 0.10 也是 C 方案的一部分：它原本是给**双边迭代的阶梯感**打的补丁，
    // 盒式低通没有阶梯；而实测它是「细纹理保留率」的最大单一来源
    // （脸颊 60.2% → 80.2%，见 C15），留着就把刚追上的差距又吐回去了。
    float freqGrain = 0.10f;
    // 低频提取：0 = 双边迭代（快，有阶梯），1 = 引导滤波（无阶梯，O(1)）
    int freqLowMode = 0;
    int guideRadius = 12;
    float guideEps = 400.0f;

    // 低频层的第二条路线（与 freqLowMode 互斥，>0 时优先）：
    //   0 = 旧的 uint8 双边迭代（绝对颜色差、中间结果压回 uint8）—— 默认
    //   1 = 浮点双边（只换精度）
    //   2 = 浮点双边 + 低频参照（权重只看局部起伏）
    //
    // **默认 0 是实测结论，不是保守**：2026-10-28 用基准输出对三条路线做了
    // 逐项对照（05_文档/C8_低频层对照_浮点与参照.md），把那套
    // 「(tap-center)-(refTap-refCenter)」搬到我们 r=2 的核上：
    //   小幅度高频保留 106.0% → 105.4%（几乎不变）
    //   大幅度保留        99.1% → 98.8%（更差）
    //   脸颊台阶像素      59.4% → 66.8%（更差，磨出更多平台边）
    //   成本              6.6 ms → 10.5 ms @640×394（+60%）
    // 四条都更差或持平，所以不投产。代码保留是因为：① 换更大半径的核时它可能
    // 才起作用（该实现等效半径 ≈12px，我们只有 2）；② 它同时也是「ROI 局部
    // 浮点平面」这套基础设施的唯一使用者，删了要重写。
    // 3 = 盒式迭代低通（C 方案，见 C14/C15）：不保边，保边交给幅度门控。
    // **默认 3**（2026-10-28 起）：实测脸颊细纹理保留 86.0% → 64.9%（参考版63.1%），
    // 四区域平均绝对差从 18.6pp 降到 2.0pp。旧的 0/1/2 三条双边路线全部保留，
    // 用 BEAUTY_LOW_PATH 切回去做对照。
    int lowPath = 3;
    // 参照半径（盒式）。8 → 17 像素窗口，约等于「大尺度」而不是「皮肤纹理」。
    int refRadius = 8;
    // lowPath=3 的尺度：等效 σ² = boxIters × ((2·boxRadius+1)² − 1) / 12。
    //
    // 默认 r=2 / iters=2 → σ≈2（两步 5×5 盒式 ≈ 9×9 高斯）。这个值是**按三频带**
    // 选的，不是按细频带：只看细频带的话 r=4/it=2 更贴近参考版（脸颊 65.0 vs 63.1），
    // 但它的中频/阴影会掉到 77.4/94.0，而参考版是 83.8/96.8 —— 中频削太多就是
    // 「脸塌」。r=2/it=2 的三频带平均绝对差 1.92pp，r=4/it=2 是 4.49pp（C15 §3）。
    //
    // 扫参时最反直觉的一条：**r 越大磨得越少**。低通越糊，|src−low| 幅度越大，
    // 越容易被 magHi 门控拦住。别再靠加大半径去追强度。
    int boxRadius = 2;
    int boxIters = 2;
    // 磨皮档位的饱和系数（见 process() 里的 t_eff 公式）。0 = 退回线性。
    float blurSat = 0.16f;

    // ---- 磨皮核模型（3.1.14 / C22）----
    // 0 = legacy：盒式高低频 + 幅度门控 + blendWithMask（3.1.12/3.1.13 默认）
    // 1 = surface3：三层表面模糊，按参考版 GLSL 结构重写（见 C22）
    // **3.1.17 起默认 1**（配合 s3Band=1 组成生产路径）。
    // 依据：真机帧 1920×1080 / 脸宽 529px / 磨皮 100 离线对拍「参考版真值」
    // （加密 bundle、不下发 blur_type = bt2）：
    //     legacy     RMSE 3.251  改动量 1.480  空间相关 0.312  ← 装机旧默认，严重欠磨
    //     band T=14  RMSE 2.280  改动量 2.740  空间相关 0.769
    //     band T=8.3 RMSE ~1.63  改动量 ~1.98  空间相关 ~0.823  ← 新默认
    //     产品真值    —          改动量 2.034
    // 回退：BEAUTY_SMOOTH_MODEL=legacy
    int smoothModel = 1;
    // surface3 的两层半径 / 迭代 / 颜色阈值。默认值只是起点，由离线拟合定。
    // 半径**随档位增长**（C22 §4：真机 50 档等效 r≈6、100 档 r≈10）：
    //     r(t) = s3R0 + (s3R1|s3R2 − s3R0)·t
    int s3R1 = 14, s3It1 = 1;
    int s3R2 = 14, s3It2 = 1;
    int s3R0 = 2;                 // t=0 时的半径（两层共用）
    float s3Thr1 = 0.0f, s3Thr2 = 0.0f;
    // 档位曲线（3.1.14 起改用 GLSL 原式，不再拟合）：
    //     intensity  = mix(t·K, 1.0, step(0.7, t))   ← step 的自变量是**乘之前**的 t
    //     evenfactor = mix(E0, intensity − E0, step(0.6, intensity))
    // 三段形态：[0, 0.42] 恒 0.3；(0.42, 0.70] 线性升到 0.7；[0.70, 1] 恒 0.7。
    // （0.42 = 0.6/1.4286，即 intensity 刚够到 EThr 的档位）
    // 关键含义：t ≥ 0.7 之后 intensity 恒为 1，**75→100 档的差别不可能来自这里**，
    // 只能来自第一级预模糊 —— 这是第 2 批必须补上 pre_blur 的直接原因。
    // 旧的线性拟合式 e = eLo + (eHi−eLo)·t（0.45→0.60）已删除：它是阶段 3 扫参的
    // 产物，与源码结构不符（见 00_先读我.md §4.3、C23 §2.3）。
    float s3K = 1.4286f;          // 强度饱和系数（= 1/0.7，与 step 阈值配套）
    float s3ESat = 0.7f;          // step(0.7, t) 的阈值，用乘之前的 t
    float s3EThr = 0.6f;          // step(0.6, intensity) 的阈值
    float s3E0 = 0.3f;            // evenfactor 的下限，同时也是减数
    // >=0 时强制 evenfactor 取该值（离线扫参／诊断用；生产保持 -1 走原式）。
    // 为什么需要：e 只由 I 决定，于是 I<0.6 的档位 e 一律 0.3，低档纹理
    // 磨削量被压在平坦区的 1/2，而产品的低档反而是纹理先动。
    float s3EFix = -1.0f;
    // 眼睛保护的排除强度（%，100 = 完全按 protect 掩码排除）。
    // 3.1.14 起 protect 只有眼睛：参考版没有 lip 通道，嘴唇当皮肤磨。
    float s3EyeK = 100.0f;

    // ---- 25 tap 表面模糊（第 2 批 #5/#6/#7，C23 §3）----
    // 0 = 旧的盒式两层（boxRgbRoi，半径随档位 + 按脸宽 fw/532 归一化）
    // 1 = 25 tap 表面模糊，固定 UV 半轴，串行两趟 yFactor 40→20
    // 用 BEAUTY_S3_TAP25=1 打开。**定稿前不翻默认**（默认仍是 0，legacy 与
    // 现有 surface3 的输出都不动）。
    int s3Tap25 = 0;
    // UV 半轴【实测】0a″ 光栅实验：四画幅 σx = 1.95/3.85/7.72/11.20 px，
    // 拟合 σx ∝ W^0.98 → 核定义在 UV 空间。这两个常量**不是拟合值**，别按
    // 画幅或脸宽去改它们 —— 改了就退回拟合路线（C23 §8.4 明令禁止）。
    float s3Sx = 0.002640845f;
    float s3Sy = 0.0046875f;
    // 两趟的值域权重系数【实测·标定，非源码】。容限 Δg = 1/sqrt(yFactor)：
    //   pass1 110 → 0.0953；pass2 55 → 0.1348
    //
    // 怎么定的（重要，别当成拟合）：C23 原来记的是 40/20【重建·待证】。用光栅法
    // 量自研引擎的等效核宽，40/20 得到 σx=4.67px，而参考版实测 σx=3.85px —— 核宽
    // 了 21%，过磨的直接原因。扫 yFactor 得到 σx：40→4.67、80→4.10、160→3.54，
    // 对 log2(yFactor) 线性插值到 σ=3.85 得 yFactor≈109。取 110。
    // 验证（不是调参）：像素级改动量 2.73 灰阶 vs参考版 2.71，几乎完全对上；
    // 皮肤内部 RMSE 2.795（盒式）→ 2.417。
    // 注意 yFactor 再往上（160/80）RMSE 只再降 0.056，但在噪声内，且幅度掉到
    // 2.46 明显欠磨 —— 按 RMSE 去挑就是拿基准输出拟合，不做。
    float s3YF1 = 110.0f;
    float s3YF2 = 55.0f;
    // 半分辨率处理【实测】。UV 与分辨率无关，所以它不改变核的物理尺寸。
    int s3Half = 1;

    // ---- 双尺度带通 + 振幅门控（第 2 批-B，C23 §11/§12）----
    // 0 = 关（25 tap / 盒式两条老路径）
    // 1 = surface3RgbBand：B1 窄、B2 宽两个不同尺度的模糊 + 振幅门控
    // **3.1.17 起默认 1**（与 smoothModel=1 配套 = 生产路径）。
    // 为什么要换掉 25 tap：§10.4 频响实测是双凹陷（稀疏环核的旁瓣），§11.4
    // 振幅依赖只有 1.05 倍（参考版2.0 倍）。根因见 §11.5：B1/B2 必须尺度不同。
    // 回退：BEAUTY_S3_BAND=0
    int s3Band = 1;
    // 振幅门控阈值 T（灰阶）。w = 1/(1+(a/T)²)，a = blur(|C.g − B2.g|)。
    // 【3.1.17 重定】原值 14 是按「改动量 2.704」标的，而那个 2.704 来自我们自己
    // 的离线基准 DLL 显式下发 bt=1 的基准输出 —— 不是参考版产品。参考版实测
    // **从不下发 blur_type**，生效档是 bundle 默认 bt2，同帧改动量只有 2.034。
    // 按产品真值重扫（同帧、磨皮 100）：T=14→2.740 / 12→2.529 / 10→2.279 /
    // 9→2.138 / 8→1.979 / 7→1.803。
    // 【3.1.17】先取 8.3：插值后改动量 ≈2.03、RMSE ≈1.63、相关 ≈0.823，与产品
    // 真值 2.034 相差 <0.3%。
    // 【3.1.19】改取 **6.5**：8.3 只对上了总量，对不上分布 —— 产品「越平越磨、
    // 越有结构越保守」，平坦/纹理比 1.39，而 8.3 只有 1.09。T 是唯一能改这个
    // 比值的旋钮（I/e 只等比缩放），降到 6.5 后比值 1.31，掉的总量由 e 补回。
    // 注意方向：T 越小 → gw=1/(1+(a/T)²) 整体越小 → 磨得越轻、纹理保留越多。
    float s3BandT = 6.5f;

    // ---- 门控陡度指数（3.1.19）------------------------------------------
    // gw = 1/(1+(a/T)^gwp)。p=2 是原式；p>2 让「越平越磨、越有结构越保守」
    // 的非对称变强 —— 平坦/纹理比是产品最明显的分布特征（产品 1.39，我方
    // p=2 时只有 1.10）。T 单独调不动这个矛盾：它同时缩放两个分区。
    float s3Gwp = 2.0f;

    // ---- 档位 → (I, T) 查表（3.1.18）------------------------------------
    // 为什么必须查表：band 路径原先只有 I 一条随档位变化的腿，而 I 在 t≥0.7
    // 就饱和到 1（s3ESat），于是 70~100 档输出逐字节相同（死区），且低档
    // 起步过强。产品五档实测（同帧离线、加密 bundle、不下发 blur_type）：
    //     档位  25      50      75      100
    //     改动  0.010   0.402   1.122   1.908     ← 凸曲线，低档几乎不动
    // 我方旧映射（T=8.3）：0.459 / 1.120 / 1.956 / 1.956 —— 低档强 46 倍、
    // 高段压死。归一化后产品的强度占比是 0.005/0.211/0.588/1.0，所以把
    // 强度项改成**按档位查表分段线性**，锚点直接取产品反解值。
    // T 不随档位变：100 档我方 flat/tex = 1.08 而产品 1.06，选择性本来
    // 就对得很齐（8 帧那次的 1.05 vs 1.39 是分类器口径不同造成的误判）。
    // 表可在运行时被 BEAUTY_S3_LV_I / BEAUTY_S3_LV_T 覆盖（离线扫参用）。
    int   lvTable = 1;
    float lvI[5] = {0.000f, 0.020f, 0.240f, 0.577f, 0.976f};
    // 3.1.19：T 由 8.3 → 6.5。T 是唯一能改「平坦/纹理相对强度」的旋钮
    // （I 与 e 只等比缩放）：8.3 时平坦/纹理比 1.09，产品是 1.39。T 降到
    // 6.5 把拐点前移，代价是总量掉 15%，由 e 补回 —— 见下。
    float lvT[5] = {6.5f, 6.5f, 6.5f, 6.5f, 6.5f};
    // evenfactor 也按档位查表。
    // 为什么把 e 从 I 上摘下来：旧式 e 由 I 决定，I<0.6 的档位 e 恒为 0.3，
    // 于是低档只磨平坦区、纹理几乎不动（同帧实测 50 档 flat/tex = 2.17）；
    // 而产品 50 档是 0.96（纹理与平坦一起磨）。e 固定后我方变为
    // 1.18/1.08/1.08，与产品的 0.96/1.11/1.06 同量级。
    // 3.1.19：0.70 → 0.95，与 T=6.5 配对。e 控制「向 B2 混合」的量，也就是
    // 低频压平（平坦区的主要改动来源）；T 压掉纹理后用它把总量补回 1.91。
    float lvE[5] = {0.95f, 0.95f, 0.95f, 0.95f, 0.95f};

    // ---- 清晰（clarity）：大半径局部对比 ----
    // 面板「清晰」100 档线性映射到 0..clarityMax。
    // 上限取 0.5 而不是 1.0：gain=1 时明暗差被原样翻倍，脸会显得像 HDR 过曝。
    // 0.30 是离线扫参的结果：面板 25/50/75/100 对应中频 100.2/101.1/103.0/104.7，
    // 大幅度带 99.7~100.0（不涨 = 边缘没被加亮）。
    float clarityMax = 0.30f;
    // 盒式半径 → (2r+1) 像素窗口。12 = 25px，大于磨皮核（≤8px）才叫「层次」。
    int clarityRadius = 12;
    // ---- 异步检测 ----
    // 1 = 检测跑在 worker 线程（默认），0 = 老的同线程同步检测。
    // 关掉它只为离线对拍：异步会让「第几帧用哪份掩码」依赖线程调度，
    // 逐字节哈希的测试会不稳定。
    bool asyncDetect = true;
    // 异步路径的提交周期（帧）。0 = 每帧都提交。
    //
    // 为什么异步路径要每帧、而同步路径仍然是每 4 帧（见 kMaskEverySync）：
    //   · 异步时检测在 worker 上，渲染线程一分钱不花，只多烧一个核的一部分 ——
    //     20 核机器上 14.1ms/33.3ms ≈ 42% 单核，整机 +1.7 个百分点；
    //   · 同步时检测压在渲染线程上，每帧跑就是每帧 +14ms，那是掉帧，不是省不省的问题。
    // 3.1.8 之前两条路共用 4 帧，真机表现为「掩码拖在脸后面」：
    // 提交周期 5 帧 = 167ms，滞后从 1 帧锯齿漂到 5 帧，而掩码边界是脸框硬切边，
    // 拖影非常显眼。实测 YuNet 只要 14.1ms，慢的从来不是推理，是节流。
    int maskEveryAsync = 0;
    // 软膝：|mid| ≤ lo 全增益，≥ hi 完全不加。
    // 8/22 是扫出来的：软膝放到 20/45 时大幅度带会涨到 105.9（= 边缘被加亮，
    // 也就是光晕）。收到 8/22 后稳定在 100.0 以内，边缘平均改动只有 0.26 灰阶。
    float clarityKneeLo = 8.0f;
    float clarityKneeHi = 22.0f;

    // ---- 细节回填（锐化 / 全局锐化）----
    // 中频带增益基准，与参考版路径的 kMidGain 取同一个值（旧版引擎）。
    // 这样「锐化 50」在我们的引擎里和在参考版里指的是同一条曲线：细频带 100% 回填、
    // 中频带推到原图的 1.65 倍。数值对照见 05_文档/C11_锐化与全局锐化.md。
    float detailMidGain = 3.30f;
    // 细频带软阈值：幅值低于它的高频直接不回填。相机噪点与毛孔在幅值上重叠
    // （±2~6 vs 2~8），只能从最细的一端削掉一点；3.0 是「削噪但不动毛孔」的
    // 折中值，靠 BEAUTY_DETAIL_THR 可现场改。
    float detailThrFine = 3.0f;
    // 3.1.35：磨皮=0 时的「纯锐化」参数（脸内锐化专用，与磨皮>0 的回填路径隔离）。
    // 磨皮>0 时脸内锐化是「回填被磨掉的高频」：dst 已失高频，加回 src 高频 =
    // 把高频补回原值量级，增益走 detailMidGain 那条曲线（沿用历史值，不动）。
    // 磨皮=0 时 dst≈src，语义变成「纯锐化增强」，机制上有两个坑：
    //   (1) 中频带(mid)在磨皮=0 时会对全脸皮肤产生一份固定基底偏移
    //       （实测 100 档 chg 2.74、且 25 档就已有 0.62 的与档位无关的基底），
    //       所以纯锐化模式**关掉中频带**（mid=0），只留细频带。
    //   (2) 细频带默认 thrFine=3.0 的软阈值会把磨皮=0 时幅度本就很小的高频
    //       几乎全削掉（实测纯细频+默认阈值时 100 档 chg 仅 0.078、dret 1.05，
    //       等于没锐化），所以纯锐化模式把阈值放开到 faceSharpThrFine（默认 0）。
    // 放开后细频带才能产生 SDK 原生锐化那种「细节翻倍」的观感（dret→1.6+）。
    // 离线用 BEAUTY_FACE_SHARP_GAIN / BEAUTY_FACE_SHARP_THR 扫参，生产初值 1.0 / 0.0。
    float faceSharpGain = 1.0f;
    float faceSharpThrFine = 0.0f;

    // ---- 全局锐化自己的旋钮（3.1.22 新增，从 detailMidGain / detailThrFine 拆出）----
    // 为什么拆：原来全局锐化与脸内锐化共用同一条中频曲线与同一个细频阈值，
    // 改一个就会动到另一个 —— 两边的档位语义根本不同，没法各自标定。
    // 参照路径（旧版引擎的 applySharpen）里它们本来就是两条独立曲线：
    //     脸内：  am = 锐化/100,          gm = min(锐化/100 × kMidGain, 3.0)
    //     掩码外：aG = 全局锐化/100,     gG = min(全局锐化/100 × kMidGain, 3.0)
    // 拆开后全局这一路就能单独扫参（见 06_离线验证/全项对齐_20261030/
    // sweep_gsharpen.json）。
    float globalMidGain = 3.30f;    // 全局锐化的中频带基准增益（参照 kMidGain）
    float globalFineGain = 1.00f;   // 全局锐化的细频带基准增益（面板 100 = 全额回填）
    // 全局锐化的细频软阈值。生产值 2.00（3.1.22 扫参定档）：
    //   阈值 3.0 → 25 档改动量 −8.1%、100 档 dret 2.478
    //   阈值 2.0 → 25 档改动量 −7.5%、100 档 dret 2.494（参照 2.837）
    //   阈值 1.0 → 25 档改动量 −6.2%、但 100 档改动量 +3.1%（开始过量）
    // 阈值 0 看着 dret 最接近（2.826 vs 2.837），但改动量过 10%（4.623 vs 4.194），
    // 净效果更差 —— 说明我方的细频带本来就比参照强一点，不能再往上加。
    float globalThrFine = 2.00f;    // 全局锐化的细频软阈值（与脸内分开标定）
    float globalThrMid = 0.00f;     // 全局锐化的中频软阈值（默认不削）

    // ---- 磨皮后的「基准细频回填」（3.1.22 新增）----------------------------
    // 为什么要它：单开磨皮 100 档时我方细节保留 dret=0.734，参照=0.834 ——
    // 我方比参照多削了 12% 的高频；同时我方总改动量 2.009 还比参照 1.908 高。
    // 也就是说：我方把能量挪到了**中频**，而不是像参照那样留在低频。
    // I / e / T / gwp 四个旋钮都改不动这个形状（它们让「总量」与「细节保留」
    // 永远反向，见 06_离线验证/全项对齐_20261030/sweep_blur.json）。
    // 真正的解法是把**细频带**按比例还回来：磨皮少了多少高频，就还回去多少。
    // 这不会额外增加改动量 —— 还回去的高频本来就是原图的一部分，
    // 补得越多反而越接近原图（chg 下降）。
    // 只在「用户没开锐化、也没开清晰」时生效（开了就用用户自己的档位）。
    // 生产默认值 0.06（3.1.22 实测定档）。
    // 扫参结论（100 档 / 3 帧均值，参照 chg=1.905 / dret=0.835）：
    //   refill 0.05 → chg 1.914  rmse 1.57  dret 0.838
    //   refill 0.10 → chg 1.913  rmse 1.58  dret 0.839
    //   refill 0.15 → chg 1.913  rmse 1.58  dret 0.841
    //   refill 0.30 → chg 1.913  rmse 1.60  dret 0.850
    //   refill 0.40 → chg 1.916  rmse 1.62  dret 0.858（已超参照，RMSE 变差）
    // 0.05~0.10 是量化吸收后的平台区（dret 基本不动），0.10 起单调上升。
    // 取 0.06：落在平台区中部，dret≈0.838 贴参照 0.835，RMSE 1.57 优于基线 1.58。
    float blurRefill = 0.06f;
    // 回填只在磨皮强度 ≥ 这个值时启用（I 是查表强度，0/25/50/75/100 档
    // 对应 0.00/0.08/0.24/0.58/0.98）。0.3 大致落在 50 与 75 档之间。
    float blurRefillMinI = 0.3f;
    // 1 = 用 delta 模式（加回「原图细频 − dst 细频」）。默认 0，
    // 见 wantBlurRefill 处的实测对比。
    int blurRefillDelta = 0;
    // 基准回填专用的细频软阈值（灰阶）。比锐化那套（3.0）低得多：
    // 磨皮要还的就是「小幅值细频」，锐化要防的却是「噪点被放大」，
    // 两者的取舍正好相反，所以不能共用一个阈值。
    float blurRefillThr = 0.8f;

    // ---- 磨皮强度总增益（3.0.7 新增）------------------------------------
    // 语义：把磨皮**已产出的改动量**整体放大 gain 倍，即
    //     dst' = src + (dst − src) · gain
    // gain=1 时逐字节等于原实现（这条路径直接跳过）。
    //
    // 为什么需要它（用户实测反馈「磨皮 0 → 100 无明显变化」）：
    //   真机帧 1080×1920、面板磨皮 100，人脸区内与源帧的平均绝对差只有
    //   **0.955 灰阶**、峰值 13 灰阶、改动>3 的像素仅 14.2%。这不是接线问题
    //   （日志里 blurType/smoothMode 都已确认送达），是**幅度**问题。
    //   而 I 在 100 档已经是 0.976（查表上限）、e 已经 0.95，两条曲线都顶死了，
    //   想再强只能动合成式本身。
    //
    // 为什么用「事后放大」而不是直接让 I 突破 1：
    //   ① 数学上等价 —— surface3 的合成是 res = c + (even − c)·I，
    //      keepEye=1 时 I>1 与事后乘 gain 是同一个式子；
    //   ② 覆盖面更全 —— 高斯档、legacy 盒式、25 tap、降采样四条老路径
    //      I 根本不是同一条腿（有的压根没有 I），事后统一乘一次就都管住了；
    //   ③ 不动标定 —— lvI/lvE 是按「产品五档强度占比 0.005/0.211/0.588/1.0」
    //      反解出来的，改表就毁掉了那份标定；乘 gain 只缩放幅度、不改形状，
    //      平坦/纹理比（产品 1.39）原样保持。
    // 风险：gain 过大时 (dst−src) 被外推到失真 —— 磨皮区的低频被推过 B2，
    //   表现为「蜡像感 / 边界发灰」。所以上限卡在 3.0，且可用
    //   BEAUTY_SMOOTH_GAIN 现场调。
    // 默认值 1.8（3.0.7 真机帧扫描定档）。定档依据（src.raw 1080×1920，
    // 磨皮 100，人脸区 400–780 × 330–710）：
    //   gain  res(与源帧平均差)  maxd  p>3%   tex(高频保留率)
    //   1.00      1.014           13   16.2%      0.942   ← v3.0.6 现状
    //   1.30      1.308           17   26.0%      0.946
    //   1.60      1.619           21   27.5%      0.950
    //   1.80      1.82            23   ~30%      ~0.953   ← 取这里
    //   2.00      1.976           26   34.2%      0.957
    //   2.40      2.334           31   35.5%      0.998   ← 纹理回涨，开始「加噪」
    // 为什么卡在 1.8 而不是 2.4：res 虽然还在涨，但 tex 从 0.957 跳回 0.998 ——
    // 那不是「磨得更狠」，是 (dst−src) 里的**振幅门控结构**被一起放大到压过了
    // 磨皮本身，观感变成「平了但发毛」。1.8 留了余量，且可用
    // BEAUTY_SMOOTH_GAIN 现场加（上限 3.0，但 2.2 以上自己看画面）。
    // 附带核对：开美白 67 + 清晰 31 时 |gain1.8 − gain1.0| 仍有 0.762 灰阶均值、
    // 6.5% 像素改动 >3，增益不会被美白曲线吃掉。
    float smoothGain = 1.8f;

    // ---- 细节层时序 EMA（0.7.2）----
    // 锐化会把相机白噪一起放大，而噪点帧间独立 → 静止的脸呈「沸腾」式闪动。
    // 把算高通用的亮度平面做一次运动自适应 EMA：静止强平滑、头一动立刻放开。
    // 常量照抄参考版侧已标定过的一组（旧版引擎的 kDetailBetaStill 等）：
    // 静止 beta 0.20（噪声 ×0.38），运动判据 0.15/0.40 对应 rawChg 0.79/1.44。
    //
    // 我们的运动判据换成「全帧亮度帧间差均值（4×4 抽样，单位=灰阶）」，比参考版的
    // rawChg 更直接：相机静止 + 传感器噪声实测在 1~3 之间，真人头动在 8 以上。
    // 阈值 2.5 / 8.0 是这两段之间取的空隙，两侧都有余量。
    bool  emaOn = true;
    float emaStill = 2.5f;       // 帧间差 <= 它 → 按静止（强平滑）
    float emaFull = 8.0f;        // 帧间差 >= 它 → 完全不平滑（零拖影）
    float emaBetaStill = 0.20f;  // 静止时新帧的权重

    // ---- 人脸局部五项（五官立体 / 亮眼 / 去黑眼圈 / 去法令纹 / 祛斑痘）----
    // 增益单位是「灰阶」：面板拉满时椭圆中心处的亮度增量（可用负值=压暗）。
    // 每个值都是先量参考版再定的 —— 用离线基准 DLL 把该项单独拉满、与该项为 0
    // 的输出相减，取连通域的峰值。所以这些数字是「对齐观感」而不是猜的：
    //     五官立体 峰值 +7.3 / -4.7    亮眼 峰值 +44（眼白本就接近饱和）
    //     去黑眼圈 峰值 +7.7           去法令纹 峰值 +3.3
    // 数据与差分图见 06_离线验证/goldens/_probe/ 与 05_文档/C12_人脸局部五项.md
    // 上面那组是**峰值**，直接当增益用会过头 —— 峰值是连通域里最亮的几个点，
    // 而椭圆中心一大片都会拿到满增益。下面这版是按「脸内 dY 均值 + p95」对拍
    // 回调过的（core vs 参考版：立体 +2.97/+6.0 → 目标 +0.64/+5.1，依此类推）。
    // ---- 五官局部三项：3.1.22 按参照差分图重建后的旋钮 --------------------
    //
    // 【亮眼】参照在这批素材上几乎不动（峰值 < 1 灰阶），保持原样不动它。
    float regEyeGain = 12.0f;
    //
    // 【五官立体】= 8 个锚在关键点上的宽椭圆（见 applyFaceRegions 的 threed 表）。
    // 三条关键结论（都是实测出来的，不是猜的）：
    //   1. 参照的立体是**解剖/网格驱动**的：额头恒亮、脖子恒暗，与画面自身
    //      明暗无关。之前用「低频明暗场」路线实测 corr≈0.05（头发/背景污染），
    //      已废弃。
    //   2. 参照的**面部中线 = 双眼中点**，不是 YuNet 的鼻尖 —— 侧脸时鼻尖
    //      会偏 0.12 脸宽，参照的中线不偏。所有中线特征（鼻梁/鼻底/额头）
    //      都锚在 midX/midY 上。
    //   3. 增益分「亮部 / 暗部」两组 feather（亮部 0.705 平台大、暗部 0.498），
    //      因为参照的提亮区是大片平顶、压暗区是窄带。
    //   regThreedGain : 整体比例因子（离线对拍用）。
    float regThreedGain = 1.0f;
    // 【去黑眼圈】参照：两个小块在眼下方，峰值 17.3~18.5，**与暗度无关**
    //   （corr(原亮度, Δ) ≈ +0.1），是纯几何区域 → 双层椭圆（亮核 + 淡晕）。
    //   转头时近侧眼袋在画面上被拉宽 → 用 yaw（鼻尖相对眼中点的偏移）做
    //   透视缩短修正，rx *= 1 − regPouchYawK·yaw·side。
    float regPouchYawK = 1.5611f;     // 3.1.23 重新拟合值（原 2.0）
    float regPouchScale = 1.0f;      // 黑眼圈双层椭圆的整体增益比例
    // 【去法令纹】参照：峰值恒为 **+52**（封顶），且 corr(原亮度, Δ) = −0.4
    //   → 它不是「固定形状 + 固定亮度」，而是**把暗线填平到周围皮肤亮度**：
    //   Δ = min(maxLift, slope·max(0, LR − Y))，LR = 邻域最大亮度。
    //   3.1.23：加**暗线闸门**（gateLo/gateHi）把响应收窄到褶皱线上；
    //   限制因子在 applyFoldRemoval 内部改成 max(皮肤掩码, 区域权重)。
    //   3.1.23 第二轮（双瓣模型 naso6，8 帧统一拟合 fit_naso6.json）：
    //   上瓣参数整体收紧（k 0.60→0.45、闸门 8.5/15.0→19.2/29.9、
    //   LR 半径 6→4、封顶 60→57.5）—— 旧值让上瓣过冲到封顶，
    //   而参照的峰只有 48。
    float regNasoSlope = 0.4500f;    // 每 1 灰阶的「暗差」提亮多少（3.1.23 双瓣重拟合）
    float regNasoMaxLift = 57.5000f; // 封顶（双瓣拟合值；参照实测峰值 51~52）
    int regNasoLrRadius = 4;         // LR = 半径 4 的邻域最大亮度（双瓣拟合值）
    float regNasoGateLo = 19.1526f;   // 暗差 < gateLo → 不起作用
    float regNasoGateHi = 29.8535f;   // 暗差 > gateHi → 完全起作用
    // 祛斑痘：拉回量 = k × 偏离局部均值的幅度。0.25 是拿「参考版的 dY p5/p95
    // = -3.6/+3.5」反推的；0.55 时 core 是 -11/+7，暗斑被打得太狠，痘印会
    // 变成一块块发亮的补丁。再大就会把毛孔一起抹平（那成了第二次磨皮）。
    float regSpotK = 0.25f;
    int regSpotRadius = 2;          // 5×5 窗口。2~4 是斑点尺度，再大就吃到结构
    float regSpotThrLo = 1.4f;      // 小于它算皮肤细纹理，不动
    float regSpotThrHi = 30.0f;     // 大于它算真边缘（睫毛/鼻孔/唇线），不动
};

// 关键点是否可信。
//
// 遮挡（手挡脸、口罩、低头）或大侧脸时，YuNet 仍会输出 5 个点，但位置可能
// 离谱到人脸框外面。直接用它们画五官保护区，椭圆会跑到画面别处，把不该保护
// 的地方保护掉（或者该保护的没保护）。
// 判据：五个点都必须落在人脸框外扩 50% 的范围里 —— 正常侧脸也就贴边，
// 超出这个范围说明这组关键点不可信，宁可这次不做五官保护。
static bool kpsSane(const beautycore::FaceBox& f) {
    if (!f.hasKps) return false;
    const float mx0 = f.x - f.w * 0.5f, mx1 = f.x + f.w * 1.5f;
    const float my0 = f.y - f.h * 0.5f, my1 = f.y + f.h * 1.5f;
    for (int p = 0; p < 5; p++) {
        const float x = f.kps[p * 2], y = f.kps[p * 2 + 1];
        if (x < mx0 || x > mx1 || y < my0 || y > my1) return false;
    }
    return true;
}

// 在缓冲上画一个软边椭圆，权重 0..255，取较大值。
//
// 公式抄 一份 MIT 许可的开源实现（见 THIRD-PARTY-NOTICES.md 第 4 节）的 kComposeShader：
//     weight = (1 - smoothstep(0.35, 1.0, du² + dv²)) × strength
// 注意它把**平方距离**直接喂给 smoothstep（不是 sqrt），这里保持一致。
void paintProtectEllipse(unsigned char* buf, int w, int h,
                         float cx, float cy, float rx, float ry) {
    if (!buf || rx < 0.5f || ry < 0.5f) return;
    int x0 = (int)(cx - rx), x1 = (int)(cx + rx) + 1;
    int y0 = (int)(cy - ry), y1 = (int)(cy + ry) + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    const float irx = 1.0f / rx, iry = 1.0f / ry;
    for (int y = y0; y < y1; y++) {
        const float dv = (y + 0.5f - cy) * iry;
        unsigned char* row = buf + (size_t)y * w;
        for (int x = x0; x < x1; x++) {
            const float du = (x + 0.5f - cx) * irx;
            const float q = du * du + dv * dv;
            if (q >= 1.0f) continue;
            const float t = smoothStep(0.35f, 1.0f, q);
            const int v = (int)((1.0f - t) * 255.0f + 0.5f);
            if (v > (int)row[x]) row[x] = (unsigned char)v;
        }
    }
}
SmoothCfg g_cfg;

// P1 的 GPU 管道开关（3.3.0 起三态，配置项 beauty.ini:gpu_pipe）。
//
// 该阶段**不改画面、只增加耗时**（上传+下载一圈再丢掉），纯粹用于在真实宿主
// 里量搬运耗时并证明管道无损，所以默认不能每帧都跑。
//
// 取值与含义：
//   off  = 完全不跑
//   on   = 每帧都跑（验证用，4K 人脸约 +3.6ms/帧）
//   auto = **首帧自检一次**（默认值）。播一下就能在日志里看到管道到底通不通，
//          之后自动关闭，后续帧零额外开销。
//
// 为什么加 auto：2026-11-06 的教训是「必须用 launch.bat 启动才测得到管道」，
// 而用户从桌面图标启动时环境变量继承不到，日志里一行管道都没有，被误判成
// 管道坏了。默认 auto 之后，正常双击启动、随便播一下就能看到验收那行日志。
enum GpuPipeMode { GPU_PIPE_OFF = 0, GPU_PIPE_ON = 1, GPU_PIPE_AUTO = 2 };

static GpuPipeMode gpuPipeMode() {
    static const GpuPipeMode m = []() -> GpuPipeMode {
        // 1) 环境变量（最高优先级，临时排查用）
        if (const char* e = getenv("BEAUTY_GPU_PIPE")) {
            if (!*e) return GPU_PIPE_AUTO;
            if (e[0] == '0' && e[1] == '\0') return GPU_PIPE_OFF;
            return GPU_PIPE_ON;
        }
        // 2) 配置文件
        if (const char* s = beautyCfgStr("gpu_pipe", nullptr)) {
            if (!_stricmp(s, "off") || !strcmp(s, "0")) return GPU_PIPE_OFF;
            if (!_stricmp(s, "on") || !strcmp(s, "1"))  return GPU_PIPE_ON;
            if (!_stricmp(s, "auto"))                   return GPU_PIPE_AUTO;
        }
        // 3) 自动：首帧自检
        return GPU_PIPE_AUTO;
    }();
    return m;
}

// P2 磨皮的 GPU 开关（3.3.0 的 beauty.ini 体系内，配置项 gpu_smooth）。
//
// **1.0.9.0 起默认 auto**（1.0.8.0 及以前是 off）。改默认值的依据是 2026-11-07 的
// 真实素材目视确认：1080×1920 竖屏、磨皮 94，累计 4928 帧零回落，无色块/断层、
// 发丝与衣领无糊、亮度无偏；同轮实测均合计 4.55ms。
//
// 这条改动的作用范围要看清：它只对「ini 里没写 gpu_smooth」和「首次生成 ini」的用户生效。
// ini 一旦落盘，那一行就写死了，存量用户不会被静默改配置（想换得手动改那一行，
// 或设环境变量 BEAUTY_GPU_SMOOTH）。这是有意的——不去动用户已经跑通的配置。
//
// 选卡只排除 WARP 软件渲染器，**不按厂商过滤**，AMD / Intel 核显同样会走 GPU 路径。
// 这两家没有实测数据（浮点末位、Map 读回性能都可能与 NVIDIA 不同），所以初始化
// 日志会打出适配器型号与厂商（1.0.9.0 加）。万一报「画面不对」或「比 CPU 还慢」，
// 先看那一行，才能分清是硬件差异还是插件缺陷。
//
// 取值：
//   off  = 完全不碰 GPU
//   auto = 用 GPU，任何一步失败（无卡 / 特性等级 < 11.0 / 显存不足）当场回落 CPU（默认）
//   on   = 同 auto，只是语义上表示「已确认，强制走」
//
// auto 与 on 目前行为一致 —— gpuSmoothRgbBand 内部本来就是「失败返回 false」，
// 由调用方回落。留两个名字是为了日志里能看出当前是「试跑」还是「已确认」。
enum GpuSmoothMode { GPU_SMOOTH_OFF = 0, GPU_SMOOTH_ON = 1, GPU_SMOOTH_AUTO = 2 };

static GpuSmoothMode gpuSmoothMode() {
    static const GpuSmoothMode m = []() -> GpuSmoothMode {
        // 1) 环境变量（最高优先级，临时排查用）
        if (const char* e = getenv("BEAUTY_GPU_SMOOTH")) {
            if (!*e) return GPU_SMOOTH_OFF;
            if (e[0] == '0' && e[1] == '\0') return GPU_SMOOTH_OFF;
            return GPU_SMOOTH_ON;
        }
        // 2) 配置文件
        if (const char* s = beautyCfgStr("gpu_smooth", nullptr)) {
            if (!_stricmp(s, "off") || !strcmp(s, "0")) return GPU_SMOOTH_OFF;
            if (!_stricmp(s, "on") || !strcmp(s, "1"))  return GPU_SMOOTH_ON;
            if (!_stricmp(s, "auto"))                   return GPU_SMOOTH_AUTO;
        }
        // 3) 默认：启用（1.0.9.0 起；此前是 off）。失败由调用方回落 CPU，见上面注释
        return GPU_SMOOTH_AUTO;
    }();
    return m;
}

static const char* gpuSmoothModeName() {
    switch (gpuSmoothMode()) {
        case GPU_SMOOTH_ON:   return "on";
        case GPU_SMOOTH_AUTO: return "auto";
        default:              return "off";
    }
}

// 2c 延迟取回的开关（beauty.ini:gpu_smooth_pipeline，1.0.11.0 起）。
//
// 打开后 GPU 磨皮不再在每帧结尾等 GPU 算完，而是把取回推迟一帧：第 N 帧只提交，
// 第 N+1 帧回来取 N 的结果。实测依据见 PROGRESS 1.0.10.2 节——真机 36 个样本里
// 18% 的帧超预算，多出的 6~14ms 全卡在两个 GPU 同步点上，CPU 侧三项纹丝不动。
//
// **代价是 ROI 内的画面晚一帧**（ROI 外仍是当前帧的 src）。真机目视与台架都测过，
// 23.976fps 下不可辨；但如果哪天要做慢动作或单帧精修，把这一行改成 off 就回旧行为。
// 只在「连续播放」时生效：跳帧、拖动播放头、单帧渲染都会自动退回同步路径
// （见 gpu_smooth.cpp 里 PendingResult 的五条前提），所以静止画面与导出首帧不受影响。
//
// 取值：off = 每帧同步取回（旧行为）；auto/on = 允许延迟取回（默认）。
static bool gpuSmoothPipelineOn() {
    static const bool on = []() -> bool {
        // 1) 环境变量（最高优先级，临时排查用）
        if (const char* e = getenv("BEAUTY_GPU_PIPELINE")) {
            if (!*e) return true;
            if (e[0] == '0' && e[1] == '\0') return false;
            return true;
        }
        // 2) 配置文件
        if (const char* s = beautyCfgStr("gpu_smooth_pipeline", nullptr)) {
            if (!_stricmp(s, "off") || !strcmp(s, "0")) return false;
            return true;
        }
        // 3) 默认开
        return true;
    }();
    return on;
}

// 流水线阻断原因的短标签（1.1.1.0）。日志里打数字看不出所以然，打这几个词就一眼明白：
//   ok=走成了 / noprev=没有上一帧 / jump=跳帧或超时 / param=参数变了 /
//   rect=人脸框变了 / motion=框内画面在动（当前素材不适合这条优化）
// 真机排障时 motion 与 jump 的含义完全不同：jump 是正常的（拖播放头），
// motion 说明这条优化在当前素材上根本用不上，该考虑不再尝试。
static const char* pipeBlockTag(int c) {
    switch (c) {
        case 1:  return "noprev";
        case 2:  return "jump";
        case 3:  return "param";
        case 4:  return "rect";
        case 5:  return "motion";
        default: return "ok";
    }
}


// on  ：永远跑。
// off ：永不跑。
// auto：只在「自检还没成功跑过、且本帧已经有掩码」时跑一次。
//       不能按帧号（如 frameNo<=1）判断 —— 首帧进到这里时 updateMask 还没
//       产出包围盒，bounds_ 是空的，按帧号会把唯一那次自检机会白白跳过，
//       日志里就又是一片空白。
//
// 注意本函数有副作用（auto 模式下会把自检标记置位），所以只在渲染路径上
// 调用一次，别放进断言或调试打印里。
bool gpuPipeTakeThisFrame(bool maskReady) {
    static std::atomic<bool> selftestDone{false};

    switch (gpuPipeMode()) {
        case GPU_PIPE_ON:  return true;
        case GPU_PIPE_OFF: return false;
        default: break;
    }
    if (selftestDone.load(std::memory_order_relaxed)) return false;
    if (!maskReady) return false;
    selftestDone.store(true, std::memory_order_relaxed);
    return true;
}

// 日志里说明当前是哪种模式（自检跑完就没了，得在启动时报一次）。
const char* gpuPipeModeName() {
    switch (gpuPipeMode()) {
        case GPU_PIPE_ON:  return "on（每帧）";
        case GPU_PIPE_OFF: return "off";
        default:           return "auto（首帧自检一次）";
    }
}

// 异步掩码的提交计数（诊断用，只增不减）。
// 为什么需要它：提交周期从 4 帧改成每帧，从「每帧毫秒」上完全看不出来 ——
// 检测跑在 worker 上，渲染线程一分钱不多花。唯一直接可测的就是「多少帧提交了几次」。
uint64_t g_maskSubmits = 0;

// 掩码↔帧对齐的计数（3.1.10，诊断用）。
// 为什么需要它：对齐有没有生效、真机上是不是常态，日志一个字都看不出来。
// ratio = alignUsed / alignTotal，30fps 下应 ≈1；worker 追不上时会掉。
uint64_t g_alignUsed = 0;
uint64_t g_alignTotal = 0;

// 掩码参数：人脸框外扩倍数。S1 对拍扫出来 IoU 峰值在 1.15
// （IoU 0.727 / 精确率 0.934 / 召回率 0.767），见 05_文档/S1_掩码对拍结论.md。
const float kFaceBoxGrow = 1.15f;

// 人脸检测的推理输入边长默认值。
//
// 必须保持 640：我们的 YuNet 2023mar ONNX 把输入声明成固定 640x640，ORT 会
// 直接拒绝其它尺寸（`Got: 320 Expected: 640`）。「降到 320 省 4 倍算力」这条
// 路在当前模型上走不通 —— 详见 beauty_face.h 里 setInputSize 的说明。
// 检测提速改走「推理多线程」而不是「降分辨率」。
const int kFaceInputDefault = 640;

// 脖子肤色通道（2026-10-28 新增）
//
// 背景：S1 当时放弃补脖子，理由是「磨皮场景下漏磨脖子比误磨背景更可接受」。
// 用户 2026-10-28 要求「摄像头场景的美颜观感要与主流产品一致」，而主流实现的
// 掩码覆盖到脖子（face_src 上到 y≈332，只磨脸的话停在 232），所以补回来。
//
// 三条硬约束（都是 S1 已经验证过的，别放宽）：
//   * **不能放大矩形框** —— grow 到 2.2 时精确率从 0.934 掉到 0.393，
//     矩形会把背景整块框进来；
//   * 横向只能收窄：半宽取脸宽的 0.30（总宽 0.60 脸宽），比脸框窄一截，
//     这样左右两侧的背景不会被拉进来；
//   * 纵向只到脸高的 0.55，再往下就是衣服；
//   * 依然要求 isSkin —— 白衣服（R≈G≈B，不满足 R>G）和灰背景靠这条挡。
// 参数怎么定的：`06_离线验证/neck_sweep.py` 在 S1 的 8 个有效场景上扫网格，
// 以参考版的掩码当真值（192×256 口径，与 S1 可直接比）：
//     只磨脸           IoU 0.6981
//     半宽0.30/长0.55  IoU 0.7934（初版估值）
//     半宽0.45/长0.65  IoU 0.8442  ← 峰值，取这组
//     半宽0.60/长0.75  IoU 0.7701（横向放到 1.2 倍脸宽开始框进背景）
// 曲面在峰值附近很平（0.40~0.45 × 0.55~0.75 都在 0.83 以上），不是过拟合尖点。
const float kNeckHalfW = 0.45f;   // 半宽 = 脸宽 × 0.45（总宽 0.90 脸宽，仍窄于脸框）
const float kNeckLen = 0.65f;     // 长度 = 脸高 × 0.65

// 脖子「跟随肤色」时的探索范围（3.1.20，g_maskShape 的 bit 4）
//
// 两个数都是**允许范围**而不是**裁剪位置**：边界最终落在肤色真正结束的地方
// （脖子侧面、头发、衣领），这两个数只决定「最多允许探多远」和「探不到时兜底多宽」。
//   * maxHalf 0.70 脸宽：比旧矩形（0.45）宽，用于覆盖真实脖子的完整宽度；
//     仍设上限是因为肤色背景（v_skinbg 场景）会把掩码拉爆，S1 已验证放大矩形
//     精确率会掉到 0.393。
//   * minHalf 0.30 脸宽：肤色被阴影/胡须/镜框打断时的兜底，避免掩码被切碎。
const float kNeckSkinMaxHalf = 0.70f;
const float kNeckSkinMinHalf = 0.30f;

// 掩码边界羽化（3.1.9，修「硬边拖影」；3.1.10 收窄 + 依赖帧对齐）
//
// 问题：异步检测最快也有 1 帧滞后（33ms，worker 忙时会到 2 帧）。肤色那层本来
// 就是软边，但**人脸框是硬裁**（下面 buildMaskInto 直接截 x0..x1 / y0..y1），
// 头部一动，脸后就会留下一条硬边——观感上就是「拖影」。
//
// 做法：在框边缘 F 像素内把权重平滑降到 0。F 是 smoothstep 而不是线性 ——
// 线性斜坡在两端有折角（一阶不连续），大脸近距离看能看到一条「淡等值线」；
// smoothstep 两端导数为 0，看不到折点。
//
// **羽化治不了滞后本身**：滞后 33ms 时边界每帧都在脸后扫动，羽化只是把"硬边扫过"
// 变成"软边扫过"，拖影带还在。3.1.10 的掩码↔帧对齐（见 updateMask / collectAsync）
// 才是把滞后从「掩码落后画面」改成「整帧落后 33ms」—— 对齐之后边界不再扫动，
// 羽化只负责软化矩形裁剪的硬角，所以才能收窄到 8%。
//
// 为什么不做运动预测：预测在急停/反向时会把掩码甩到脸前面（过冲），那是另一种
// 更难看的东西。帧对齐 + 软边比它便宜且稳。
//
// 为什么是 0.08 而不是 0.12：3.1.9 第一版照抄了「12% 脸宽」这个常见默认口径，
// 真机反馈「整体效果弱了较多」—— 我们的掩码不只是脸的椭圆，还有脖子通道（横向
// 0.90 脸宽、一直伸到领口），12% 脸宽的带子打进脖子的横向范围就是 13% 半宽，
// 脸框四条边各削一条带子加起来实测把掩码总能量削掉 10%，而 face_src 离线图上
// 脸不贴框，这个数字被低估了。8% 回到自定的第一版，配合 3.1.10 的「掩码与帧
// 对齐」够用 —— 对齐之后边界不再每帧移动，羽化只需要把矩形裁剪的硬角软化，
// 不再需要补偿滞后。
// （12% 那个口径是骑在椭圆边界上内外各一半的，我们只能向内，等价于更厚的边。）
const float kMaskFeatherRatio = 0.08f;

// 脖子通道的羽化宽度（1.1.3.0）—— **按带长取比例，不按脸宽**。
//
// 真机反馈（2026-11-07）：「胸部靠上有白色长方形遮罩，跟着人走」。
// 像素级取证（BEAUTY_DUMP_OUT 落的 src/dst/mask 三张图）结论：
//   * 那块确实是真皮肤（颈根+锁骨+上胸，src RGB≈(110,110,101)，R≈G>B），
//     不是衣物误判 —— 领口以下的浅绿吊带 (87,120,110) 被正确排除了；
//   * 边长全部是**几何直线**：下缘 y=616 水平、左右都是 cx±0.70 脸宽的竖直边，
//     四条硬边把一块均匀提亮的皮肤围成了「长方形」；
//   * 磨皮 98 把这块皮肤的纹理抹平，美白再提亮 +7~+10，于是像贴了一块白板。
//
// 试过的两条死路（都只用环境变量验过，代码已回退）：
//   * BEAUTY_NECK_LEN=0.90（延长到领口）→ 白块更大。
//     带长 156→216 px，多盖 60 px 上胸，面积 +38%。
//   * BEAUTY_NECK_LEN=0.35（收短到颈根）→ 白块变小但边还在，且出现
//     「脸磨了、脖子没磨」的色差。缩短只是把硬边往上挪，没消掉它。
//   ⇒ 只要下缘是写死的长度，就必然切在皮肤上，缩短/延长都是在
//     「有色差」和「有边」之间来回换。真正的解是把过渡做宽。
//
// 旧值 2*halfW*f.w+0.08 等价于 0.072 脸宽 —— 脸宽 255 px 时只有 18 px，
// 拿 18 px 的斜坡去接 156 px 的带子末端，等于没有过渡，那条边就是这么来的。
// 1.1.3.0 先改成 0.35 带长（156 px 带子 → 55 px 羽化），真机复验仍有「淡淡的边」；
// 1.1.4.0 再提到 0.50：156 × 0.50 = 78 px，但被 kFeatherMax=64 截到 64 px，
// 所以实际只从 55 px 加宽到 64 px（+9 px，约 +16%）。
// 带子的前 50% 仍是满强度，不会让脖子整体「效果变弱」
// （那是 3.1.9 踩过的坑，见上面 kMaskFeatherRatio 的注释）。
// 上限受 kFeatherMax=64 约束：0.90 档的 75 px 同样会被截到 64 px。
const float kNeckFeatherRatio = 0.50f;

// -1 = 按脸宽自适应；0 = 关闭（与 3.1.8 逐字节相同，A/B 用）；>0 = 固定像素
int g_maskFeatherPx = -1;

// 掩码几何形状（3.1.20）。
//
// 问题：旧版把掩码画成**两个矩形**——脸框一个、脖子通道一个。羽化只能把硬边变成
// 软边，边本身还是**直的**：脖子左右两条竖线永远「切」在脖子皮肤上，线外一刀不磨。
// 头部快速摆动时这两条直边在脖子上来回扫，观感就是「遮罩残影」（用户 2026-10-30 反馈）。
//
// 做法：把裁剪边界从矩形换成**椭圆距离场**，边界沿 (x/a)²+(y/b)²=1 连续收敛，
// 没有一段是直的；羽化仍然沿用 kMaskFeatherRatio 的斜坡，所以只是「形状」变了，
// 「软硬」不变。
//
// 位掩码（优先级：BEAUTY_MASK_SHAPE 环境变量 > beauty.ini 的 mask_shape > 下面的默认值）：
//   0 = 矩形（3.1.19 及以前的旧行为）
//   1 = 脸用椭圆
//   2 = 脖子用椭圆
//   3 = 两者都用椭圆
//   4 = 脖子逐行跟随肤色真实边界（脸保持矩形）
//   5 = 脸椭圆 + 脖子逐行跟随肤色
// 注：bit2(4) 与 bit1(2) 同时给时 bit2 优先（见 buildMaskInto 的分支顺序）。
//
// 1.1.1.5 起默认 4。定案过程（2026-11-07 真机）：
//   现象：脖子附近一块白色长方形遮罩，跟着人走，还在闪。
//   ① 磨皮拉到 0，白块仍在 —— 排除磨皮。代码上此时 process 走的是
//      「磨皮为 0」分支，只是 memcpy(dst, src)，整条磨皮链没参与；
//   ② 关掉整个效果，画面正常 —— 确认是插件写的，不是素材或宿主；
//   ③ BEAUTY_NECK_LEN=0 关掉脖子通道，白块完全消失 —— 作用域锁定为脖子通道，
//      即 0.90 脸宽 × 0.65 脸高那条矩形带；
//   ④ 美白/红润走 toneColor，只对 mask 非零处生效（mv==0 直接 continue），
//      于是那条矩形带被整块提亮，边界是直的、位置跟着脸走 = 用户看到的「遮罩」。
// 解药不是关掉脖子（会把脖子磨皮一起丢掉），而是别再用矩形：
// bit2 逐行从中心向两侧探肤色，左右边界落在皮肤/头发/衣领的真实分界上。
int g_maskShape = 4;

// 1.2.9：人脸检测复用（见 buildMaskInto 里的判据）。
// 默认开：导出/慢镜头下能省掉 6.75 ms/次的大头；极快甩头时靠 detAge 兜底重测。
bool   g_detReuse = true;
double g_detThr   = 2.0;    // 64x36 灰度缩略图的平均绝对差阈值（0..255 级）
int    g_detAge   = 4;      // 最多连续复用几次就必须重测
long   g_detReuseHits = 0;  // 诊断：累计复用次数

// 羽化斜坡查表：ramp[dist * 65 + fe] = smoothstep(dist/fe) × 256。
//
// 为什么查表：羽化在 worker 上逐像素跑，1080p 脸框约 40 万像素，每像素至少一次
// 整数除法就是 40 万次（约 10ms），能把这层优化整个吃掉。表只有 65×65 个 int
// （17KB，常驻 L1/L2），首次使用时算一次，之后每像素只剩一次乘法寻址。
const int kFeatherMax = 64;          // 与 buildMaskInto 里的 clamp 上限一致
int g_featherRamp[(kFeatherMax + 1) * (kFeatherMax + 1)];
bool g_featherReady = false;

void initFeatherRamp() {
    for (int fe = 0; fe <= kFeatherMax; fe++) {
        for (int d = 0; d <= kFeatherMax; d++) {
            int v;
            if (fe <= 0 || d >= fe) {
                v = 256;
            } else if (d <= 0) {
                v = 0;
            } else {
                const float t = (float)d / (float)fe;
                v = (int)(t * t * (3.0f - 2.0f * t) * 256.0f + 0.5f);
            }
            g_featherRamp[d * (kFeatherMax + 1) + fe] = v;
        }
    }
    g_featherReady = true;
}

// 距边界 dist 像素处的羽化系数：0（边界）→ 256（dist >= fe）。
// 必须在 initFeatherRamp() 之后调用（ensureReady 里初始化，早于任何掩码构建）。
inline int featherRamp(int dist, int fe) {
    if (fe <= 0) return 256;
    if (!g_featherReady) return 256;   // 表没建好就当没羽化，退回 3.1.8 行为
    if (fe > kFeatherMax) fe = kFeatherMax;
    if (dist <= 0) return 0;
    if (dist >= fe) return 256;
    if (dist > kFeatherMax) dist = kFeatherMax;
    return g_featherRamp[dist * (kFeatherMax + 1) + fe];
}

// 脖子通道参数（离线扫参用）。生产路径不设这两个环境变量，走上面的默认值。
// 与 BEAUTY_FACE_MODEL 同一套写法：只在 ensureReady 里读一次。
struct NeckCfg {
    float halfW = kNeckHalfW;
    float len = kNeckLen;
    // 下缘收口深度（1.1.6.0）：末端不再是 y=常数 的水平截断，改成按列收的圆弧。
    // 取值是**带长的比例**：0 = 保持水平截断（1.1.5.0 及以前的行为），
    // 0.30 = 正中央仍到 0.65 脸高、两侧 |dx|=0.45 脸宽处收到 0.65×(1−0.30)=0.455 脸高。
    // 为什么必须改几何：像素取证量到最长一段下缘 120 列共用同一条 y、全画面 70.3%
    // 的掩码列落在常数 y 上；同段沿列平均的亮度剖面里，距下缘 64 行 +4.87 级、
    // 8 行 +0.26 级，逐行变化最大 0.60 级 —— 亮度过渡早就摊平到看不见台阶，
    // 剩下的「淡淡的边」是**共线**造成的几何感，加宽羽化治不了（1.1.3.0/1.1.4.0 已证）。
    float taper = 0.30f;
    // 下缘跟随肤色（1.1.7.0）：1 = 每列的下缘钉在**这一列最后一个肤色像素**上，
    // 但不超过 taper 给出的几何上限；0 = 保持 1.1.6.0 的几何弧。
    //
    // 为什么光靠几何弧不够：1.1.6.0 的真图取证显示弧确实生效了（下缘 577→670→602，
    // 矢高 93 px），但中央 x1004..1190 共 186 列只变 35 px，肉眼仍然是一条直线；
    // 更关键的是这条弧**横切在皮肤中间**（绿线落在锁下一指宽的皮肤上，吊带边在其下
    // 约 20 px），边界两侧都是同一块皮肤，所以无论弧多弯，磨与不磨的分界都看得见。
    // 肤色末端的落点才是真实的皮肤/衣物轮廓，那里本来就有明暗变化，边界藏得进去。
    //
    // 只缩不扩：depthEff = min(几何深, 肤色末端深)，永不越过 taper 的弧，
    // 因此不会重现 1.1.5.0 之前「掩码盖到衣领上」的白块。
    int skinEnd = 1;
};
NeckCfg g_neck;

// 自身 DLL 所在目录（找模型用）
std::string selfDir() {
    char path[MAX_PATH] = {0};
    HMODULE hm = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&beautyCoreSetSmoothParams, &hm);
    if (hm) GetModuleFileNameA(hm, path, MAX_PATH);
    std::string s(path);
    const size_t p = s.find_last_of("\\/");
    return (p == std::string::npos) ? std::string(".") : s.substr(0, p);
}

// YCbCr 肤色判据（与旧版引擎的 applyWhiten 同一套阈值）
// A/B 开关：true = 旧的 YCbCr 矩形窗 + 硬 0/255（改造前行为），
// false = 椭圆 + 软边 + 亮度门控（默认）。只用于离线对比，生产不设环境变量。
bool g_skinHard = false;

// 肤色椭圆的**中心与半径**（C20）。
//
// 原来这两个中心是写死的常量（抄自那份 MIT 实现）。它是相机在标准
// 白平衡下统计出来的，可摄像头不保证白平衡 —— C20 实测：真机帧平均 RGB
// 151/118/134（B > G，白平衡偏冷），整张脸被推出椭圆，皮肤掩码只剩 41%，
// 磨皮强度掉到参考版的 1/8。而离线素材 face_src 在同一判据下是 99%。
// 同一个核参数只因素材白平衡不同就差 2.8 倍，此前所有离线调参都建立在
// 「掩码 ≈ 95%」这个失真前提上。
//
// 修法：中心改为**逐帧从画面本身估计**（脸框中心区色度中位数），半径保留常量。
// 关键性质：在 face_src 这种标准白平衡素材上，估计出的中心 (−0.063, 0.096)
// 与默认 (−0.065, 0.095) 几乎重合 —— 所以开启自适应不会推翻此前结论。
float g_skinCb = -0.065f;         // 椭圆中心（逐帧估计，这里是初值/兜底值）
float g_skinCr = 0.095f;
float g_skinRu = 0.075f;          // 椭圆半径（C20 方案 B2 的兜底旋钮）
float g_skinRv = 0.070f;
bool  g_skinAdapt = true;         // 自适应中心开关（BEAUTY_SKIN_ADAPT=0 关闭）
float g_skinAdaptEma = 1.0f;      // 帧间平滑系数，**默认 1.0 = 纯函数无状态**
bool  g_skinAdaptValid = false;   // 上一帧是否取得有效中心（EMA 初始化用）

// 「估计首次收效」的一次性上报标志（C20 真机自查）。3.1.13 首版只在 init 时
// 打了一行 skin=，那一行永远是"(center=default, not yet estimated)" —— 用户
// 拿着日志没法回答「第 2 帧以后估计真的成功了？」。worker 第一次把估计结果
// 写进 g_skinCb/g_skinCr 时置位，渲染线程在 video_render 里取走并打一行。
// panel 只在 init 调 beautyCoreSkinInfo()、没有周期性 re-log，这是补的出口。
std::atomic<bool> g_skinAnnounce{false};

// 为什么 EMA 默认必须是 1.0：任何 <1 的系数都会让中心携带历史，同一张图
// 重复处理（ROI 对拍、多次 BeautyCoreRun）得到不同掩码 —— verify_roi 实测
// maxdiff=1（EMA=0.15），EMA=0 或 1 时 ALL PASS。确定性优先于平滑。
// 真机若观察到磨皮强度逐帧闪，可设 BEAUTY_SKIN_EMA=0.2 压住抖动
// （代价是失去确定性，只在真机场景用）。

// 从画面估计本帧肤色色度中心。
//
// 采样区 = 最大脸框的中心 40%×40%（避开背景、头发、额头镜面高光）；
// 迭代两次「取中位数 → 用新中心剔除离群 → 再取中位数」—— 实测迭代 1 次即收敛，
// 第 2 次只是防极端光照下的抖动。
//
// 返回 false = 采样不足（戴口罩、手挡脸、极暗），调用方保持默认/上一帧中心。
bool estimateSkinCenter(const unsigned char* src, int w, int h,
                        const std::vector<beautycore::FaceBox>& faces,
                        float& outCb, float& outCr, bool flipY) {
    if (faces.empty()) return false;
    // 多脸时取最大的那张 —— 主脸的肤色才是要磨的对象，小脸样本少噪声大
    const beautycore::FaceBox* best = &faces[0];
    for (const auto& f : faces)
        if (f.w * f.h > best->w * best->h) best = &f;

    const float fx0 = best->x + best->w * 0.30f;
    const float fx1 = best->x + best->w * 0.70f;
    // 视觉 0.40~0.80 脸高那一段（脸颊/鼻/嘴，避开眼睛、额头高光与下巴阴影）。
    // v3.0.9：bottom-up 下缓冲行号是反的，直接 +0.40h 采到的是**额头与头发**，
    // 色度中位数被拉走 → 肤色椭圆整体偏移、掩码变薄（「效果不明显」的隐形推手）。
    const float fy0 = best->y + best->h * (flipY ? 0.20f : 0.40f);
    const float fy1 = best->y + best->h * (flipY ? 0.60f : 0.80f);
    int x0 = (int)fx0, x1 = (int)fx1, y0 = (int)fy0, y1 = (int)fy1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    const int bw = x1 - x0, bh = y1 - y0;
    if (bw < 8 || bh < 8) return false;

    // 采样步长：控制采样点在 8000 以内，保证每 4 帧一次的这个 pass 足够便宜
    int step = 1;
    while ((bw / step) * (bh / step) > 8000) step++;
    if (step > 8) step = 8;

    std::vector<float> cbs, crs;
    cbs.reserve(8192);
    crs.reserve(8192);
    // 先按亮度粗筛一次并缓存色度，后面两轮迭代只做剔除、不再重算 RGB→YCbCr
    std::vector<float> cbAll, crAll;
    cbAll.reserve(8192);
    crAll.reserve(8192);
    std::vector<unsigned char> keep;
    keep.reserve(8192);
    for (int y = y0; y < y1; y += step) {
        const unsigned char* row = src + ((size_t)y * w) * 4;
        for (int x = x0; x < x1; x += step) {
            const int i = x * 4;
            const float r = row[i] * (1.0f / 255.0f);
            const float g = row[i + 1] * (1.0f / 255.0f);
            const float b = row[i + 2] * (1.0f / 255.0f);
            const float yv = 0.299f * r + 0.587f * g + 0.114f * b;
            // 暗部与过曝区不参与统计（鼻孔、发缝、镜面高光）
            if (yv < 0.12f || yv > 0.94f) continue;
            cbAll.push_back(-0.168736f * r - 0.331264f * g + 0.5f * b);
            crAll.push_back(0.5f * r - 0.418688f * g - 0.081312f * b);
        }
    }
    const size_t n = cbAll.size();
    const size_t need = (size_t)(((float)bw / step) * ((float)bh / step) * 0.30f);
    if (n < 64 || n < need) return false;   // 命中太少 → 不信任这个中心

    keep.assign(n, 1);
    float ccB = g_skinCb, ccR = g_skinCr;
    for (int iter = 0; iter < 2; iter++) {
        cbs.clear();
        crs.clear();
        for (size_t i = 0; i < n; i++) {
            if (!keep[i]) continue;
            cbs.push_back(cbAll[i]);
            crs.push_back(crAll[i]);
        }
        if (cbs.size() < 32) return false;
        // 中位数（nth_element 是 O(n)，比完整排序便宜）
        const size_t mid = cbs.size() / 2;
        std::nth_element(cbs.begin(), cbs.begin() + mid, cbs.end());
        std::nth_element(crs.begin(), crs.begin() + mid, crs.end());
        ccB = cbs[mid];
        ccR = crs[mid];
        if (iter == 1) break;
        // 用新中心剔除离群点，下一轮再取一次中位数
        for (size_t i = 0; i < n; i++) {
            const float du = (cbAll[i] - ccB) / g_skinRu;
            const float dv = (crAll[i] - ccR) / g_skinRv;
            keep[i] = (du * du + dv * dv < 2.25f) ? 1 : 0;   // d < 1.5
        }
    }
    outCb = ccB;
    outCr = ccR;
    return true;
}

// 肤色权重，返回 0..255 的**连续值**（不是布尔）。
//
// 判据与参数抄自 同一份 MIT 实现的 skinMask：
//   - 在 CbCr **色度平面上画椭圆**（中心 -0.065/+0.095，半径 0.075/0.070），
//     距离 0.6~1.3 之间用 smoothstep 软过渡；
//   - 再乘亮度门控 smoothstep(0.12, 0.30, Y)：暗部不算皮肤，避免把阴影、
//     鼻孔、发缝当成肤色。
//
// 为什么换掉原来的矩形窗：
//   1) 矩形窗在 (Cb,Cr) 平面的**四个角**会误判 —— 那些组合真实肤色里不存在；
//   2) 硬 0/255 让磨皮强度在掩码边界**瞬间跳变**，观感上就是鼻梁侧面、
//      下颌线出现台阶状硬边。软边把跳变摊开成过渡带。
inline int skinWeight(int R, int G, int B) {
    if (g_skinHard) {
        // 改造前的判据，原样保留以便 A/B 对拍
        const int Y = (66 * R + 129 * G + 25 * B + 128) >> 8;
        const int Cb = ((-38 * R - 74 * G + 112 * B + 128) >> 8) + 128;
        const int Cr = ((112 * R - 94 * G - 18 * B + 128) >> 8) + 128;
        const bool ok = (Cr >= 133 && Cr <= 173 && Cb >= 77 && Cb <= 127 &&
                         R > G && G > B && R > 80 && Y > 60 && Y < 235);
        return ok ? 255 : 0;
    }
    const float r = R * (1.0f / 255.0f);
    const float g = G * (1.0f / 255.0f);
    const float b = B * (1.0f / 255.0f);
    const float y = 0.299f * r + 0.587f * g + 0.114f * b;
    const float cb = -0.168736f * r - 0.331264f * g + 0.5f * b;
    const float cr = 0.5f * r - 0.418688f * g - 0.081312f * b;

    // 椭圆中心来自 g_skinCb/g_skinCr：默认常量，或由 estimateSkinCenter()
    // 逐帧从画面估计（C20）。半径 g_skinRu/g_skinRv 是方案 B2 的兜底旋钮。
    const float du = (cb - g_skinCb) / g_skinRu;
    const float dv = (cr - g_skinCr) / g_skinRv;
    const float d = sqrtf(du * du + dv * dv);

    float m = 1.0f - smoothStep(0.6f, 1.3f, d);
    // 暗部不算皮肤（避免把阴影、鼻孔、发缝当肤色）；
    // 同理过曝区也不算 —— 鼻梁、额头的镜面高光如果被磨平，会变成一块死白。
    m *= smoothStep(0.12f, 0.30f, y) * (1.0f - smoothStep(0.94f, 0.99f, y));
    if (m <= 0.0f) return 0;
    const int v = (int)(m * 255.0f + 0.5f);
    return (v > 255) ? 255 : v;
}

inline bool isSkin(int R, int G, int B) {
    return skinWeight(R, G, B) > 0;
}

// 毫秒级单调时钟（GetTickCount64 只有 15.6ms 精度，量几毫秒的核不够用）
double nowMsCore() {
    static LARGE_INTEGER freq = {0};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
}

// ---------------------------------------------------------------------------
// 诊断导出（1.1.2.1）：真机「白块」的像素级定性
//
// 起因：真机反馈「脖子附近有一块白色方形遮罩，跟着人走」。日志能给出掩码占比，
// 但给不出像素 —— 那块到底是「真皮肤被美白提亮」还是「米色衣物被判成皮肤」，
// 两者修法完全相反（前者加长下缘羽化，后者把脖子从色调里剔除），靠推理分不开。
//
// 格式选 PPM(P6)/PGM(P5) 而不是 PNG：零依赖、零链接，Pillow / ffmpeg / 浏览器
// 都能直接读，也不给插件增加任何第三方库。
//
// 行序按 imgBottomUp_ 翻正，所以落盘的图「看起来就是正的」，三张图同尺寸同朝向，
// 可以直接叠图比对。
//
// 只在 process() 末尾、且环境变量 BEAUTY_DUMP_OUT 非空时调用 —— 不设变量时这段
// 连一次判断都不做，对画面逐字节零影响。
// ---------------------------------------------------------------------------
static void dumpBgraPpm(const char* path, const unsigned char* px, int w, int h, bool flip) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    std::vector<unsigned char> row((size_t)w * 3);
    for (int y = 0; y < h; y++) {
        const int sy = flip ? (h - 1 - y) : y;
        // 像素顺序是 R,G,B,A（skinWeight(row[x*4+0..2]) 按 R,G,B 传参可证）
        const unsigned char* p = px + (size_t)sy * (size_t)w * 4;
        for (int x = 0; x < w; x++) {
            row[(size_t)x * 3 + 0] = p[x * 4 + 0];
            row[(size_t)x * 3 + 1] = p[x * 4 + 1];
            row[(size_t)x * 3 + 2] = p[x * 4 + 2];
        }
        fwrite(&row[0], 1, row.size(), f);
    }
    fclose(f);
}

static void dumpGrayPgm(const char* path, const unsigned char* m, int w, int h, bool flip) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P5\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++) {
        const int sy = flip ? (h - 1 - y) : y;
        fwrite(m + (size_t)sy * (size_t)w, 1, (size_t)w, f);
    }
    fclose(f);
}

// ---------------------------------------------------------------------------
// CoreEngine —— 自研引擎（S2 起逐步成型）
//
// 第一版只做磨皮一件事，这样对拍最干净：输入原图 → 自研双边滤波 → 按档位线性
// 混合。掩码、美白、红润、细节回填随后逐个接进来 —— 每接一个都在基准上
// 对拍一次，而不是一次性写完再看差多少。
//
// 参数映射：面板磨皮 0..100 → 混合系数 t = 档位/100。与参考版侧
// bgraToRgbaBlend 的定义完全一致，所以两边同档位可以直接比。
// ---------------------------------------------------------------------------
class CoreEngine : public IBeautyEngine {
public:
    const char* id() const override { return "core"; }
    const char* version() const override { return beautyBuildStr(); }
    // GPU 执行提供器名：人脸检测（YuNet/ONNX）实际使用的执行提供器，
    // 自动检测为 "DirectML"（onnxruntime-directml 构建版）或 "CPU"。
    const char* gpuProvider() const override { return face_.provider(); }

    bool ensureReady(std::string& err) override {
        // 人脸检测模型只试一次。失败不阻止引擎就绪 —— 降级为「无掩码，全帧磨皮」，
        // 与参考版在 region=off 时的行为一致，不会让滤镜整体失效。
        if (!faceTried_) {
            faceTried_ = true;
            const std::string d = selfDir();
            // 必须告诉 ORT 去哪找自己的 DLL：系统里可能另有别的版本
            beautycore::onnxSetRuntimeDir(d.c_str());
            std::string mp;
            const char* envp = getenv("BEAUTY_FACE_MODEL");
            if (envp && *envp) {
                mp = envp;
            } else {
                mp = d + "\\models\\yunet.onnx";
                FILE* probe = nullptr;
                if (fopen_s(&probe, mp.c_str(), "rb") != 0 || !probe) {
                    mp = d + "\\yunet.onnx";
                } else {
                    fclose(probe);
                }
            }
            std::string ferr;
            faceOk_ = face_.load(mp.c_str(), ferr);
            faceErr_ = faceOk_ ? std::string() : (mp + " : " + ferr);
            if (faceOk_) {
                // 推理输入边长：320 比 640 快约 4 倍（像素数差 4 倍）。
                // 代价是小脸检出率下降 —— 默认值由离线对拍定，环境变量留给复验。
                int isz = kFaceInputDefault;
                const char* fenv = getenv("BEAUTY_FACE_INPUT");
                if (fenv && *fenv) isz = atoi(fenv);
                face_.setInputSize(isz);
            }
            // 脖子通道参数（离线扫参用；生产不设环境变量）
            const char* nhw = getenv("BEAUTY_NECK_HALFW");
            if (nhw && *nhw) g_neck.halfW = (float)atof(nhw);
            const char* nln = getenv("BEAUTY_NECK_LEN");
            if (nln && *nln) g_neck.len = (float)atof(nln);
            // 下缘收口（1.1.6.0）：优先级「BEAUTY_NECK_TAPER > beauty.ini 的
            // neck_taper > 默认 0.30」。taper=0 走旧的水平截断，用于逐字节回归；
            // 0.30 是本次默认。上限 0.80，再大两侧会缩到 0.13 脸高、等于砍掉脖子。
            if (const char* s = beautyCfgStr("neck_taper", "")) {
                if (s && *s) g_neck.taper = (float)atof(s);
            }
            const char* ntp = getenv("BEAUTY_NECK_TAPER");
            if (ntp && *ntp) g_neck.taper = (float)atof(ntp);
            if (g_neck.taper < 0.0f) g_neck.taper = 0.0f;
            if (g_neck.taper > 0.80f) g_neck.taper = 0.80f;
            // 下缘跟随肤色（1.1.7.0）：优先级「BEAUTY_NECK_SKINEND > beauty.ini 的
            // neck_skinend > 默认 1」。置 0 时逐像素回落到 1.1.6.0 的几何弧，
            // 用来做逐字节回归 —— 两条路径必须给出完全相同的掩码。
            if (const char* s = beautyCfgStr("neck_skinend", "")) {
                if (s && *s) g_neck.skinEnd = atoi(s);
            }
            const char* nse = getenv("BEAUTY_NECK_SKINEND");
            if (nse && *nse) g_neck.skinEnd = atoi(nse);
            g_neck.skinEnd = (g_neck.skinEnd != 0) ? 1 : 0;
            if (const char* s = getenv("BEAUTY_DET_REUSE")) g_detReuse = (atoi(s) != 0);
            if (const char* s = getenv("BEAUTY_DET_THR")) { double v = atof(s); if (v >= 0.0) g_detThr = v; }
            if (const char* s = getenv("BEAUTY_DET_AGE")) { int v = atoi(s); if (v >= 1 && v <= 64) g_detAge = v; }
            // 边缘门控灵敏度（0 = 关闭）。生产默认 10，这里只为离线复验留口子。
            const char* ek = getenv("BEAUTY_EDGE_K");
            if (ek && *ek) g_cfg.edgeK = atoi(ek);
            // 肤色判据 A/B：BEAUTY_SKIN_HARD=1 回到旧矩形窗（离线对比用）
            const char* sh = getenv("BEAUTY_SKIN_HARD");
            g_skinHard = (sh && sh[0] == '1');
            // 肤色椭圆中心自适应（C20 方案 A）：BEAUTY_SKIN_ADAPT=0 回到固定中心。
            // 默认开 —— 固定中心在白平衡偏冷的摄像头上会把掩码压到 34%。
            const char* sa = getenv("BEAUTY_SKIN_ADAPT");
            if (sa && *sa) g_skinAdapt = (sa[0] != '0');
            // 半径缩放（C20 方案 B2 兜底）：BEAUTY_SKIN_RADIUS=1.5 放宽 1.5 倍。
            // 自适应失效时的保底手段，代价是暖色背景误判增加。
            const char* sr = getenv("BEAUTY_SKIN_RADIUS");
            if (sr && *sr) {
                const float k = (float)atof(sr);
                if (k > 0.1f) { g_skinRu = 0.075f * k; g_skinRv = 0.070f * k; }
            }
            // 中心帧间平滑系数（0 = 不平滑，1 = 完全跟随本帧）
            const char* se = getenv("BEAUTY_SKIN_EMA");
            if (se && *se) g_skinAdaptEma = (float)atof(se);
            // 五官保护强度 A/B：BEAUTY_PROTECT_K=0 关闭（离线对比用）
            const char* pk = getenv("BEAUTY_PROTECT_K");
            if (pk && *pk) g_cfg.protectK = atoi(pk);
            // 阈值软膝 A/B：BEAUTY_LIMIT_THR=0 关闭
            const char* lt = getenv("BEAUTY_LIMIT_THR");
            if (lt && *lt) g_cfg.limitThr = atoi(lt);
            // 高低频磨皮：BEAUTY_FREQ=0 退回旧的「双边+线性混合」做 A/B
            const char* fq = getenv("BEAUTY_FREQ");
            if (fq && *fq) g_cfg.freqMode = (fq[0] != '0');
            const char* fi = getenv("BEAUTY_FREQ_ITERS");
            if (fi && *fi) g_cfg.freqIters = atoi(fi);
            const char* fg = getenv("BEAUTY_FREQ_GRAIN");
            if (fg && *fg) g_cfg.freqGrain = (float)atof(fg);
            // 低频提取方式：BEAUTY_LOW_MODE=1 用引导滤波
            const char* lm = getenv("BEAUTY_LOW_MODE");
            if (lm && *lm) g_cfg.freqLowMode = atoi(lm);
            const char* gr = getenv("BEAUTY_GUIDE_R");
            if (gr && *gr) g_cfg.guideRadius = atoi(gr);
            const char* ge = getenv("BEAUTY_GUIDE_EPS");
            if (ge && *ge) g_cfg.guideEps = (float)atof(ge);
            // 低频层路线：BEAUTY_LOW_PATH=0 退回旧的 uint8 双边迭代，1 = 只换浮点，
            // 2 = 浮点 + 低频参照（默认）。三个值对应三组对照数据。
            const char* lp = getenv("BEAUTY_LOW_PATH");
            if (lp && *lp) g_cfg.lowPath = atoi(lp);
            const char* lr = getenv("BEAUTY_REF_R");
            if (lr && *lr) g_cfg.refRadius = atoi(lr);
            // C 方案（lowPath=3）的低频尺度：等效 σ² = iters × ((2r+1)²−1)/12
            const char* br = getenv("BEAUTY_BOX_R");
            if (br && *br) g_cfg.boxRadius = atoi(br);
            const char* bi = getenv("BEAUTY_BOX_ITERS");
            if (bi && *bi) g_cfg.boxIters = atoi(bi);
            // 档位饱和：BEAUTY_BLUR_SAT=0 退回线性（对比用）
            const char* bs = getenv("BEAUTY_BLUR_SAT");
            if (bs && *bs) g_cfg.blurSat = (float)atof(bs);
            // ---- surface3 三层磨皮（C22；默认关，只走环境变量）----
            // BEAUTY_SMOOTH_MODEL=surface3|1 打开；不设或 =legacy|0 走旧核。
            const char* smodel = getenv("BEAUTY_SMOOTH_MODEL");
            if (smodel && *smodel)
                g_cfg.smoothModel = (smodel[0] == 's' || smodel[0] == 'S' ||
                                     smodel[0] == '1') ? 1 : 0;
            const char* s3r1 = getenv("BEAUTY_S3_R1");
            if (s3r1 && *s3r1) g_cfg.s3R1 = atoi(s3r1);
            const char* s3i1 = getenv("BEAUTY_S3_IT1");
            if (s3i1 && *s3i1) g_cfg.s3It1 = atoi(s3i1);
            const char* s3r2 = getenv("BEAUTY_S3_R2");
            if (s3r2 && *s3r2) g_cfg.s3R2 = atoi(s3r2);
            const char* s3r0 = getenv("BEAUTY_S3_R0");
            if (s3r0 && *s3r0) g_cfg.s3R0 = atoi(s3r0);
            const char* s3i2 = getenv("BEAUTY_S3_IT2");
            if (s3i2 && *s3i2) g_cfg.s3It2 = atoi(s3i2);
            const char* s3t1 = getenv("BEAUTY_S3_THR1");
            if (s3t1 && *s3t1) g_cfg.s3Thr1 = (float)atof(s3t1);
            const char* s3t2 = getenv("BEAUTY_S3_THR2");
            if (s3t2 && *s3t2) g_cfg.s3Thr2 = (float)atof(s3t2);
            const char* s3k = getenv("BEAUTY_S3_K");
            if (s3k && *s3k) g_cfg.s3K = (float)atof(s3k);
            // 3.1.14：e 模型换成 GLSL 原式，旧旋钮 EHI/EOFF 已废弃（不再解析）。
            // ELO 语义等价于新的 E0（evenfactor 下限），保留为别名，免得老脚本
            // 静默失效 —— 但注意 ELO 现在**只影响下限**，不再影响高段斜率。
            const char* s3e0 = getenv("BEAUTY_S3_E0");
            if (s3e0 && *s3e0) g_cfg.s3E0 = (float)atof(s3e0);
            const char* s3el = getenv("BEAUTY_S3_ELO");
            if (s3el && *s3el) g_cfg.s3E0 = (float)atof(s3el);
            const char* s3es = getenv("BEAUTY_S3_ESAT");
            if (s3es && *s3es) g_cfg.s3ESat = (float)atof(s3es);
            const char* s3et = getenv("BEAUTY_S3_ETHR");
            if (s3et && *s3et) g_cfg.s3EThr = (float)atof(s3et);
            const char* s3ek = getenv("BEAUTY_S3_EYEK");
            if (s3ek && *s3ek) g_cfg.s3EyeK = (float)atof(s3ek);
            // 25 tap（第 2 批 #5/#6/#7）。TAP25 是总开关，其余只在它打开时有用。
            const char* s3t25 = getenv("BEAUTY_S3_TAP25");
            if (s3t25 && *s3t25) g_cfg.s3Tap25 = atoi(s3t25);
            const char* s3sx = getenv("BEAUTY_S3_SX");
            if (s3sx && *s3sx) g_cfg.s3Sx = (float)atof(s3sx);
            const char* s3sy = getenv("BEAUTY_S3_SY");
            if (s3sy && *s3sy) g_cfg.s3Sy = (float)atof(s3sy);
            const char* s3y1 = getenv("BEAUTY_S3_YF1");
            if (s3y1 && *s3y1) g_cfg.s3YF1 = (float)atof(s3y1);
            const char* s3y2 = getenv("BEAUTY_S3_YF2");
            if (s3y2 && *s3y2) g_cfg.s3YF2 = (float)atof(s3y2);
            const char* s3hf = getenv("BEAUTY_S3_HALF");
            if (s3hf && *s3hf) g_cfg.s3Half = atoi(s3hf);
            // 双尺度带通（第 2 批-B）。BAND 是总开关，T 只在它打开时有用。
            const char* s3bd = getenv("BEAUTY_S3_BAND");
            if (s3bd && *s3bd) g_cfg.s3Band = atoi(s3bd);
            const char* s3bt = getenv("BEAUTY_S3_BT");
            if (s3bt && *s3bt) g_cfg.s3BandT = (float)atof(s3bt);
            // 档位 → (I, T) 查表（3.1.18）。LV_TABLE=0 退回旧的饱和式 I（对比用）。
            // LV_I / LV_T 形如 "0,0.005,0.205,0.574,0.975"，对应面板 0/25/50/75/100。
            const char* lvt = getenv("BEAUTY_S3_LV_TABLE");
            if (lvt && *lvt) g_cfg.lvTable = atoi(lvt);
            parse5(getenv("BEAUTY_S3_LV_I"), g_cfg.lvI);
            parse5(getenv("BEAUTY_S3_LV_T"), g_cfg.lvT);
            parse5(getenv("BEAUTY_S3_LV_E"), g_cfg.lvE);
            const char* sef = getenv("BEAUTY_S3_E_FIX");
            if (sef && *sef) g_cfg.s3EFix = (float)atof(sef);
            const char* gwp = getenv("BEAUTY_S3_GWP");
            if (gwp && *gwp) g_cfg.s3Gwp = (float)atof(gwp);
            // clarity：BEAUTY_CLARITY_MAX 改上限（离线标定用），BEAUTY_CLARITY_R 改窗口
            const char* cm = getenv("BEAUTY_CLARITY_MAX");
            if (cm && *cm) g_cfg.clarityMax = (float)atof(cm);
            const char* cr = getenv("BEAUTY_CLARITY_R");
            if (cr && *cr) g_cfg.clarityRadius = atoi(cr);
            // 异步检测：BEAUTY_ASYNC_DETECT=0 关掉（离线对拍要确定性）
            const char* ad = getenv("BEAUTY_ASYNC_DETECT");
            if (ad && *ad) g_cfg.asyncDetect = (ad[0] != '0');
            // 帧行序（v3.0.4）：默认 1 = bottom-up（OFX/Resolve）。离线工具若直接
            // 喂 top-down 的 raw，可设 BEAUTY_IMG_BOTTOMUP=0 而不用重编译。
            const char* bu = getenv("BEAUTY_IMG_BOTTOMUP");
            if (bu && *bu) imgBottomUp_ = (bu[0] != '0');
            // 异步提交周期：BEAUTY_MASK_EVERY=4 退回 3.1.7 的节奏（拖影回来，但省 CPU）。
            // 留这个口子是为了让真机能当场对比 0 / 1 / 2 / 4 四档，不用改代码重编译。
            const char* me = getenv("BEAUTY_MASK_EVERY");
            if (me && *me) {
                int v = atoi(me);
                if (v < 0) v = 0;
                g_cfg.maskEveryAsync = v;
            }
            // 掩码边界羽化：BEAUTY_MASK_FEATHER=0 关闭（与 3.1.8 逐字节相同，A/B 用），
            // >0 固定像素；不设则按脸宽自适应（0.12 × 脸宽，对齐常见默认口径）。
            const char* mf = getenv("BEAUTY_MASK_FEATHER");
            if (mf && *mf) {
                int v = atoi(mf);
                g_maskFeatherPx = (v < 0) ? -1 : v;
            }
            // 掩码几何：优先级「环境变量 > beauty.ini 的 mask_shape > 默认 4」。
            // 默认 4 = 脖子逐行跟随肤色（1.1.1.5 起），原因见 g_maskShape 定义处。
            // 0 退回矩形（3.1.19 行为），3 = 脸+脖子都用椭圆，都留给真机 A/B。
            // ini 这一项是回退口：改完重启 Resolve 即生效，不必重编，
            // 也不必在启动器里塞环境变量。
            g_maskShape = (int)beautyCfgInt("mask_shape", g_maskShape);
            const char* msh = getenv("BEAUTY_MASK_SHAPE");
            if (msh && *msh) g_maskShape = atoi(msh);
            initFeatherRamp();   // 羽化查表：必须早于任何一次 buildMaskInto
            // 3.1.10 掩码↔帧对齐：BEAUTY_MASK_ALIGN=0 关闭（回到 3.1.9 的
            // 「当前帧 + 旧掩码」，会拖影但零延迟，A/B 用）。
            const char* ma = getenv("BEAUTY_MASK_ALIGN");
            if (ma && *ma) alignOn_ = (ma[0] != '0');
            asyncOn_ = g_cfg.asyncDetect;
            // 异步检测会和磨皮抢核：留一个核给 worker，否则磨皮那一帧会被拖慢
            // （实测 1080p 非检测帧从 6.4ms 涨到 13.7ms，等于尖峰换了个形状）。
            //
            // 3.3.0 之前这里写死「超过 9 核就用 8」。硬件探测模块
            // （beauty_hwprobe）已经给出更准的值：物理核 − 1、上限 12、按内存余量
            // 降档。本机 20 核：旧值 8 → 探测值 12 → 再给 worker 让 1 核 = 11。
            //
            // 为什么探测值之外还要再让一核：beautyHwCpuThreads() 让出的是
            // 「Resolve 主线程 + 解码」那一核，异步检测的 worker 是另一份开销，
            // 得单独算 —— 这就是上面那条 13.7ms 教训的来源。
            //
            // 收益边界：实测 1080p 8→16 线程 24.6→18.8ms；4K 几乎无变化
            // （71.5→71.1ms，瓶颈是内存带宽不是核数）。所以这条只对中低
            // 分辨率有效，4K 要靠 P2 的 GPU 路径。
            if (asyncOn_ && g_cfg.threads <= 0) {
                int t = beautyHwCpuThreads();
                if (t > 2) t -= 1;
                if (t > 1) g_cfg.threads = t;
            }
            const char* fm = getenv("BEAUTY_FREQ_MAG");
            if (fm && *fm) {
                float lo = 0, hi = 0;
                if (sscanf(fm, "%f,%f", &lo, &hi) == 2) {
                    g_cfg.freqMagLo = lo;
                    g_cfg.freqMagHi = hi;
                }
            }
            // 细节回填（锐化）：BEAUTY_DETAIL_THR 改细频带软阈值（削噪量），
            // BEAUTY_DETAIL_MID 改中频带基准增益。两者只在离线标定时用。
            const char* dt = getenv("BEAUTY_DETAIL_THR");
            if (dt && *dt) g_cfg.detailThrFine = (float)atof(dt);
            const char* dm = getenv("BEAUTY_DETAIL_MID");
            if (dm && *dm) g_cfg.detailMidGain = (float)atof(dm);
            // 3.1.35：磨皮=0 纯锐化模式的细频增益 / 细频阈值（见配置段 faceSharpGain / faceSharpThrFine）。
            const char* fsg = getenv("BEAUTY_FACE_SHARP_GAIN");
            if (fsg && *fsg) g_cfg.faceSharpGain = (float)atof(fsg);
            const char* fst = getenv("BEAUTY_FACE_SHARP_THR");
            if (fst && *fst) g_cfg.faceSharpThrFine = (float)atof(fst);
            // 全局锐化自己的旋钮（3.1.22，从 detailMidGain / detailThrFine 拆出）
            const char* gm = getenv("BEAUTY_DETAIL_MID_GLOBAL");
            if (gm && *gm) g_cfg.globalMidGain = (float)atof(gm);
            const char* gf = getenv("BEAUTY_DETAIL_FINE_GLOBAL");
            if (gf && *gf) g_cfg.globalFineGain = (float)atof(gf);
            const char* gt = getenv("BEAUTY_DETAIL_THR_GLOBAL");
            if (gt && *gt) g_cfg.globalThrFine = (float)atof(gt);
            const char* gt2 = getenv("BEAUTY_DETAIL_THR_MID_GLOBAL");
            if (gt2 && *gt2) g_cfg.globalThrMid = (float)atof(gt2);
            // 磨皮后的基准细频回填比例（0 = 关）。见配置段 blurRefill 的说明。
            const char* brf = getenv("BEAUTY_BLUR_REFILL");
            if (brf && *brf) g_cfg.blurRefill = (float)atof(brf);
            const char* brt = getenv("BEAUTY_BLUR_REFILL_THR");
            if (brt && *brt) g_cfg.blurRefillThr = (float)atof(brt);
            const char* brm = getenv("BEAUTY_BLUR_REFILL_MINI");
            if (brm && *brm) g_cfg.blurRefillMinI = (float)atof(brm);
            const char* brd = getenv("BEAUTY_BLUR_REFILL_DELTA");
            if (brd && *brd) g_cfg.blurRefillDelta = atoi(brd);
            // 磨皮强度总增益（3.0.7）。1 = 原实现；见配置段 smoothGain 的说明。
            const char* sg = getenv("BEAUTY_SMOOTH_GAIN");
            if (sg && *sg) {
                float v = (float)atof(sg);
                if (v < 0.25f) v = 0.25f;
                if (v > 3.0f)  v = 3.0f;
                g_cfg.smoothGain = v;
            }
            // 细节层时序 EMA：BEAUTY_EMA=0 关掉（做 A/B 用），其余三个是阈值与 beta
            const char* em = getenv("BEAUTY_EMA");
            if (em && *em) g_cfg.emaOn = (em[0] != '0');
            const char* es = getenv("BEAUTY_EMA_STILL");
            if (es && *es) g_cfg.emaStill = (float)atof(es);
            const char* ef = getenv("BEAUTY_EMA_FULL");
            if (ef && *ef) g_cfg.emaFull = (float)atof(ef);
            const char* eb = getenv("BEAUTY_EMA_BETA");
            if (eb && *eb) g_cfg.emaBetaStill = (float)atof(eb);
            // 五官局部五项增益标定：BEAUTY_REG_GAIN="立体,亮眼,去黑眼圈,法令纹"
            // 只在离线对拍时用（要和参考版的差分峰值对齐时改这里，别改源码重编译）。
            // 3.1.22：后两项不再是「增益」了 ——
            //   黑眼圈 = 双层椭圆的**整体比例因子**（形状在代码里定档）
            //   法令纹 = 暗度模型的「斜率,封顶」
            const char* rg = getenv("BEAUTY_REG_GAIN");
            if (rg && *rg) {
                float a = 0, b = 0, c = 0, d = 0;
                if (sscanf(rg, "%f,%f,%f,%f", &a, &b, &c, &d) == 4) {
                    g_cfg.regThreedGain = a;
                    g_cfg.regEyeGain = b;
                    g_cfg.regPouchScale = c;
                    g_cfg.regNasoSlope = d;
                }
            }
            const char* rs = getenv("BEAUTY_REG_SPOT");
            if (rs && *rs) g_cfg.regSpotK = (float)atof(rs);
            // 五官局部三项（3.1.22 重建）：
            //   BEAUTY_REG_THREED = 立体增益比例因子（单值）
            //   BEAUTY_REG_POUCH  = 黑眼圈增益比例因子（单值）
            //   BEAUTY_REG_NASO   = "斜率,封顶"
            const char* rt = getenv("BEAUTY_REG_THREED");
            if (rt && *rt) g_cfg.regThreedGain = (float)atof(rt);
            const char* rp = getenv("BEAUTY_REG_POUCH");
            if (rp && *rp) g_cfg.regPouchScale = (float)atof(rp);
            const char* rn = getenv("BEAUTY_REG_NASO");
            if (rn && *rn) {
                float a, b;
                if (sscanf(rn, "%f,%f", &a, &b) == 2) {
                    g_cfg.regNasoSlope = a;
                    g_cfg.regNasoMaxLift = b;
                }
            }
            // 暗线闸门阈值（离线对拍用）："下限,上限"
            const char* rgate = getenv("BEAUTY_REG_NASO_GATE");
            if (rgate && *rgate) {
                float a, b;
                if (sscanf(rgate, "%f,%f", &a, &b) == 2) {
                    g_cfg.regNasoGateLo = a;
                    g_cfg.regNasoGateHi = b;
                }
            }
        }
        // 3.1.21：把「首帧」的一次性成本提前到这里（模块加载 / 引擎首次就绪）。
        warmUp();
        err.clear();
        return true;
    }
    // 预热：加载 YuNet 人脸模型（一次性，约 100~300 ms）。
    //
    // 3.1.4 修：这里以前是空壳（只 clear err 就返回），而 ensureReady() 是**唯一**
    // 做 face_.load() 的地方，旧宿主的插件实现又从不直接调 ensureReady() —— 结果
    // 旧宿主里 faceOk_ 永远是 false，updateMask() 第 764 行第一句就 return false：
    //   · 磨皮退回全帧（背景一起磨，CPU 顶到最高）；
    //   · 清晰 / 美白 / 红润 / 五官五项 / 脸内锐化全部落在 if (haveMask) 里，
    //     整片不执行 —— 面板 11 项只剩磨皮和全局锐化能用。
    // 离线脚本（core_probe / golden_capture）是显式调 ensureReady() 的，所以离线
    // 对拍一直是带掩码的：旧宿主里跑的画面和验收过的画面根本不是一回事。
    bool preheat(std::string& err, bool& didInit) override {
        didInit = false;
        err.clear();
        const bool wasTried = faceTried_;
        if (!ensureReady(err)) return false;
        // 只有这一次真的做了加载才回 true —— 插件层靠它决定要不要打 init OK
        didInit = !wasTried;
        return true;
    }
    bool ready() const override { return true; }

    int acquireSlot(std::string& err) override {
        // 兜底：preheat 之外还有别的入口（宿主可能很久以后才建滤镜，或 preheat
        // 因故没跑到）。ensureReady 自身幂等，第二次调用只判一次 faceTried_。
        if (!ensureReady(err)) return -1;
        // BUG4（2026-11-06）：以前这里恒 return 0，于是「两个实例」在引擎眼里
        // 长得一模一样 —— 日志里也都是 slot=0，也没法判断刚才那次渲染是不是
        // 另一个实例。现在给每个实例一个唯一 id（只用于诊断与串台判据，
        // 引擎状态仍然全局共享，见 beauty_engine.h 的警示）。
        static std::atomic<int> next{1};
        return next.fetch_add(1);
    }
    void releaseSlot(int slot) override {
        // BUG4：实例销毁时把它那份状态（掩码 2MB + 对齐帧 8MB + …）真的释放掉。
        // 宿主关掉片段/工程时不收，长工程会白占几百 MB。
        //
        // P0：必须先拿整帧状态锁。否则「另一个线程正在 process(同一个 slot)」
        // 时把 SlotState 删掉，那边 SlotScope 手里的指针就成了野指针，析构时
        // 对已释放内存做 10 次 swap —— 释放后使用。加锁后这里会等那一帧跑完。
        std::lock_guard<std::recursive_mutex> st(slotStateMx());
        std::lock_guard<std::mutex> lk(slotMx_);
        slots_.erase(slot);
    }
    void setDetectOptions(int, int, int) override {}
    // v3.0.4：OFX/Resolve 给的是 bottom-up 帧（默认 true，见 beauty_core.h）
    void setImageBottomUp(bool v) override { imgBottomUp_ = v; }
    // v3.0.4：自测/离线对拍必须确定性 —— 关掉异步检测（见 BeautySelfTest）
    void setAsyncDetect(bool on) override {
        asyncOn_ = on;
        if (!on) stopAsync();
    }

    bool process(int slot, const unsigned char* srcIn, unsigned char* dst,
                 int w, int h, const BeautySettings& s,
                 bool = true) override {
        const unsigned char* src = srcIn;
        if (!src || !dst || w <= 0 || h <= 0) return false;
        g_beautyDiagFrameSubst = 0;   // 诊断：本帧未发生整帧替换
        // BUG4：把本实例的掩码/帧缓存/EMA 状态换进来，离开作用域自动换回。
        // 这一步之后 process() 里的成员访问就是「本实例的」了。
        SlotScope slotScope(this, slot);
        const size_t need = (size_t)w * (size_t)h * 4;
        frameNo_++;   // 时序 EMA 用它判断历史有没有断帧（见 detailEmaGen_ 的注释）

        double blur = s.panel[P_BLUR];
        if (blur < 0) blur = 0;
        if (blur > 100) blur = 100;
        // 档位 → 强度的**饱和映射**：t_eff = t · (1 − sat·t²)
        //
        // 为什么需要它：参考版的档位曲线在高段是饱和的（脸颊 50→75→100 对应
        // 63.1→48.0→35.5，每档增量递减），而我们换盒式低通后几乎是线性的
        // （64.9→43.7→23.4）—— 磨皮拉满时我们比参考版狠 12 个百分点，脸会糊。
        // sat=0.16 是按参考版五个档位反解出来的：
        //     面板   25     50     75     100
        //     等效  0.248  0.480  0.683  0.840   （目标 0.187/0.482/0.679/0.842）
        // 中高段几乎重合，25 档偏强约 3.7pp（磨得稍狠，观感上可接受）。
        //
        // ⚠️ 这条曲线是**单张素材拟合**的（基准素材里只有 face_src 一张脸）。
        // 素材补齐后必须复核，BEAUTY_BLUR_SAT=0 可一键关掉回到线性。
        const float tLin = (float)(blur / 100.0);
        const float t = tLin * (1.0f - g_cfg.blurSat * tLin * tLin);

        // 面板其余各项（0..1）。谁生效取决于它自己，不再被「磨皮是否为 0」绑死 ——
        // 旧实现在 t<=0 时直接逐字节返回，导致磨皮归零时美白/红润/全局锐化一起失效，
        // 与参考版路线的行为不一致（那边 applyRegionEffects 在磨皮门控之外）。
        const float shFace = clampi01(s.panel[P_SHARPEN] / 100.0);
        const float shGlobal = clampi01(s.panel[P_GLOBAL_SHARP] / 100.0);
        // 脸内锐化原先以「磨皮开启」为前提（旧版引擎的
        // wantFaceSharpen = panel[P_BLUR] > 0 && ...）。3.1.35 起取消这个前提：
        // 磨皮为 0 时脸内虽然没有「被磨掉的高频」可回填，但 dst≈src，
        // 回填一份原图细频就等于叠加锐化 —— 这正是用户拉「锐化」时的预期。
        // 旧行为实测：面板 100 档、其余全 0 → 改动量 0.000，滑块完全无效；
        // 同条件下 SDK 的 sharpen 是独立锐化器，改动量 1.012、细节 ×2.00。
        // 磨皮 > 0 时这条路径的行为逐字节不变（仍按 shFace 回填）。
        const bool wantFaceDetail = (shFace > 0.0f);
        const bool wantGlobalDetail = (shGlobal > 0.0f);
        // 3.1.22：磨皮后的基准细频回填。只在「开了磨皮、没开锐化、没开清晰」时
        // 生效 —— 用户自己开了锐化/清晰时，那两项的回填已经起作用了，
        // 再叠一层基准会把毛孔放大成噪声。
        // 基准回填用的实际磨皮强度（band 路径算出来的 I）。
        // I 是在磨皮分支内部定义的，这里先声明，等分支里算出来再回填。
        float blurRefillI = 0.0f;
        // 磨皮强度低于阈值就不回填：轻磨时几乎没削掉高频，
        // 回填带来的只是量化噪声（±1 灰阶），而参照实现那一档本来就几乎不动。
        //
        // 注意这里是**可变**的：真正的判定要等磨皮分支算出实际强度 I 之后
        // （band 分支里才赋值 blurRefillI）。先在这里求值的话，
        // blurRefillI 恒为 0，条件永远不成立 —— 这个坑踩过一次。
        bool wantBlurRefill = false;
        const bool wantTone = (s.panel[P_WHITEN] > 0.0 || s.panel[P_RED] > 0.0);
        // 清晰：原生参考版的 clarity 是**纯颜色变换**（S 形对比 + 轻微暖调），
        // 与磨皮毫无关系 —— panel 全 0、只下发 clarity=1.0 时改动量 6.16、
        // 细节 ×1.17（2026-10-30 实测，见 全项对齐_20261030/sdk_native.py）。
        // 3.1.19 之前这里要求 `t > 0`，于是「只开清晰、磨皮为 0」的画面零变化，
        // 离线矩阵直接判「欠 100%」。这条前提已删除。
        const bool wantClarity = (s.panel[P_CLARITY] > 0.0);
        // 五官局部五项：全部要求「有掩码 + 有脸」，所以也算掩码需求方。
        // 与参考版一致地**不**以磨皮为前提（这是妆容层，磨皮关掉也该生效）。
        const bool wantRegion = (s.panel[P_THREED] > 0.0 ||
                                 s.panel[P_EYEBRIGHT] > 0.0 ||
                                 s.panel[P_POUCH] > 0.0 ||
                                 s.panel[P_NASOLABIAL] > 0.0 ||
                                 s.panel[P_DELSPOT] > 0.0);

        // 3.1.27：美形三项（瘦脸 / 大眼 / 下巴）—— 本项目第一次做**几何变形**。
        // 它与上面十项完全独立：不需要磨皮、不需要皮肤掩码，只要「人脸框 +
        // 关键点」。但它需要 faces_ 是新鲜的，而检测链是唯一更新途径，所以
        // 下面把它并进 needMask 的触发条件（而不是另起一条检测路径）。
        // BEAUTY_WARP=0 整体关闭 → 逐字节回到 3.1.26 的行为。
        static const bool warpEnabled = []() {
            const char *e = getenv("BEAUTY_WARP");
            return !(e && *e == '0');
        }();
        const bool wantWarp = warpEnabled &&
                              (s.panel[P_THIN_FACE] > 0.0 ||
                               s.panel[P_BIG_EYE] > 0.0 ||
                               s.panel[P_CHIN] > 0.0);

        // 3.1.35：早退条件补 wantFaceDetail。3.1.19 修「只开清晰、磨皮为 0
        // 零变化」时补过 wantClarity，但同一条链上漏了锐化 —— 于是把门控
        // 放开之后仍然被这里拦掉，实测仍是 0.000。两个条件必须成对维护。
        if (t <= 0.0f && !wantGlobalDetail && !wantFaceDetail && !wantTone &&
            !wantClarity && !wantRegion && !wantWarp) {
            // 面板全是 0：逐字节透传，零成本（与旧实现同一条路径）
            if (dst != src) memcpy(dst, src, need);
            lastMs_ = 0.0;
            return true;
        }

        if (smooth_.size() < need) smooth_.resize(need);
        unsigned char* sm = &smooth_[0];

        const double t0 = nowMsCore();
        // 掩码用途变了：以前只有磨皮要它，现在美白/红润/脸内锐化都要。
        const bool needMask = (t > 0.0f) || wantFaceDetail || wantTone ||
                              wantClarity || wantRegion || wantWarp;
        // 先更新掩码再磨皮：ROI 裁剪要用掩码的包围盒。顺序反了的话，
        // 磨皮那一遍全帧算完才知道 ROI，等于白算。
        const bool haveMask = needMask ? updateMask(slot, src, w, h) : false;
        if (haveMask) everHadMask_ = true;
        // ---- 帧分解打点 ①：掩码 ----------------------------------------------
        // 这一段包含的东西比名字多：同步模式下是「检测 + 画掩码」的全部；
        // 异步模式下主要是 submitAsync 那次整帧 memcpy（1080p 约 0.7ms，
        // maskEveryAsync 默认 0 = 每帧都提交）加上 collectAsync 的缓冲交换。
        // 真正的检测与画掩码跑在 worker 上，**不在这段时间里**，所以这里还要
        // 同时看 asyncMs_（worker 上一次任务的耗时），否则会误判成「掩码很便宜」。
        const double tMask = nowMsCore();

        // 3.1.21：**首次启用**这几帧直接透传，不退回全帧磨皮。
        //
        // 打开开关的那一刻异步检测还没跑完（模型 15 ms + worker 排队），掩码是空的。
        // 旧逻辑此时退回「全帧磨皮」，代价是双份的：
        //   · 算力：1080p 全帧双边 ~30 ms，而稳态只磨脸（掩码 16%）~18 ms，
        //     首帧必超 30fps 预算（实测首帧 42 ms vs 稳态 19 ms）；
        //   · 观感：整幅画面先糊一下，等掩码到了再跳回「只磨脸」——每次开开关
        //     都闪一次背景。
        // 一旦得到过掩码就不再走这条分支：后面「人真的走开了」仍按原逻辑全帧兜底，
        // 与旧行为逐字节一致。
        if (needMask && !haveMask && faceOk_ && !everHadMask_) {
            if (dst != src) memcpy(dst, src, need);
            lastMs_ = 0.0;
            return true;
        }

        // 3.1.10 掩码↔帧对齐：只在「worker 的结果刚到」这一帧把它描述的原始帧
        // 拿来处理，掩码与画面就逐像素同源了。没赶上（worker 晚一帧）就照旧用
        // 当前帧 + 旧掩码 —— 停顿一次比一帧淡边更显眼。
        if (alignOn_ && maskFresh_ && haveMask && heldFrame_.size() == need) {
            src = &heldFrame_[0];
            g_alignUsed++;
            g_beautyDiagFrameSubst = 1;   // 诊断：本帧整帧换成了缓存帧
        }
        g_alignTotal++;

        // ---- P1：GPU 传输管道验证（beauty.ini: gpu_pipe，默认 auto 自检一次）--
        // 这一步**不改画面**：只把 ROI 上传 → 恒等 CS → 下载，再与源逐字节比对。
        //
        // 为什么放在这里、又为什么不改画面：
        //   · ROI 必须等 updateMask 算出包围盒才有，所以只能排在它后面；
        //   · P1 的验收标准是「管道无损 + 量出真实搬运耗时」，算法移植是 P2。
        //     提前接进画面会让「基准没跑通」和「算法写错」两种故障混在一起。
        //
        // P2 会把 GPU_BEAUTY_IDENTITY 换成磨皮 21 趟，届时 ROI 行直接写进 sm
        // 缓冲，这段只剩「选路 + 计时」。
        const bool pipeRoiReady = haveMask && !bounds_.empty();
        if (pipeRoiReady && gpuPipeTakeThisFrame(pipeRoiReady)) {
            static const bool gpuOk = gpuBeautyInit();
            if (gpuOk) {
                GpuBeautyRoi gr;
                gr.x0 = bounds_.x0; gr.y0 = bounds_.y0;
                gr.x1 = bounds_.x1; gr.y1 = bounds_.y1;
                GpuBeautyTiming gt{};
                int mism = -1;
                int maxd = 0;
                // 帧缓冲是 RGBA8（w*h*4 字节），每行 w 个像素 —— 不是 float32。
                gpuBeautyVerifyRoi(src, w, h, w, gr, GPU_BEAUTY_IDENTITY,
                                   &gt, &mism, &maxd);
                // auto 模式下一次就是自检那一次：**必须无条件打日志**。
                // 原来只按 frameNo_ 节流（<=2 或 %60），可是掩码常常要到几十帧
                // 之后才到位，自检会撞在节流窗口外 —— 于是「管道跑了但日志空白」，
                // 与「完全没跑」在日志上完全无法区分，这正是 2026-11-06 那次
                // 误判的成因。
                const bool isSelftest = (gpuPipeMode() == GPU_PIPE_AUTO);
                if (isSelftest || frameNo_ <= 2 || (frameNo_ % 60) == 0) {
                    long long n = 0; double au = 0, ag = 0, ad = 0;
                    int lm = -1, lmd = 0;
                    gpuBeautyStats(&n, &au, &ag, &ad, &lm, &lmd);
                    NALOG(BEAUTY_LOG_INFO,
                          "GPU 管道[%s] roi=%dx%d 上传=%.2f 提交=%.2f 读回=%.2f(含GPU) 合计=%.2f ms"
                          " | 累计 %lld 帧 均上传=%.2f 均提交=%.2f 均读回=%.2f"
                          " | 不一致=%d 字节 最大差=%d",
                          gpuPipeModeName(),
                          gt.roiW, gt.roiH, gt.uploadMs, gt.submitMs, gt.downloadMs,
                          gt.totalMs, n, au, ag, ad, lm, lmd);
                }
            }
        }
        maskFresh_ = false;   // 新鲜度只对「到货那帧」有效，过了这帧就作废

        bool didSurface3 = false;
        // v3.0.6：实际用于混合的强度。默认就等于 t —— 除「高斯档 + 轻磨皮」之外
        // 所有路径都满足 tBlend == t，因此逐字节不变。
        // 为什么需要它：surface3 分支有自己的 I（含 `smooth_mode==3` 的 ×0.55），
        // 但高斯分支只产出 sm、强度完全由 blendWithMask 的 t 决定，没有 I 可打折。
        // 不打 tBlend 的话，「轻磨皮」在高斯档下就没有任何效果。
        float tBlend = t;
        if (t > 0.0f) {
            // v3.0.5：磨皮算法三档**真正接线**。
            // 此前 blur_type 只是被塞进 BeautySettings，process() 从不读它 ——
            // 三档输出逐位相同（基线里 mode1/2/3 与 blur62_wh30 是同一个 md5，
            // 那就是空接线最直接的证据）。面板顺序见 ofx_plugin.cpp 的
            // kBlurTypeChoices = {双边滤波, 高斯, 弱磨皮}。
            //   0 双边滤波 = 现状 surface3 保边路径，逐字节不变（基线依赖它）
            //   1 高斯     = 同一套空间核，值域 sigma 放到 255 → 双边退化成纯高斯
            //   2 弱磨皮   = 仍走 surface3，强度 I 打对折（见下面的 I 计算处）
            if (s.blur_type == 1) {
                beautycore::RoiRect groi;
                if (haveMask && g_cfg.roi && !bounds_.empty()) {
                    groi = bounds_;
                } else {
                    groi.x0 = 0; groi.y0 = 0; groi.x1 = w; groi.y1 = h;
                }
                // sigmaColor = 255：任何灰阶差都算「同色」，值域权重恒为 1，
                // 于是双边只剩空间高斯核。不叠五官保护 —— 「高斯」本来就该把
                // 边缘一起糊掉，这是这一档与「双边」唯一但决定性的区别。
                //
                // v3.0.7：半径 ×2。v3.0.6 实测这一档与「双边」距源帧只差 2.0×
                // （0.12162 vs 0.06098），肉眼几乎分不出。但**只去保边不够** ——
                // 保边与否改变的是「糊不糊边缘」，不改变糊的**范围**。
                // 出厂 r=2、σs=7：exp(−r²/2σ²)=exp(−4/98)=0.96，核几乎是平的，
                // 5×5 盒平均的等效标准差只有 1.4px，本来就没糊开。
                // 半径翻到 4（9×9 盒平均，σ≈2.6px，1.8×）才让「高斯」这一档
                // 在 1:1 下看得出来。σs 不动：它在这个量级下几乎不起作用，
                // 改了只会让注释里的标定值失效。
                const int gRad = g_cfg.radius * 2;
                beautycore::bilateralRgbaRoi(src, sm, w, h,
                                             gRad, g_cfg.sigmaSpace,
                                             255.0f, groi, g_cfg.threads);
                // v3.0.6：这条分支不产生 surface3 的 I，所以
                //   · 「轻磨皮」（模式 3）改打 t；
                //   · 回填强度 blurRefillI 也用 tBlend 顶上。
                // 不补的话 blurRefillI 恒为 0 → wantModeRefill 恒假 →
                // 磨皮模式在这一档彻底失效（v3.0.5 实测四档距源帧完全相同）。
                if (s.smooth_mode == 3) tBlend = t * 0.40f;
                blurRefillI = tBlend;
            } else if (g_cfg.smoothModel == 1) {
                // ---- surface3：三层表面模糊（C22）------------------------
                // 档位曲线用参考版的 I = min(1, k·t)（不是 legacy 的饱和曲线），
                // 所以这里用 tLin 而不是上面的 t。
                beautycore::Surface3Params sp;
                // 半径随档位线性增长（C22 §4），并按**人脸宽度归一化**：
                // 真机 1080p 素材脸宽 532px 上拟合出 r(50)≈8、r(100)≈14；
                // face_src 640×394 的脸只有 111px，等效半径就是它的 1/4.8。
                // 不做归一化的话，小脸会被大半径糊成一片（face_src 100 档
                // 实测细纹理 70.2% vs参考版 75.7%，归一化后 72.4%）。
                float scale = 1.0f;
                if (!faces_.empty()) {
                    float fw = 0.0f;
                    for (size_t i = 0; i < faces_.size(); i++)
                        if (faces_[i].w > fw) fw = faces_[i].w;
                    if (fw > 8.0f) {
                        scale = fw / 532.0f;          // 532 = 拟合素材的脸宽
                        if (scale < 0.20f) scale = 0.20f;
                        if (scale > 2.50f) scale = 2.50f;
                    }
                }
                int r1 = (int)((g_cfg.s3R0 + (g_cfg.s3R1 - g_cfg.s3R0) * tLin)
                               * scale + 0.5f);
                int r2 = (int)((g_cfg.s3R0 + (g_cfg.s3R2 - g_cfg.s3R0) * tLin)
                               * scale + 0.5f);
                sp.r1 = (r1 < 1) ? 1 : r1;
                sp.r2 = (r2 < 1) ? 1 : r2;
                sp.it1 = g_cfg.s3It1;
                sp.it2 = g_cfg.s3It2;
                sp.thr1 = g_cfg.s3Thr1;
                sp.thr2 = g_cfg.s3Thr2;
                // intensity = mix(t·K, 1.0, step(0.7, t))
                //   step 的自变量是**乘之前的 tLin** —— GLSL 的 mix 三个参数先
                //   求值，所以右式的 intensity 还是旧值。因为 K = 1/0.7，这一行
                //   在数值上等价于 min(1, tLin·K)，但保留原式写法以免后人再"优化"掉。
                float I;
                if (g_cfg.lvTable) {
                    // 3.1.18：档位查表。旧式 min(1, t/0.7) 在 t≥0.7 就恒为 1，
                    // 75 与 100 档输出逐字节相同 —— 那是「拉满没反应」的根因。
                    I = lvLerp5(g_cfg.lvI, tLin);
                } else {
                    I = (tLin >= g_cfg.s3ESat) ? 1.0f : (tLin * g_cfg.s3K);
                }
                if (I > 1.0f) I = 1.0f;
                // v3.0.5：弱磨皮档（blur_type=2）—— 同一套 surface3，强度压低。
                // 放在两条档位曲线都算完之后，保证与档位曲线正交（不改变手感形状，
                // 只整体压低一档）。
                // v3.0.7：0.5 → 0.30。0.5 档实测距源帧 0.02230，只有「双边」的
                // 0.37×，与「弱」字对得上却小到看不出来；0.30 把它压到 0.22×，
                // 成为四档里最轻的一档（比「轻磨皮」0.40 还轻），档位之间才拉得开。
                if (s.blur_type == 2) I *= 0.30f;
                // v3.0.5：轻磨皮档（smooth_mode=3）—— 整体最轻。
                // 依据是旧宿主侧四档实测的「细频保留率」92.3 / 94.3 / 96.8 / 96.7%：
                // 第 4 档与第 3 档几乎并列，所以它靠**降强度**而不是靠多回填来
                // 实现，才与「轻」这个字对得上（原名「朦胧磨皮」是错的，见
                // ofx_plugin.cpp kSmoothModeChoices 的注释）。
                // v3.0.7：0.55 → 0.40，同样是为了把「轻磨皮」从「比双边略轻」推到
                // 「明显更轻」，与「弱磨皮」形成 0.22× / 0.40× 两个可分辨的台阶。
                if (s.smooth_mode == 3) I *= 0.40f;
                sp.I = I;
                // v3.0.5：surface3 路径也要把实际强度交出去，磨皮模式的基准回填
                // （wantModeRefill）才有强度可依。此前只有 band 分支给它赋值。
                blurRefillI = I;
                // evenfactor = mix(E0, intensity − E0, step(0.6, intensity))
                // 三段：[0,0.42] 恒 0.3；(0.42,0.70] 升到 0.7；[0.70,1] 恒 0.7。
                float e;
                if (g_cfg.s3EFix >= 0.0f) {
                    e = g_cfg.s3EFix;
                } else if (g_cfg.lvTable) {
                    e = lvLerp5(g_cfg.lvE, tLin);
                } else {
                    e = (I >= g_cfg.s3EThr) ? (I - g_cfg.s3E0) : g_cfg.s3E0;
                }
                if (e < 0.0f) e = 0.0f;
                sp.e = e;
                sp.protectK = g_cfg.s3EyeK;

                beautycore::RoiRect s3roi;
                if (haveMask && g_cfg.roi && !bounds_.empty()) {
                    s3roi = bounds_;
                } else {
                    s3roi.x0 = 0; s3roi.y0 = 0; s3roi.x1 = w; s3roi.y1 = h;
                }
                const unsigned char* pr3 =
                    (haveMask && protect_.size() == (size_t)w * h)
                        ? &protect_[0] : nullptr;
                if (g_cfg.s3Band) {
                    // 双尺度带通 + 振幅门控（第 2 批-B）。与 25 tap 路径的差别
                    // 只有「两个低频层怎么算」——B1/B2 换成两个不同尺度的模糊，
                    // 外加一道振幅门控；合成段 I/e/skin/eye 仍逐行同口径。
                    // s3R0/R1/R2、s3Sx/s3Sy、s3YF1/YF2 在这条路上**都不参与**。
                    beautycore::BandParams bp;
                    bp.T = g_cfg.lvTable ? lvLerp5(g_cfg.lvT, tLin) : g_cfg.s3BandT;
                    bp.gwp = g_cfg.s3Gwp;
                    bp.halfRes = g_cfg.s3Half;
                    bp.I = I;
                    blurRefillI = I;
                    // I 出来了才能判定要不要做基准回填（见 wantBlurRefill 的注释）
                    wantBlurRefill = (t > 0.0f) && (g_cfg.blurRefill > 0.0f) &&
                                     (s.panel[P_SHARPEN] <= 0.0) &&
                                     (s.panel[P_CLARITY] <= 0.0) &&
                                     (blurRefillI >= g_cfg.blurRefillMinI);
                    bp.e = e;
                    bp.protectK = g_cfg.s3EyeK;

                    // ---- P2：先试 GPU，失败当场回落 CPU ----
                    // 两者同签名、同语义（包括 dst 全帧有效），所以这里就是二选一，
                    // 不需要为 GPU 路径另做任何预处理。参数直接复用上面填好的 bp，
                    // 半径由 gpu_smooth 内部调同一个 bandBoxW 求，两边的 r 必然一致。
                    bool gpuSmoothOk = false;
                    if (gpuSmoothMode() != GPU_SMOOTH_OFF) {
                        GpuSmoothTiming gst{};
                        gpuSmoothOk = gpuSmoothRgbBand(src, dst, w, h,
                                                       haveMask ? &mask_[0] : nullptr,
                                                       pr3, bp, s3roi, &gst,
                                                       this, (long long)frameNo_,
                                                       gpuSmoothPipelineOn());
                        // 失败必须能追到原因，否则「为什么还是 CPU 那么慢」无从下手。
                        // 只打前 3 次：低端卡上可能每帧都失败，否则日志会被刷满。
                        static int gsFailLogs = 0;
                        if (!gpuSmoothOk) {
                            if (gsFailLogs < 3) {
                                ++gsFailLogs;
                                NALOG(BEAUTY_LOG_INFO,
                                      "GPU 磨皮[%s] 失败，本帧回落 CPU：%s",
                                      gpuSmoothModeName(), gpuSmoothLastError());
                            }
                        // 采样周期 60 帧（1.1.1.0 定版恢复）。
                        // 1.0.10.2~1.1.0.0 为了定位「等 GPU 尖峰」与「延迟取回是否触发」
                        // 临时加密到 10 帧（≈每秒 2.4 条）。1.1.1.0 真机数据已拿到：
                        // 中位 3.5ms、p90 4.8ms、峰值只出现在起播/跳转，稳态无掉帧，
                        // 诊断结束，恢复 60 帧（≈每 2.5 秒一条），避免日志写盘本身的开销。
                        // 以后再需要采样时改这个数字即可，format 字符串里已带 阻=/差= 字段。
                        } else if (frameNo_ <= 2 || (frameNo_ % 60) == 0) {
                            long long n = 0; double at = 0.0, ap = 0.0;
                            gpuSmoothStats(&n, &at, &ap);
                            NALOG(BEAUTY_LOG_INFO,
                                  "GPU 磨皮[%s] roi=%dx%d@%d,%d 平面=%dx%d r=%d/%d/%d P=%d"
                                  " 上传=%.2f(Map %.2f/拷 %.2f) 算=%.2f 读回=%.2f(等GPU %.2f/拷 %.2f)"
                                  " 补齐=%.2f 合计=%.2f ms"
                                  " | 累计 %lld 帧 均合计=%.2f 均平面像素=%.0f"
                                  " 管=%d/取=%d 阻=%s 差=%.2f/%.4f",
                                  gpuSmoothModeName(),
                                  gst.imgW, gst.imgH, gst.imgX0, gst.imgY0,
                                  gst.pw, gst.ph, gst.r1, gst.r2, gst.ra, gst.passes,
                                  gst.uploadMs, gst.upMapMs, gst.upMemMs, gst.computeMs,
                                  gst.downloadMs,
                                  gst.mapWaitMs, gst.mapCopyMs, gst.fillMs, gst.totalMs,
                                  n, at, ap,
                                  gst.pipeDeferred, gst.pipeRetrieved,
                                  pipeBlockTag(gst.pipeBlock),
                                  gst.pipeSigAvg, gst.pipeSigBad);
                        }
                    }
                    if (!gpuSmoothOk) {
                        beautycore::surface3RgbBand(src, dst, w, h,
                                                    haveMask ? &mask_[0] : nullptr,
                                                    pr3, bp, s3roi, g_cfg.threads,
                                                    s3ScratchA_);
                    }
                    // ---- 掩码诊断（1.1.2.0）--------------------------------------
                    // 目的只有一个：搞清楚 ROI 为什么能涨到画面的 74%x88%。
                    // 每 12 帧一条（掩码 4 帧一算，等于每 3 份掩码采一条）。
                    // 只读成员，不参与任何像素运算 —— 对画面逐字节零影响。
                    //
                    // 四个数怎么读：
                    //   倍数 = 盒 / 脸盒。接近 1 说明脸框本身就大（只能改脸框策略）；
                    //          远大于 1 说明脸框之外的皮肤在拉大 ROI。
                    //   分块 = 各脸分块面积之和 / 盒。**这是多矩形重构的收益上限**：
                    //          ≥1 说明单矩形并不吃亏（各块自己的 pad 与脖子带把
                    //          「并集浪费」抵消掉了），拆了白拆；
                    //          <1 才有得赚，如 0.7 表示重构后搬图量降到 70%。
                    //   脖 = 掩码里由脖子通道新增的比例。占皮肤一半以上，收窄脖子立刻见效。
                    //   皮肤 = 掩码像素占全帧比例。它若远大于 脸盒，说明肤色判据把
                    //          背景也算进来了（墙、木色家具、衣物）。
                    if ((frameNo_ % 12) == 0 && maskStat_.skinPx > 0) {
                        const double total = (double)((size_t)w * h);
                        const double skinPct = 100.0 * maskStat_.skinPx / total;
                        const double neckPct = 100.0 * maskStat_.neckPx / total;
                        const double boxPct  = 100.0 * (double)bounds_.area() / total;
                        const double facePct = 100.0 * (double)maskStat_.faceBoxA / total;
                        const double boxA    = (double)bounds_.area();
                        NALOG(BEAUTY_LOG_INFO,
                              "掩码诊断 roi=%dx%d@%d,%d 盒=%.1f%% 脸盒=%.1f%% 倍数=%.1fx"
                              " 分块=%.2fx 皮肤=%.2f%% 脖=%.2f%% 脸=%.2f%% 脸数=%d",
                              bounds_.width(), bounds_.height(), bounds_.x0, bounds_.y0,
                              boxPct, facePct,
                              (facePct > 0.01) ? (boxPct / facePct) : 0.0,
                              (boxA > 0.0) ? ((double)maskStat_.tileA / boxA) : 0.0,
                              skinPct, neckPct, skinPct - neckPct, lastFaces_);
                    }
                } else if (g_cfg.s3Tap25) {
                    // 25 tap 表面模糊（第 2 批 #5/#6/#7）。与下面盒式路径的
                    // 唯一差别是「两个低频层怎么算」；合成段 I/e/skin/eye 逐行相同。
                    // 半径参数 s3R0/R1/R2 在这条路上**完全不参与** —— 真结构里
                    // 核是固定的，档位只改 I 和 e。fw/532 的归一化也一并作废。
                    beautycore::Surface25Params p25;
                    p25.sx = g_cfg.s3Sx;
                    p25.sy = g_cfg.s3Sy;
                    p25.yFactor1 = g_cfg.s3YF1;
                    p25.yFactor2 = g_cfg.s3YF2;
                    p25.halfRes = g_cfg.s3Half;
                    p25.I = I;
                    p25.e = e;
                    p25.protectK = g_cfg.s3EyeK;
                    beautycore::surface3Rgb25(src, dst, w, h,
                                              haveMask ? &mask_[0] : nullptr, pr3,
                                              p25, s3roi, g_cfg.threads,
                                              s3ScratchA_);
                } else {
                    beautycore::surface3Rgb(src, dst, w, h,
                                            haveMask ? &mask_[0] : nullptr, pr3,
                                            sp, s3roi, g_cfg.threads,
                                            s3ScratchA_, s3ScratchB_);
                }
                didSurface3 = true;
            } else if (g_cfg.down > 1) {
                // 降采样路径已实测失败（档位曲线误差 27+），只为复现当年结论保留
                beautycore::bilateralRgbaDown(src, sm, w, h,
                                              g_cfg.radius, g_cfg.sigmaSpace,
                                              g_cfg.sigmaColor, g_cfg.down);
            } else if (haveMask && g_cfg.roi && !bounds_.empty() && g_cfg.freqMode) {
                // 高低频分离：低频用大半径高斯，只压小幅度高频，低频结构原样保留。
                // strength 传 100（全强度），面板档位由后面的 blendWithMask 按 t 缩放。
                beautycore::FreqParams fp = makeFreqParams();
                beautycore::highLowSmooth(src, sm, w, h, fp, bounds_, g_cfg.threads,
                                          lowBuf_, scratch_, guideScratch_);
            } else if (haveMask && g_cfg.roi && !bounds_.empty()) {
                // 只磨掩码包围盒。逐像素独立，结果与全帧跑逐字节一致，纯省算力。
                beautycore::bilateralRgbaRoi(src, sm, w, h,
                                             g_cfg.radius, g_cfg.sigmaSpace,
                                             g_cfg.sigmaColor,
                                             bounds_, g_cfg.threads);
            } else if (g_cfg.freqMode) {
                // 没有可用掩码（模型没加载 / 这一轮没检出脸）或关掉 ROI 对照时，
                // 同样走高低频，只是范围放到全图 —— 否则「ROI 开/关」对比的是
                // 两种不同算法，等价性测试就失去意义了。
                beautycore::FreqParams fp = makeFreqParams();
                beautycore::RoiRect full;
                full.x0 = 0; full.y0 = 0; full.x1 = w; full.y1 = h;
                beautycore::highLowSmooth(src, sm, w, h, fp, full, g_cfg.threads,
                                          lowBuf_, scratch_, guideScratch_);
            } else {
                // 没有可用掩码（模型没加载 / 这一轮没检出脸）时退回全帧：
                // 宁可多算，也不能出现「有脸却不磨」。
                beautycore::bilateralRgba(src, sm, w, h,
                                          g_cfg.radius, g_cfg.sigmaSpace,
                                          g_cfg.sigmaColor, g_cfg.threads);
            }

            // ---- v3.0.7：磨皮强度总增益（混合路径这一半）------------------
            // 位置很关键：必须在「sm 已算好」之后、「混合写 dst」之前，
            // 且只在**非 surface3** 的路径上生效（surface3 在上面用
            // amplifySmooth 做，见那段注释）。
            //
            // 为什么混合路径不外推：这几条路的强度就是混合权重 tBlend，
            // tBlend>1 的语义是「比完全平滑还平滑」，落到模糊图上等于反向补偿
            // 边缘 —— 观感是硬边 + 光晕（实测高斯档外推到 1.51 时人脸区最大
            // 改动 87 灰阶，就是边缘过冲）。所以乘完直接封顶 1.0：
            // 「尽量磨到完全平滑为止，但不多走一步」。
            // 高斯档本身靠 v3.0.7 的半径翻倍已经强了 2×，这里封顶足够。
            if (!didSurface3) {
                tBlend *= g_cfg.smoothGain;
                if (tBlend > 1.0f) tBlend = 1.0f;
            }

            if (didSurface3) {
                // dst 已由 surface3Rgb 写满（含 I / e / skin / eye 四步），
                // 不能再走 blendWithMask —— 否则 skin 与 t 会被应用两遍。
                // v3.0.7：磨皮强度增益走 amplifySmooth（外推 dst−src），
                // 只给这条重建路径用 —— 见下面 tBlend 处的说明。
                beautycore::amplifySmooth(src, dst, w, h, g_cfg.smoothGain,
                                          g_cfg.threads);
            } else if (haveMask) {
                const unsigned char* pr =
                    (protect_.size() == (size_t)w * h) ? &protect_[0] : nullptr;
                // v3.0.6：用 tBlend（除高斯档外恒等于 t，逐字节不变）
                // v3.0.7：tBlend 已在上面的分支之后乘过 smoothGain 并封顶 1.0
                blendWithMask(src, sm, dst, w, h, tBlend, &mask_[0], pr,
                              g_cfg.protectK);
            } else {
                beautycore::mixRgba(src, sm, dst, w, h, tBlend);
            }
        } else {
            // 磨皮为 0：其余各项仍要在原图基础上生效，所以先把原图铺到 dst
            if (dst != src) memcpy(dst, src, need);
        }
        // ---- 帧分解打点 ②：磨皮 ----------------------------------------------
        // 这一段 = 选路（surface3 / 双边 / 高低频）+ 混合写 dst。
        // GPU 路径下它应该约等于 GPU 磨皮日志里的「合计」；若明显更大，
        // 差值是 amplifySmooth / blendWithMask 的 CPU 侧开销。
        const double tSmooth = nowMsCore();

        if (haveMask) {
            // 清晰 / 美白 / 红润：三项合并成一次遍历（toneColor）。
            //
            // 2026-10-30 起三项都改成**照抄原生参考版的逐通道查找表**，不再有各自
            // 的拟合公式。理由：把它们拆开量化后发现"箱内 σ"只有零点几灰阶
            // （clarity 0.08~0.27 / color_level 0.36~1.24 / red_level 0.73）——
            // 即它们与空间位置、邻域无关，是纯颜色变换。改表即可对齐到量化误差。
            //
            // 顺序：清晰 → 美白 → 红润。先抬对比再提亮，否则提亮会把对比压平。
            // 输入用 dst（磨皮之后）而不是 src：拿原图算会把刚磨掉的瑕疵加回来。
            //
            // 旧实现（12px 中频回填当"清晰"）保留在 BEAUTY_CLARITY_LEGACY=1 下，
            // 只作 A/B 对照，不再走默认路径。
            static const bool legacyClarity =
                (getenv("BEAUTY_CLARITY_LEGACY") != nullptr);
            const float cv = wantClarity ? (float)(s.panel[P_CLARITY] / 100.0) : 0.0f;
            const float wv = (float)(s.panel[P_WHITEN] / 100.0);
            const float rv = (float)(s.panel[P_RED] / 100.0);
            if (legacyClarity) {
                if (cv > 0.0f) {
                    beautycore::ClarityParams cp;
                    cp.gain = cv * g_cfg.clarityMax;
                    cp.radius = g_cfg.clarityRadius;
                    cp.kneeLo = g_cfg.clarityKneeLo;
                    cp.kneeHi = g_cfg.clarityKneeHi;
                    beautycore::clarityRgb(dst, w, h, &mask_[0], bounds_, cp,
                                           g_cfg.threads, clarityScratch_);
                }
                if (wv > 0.0f || rv > 0.0f) {
                    beautycore::toneWhitenRosy(dst, w, h, &mask_[0], bounds_,
                                               wv, rv, g_cfg.threads);
                }
            } else {
                // v3.0.4：三项全 0 时必须**跳过**。toneColor 内部是浮点查表往返，
                // 即使系数为 0 也会把 G 通道改动 ±1（实测 blur00 用例里 6.7 万
                // 像素、最大差 1，全在脸框内）—— 那就破坏了「面板全 0 = 逐字节
                // 透传」这条契约，也让逐字节基线里 blur00 不再是纯拷贝。
                if (cv > 0.0f || wv > 0.0f || rv > 0.0f)
                    beautycore::toneColor(dst, w, h, &mask_[0], bounds_,
                                          cv, wv, rv, g_cfg.threads);
            }

            // 五官局部五项：妆容层，放在磨皮与美白之后（理由见 applyFaceRegions
            // 的注释）。祛斑痘也在里面 —— 它要「修掉刚磨过之后还剩下的斑点」。
            if (wantRegion) applyFaceRegions(dst, w, h, s);
        }
        // ---- 帧分解打点 ③：色调 ----------------------------------------------
        // 清晰 / 美白 / 红润三项合并成一次 toneColor 遍历，外加五官局部五项。
        const double tTone = nowMsCore();

        // ---- 细节回填（锐化 / 全局锐化）------------------------------------
        // 放在整条链的最后一个「加法」位置，理由：
        //   · 亮度平面取自 **src（原图）**，所以磨皮抹掉多少都不影响可回填的量；
        //   · 加在 dst 上，不会再被磨皮平均掉，也不会被美白曲线压平；
        //   · 全局锐化在「没有掩码」时也要跑 —— 否则掩码一丢，背景锐度跟着跳档，
        //     这正是参考版 3.0.13 修过的那个 bug，core 不能重演。
        // v3.0.5：磨皮模式 1/2 自带「基准细节回填」，不依赖面板「锐化」。
        // 语义：磨皮模式控制的是「磨皮削掉的那些细节按什么策略还回去」。
        //   0 人像磨皮 = 不回填（现状，逐字节不变，基线依赖它）
        //   1 清晰磨皮 = 回填 1.0×，细频软阈值用 0.8（比锐化那套 3.0 低得多）
        //   2 精细磨皮 = 回填 1.6×，并补一点中频 → 结构最完整
        //   3 轻磨皮   = 不回填，靠降低强度 I 实现（见上面 I 计算处）
        // 为什么必须自带而不是复用 wantBlurRefill：那条路要求「锐化=0 且清晰=0」，
        // 而且只在 band 分支里求值。用户拖「磨皮模式」时锐化通常是 0，
        // 不另开一条的话这个下拉框依旧是死的。
        const bool wantModeRefill =
            (s.smooth_mode == 1 || s.smooth_mode == 2) && t > 0.0f && haveMask &&
            (g_cfg.blurRefill > 0.0f) && (blurRefillI >= g_cfg.blurRefillMinI);

        if (wantFaceDetail || wantGlobalDetail || wantBlurRefill || wantModeRefill) {
            // 运动判据每帧算一次（4×4 抽样，720p 约 5.7 万次比较，可忽略）。
            // 只在真的要跑锐化时才算 —— 锐化关着就没有噪点放大问题。
            const double motion = updateMotion(src, w, h);
            const float beta = emaBeta(motion);

            beautycore::DetailParams dp;
            if (wantFaceDetail && haveMask) {
                dp.fine = shFace;
                dp.mid = shFace * g_cfg.detailMidGain;
                if (dp.mid > 3.0f) dp.mid = 3.0f;
                // 3.1.35：磨皮=0 时是「纯锐化」语义（dst≈src，回填 src 高频 = 叠加锐化）。
                // 这条路不能用中频带（会在全脸皮肤产生与档位无关的固定基底），
                // 也不能用默认细频阈值（会把小幅度高频削光），所以走专属参数：
                //   细频满增益 × faceSharpGain、中频关、阈值放开到 faceSharpThrFine。
                // 磨皮>0 时是「回填被磨掉的高频」，增益沿历史曲线不变。
                if (t <= 0.0f) {
                    dp.fine = shFace * g_cfg.faceSharpGain;
                    dp.mid = 0.0f;
                    dp.thrFine = g_cfg.faceSharpThrFine;
                }
            }
            if (wantBlurRefill && haveMask) {
                // 基准回填：只还细频，不碰中频。
                // 中频是「我方比参照多出来的那部分能量」，还回去只会更糟。
                //
                // 比例随**实际磨皮强度 I** 缩放：磨皮削掉的高频本来就正比于 I，
                // 不乘 I 的话低档会把原图细节放大成超过原图
                // （实测 25 档 dret 从 1.000 涨到 1.083、改动量 0.000→0.104）。
                //
                // delta 模式（加回「原图细频 − dst 细频」）实测更保守：
                // 100 档细节保留只从 0.737 到 0.761（目标 0.835），因为
                // 被磨掉的细频差值本身就在 1 灰阶上下，被软阈值削掉大半。
                // 所以生产用「I 缩放 + 低档不启用」，delta 留作开关备用。
                dp.fine = g_cfg.blurRefill * blurRefillI;
                dp.mid = 0.0f;
                dp.deltaRefill = g_cfg.blurRefillDelta != 0;
            }
            if (wantModeRefill) {
                // 模式 1/2 的基准回填。倍率是相对 blurRefill 的增益。
                //
                // 为什么倍率远大于 1、而且模式 1 也要带一点中频：
                // blurRefill=0.06 是给 band 路径标定的，单靠细频在这条路上
                // 只回填 5% 的细频带，落在 8bit 上四舍五入后**一格都不动**
                // （实测 mode1 与默认档 md5 完全相同）。真正能推动像素的是中频，
                // 所以两档都带中频，只是模式 2 带得更多。
                // v3.0.7：2.2/0.18 → 4.0/0.45（清晰）、3.2/0.40 → 5.5/0.75（精细）。
                // 依据是 v3.0.6 的实测：清晰 0.06355 / 精细 0.06667 对 人像 0.06098，
                // 只有 1.04× / 1.09× —— 三个档位在 1:1 下不可分辨。
                // 注意这两档**不能靠调 I 来加强**：I 同时驱动磨皮本身，动它就把
                // 「磨皮」和「磨皮模式」两个旋钮绑在一起了。回填是正交的那条腿，
                // 加大它才是「磨得多、细节还回去更多」的本意。
                static const float kModeFineGain[4] = {0.0f, 4.0f, 5.5f, 0.0f};
                static const float kModeMidGain[4]  = {0.0f, 0.45f, 0.75f, 0.0f};
                const int mm = (s.smooth_mode > 3) ? 3 : s.smooth_mode;
                dp.fine = g_cfg.blurRefill * blurRefillI * kModeFineGain[mm];
                dp.mid = kModeMidGain[mm] * blurRefillI;
                dp.deltaRefill = false;
            }
            if (wantGlobalDetail) {
                // 全局锐化用**自己**的中频/细频基准（3.1.22 从 detailMidGain 拆出）。
                // 参照路径那边也是两条独立曲线（见 g_cfg.globalMidGain 的注释）。
                dp.globalFine = shGlobal * g_cfg.globalFineGain;
                dp.globalMid = shGlobal * g_cfg.globalMidGain;
                if (dp.globalMid > 3.0f) dp.globalMid = 3.0f;
                dp.thrFineGlobal = g_cfg.globalThrFine;
                dp.thrMidGlobal = g_cfg.globalThrMid;
            }
            // 基准回填用自己的一套细频阈值（默认 0.8，比锐化那套的 3.0 低得多）：
            // 磨皮要还的就是「小幅值细频」，而锐化要防的是「噪点被放大」，
            // 两者的取舍正好相反，所以不能共用一个阈值。
            dp.thrFine = (wantBlurRefill || wantModeRefill) ? g_cfg.blurRefillThr
                                                            : g_cfg.detailThrFine;

            // 有全局档就整帧跑（掩码只决定「用哪一档权重」）；否则只跑掩码包围盒。
            beautycore::RoiRect droi;
            if (wantGlobalDetail) {
                droi.x0 = 0; droi.y0 = 0; droi.x1 = w; droi.y1 = h;
            } else {
                droi = bounds_;
            }
            if (!droi.empty() && (dp.fine > 0.0f || dp.mid > 0.0f ||
                                  dp.globalFine > 0.0f || dp.globalMid > 0.0f)) {
                // ---- 时序 EMA 的上下文 --------------------------------------
                // lumT 传 nullptr 时 detailRefill 走原路径，逐字节等于 0.7.1。
                // 所以 BEAUTY_EMA=0 是「完全关闭」而不是「beta=1 的近似关闭」。
                beautycore::DetailEma ema;
                if (g_cfg.emaOn) {
                    const size_t np = (size_t)w * (size_t)h;
                    if (detailLumT_.size() != np) {
                        detailLumT_.assign(np, 0);
                        detailEmaReset_ = true;   // 尺寸变了，历史整片作废
                    }
                    // 上一帧没写历史（断帧）→ 这一帧必须用当前帧重置，
                    // 否则会把陈旧位置的亮度混进来。
                    if (detailEmaGen_ + 1 != frameNo_) detailEmaReset_ = true;
                    ema.lumT = &detailLumT_[0];
                    ema.beta = beta;
                    ema.reset = detailEmaReset_;
                    detailEmaReset_ = false;
                }
                beautycore::detailRefill(src, dst, w, h,
                                         haveMask ? &mask_[0] : nullptr,
                                         droi, dp, g_cfg.threads, detailScratch_,
                                         ema);
                detailEmaGen_ = frameNo_;
            }
        }
        // ---- 帧分解打点 ④：回填 ----------------------------------------------
        // 细节回填（锐化 / 全局锐化 / 磨皮基准回填）。全局锐化开时是整帧范围。
        const double tDetail = nowMsCore();

        // ---- 美形三项（3.1.27）--------------------------------------------
        // 为什么放在**最末端**而不是最前面：
        //   · 前面十项都是逐像素颜色运算，先上色再变形与先变形再上色在颜色上
        //     等价（变形只是重采样，不改变颜色本身）；
        //   · 皮肤掩码 / ROI 包围盒 / 五官椭圆全是异步在**原图**上算的。放到
        //     前面就必须把「掩码↔画面同源」那套对齐逻辑重做一遍，风险大得多；
        //   · 放末端只需一次重采样，不碰任何已验收过的路径。
        // 代价：位移场的几何基准来自**原图**的人脸框，如果一帧内脸动得很快，
        // 变形会比磨皮晚一帧生效。实测 30fps 下不可见（与掩码滞后同源）。
        if (wantWarp && !faces_.empty()) {
            beautycore::WarpParams wp;
            wp.thin = clampi01((float)(s.panel[P_THIN_FACE] / 100.0));
            wp.eye = clampi01((float)(s.panel[P_BIG_EYE] / 100.0));
            wp.chin = clampi01((float)(s.panel[P_CHIN] / 100.0));
            if (wp.thin > 0.0f || wp.eye > 0.0f || wp.chin > 0.0f) {
                // 逐脸处理。多张脸时 ROI 互不重叠（检测框本身就不会大面积交叠），
                // 所以串行就地修改是安全的。
                // v3.0.5：必须把帧行序传下去。三项里「嘴在眼下、下巴在嘴下」
                // 是有方向的语义，faces_ 里的 y 是缓冲行号，不告诉 warpFace
                // 行序就会把瘦脸压到眉眼、下巴压到发际线（大眼是径向的，
                // 所以它是三项里唯一「看起来正常」的一个）。
                for (size_t i = 0; i < faces_.size(); i++)
                    beautycore::warpFace(dst, w, h, faces_[i], wp, warpScratch_,
                                         imgBottomUp_);
            }
        }

        const double tEnd = nowMsCore();
        lastMs_ = tEnd - t0;
        // ---- 帧分解日志（1.1.1.4）--------------------------------------------
        // 为什么要有这一行：到 1.1.1.3 为止，我们对「引擎内部」只有一个总数
        // （lastMs_，也就是 ofx_plugin 打的 sdk=），而 GPU 磨皮自己报了 3.8ms。
        // 真机 1080p 实测 sdk=7.4~13.0ms，说明还有 4~9ms 花在别处，但「别处」
        // 是哪一段此前完全没有数据 —— 判断「还能不能优化」全靠猜，这正是
        // 1.1.0.0 那次事故的成因（未验证的假设被当成事实）。
        //
        // 六个数的关系是恒等式，可以和 sdk= 对账：
        //   合计 = 掩码 + 磨皮 + 色调 + 回填 + 美形  （都从 t0 起算，逐段差分）
        // 判读：
        //   掩码 大而 工人掩码 也大 → 检测/画掩码本身贵（异步没吃满 / 周期太密）
        //   掩码 大而 工人掩码 小   → 贵在 submitAsync 的整帧 memcpy
        //   磨皮 远大于 GPU 磨皮合计 → CPU 侧混合（amplify/blend）才是大头
        //   回填 / 色调 大          → 这两条是纯 CPU 逐像素，可并行化
        //
        // 只读计时，不参与任何像素运算 —— 对画面逐字节零影响。采样 30 帧一条
        // （24fps 下约 1.25 秒一条），够取中位数又不刷屏。
        //
        // `frameNo_ <= 2` 与 GPU 磨皮那行同一条约定，为的是「短渲染也能看见一行」：
        // 台架/单帧导出这类宿主只渲染几帧，纯 `% 30` 会一条都打不出来，等于装完
        // 仪表却看不到读数。另外 frameNo_ 是**按槽位**保存的（SlotScope 换入换出），
        // 宿主每帧新建实例时它会一直停在 1，此时这条分支就是那条兜底。
        if (frameNo_ <= 2 || (frameNo_ % 30) == 0) {
            NALOG(BEAUTY_LOG_INFO,
                  "帧分解 %dx%d 掩码=%.2f 磨皮=%.2f 色调=%.2f 回填=%.2f 美形=%.2f"
                  " 合计=%.2f ms | 工人掩码=%.2f 脸=%d 掩码=%d",
                  w, h,
                  tMask - t0, tSmooth - tMask, tTone - tSmooth,
                  tDetail - tTone, tEnd - tDetail, tEnd - t0,
                  asyncMs_, lastFaces_, (int)maskValid_);
        }

        // ---- 诊断导出（1.1.2.1）：BEAUTY_DUMP_OUT=<目录> --------------------
        // 见 dumpBgraPpm 上方的说明。这里只在「本帧真的有掩码、真的检出了脸」时
        // 才导 —— 没有掩码的帧对「白块」定性没有意义，白导三张全黑的图。
        // 只导前 6 帧：用户的操作顺序是「播到白块处暂停 → ← → 强制重渲染」，
        // 前面可能已经有几帧被 Resolve 的缩略图/缓存渲染耗掉，留 6 个名额才
        // 保证白块那一帧一定在里面；再多人眼挑图也麻烦、磁盘也不划算
        // （1080p 一帧 src+dst+mask 约 14MB）。
        if (maskValid_ && lastFaces_ > 0 && !mask_.empty() &&
            (size_t)w * (size_t)h <= mask_.size()) {
            static const int kDumpFrames = 6;
            static char dumpDir[MAX_PATH] = {0};   // 1.1.5.0：改值拷贝，不再长期持有 getenv 指针
            static int dumpLeft = -1;      // -1 = 还没读过配置
            static int prevFrameNo = -1;   // 上一帧的帧号，用于识别「重渲染同一帧」
            static long long prevMs = 0;   // 上一帧的墙钟时刻，用于识别「停下来又渲染了一次」
            static int reLeft = 20;        // 覆盖式导出的剩余次数，防爆盘
            static int warnLeft = 1;       // 「开关没开」的提示只打一次，避免刷屏
            if (dumpLeft < 0) {
                // 1.1.5.0：三级优先级，与 beauty.ini 的既有体系（env > ini > 默认）一致。
                //
                // 为什么必须让开关落到磁盘上：2026-11-06 已经踩过一次同一个坑 ——
                // Resolve 从桌面图标启动时，launch.bat 里 set 的变量压根继承不到，
                // 于是「日志里一行调试输出都没有」被当成功能故障，白跑一个来回
                // （见 beauty_hwprobe.cpp 第 59–62 行记下的那次复盘）。
                // 1.1.3.0 的 dump 通道只认 BEAUTY_DUMP_OUT，等于把同一个坑又挖了一遍：
                // 1.1.4.0 那次验证就是空手而归 —— 环境变量没进来，dumpLeft 判成 0、
                // dumpDir 是空指针，而重渲染分支没有先判空，路径被拼成
                // "(null)\dump_re_src.ppm"，写失败且不报错（ofx.log 里有那行记录）。
                const char* e = getenv("BEAUTY_DUMP_OUT");
                if (e && *e) {
                    snprintf(dumpDir, sizeof(dumpDir), "%s", e);
                } else if (beautyCfgInt("dump_enable", 0) != 0) {
                    const char* d = beautyCfgStr("dump_dir", "");
                    if (d && *d) {
                        snprintf(dumpDir, sizeof(dumpDir), "%s", d);
                    } else {
                        const char* pd = getenv("ProgramData");
                        snprintf(dumpDir, sizeof(dumpDir), "%s\\DaVinciBeauty\\dump",
                                 (pd && *pd) ? pd : "C:\\ProgramData");
                    }
                }
                dumpLeft = dumpDir[0] ? kDumpFrames : 0;
                if (dumpDir[0]) CreateDirectoryA(dumpDir, nullptr);   // 已存在则返回 FALSE，忽略
            }
            // 1.1.3.0：新增「同一帧重渲染」通道。
            //
            // 实测教训：用户的操作是「播到白块处 → 空格暂停 → ← → 强制重渲染」，
            // 但前 6 个名额早在播放阶段就被开头的镜头用光了，导出来的永远是
            // 第 0 秒那几帧，对白块完全没用（连着两轮都是别的镜头）。
            // 暂停后按 ← → 时，宿主会重新请求**同一帧**，frameNo 与上一帧相同 ——
            // 就用这个信号覆盖写 dump_re_*.{ppm,pgm}，不占名额、不新增文件。
            const int curNo = (int)frameNo_;
            const long long curMs = (long long)t0;
            // 判据一：宿主重新请求**同一帧**（暂停后按 ← →、拖动播放头回原位等）。
            const bool sameFrame = (prevFrameNo >= 0 && prevFrameNo == curNo);
            // 判据二（1.1.5.0）：与上一帧的墙钟间隔超过 400ms。
            //
            // 为什么补这一条：1.1.4.0 那次真机验证日志里一条「诊断导出」都没有，
            // 说明判据一没成立 —— 宿主在暂停后重新渲染时，未必沿用原帧号（也可能是
            // 走了另一条求值路径）。连续播放时两帧间隔约 40ms，400ms 只有「停下来
            // 之后又渲染了一次」才会跨过，所以这条判据不会在播放期误触发。
            const bool afterPause = (prevMs > 0 && curMs - prevMs > 400);
            const bool reRender = sameFrame || afterPause;
            prevFrameNo = curNo;
            prevMs = curMs;

            if (dumpDir[0] && (dumpLeft > 0 || (reRender && reLeft > 0))) {
                const bool isRe = (dumpLeft <= 0);
                char tag[16];
                if (isRe) {
                    reLeft--;
                    snprintf(tag, sizeof(tag), "re");
                } else {
                    dumpLeft--;
                    snprintf(tag, sizeof(tag), "%d", kDumpFrames - 1 - dumpLeft);
                }
                const bool flip = imgBottomUp_;
                char path[MAX_PATH];
                snprintf(path, sizeof(path), "%s\\dump_%s_src.ppm", dumpDir, tag);
                dumpBgraPpm(path, src, w, h, flip);
                snprintf(path, sizeof(path), "%s\\dump_%s_dst.ppm", dumpDir, tag);
                dumpBgraPpm(path, dst, w, h, flip);
                snprintf(path, sizeof(path), "%s\\dump_%s_mask.pgm", dumpDir, tag);
                dumpGrayPgm(path, &mask_[0], w, h, flip);
                NALOG(BEAUTY_LOG_INFO,
                      "诊断导出 dump_%s %dx%d -> %s (flip=%d 脸=%d 帧号=%d 暂停帧=%d)",
                      tag, w, h, dumpDir, (int)flip, lastFaces_, curNo, (int)afterPause);
            } else if (reRender && !dumpDir[0] && warnLeft > 0) {
                // 1.1.5.0：开关没开时也要留一行痕迹。
                //
                // 上一轮排查之所以要多跑一个来回，就是因为「分支没进」和「进了但写
                // 失败」在日志里长得一样（都是什么都没有）。这一行把两者分开：
                // 有这行 = 触发信号到了，只是开关没开；没这行 = 连触发信号都没到。
                warnLeft--;
                NALOG(BEAUTY_LOG_INFO,
                      "诊断导出未启用：未设 BEAUTY_DUMP_OUT，且 %s 里 dump_enable 不是 1，"
                      "本次跳过（把 dump_enable 改成 1 后重启 Resolve 即可）",
                      beautyCfgPath());
            }
        }
        return true;
    }

    int lastFaces() const override { return t_lastFacesOut; }
    // v3.0.4：返回**本帧掩码那次检测**的候选最高分。
    //
    // 以前直接读 face_.lastBestScore() —— 那只在 detect() 跑完的线程上有值。
    // 异步检测时 detect() 在 worker 上跑，渲染线程读到的永远是 0.00，于是
    // 日志里「faces=0 score=0.00」既可能是「参数全 0 透传」也可能是「真的没
    // 检出脸」，两种情况无法区分。现在由 updateMask / collectAsync 把 worker
    // 的值搬到 lastScore_ 上，渲染线程读到的就是本帧的真实分数。
    float lastFaceScore() const override { return t_lastScoreOut; }
    bool regionActive() const override { return faceOk_; }
    double lastCoreMs() const override { return lastMs_; }
    // BUG4：这三样取「上一次 process 的只读快照」。成员本身在 process 退出时
    // 已经被换回别的实例了，直接读成员会读到邻居的值（或者空）。
    // 快照是 thread_local 的，指针只在本线程下一次 process() 之前有效 —— 调用方
    // （ofx_plugin.cpp）本来就是拿完即用，语义不变。
    const char* lastErr() const override { return t_lastErrOut.c_str(); }
    int itemId() const override { return 0; }
    unsigned long ownerThread() const override { return (unsigned long)GetCurrentThreadId(); }

    // 仅供离线对拍：强制重算一次掩码并拷出，不改磨皮状态、不动时序。
    // 返回 false 表示本帧没有可用掩码，此时 out 全 0。
    bool computeMaskForDump(const unsigned char* src, int w, int h,
                            unsigned char* out) {
        if (!out) return false;
        // 离线对拍必须确定性：停掉 worker，并且以后都走同步路径。
        // 异步下「第 k 帧用哪份掩码」取决于线程调度，哈希会飘。
        stopAsync();
        asyncOn_ = false;
        // BUG4：离线对拍用独立 slot（-1），不碰任何真实实例的掩码状态。
        SlotScope slotScope(this, -1);
        maskAge_ = 999;                 // 让它走重算分支，不吃缓存
        const bool ok = updateMask(-1, src, w, h);
        if (!ok) {
            memset(out, 0, (size_t)w * h);
            return false;
        }
        memcpy(out, &mask_[0], (size_t)w * h);
        return true;
    }

    void release() override {
        // P0：整段释放与 process() 互斥。范围要覆盖到 stopAsync/poolShutdown ——
        // 只锁 clear 的话，「渲染线程正在 updateMask、主线程在 join worker」这个
        // 窗口仍然存在，而且 join 之后 poolShutdown 收掉的正是渲染线程即将要用的
        // 线程池。recursive 锁允许下面 releaseSlot 那层再次进入。
        std::lock_guard<std::recursive_mutex> st(slotStateMx());
        stopAsync();          // 先停 worker，否则它可能正在用 face_
        face_.release();
        faceTried_ = false;
        faceOk_ = false;      // 模型已经卸了，别再让 process() 以为掩码路径可用
        // BUG4：per-slot 状态（掩码 / 对齐帧缓存 / EMA 历史）一并释放。
        // 1080p 下每个实例约 16MB，工程里挂了很多片段时不能只靠析构。
        {
            std::lock_guard<std::mutex> lk(slotMx_);
            slots_.clear();
        }
        // 3.1.21：持久线程池也要收掉。否则旧宿主卸载滤镜后 worker 线程还在，
        // 它们持有本 DLL 里的代码地址 —— 模块卸载时就是一次野跳转。
        beautycore::poolShutdown();
    }

private:
    // 首帧预热点（3.1.21）。
    //
    // 「开关美颜时预览窗卡一下」的根因不是稳态算力，而是**第一帧**要额外付三笔
    // 一次性成本，全部压在渲染线程上：
    //   ① smooth_ 这块 1080p 8.3MB 的工作缓冲要 malloc + 逐页缺页；
    //   ② 持久线程池的 7 个 worker 要创建、栈要缺页；
    //   ③ ORT 的第一次推理要分配 arena 并选定 kernel。
    // 实测 1080p 首帧 43.2 ms vs 稳态 21 ms，多出来的 22 ms 就是这三笔（外加
    // 首帧必然要跑的一次检测）。把它们挪到模块加载阶段（那里本来就要花 200 ms
    // 加载模型），用户开开关时就是纯稳态开销。
    //
    // 注意**不跑完整的 process()**：那会把肤色自适应中心（g_skinCb/Cr）按合成
    // 帧算歪，真机前几帧的掩码跟着错。这里只碰纯缓存与纯推理。
    void warmUp() {
        if (getenv("BEAUTY_NO_PREWARM")) return;   // A/B 用：跳过预热看首帧原始成本
        const int w = 1920, h = 1080;
        const size_t need = (size_t)w * h * 4;
        const size_t px = (size_t)w * h;
        if (smooth_.size() < need) {
            smooth_.assign(need, 0);   // 分配 + 逐页触碰（等价于首帧那次缺页）
        }
        // 低频/中间缓冲：默认路径未必用到，但一旦用到就是 8MB 级的首次缺页，
        // 这里先垫上（纯 scratch，写 0 不影响任何结果）。
        if (lowBuf_.size() < need) lowBuf_.assign(need, 0);
        if (scratch_.size() < need) scratch_.assign(need, 0);
        // 掩码与五官保护区同理。maskAge_ 显式压回 999，保证首帧仍然走重算分支，
        // 不会因为「缓冲已经有内容」而误用一份全 0 的掩码。
        if (mask_.size() != px) {
            mask_.assign(px, 0);
            protect_.assign(px, 0);
            maskAge_ = 999;
        }
        // 池提前建好；数量取和 process 里同一口径（≤8，且不超过核数）
        unsigned hc = std::thread::hardware_concurrency();
        int n = (hc == 0) ? 4 : (int)hc;
        if (n > 8) n = 8;
        beautycore::poolReserve(n);
        // ORT 首次推理：只走人脸检测，输入用 640×360 的合成灰帧（模型内部本来
        // 就缩到 640×640，用 1080p 只是白付 8MB 的分配）。
        if (faceOk_) {
            std::vector<unsigned char> fake((size_t)640 * 360 * 4, 128);
            std::vector<beautycore::FaceBox> dummy;
            std::string e;
            face_.detect(&fake[0], 640, 360, dummy, e, 0.9f, 0.3f, 4);
        }
    }

    // 生成「肤色 ∩ 人脸框」掩码。返回 false 表示本帧没有可用掩码。
    //
    // 这个函数只做三件轻活：① 收走 worker 算好的结果（O(1) swap）；② worker 空闲
    // 时把本帧提交出去；③ 沿用上一份掩码。**检测本身不在这里跑** —— 1080p 下
    // YuNet 一次约 15ms，压在渲染线程上就是 60fps 每 4 帧掉 1 帧。
    bool updateMask(int slot, const unsigned char* src, int w, int h) {
        if (!faceOk_) return false;
        if (mask_.size() != (size_t)w * h) {
            mask_.assign((size_t)w * h, 0);
            protect_.assign((size_t)w * h, 0);
            maskAge_ = 999;
            maskValid_ = false;      // 换分辨率等于重来：别拿旧尺寸的掩码凑合
            everHadMask_ = false;    // 新尺寸下第一帧按「透传」处理，见 process()
        }

        if (asyncOn_) {
            // v3.0.3：**从没拿到过掩码时，先同步做一次检测**。
            //
            // 异步的前提是「宿主会连续渲染很多帧」—— 旧宿主 30fps 下成立；但
            // Resolve / 单帧导出这类按需渲染的宿主可能只渲染一两帧，worker 的
            // 结果永远赶不回来，everHadMask_ 恒为 false，process() 就一直走
            // 「首帧透传」分支：表现为**插件完全没效果**（实测日志 sdk≈1ms、
            // faces=0、score=0.00，而且持续到最后一帧）。
            // 这里同步跑一次只多约 15ms，且仅限首帧，换来「第一帧就有掩码」。
            if (!everHadMask_) {
                std::string msg;
                MaskStat st;
                int faces = 0;
                if (buildMaskInto(src, w, h, mask_, protect_, bounds_, faces_, st, faces, msg,
                                  imgBottomUp_)) {
                    maskStat_ = st;
                    lastFaces_ = faces;
                    lastScore_ = face_.lastBestScore();   // v3.0.4：本帧真实分数
                    maskHold_ = 0;
                    maskValid_ = true;
                    everHadMask_ = true;
                    faceErr_ = msg;
                    maskAge_ = 0;
                    return true;
                }
                // 同步也没检出：本帧照旧透传，同时把结果丢给 worker，
                // 后面几帧还有机会补上。
                maskAge_ = 0;
                lastScore_ = face_.lastBestScore();
                submitAsync(src, w, h, slot);
                return onDetectFailed(msg, face_.inputSize(), face_.lastBestScore());
            }
            startAsyncIfNeeded();
            collectAsync(slot);             // 每帧都收，不受周期限制
            // 到了重算周期：提交一帧给 worker。周期 0 = 每帧提交。
            // worker 忙时 submitAsync 会自己放弃这一轮（见它的注释），
            // 所以 60fps + 1080p 这种「推理 15ms / 帧 16.7ms」的极端情况
            // 会自我节流成「隔帧一次」，滞后从 1 帧变成 1~2 帧，不会堆积。
            if (maskAge_ >= g_cfg.maskEveryAsync) {
                maskAge_ = 0;
                submitAsync(src, w, h, slot);
            } else {
                maskAge_++;
            }
            return maskValid_;              // 结果没回来就继续用旧掩码，不等
        }

        if (maskAge_ < kMaskEverySync) {    // 沿用上一次的掩码
            maskAge_++;
            return maskValid_;
        }
        maskAge_ = 0;

        std::string msg;
        MaskStat st;
        int faces = 0;
        if (!buildMaskInto(src, w, h, mask_, protect_, bounds_, faces_, st, faces, msg,
                           imgBottomUp_)) {
            lastScore_ = face_.lastBestScore();   // v3.0.4：同步路径本来就有值
            return onDetectFailed(msg, face_.inputSize(), face_.lastBestScore());
        }
        maskStat_ = st;
        lastFaces_ = faces;
        lastScore_ = face_.lastBestScore();
        maskHold_ = 0;
        maskValid_ = true;
        faceErr_ = msg;
        return true;
    }

    // 掩码诊断三件套（1.1.2.0 加）。目的只有一个：搞清楚「ROI 为什么这么大」。
    //
    //   skinPx  掩码非零像素数（脸 + 脖子，重叠处只算一次）
    //   neckPx  其中由**脖子通道**新增的像素数（脸已经画过的不重复计）
    //   faceBoxA 所有脸框（按 kFaceBoxGrow 外扩后）并集的面积
    //   tileA   （1.1.1.3 加）**按脸分块**后，各块自己包围盒的面积之和
    //
    // 这几个数跟 ROI 面积一比，就能判定 ROI 是被什么撑大的：
    //   盒/脸盒 接近 1   → 脸框本身就大，只能改脸框策略
    //   盒/脸盒 远大于 1 → 脸框外的皮肤（脖子、背景误判、多脸并集）在拉大 ROI
    //   脖/skin 占比高    → 脖子通道是主因，收窄它立刻见效
    //   分块 = tileA/盒 → **拆成每张脸一块**之后要搬的像素是现在的几倍。
    //                      ≥1 说明「单矩形」并不比「多块」吃亏，拆了白拆；
    //                      <1 才是多矩形重构的收益上限。
    //
    // 这几个数只是统计，**不参与任何像素写入**，所以对画面逐字节零影响。
    struct MaskStat {
        int skinPx     = 0;
        int neckPx     = 0;
        int faceBoxA   = 0;
        long long tileA = 0;
    };

    // 检测 + 肤色掩码 + 五官保护 + 包围盒，全部算进调用方给的缓冲。
    //
    // 刻意做成**只读 src、不碰任何成员**的函数：异步化时它整段跑在 worker 线程上，
    // 只要不碰成员就不存在数据竞争。这也是异步化能做得这么轻的原因 ——
    // 不需要给磨皮链加锁。
    bool buildMaskInto(const unsigned char* src, int w, int h,
                       std::vector<unsigned char>& maskOut,
                       std::vector<unsigned char>& protectOut,
                       beautycore::RoiRect& boundsOut,
                       std::vector<beautycore::FaceBox>& fboxOut,
                       MaskStat& stOut, int& facesOut, std::string& msgOut,
                       bool flipY) {
        // 1.2.8：掩码内部打点。BEAUTY_MASK_PROFILE=1 才启用，默认零开销
        // （只有 4 次 nowMsCore，不进任何像素循环）。日志一行：
        //   掩码分解 检测=.. memset=.. 肤色=.. 脸框+脖=.. 保护=.. 合计=..
        // 目的：1.2.7 时「掩码 11 ms 里绘制只占 2~3、检测约 2」，剩下 ~7 ms
        // 找不到出处，只能靠打点而不是继续猜。
        static const bool prof = (getenv("BEAUTY_MASK_PROFILE") != nullptr);
        const double tp0 = prof ? nowMsCore() : 0.0;
        double tpDet = 0, tpMem = 0, tpSkin = 0, tpFace = 0, tpProt = 0;

        if (maskOut.size() != (size_t)w * h) maskOut.assign((size_t)w * h, 0);
        if (protectOut.size() != (size_t)w * h) protectOut.assign((size_t)w * h, 0);

        std::vector<beautycore::FaceBox> faces;
        std::string ferr;
        // 直接喂 RGBA（stride=4）。原先要先摊一份全分辨率 RGB，1080p 下那是
        // 600 万字节的写入，而检测只吃 640x640 —— 这份中间缓冲是纯浪费。
        //
        // flipY（由调用方给，不在本函数里读成员 —— 异步时这里跑在 worker 上）：
        // 源是 bottom-up（OFX）时让检测按视觉方向读行，返回的框/关键点再映射回
        // 缓冲行号。掩码与几何美形本来就按缓冲行号落笔，下游一行都不用改
        // （详见 beauty_face.h 的 detect 注释）。
        // 1.2.9：能复用上一帧的人脸框就复用 —— 检测是掩码里最贵的一步
        // （1080p 实测 6.75 ms，占掩码 2/3），而导出/慢镜头下人像移动很慢。
        // 判据：64x36 灰度缩略图的平均绝对差 ≤ g_detThr，且距上次真检测不超过
        // g_detAge 次重算。任一条不满足就老实重测。
        // 回退：BEAUTY_DET_REUSE=0 逐字节回到旧行为。
        const int TW = 64, TH = 36;
        auto thumbOf = [&](std::vector<unsigned char>& out) {
            out.assign((size_t)TW * TH, 0);
            for (int ty = 0; ty < TH; ty++) {
                const int sy = (int)((long)ty * h / TH);
                const unsigned char* row = src + (size_t)sy * w * 4;
                for (int tx = 0; tx < TW; tx++) {
                    const int sx = (int)((long)tx * w / TW);
                    out[(size_t)ty * TW + tx] = (unsigned char)
                        ((66 * row[sx * 4 + 0] + 129 * row[sx * 4 + 1] +
                          25 * row[sx * 4 + 2]) >> 8);
                }
            }
        };
        bool reused = false;
        if (g_detReuse && detValid_ && detW_ == w && detH_ == h &&
            (int)detThumb_.size() == TW * TH) {
            std::vector<unsigned char> th;
            thumbOf(th);
            long acc = 0;
            for (size_t i = 0; i < th.size(); i++)
                acc += std::abs((int)th[i] - (int)detThumb_[i]);
            const double mad = (double)acc / (double)(TW * TH);
            if (mad <= g_detThr && detAge_ < g_detAge && !detFaces_.empty()) {
                faces = detFaces_;
                reused = true;
                detAge_++;
                g_detReuseHits++;
            }
        }
        if (!reused) {
            if (!face_.detect(src, w, h, faces, ferr, 0.6f, 0.3f, 4, flipY) ||
                faces.empty()) {
                msgOut = ferr;
                facesOut = 0;
                return false;
            }
            detFaces_ = faces;
            detW_ = w; detH_ = h; detAge_ = 0; detValid_ = true;
            thumbOf(detThumb_);
        }
        facesOut = (int)faces.size();
        // 关键点要留给后续的「五官局部」五项用（亮眼/去黑眼圈/法令纹…都要位置）。
        // 在这里一次性交出去，而不是让那几项自己去调检测 —— 一帧只跑一次推理。
        fboxOut = faces;
        tpDet = prof ? nowMsCore() : 0.0;

        unsigned char* mask = &maskOut[0];
        unsigned char* protect = &protectOut[0];
        memset(mask, 0, (size_t)w * h);
        tpMem = prof ? nowMsCore() : 0.0;

        // 肤色椭圆中心自适应（C20）：先按本帧画面估计一次，再画掩码。
        //
        // 线程安全：g_skinCb/g_skinCr 只有这一处写。异步模式下本函数整段跑在
        // 唯一的 worker 上，同步模式下跑在渲染线程上，两条路径互斥（asyncOn_），
        // 不存在并发写。skinWeight() 也只被下面的 paintSkin 调用，磨皮链不读它。
        //
        // EMA 平滑：默认 1.0 = 每帧直接采纳（纯函数，与调用次数无关）。
        // <1 的系数只在真机防抖时用，理由见 g_skinAdaptEma 的定义处。
        if (g_skinAdapt) {
            float ecb = 0.0f, ecr = 0.0f;
            if (estimateSkinCenter(src, w, h, faces, ecb, ecr, flipY)) {
                if (!g_skinAdaptValid) {
                    g_skinCb = ecb;          // 首帧直接采纳，避免从旧值慢慢爬
                    g_skinCr = ecr;
                    g_skinAdaptValid = true;
                    g_skinAnnounce.store(true, std::memory_order_relaxed);
                } else {
                    g_skinCb += (ecb - g_skinCb) * g_skinAdaptEma;
                    g_skinCr += (ecr - g_skinCr) * g_skinAdaptEma;
                }
            }
            // 估计失败（脸太小 / 被遮挡 / 极暗）时保持上一帧中心，不重置
        }

        tpSkin = prof ? nowMsCore() : 0.0;

        // 掩码像素数与包围盒在写入时顺带统计，不再单独扫两遍全帧。
        // 1080p 下那两遍各是 200 万次内存访问，纯属为诊断付的代价。
        // 只有「从 0 变 255」才计数，否则脸框与脖子通道重叠处会被重复算。
        int px = 0;
        // 脖子通道单独计数（1.1.2.0 诊断）。做法是「记下进脖子前的 px，
        // 出来后求差」—— paintSkin/paintW 只在 mrow[x] 由 0 变非零时才 ++px，
        // 所以这个差值天然不含脸已经占过的像素，不需要额外的去重逻辑。
        int neckPx = 0;
        int bx0 = w, by0 = h, bx1 = -1, by1 = -1;

        // 1.2.6：掩码绘制按行并行。绘制本身写的是**自己那一行**的 mask，
        // 行与行不重叠，所以可以切块；只有这两个统计量是共享的
        // （px = 新增像素数、包围盒），必须每块各记一份再合并。
        //
        // 为什么只并行脸框、不并行脖子带：脖子带的 colEnd 是 thread_local 的，
        // 由调用线程在上一趟算好，换到 worker 线程会读到空表。
        struct PaintStat {
            long px = 0;
            int bx0 = 1 << 30, by0 = 1 << 30, bx1 = -1, by1 = -1;
        };
        PaintStat pstat;
        std::mutex pstatMu;
        auto mergePaint = [&](const PaintStat& s) {
            std::lock_guard<std::mutex> lk(pstatMu);
            pstat.px += s.px;
            if (s.bx0 < pstat.bx0) pstat.bx0 = s.bx0;
            if (s.bx1 > pstat.bx1) pstat.bx1 = s.bx1;
            if (s.by0 < pstat.by0) pstat.by0 = s.by0;
            if (s.by1 > pstat.by1) pstat.by1 = s.by1;
        };
        auto rowThreads = [](int rows) {
            int nt = (g_cfg.threads > 0) ? g_cfg.threads
                                         : (int)std::thread::hardware_concurrency();
            if (nt > 16) nt = 16;
            if (nt > rows / 32) nt = rows / 32;
            return (nt < 1) ? 1 : nt;
        };

        // 单像素落笔：肤色权重 × 边界羽化系数，取最大值合并（脸框与脖子通道重叠）。
        // 返回 true = 这一像素真的落了非零值（用于统计包围盒的 y 范围）。
        auto paintSkin = [&](unsigned char* mrow, const unsigned char* row,
                             int x, int feather, PaintStat& st) {
            const int wgt = skinWeight(row[x * 4 + 0], row[x * 4 + 1],
                                       row[x * 4 + 2]);
            if (wgt <= 0) return false;
            const int v = (feather >= 256) ? wgt : (wgt * feather + 128) / 256;
            if (v <= 0) return false;
            if (!mrow[x]) {
                st.px++;
                if (x < st.bx0) st.bx0 = x;
                if (x > st.bx1) st.bx1 = x;
            }
            // 脸框与脖子通道可能重叠：取较大值，别让后画的覆盖掉
            if (v > (int)mrow[x]) mrow[x] = (unsigned char)v;
            return true;
        };

        // paintSkin 的「权重已算好」版本。脖子跟随肤色时，同一像素的权重
        // 要先用一次（探边界）、再用一次（落笔），重复调 skinWeight 里那个 sqrt
        // 是白费的。
        auto paintW = [&](unsigned char* mrow, int x, int wgt, int feather,
                          PaintStat& st) {
            if (wgt <= 0) return false;
            const int v = (feather >= 256) ? wgt : (wgt * feather + 128) / 256;
            if (v <= 0) return false;
            if (!mrow[x]) {
                st.px++;
                if (x < st.bx0) st.bx0 = x;
                if (x > st.bx1) st.bx1 = x;
            }
            if (v > (int)mrow[x]) mrow[x] = (unsigned char)v;
            return true;
        };

        for (const auto& f : faces) {
            const float cx = f.x + f.w * 0.5f, cy = f.y + f.h * 0.5f;
            const float hw = f.w * kFaceBoxGrow * 0.5f, hh = f.h * kFaceBoxGrow * 0.5f;
            int x0 = (int)(cx - hw), x1 = (int)(cx + hw);
            int y0 = (int)(cy - hh), y1 = (int)(cy + hh);
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > w) x1 = w;
            if (y1 > h) y1 = h;

            // v3.0.9：帧行序（开发指南 约束⑧）。
            // f.y 是**缓冲行号**：bottom-up 时 y0 才是视觉下沿（下巴一侧）、
            // y1 是视觉上沿（发际线一侧）；top-down 时反过来。
            // 以前这里把 y1 一律当「下巴」，于是 Resolve 上整条脖子通道被铺到了
            // 发际线以上 —— 实测高出 0.65 脸高、一直到画面顶边，用户看到的就是
            // 「遮罩比额头还高一大截」，而且真正的脖子一点没磨到。
            const int yChin = flipY ? y0 : y1;   // 视觉下沿（下巴一侧）
            const int yTop = flipY ? y1 : y0;    // 视觉上沿（发际线一侧）

            // 脖子通道的横向范围。脸框底边在它之内时**不做下羽化**，否则下巴
            // 与脖子之间会出现一条水平接缝。
            int nx0 = (int)(cx - f.w * g_neck.halfW);
            int nx1 = (int)(cx + f.w * g_neck.halfW);
            if (nx0 < 0) nx0 = 0;
            if (nx1 > w) nx1 = w;

            int fe = (g_maskFeatherPx >= 0)
                         ? g_maskFeatherPx
                         : (int)(f.w * kMaskFeatherRatio + 0.5f);
            if (fe > kFeatherMax) fe = kFeatherMax;
            if (fe < 0) fe = 0;
            // 脖子通道的羽化宽度（1.1.3.0 起按带长，1.1.4.0 提到 0.50）。
            //
            // 旧写法 2*halfW*f.w*kMaskFeatherRatio = 0.072 脸宽，脸宽 255 px 时
            // 只有 18 px。而带长是 0.65 脸高 ≈ 156 px —— 18 px 的斜坡接不住
            // 156 px 的末端，下缘那条横线就是这么来的（详细取证见
            // kNeckFeatherRatio 的定义处）。
            //
            // 现在 0.50 × 带长：156 px 带子算 78 px，被 kFeatherMax=64 截到
            // 64 px，前 50% 仍是满强度。
            const float neckLenPxForFe = f.h * g_neck.len;
            int feNeck = (g_maskFeatherPx >= 0)
                             ? g_maskFeatherPx
                             : (int)(neckLenPxForFe * kNeckFeatherRatio + 0.5f);
            if (feNeck > kFeatherMax) feNeck = kFeatherMax;
            if (feNeck < 0) feNeck = 0;

            if (g_maskShape & 1) {
                // 椭圆距离场（3.1.20）：边界按 (x/a)²+(y/b)² = 1 收敛，四条直边消失。
                // 到边界的像素距离用 (1−q)·rEff 近似（q 是归一化半径），脸框接近方形
                // 时误差 <10%；羽化斜坡吃的是整数像素，这点误差看不出来。
                const float a = hw, b = hh;
                const float ia2 = 1.0f / (a * a), ib2 = 1.0f / (b * b);
                const float rEff = 0.5f * (a + b);
                auto ellBody = [&](int yb, int ye) {
                    PaintStat st;
                    for (int y = yb; y < ye; y++) {
                        const unsigned char* row = src + ((size_t)y * w) * 4;
                        unsigned char* mrow = &mask[(size_t)y * w];
                        const float dy = (float)y + 0.5f - cy;
                        const float ty = dy * dy * ib2;
                        bool any = false;
                        for (int x = x0; x < x1; x++) {
                            const float dx = (float)x + 0.5f - cx;
                            const float q = sqrtf(dx * dx * ia2 + ty);
                            if (q >= 1.0f) continue;      // 椭圆外不落笔，等价于旧的矩形裁剪
                            const int dpx = (int)((1.0f - q) * rEff + 0.5f);
                            if (paintSkin(mrow, row, x, featherRamp(dpx, fe), st)) any = true;
                        }
                        // 只在**这一行真的落了像素**时扩 y：否则椭圆上下顶点那几行没肤色，
                        // 也会把包围盒拽到最大，ROI 白白变大、性能数据失真。
                        if (any) {
                            if (y < st.by0) st.by0 = y;
                            if (y > st.by1) st.by1 = y;
                        }
                    }
                    mergePaint(st);
                };
                const int ntE = rowThreads(y1 - y0);
                if (ntE <= 1) ellBody(y0, y1);
                else beautycore::parallelRows(y0, y1, ntE, ellBody);
            } else {
            auto rectBody = [&](int yb, int ye) {
            PaintStat st;
            for (int y = yb; y < ye; y++) {
                const unsigned char* row = src + ((size_t)y * w) * 4;
                unsigned char* mrow = &mask[(size_t)y * w];
                // 羽化只削「远离脸」的边：上沿永远是发际线一侧，下沿是下巴一侧。
                const int fTop = featherRamp((y > yTop ? y - yTop : yTop - y) + 1, fe);
                const int fBot = featherRamp(yChin > y ? yChin - y : y - yChin, fe);
                bool any = false;
                for (int x = x0; x < x1; x++) {
                    const int dl = x - x0 + 1, dr = x1 - x;
                    int feather = fTop;
                    const int fx = featherRamp(dl < dr ? dl : dr, fe);
                    if (fx < feather) feather = fx;
                    // 下巴一侧的底边**永远**羽化（1.1.8.0）。
                    //
                    // 旧写法在 [nx0, nx1) 内整段跳过 fBot，本意是「下巴正下方交给
                    // 脖子通道续上，底边不羽化就不会出现接缝」。但它假设了脖子通道
                    // 一定覆盖这段宽度 —— 实测不成立：脖子带的左右边界是逐行按肤色
                    // 探出来的，遇到项链、下颌阴影、头发、深色衣领就会比 [nx0, nx1)
                    // 窄，没被盖住的那些列于是在脸框底边**一整行内从满值掉到 0**。
                    // 真机取证（1920x1080，`02_街头_正面微笑` 第 10 帧）：脸框底边
                    // y=667 上一行 215..255、下一行直接 0，横跨 306 列 —— 就是用户
                    // 看到的那条横线；它跟着脸走，因为那就是脸框的底边。
                    //
                    // 现在改成无条件参与 min：脖子带已经**向上多铺了 fe 行**（见下面
                    // ny0），在交界那几行它本来就是满值，取 max 之后仍然是满值，所以
                    // 不会重新出现「下巴接缝」；而脖子带没盖到的地方会老老实实按 fe
                    // 平滑淡出，不再有一级台阶。
                    if (fBot < feather) feather = fBot;
                    if (paintSkin(mrow, row, x, feather, st)) any = true;
                }
                // 只在**这一行真的落了像素**时扩 y：否则脸框下沿那几行没肤色，
                // 也会把包围盒拽到最大，ROI 白白变大、性能数据失真。
                if (any) {
                    if (y < st.by0) st.by0 = y;
                    if (y > st.by1) st.by1 = y;
                }
            }
            mergePaint(st);
            };
            {
                const int ntR = rowThreads(y1 - y0);
                if (ntR <= 1) rectBody(y0, y1);
                else beautycore::parallelRows(y0, y1, ntR, rectBody);
            }
            }

            // 脖子通道：从**下巴一侧**接着往视觉下方铺，横向收窄到 0.45 脸宽，
            // 仍然只认肤色。
            //
            // v3.0.9：起点从写死的 y1 改成 yChin。bottom-up（Resolve）下 y1 是
            // 发际线一侧，旧写法把「脖子」铺到了头顶以上；top-down 下 yChin==y1，
            // 与旧版逐字节相同（离线基线因此不变）。
            // 1.1.8.0：起点从下巴**往脸那一侧多铺 fe 行**，与脸框底边的羽化带重叠。
            //
            // 为什么必须重叠：底边羽化（fBot）现在无条件生效，如果脖子带仍然从
            // yChin 才开始，脸框最后一行会被 fBot 削到 ~0、下一行立刻满值 ——
            // 那就是把「横线」从底边挪到了下一行。多铺 fe 行之后，重叠区里
            // 脖子带是满值、取 max 后也是满值，交界处两边都连续。
            const int ny0 = (flipY ? yChin + fe : yChin - fe);
            int ny0c = ny0;
            if (ny0c < 0) ny0c = 0;
            if (ny0c > h) ny0c = h;
            const float neckLen = f.h * g_neck.len;
            int ny1 = (int)((flipY ? (float)yChin - neckLen : (float)yChin + neckLen) + 0.5f);
            if (ny1 < 0) ny1 = 0;
            if (ny1 > h) ny1 = h;
            // 统一按 [nyLo, nyHi) 走，羽化只削「远离脸」的那一端（ny1）。
            const int nyLo = (ny0c < ny1) ? ny0c : ny1;
            const int nyHi = (ny0c < ny1) ? ny1 : ny0c;
            const int nyFar = ny1;

            // 诊断（1.1.1.3）：这一张脸**单独成块**时要搬的像素数。
            // 块 = 外扩脸框 ∪ 脖子带，再按 pad 往四周放 —— pad 与 bounds_ 用的
            // 是同一个 (g_cfg.radius + 1)，所以「分块 ÷ 盒」可以横向直接比。
            // 已知偏差（都朝**高估分块面积**的方向，即低估收益，属安全方向）：
            //   ① 相邻块可能互相重叠，这里照样各算一次；
            //   ② 用的是理论矩形，不是肤色实际探到的边界。
            {
                const int padT = g_cfg.radius + 1;
                const int ty0 = (y0 < nyLo) ? y0 : nyLo;
                const int ty1 = (y1 > nyHi) ? y1 : nyHi;
                const int tw = (x1 - x0) + 2 * padT;
                const int th = (ty1 - ty0) + 2 * padT;
                if (tw > 0 && th > 0) stOut.tileA += (long long)tw * (long long)th;
            }
            // 诊断：三条脖子分支（跟随肤色 / 椭圆 / 矩形）共用一个「前后求差」，
            // 不必给每一条都插计数器 —— 分支出错时差值一样能反映出来。
            const long pxBeforeNeck = pstat.px;
            PaintStat bandSt;
            if (g_maskShape & 4) {
                // 脖子：**逐行跟随肤色的实际边界**（3.1.20）。
                //
                // 旧版 nx0..nx1 是固定 0.90 脸宽的矩形。脖子比它宽时，那两条竖边就
                // 切在皮肤上：磨与不磨的分界是一条直线，头部一摆就在脖子上来回扫，
                // 用户看到的「遮罩残影」有一半是它（另一半是检测滞后，见 BEAUTY_PROFILE
                // 的对齐统计）。这里从中心向两侧探到肤色真正结束的位置，边界落在
                // 皮肤/头发/衣领的**真实轮廓**上，不再是人为几何。
                static thread_local std::vector<unsigned char> wrow;
                if (wrow.size() < (size_t)w) wrow.assign((size_t)w, 0);
                const int cxI = (int)cx;
                const int maxHalf = (int)(f.w * kNeckSkinMaxHalf);
                const int minHalf = (int)(f.w * kNeckSkinMinHalf);
                int rowL = cxI - maxHalf;
                int rowR = cxI + maxHalf + 1;
                if (rowL < 0) rowL = 0;
                if (rowR > w) rowR = w;
                const int gap = 3;      // 连续 3 个非肤色像素算「到头」
                // 下缘收口（1.1.6.0）：参数见 NeckCfg::taper 的注释。
                // taperK == 0 时下面这段整段旁路，逐像素回落到 1.1.5.0 的
                // 「统一 nyFar」写法 —— 那条路径必须逐字节不变，用来做回归判据。
                const float taperK = g_neck.taper;
                const float neckHalfN = f.w * g_neck.halfW;
                const float invNeckHalf2 =
                    (taperK > 0.0f && neckHalfN > 1.0f) ? 1.0f / (neckHalfN * neckHalfN) : 0.0f;
                const float dirN = flipY ? -1.0f : 1.0f;   // 朝「远离脸」的方向
                const float yChinF = (float)yChin;

                // 下缘跟随肤色（1.1.7.0）：先扫出**每列的下缘深度** colEnd[x]，
                // 再拿它参与落笔与羽化。必须两趟 —— 单趟只能在下缘已越过之后
                // 才知道下缘在哪，那时像素本来就不落笔，缩不缩输出完全一样
                // （2026-11-07 实测：单趟实现跑基线 18/18 全不变，即改动无效）。
                //
                // 趟 1 逐行推：colEnd 取「这一列最后一个肤色行的深度」，判据是
                // 连续 gap(=3) 行非肤色；上限是该列的几何深度 colDmax（taper 的弧）。
                // 起始行没肤色不算「结束」（下巴阴影、项链扣），要先见过肤色才开始计数。
                // −1 = 整列没探到肤色 → 落笔阶段退回几何上限，行为与 1.1.6.0 相同。
                //
                // 边界落在哪：脖子下缘是「皮肤 → 锁骨下的皮肤 → 衣物」这条真实轮廓，
                // 那里本来就有明暗与纹理变化，64 px 的羽化过渡藏得进去；而 1.1.6.0 的
                // 几何弧不论怎么弯都横切在同一块皮肤中间，两侧亮度相同 —— 这才是
                // 用户看到「淡淡的边」的真正来源（真图取证见 NeckCfg::skinEnd）。
                static thread_local std::vector<short> colEnd;
                static thread_local std::vector<short> colLast;
                static thread_local std::vector<int> colDmax;
                static thread_local std::vector<unsigned char> colSeen, colMiss;
                const bool skinEndOn = (g_neck.skinEnd != 0);
                if (skinEndOn) {
                    if (colEnd.size() < (size_t)w) {
                        colEnd.assign((size_t)w, -1);
                        colLast.assign((size_t)w, -1);
                        colDmax.assign((size_t)w, 0);
                        colSeen.assign((size_t)w, 0);
                        colMiss.assign((size_t)w, 0);
                    }
                    for (int x = rowL; x < rowR; x++) {
                        colEnd[x] = -2;          // −2 = 尚未定案
                        colLast[x] = -1;
                        colSeen[x] = 0;
                        colMiss[x] = 0;
                        float dg = neckLen;
                        if (taperK > 0.0f) {
                            const float dxn = (float)x - cx;
                            float q2 = dxn * dxn * invNeckHalf2;
                            if (q2 > 1.0f) q2 = 1.0f;
                            dg = neckLen * (1.0f - taperK * q2);
                        }
                        colDmax[x] = (int)(dg + 0.5f);
                    }
                    for (int y = nyLo; y < nyHi; y++) {
                        const int dRow1 = (int)(dirN * ((float)y - yChinF) + 0.5f);
                        if (dRow1 < 0) continue;
                        const unsigned char* row1 = src + ((size_t)y * w) * 4;
                        for (int x = rowL; x < rowR; x++) {
                            if (colEnd[x] != -2) continue;
                            if (dRow1 > colDmax[x]) {
                                colEnd[x] = (short)(colSeen[x] ? colLast[x] : -1);
                                continue;
                            }
                            const int wv = (int)skinWeight(
                                row1[x * 4 + 0], row1[x * 4 + 1], row1[x * 4 + 2]);
                            if (wv) {
                                colSeen[x] = 1;
                                colLast[x] = (short)dRow1;
                                colMiss[x] = 0;
                            } else if (colSeen[x] && (unsigned char)(++colMiss[x]) >= (unsigned char)gap) {
                                colEnd[x] = colLast[x];
                            }
                        }
                    }
                    for (int x = rowL; x < rowR; x++) {
                        if (colEnd[x] != -2) continue;
                        // 像素级噪声（一颗痘、一根汗毛）不该把整列砍到 4 px 以内，
                        // 那不像真实衣领。小于 4 px 的一律当噪声，退回几何上限。
                        colEnd[x] = (short)((colSeen[x] && colLast[x] >= 4) ? colLast[x] : -1);
                    }
                }
                for (int y = nyLo; y < nyHi; y++) {
                    const unsigned char* row = src + ((size_t)y * w) * 4;
                    unsigned char* mrow = &mask[(size_t)y * w];
                    for (int x = rowL; x < rowR; x++) {
                        wrow[x] = (unsigned char)skinWeight(
                            row[x * 4 + 0], row[x * 4 + 1], row[x * 4 + 2]);
                    }
                    int lo = cxI, hi = cxI;
                    if (cxI < rowL || cxI >= rowR || wrow[cxI] == 0) {
                        // 中心不是肤色（衣领挡住、下巴阴影）：这一行退回兜底宽度，
                        // 落笔时 wrow 仍然会把非肤色挡掉，不会真的画上去。
                        lo = cxI - minHalf;
                        hi = cxI + minHalf;
                    } else {
                        int miss = 0;
                        for (int x = cxI; x >= rowL; x--) {
                            if (wrow[x]) { lo = x; miss = 0; }
                            else if (++miss >= gap) break;
                        }
                        miss = 0;
                        for (int x = cxI; x < rowR; x++) {
                            if (wrow[x]) { hi = x; miss = 0; }
                            else if (++miss >= gap) break;
                        }
                        // 探到的范围太窄说明肤色被打断（胡须、镜框、阴影），
                        // 按兜底宽度铺开，免得掩码被切成碎片。
                        if (hi - lo < 2 * minHalf) {
                            lo = cxI - minHalf;
                            hi = cxI + minHalf;
                        }
                    }
                    if (lo < rowL) lo = rowL;
                    if (hi > rowR - 1) hi = rowR - 1;
                    if (lo < 0) lo = 0;
                    bool any = false;
                    for (int x = lo; x <= hi; x++) {
                        int dBot;
                        if (taperK > 0.0f) {
                            // 末端按列收成圆弧：dx 按 0.45 脸宽归一化后平方，靠外收得多。
                            // 越过弧线的像素必须跳过 —— 否则 |farX−y| 会在弧外重新变大，
                            // featherRamp 又会把已经收掉的部分涂回来，收口等于没做。
                            const float dxn = (float)x - cx;
                            float q2 = dxn * dxn * invNeckHalf2;
                            if (q2 > 1.0f) q2 = 1.0f;
                            float depthX = neckLen * (1.0f - taperK * q2);
                            // 1.1.7.0：这一列肤色在弧之前就结束了（衣领/吊带边），
                            // 下缘改钉在最后一个肤色行上。取 min 保证只缩不扩。
                            // 羽化按**新下缘**算，靠近衣领的皮肤才会逐渐减弱 ——
                            // 这正是把分界藏进衣物明暗变化里的关键。
                            if (skinEndOn && colEnd[x] >= 0) {
                                const float dSkin = (float)colEnd[x];
                                if (dSkin < depthX) depthX = dSkin;
                            }
                            if (dirN * ((float)y - yChinF) > depthX) continue;
                            dBot = (int)fabsf(yChinF + dirN * depthX - (float)y);
                        } else if (skinEndOn && colEnd[x] >= 0) {
                            // 水平截断分支同样跟随肤色末端（taper=0 的逐字节回归
                            // 只在 skinEnd=0 时才走，两者不会同时生效，互不干扰）。
                            const float dSkin = (float)colEnd[x];
                            if (dirN * ((float)y - yChinF) > dSkin) continue;
                            dBot = (int)fabsf(yChinF + dirN * dSkin - (float)y);
                        } else {
                            dBot = nyFar > y ? nyFar - y : y - nyFar;
                        }
                        int feather = featherRamp(dBot, feNeck);
                        const int dl = x - lo + 1, dr = hi - x + 1;
                        const int fx2 = featherRamp(dl < dr ? dl : dr, feNeck);
                        if (fx2 < feather) feather = fx2;
                        if (paintW(mrow, x, wrow[x], feather, bandSt)) any = true;
                    }
                    if (any) {
                        if (y < bandSt.by0) bandSt.by0 = y;
                        if (y > bandSt.by1) bandSt.by1 = y;
                    }
                }
                mergePaint(bandSt);
            } else if (g_maskShape & 2) {
                // 脖子也用椭圆距离场（3.1.20）：最宽处落在下颌线（yChin），向下按椭圆收窄。
                // 旧版是 nx0..nx1 的**竖直直边**，头部摆动时两条直边在脖子上来回扫，
                // 用户看到的「遮罩残影」正是它们。椭圆没有直边段，收窄也连续。
                const float A = f.w * g_neck.halfW;
                const float B = f.h * g_neck.len;
                const float ia2 = 1.0f / (A * A), ib2 = 1.0f / (B * B);
                const float rEff = 0.5f * (A + B);
                const float cyN = (float)yChin;   // v3.0.9：下颌线，不是写死的 y1
                int ey0 = (int)(cyN - B);
                int ey1 = (int)(cyN + B);
                if (ey0 < 0) ey0 = 0;
                if (ey1 > h) ey1 = h;
                for (int y = ey0; y < ey1; y++) {
                    const unsigned char* row = src + ((size_t)y * w) * 4;
                    unsigned char* mrow = &mask[(size_t)y * w];
                    const float dy = (float)y + 0.5f - cyN;
                    const float ty = dy * dy * ib2;
                    bool any = false;
                    PaintStat st;
                    for (int x = nx0; x < nx1; x++) {
                        const float dx = (float)x + 0.5f - cx;
                        const float q = sqrtf(dx * dx * ia2 + ty);
                        if (q >= 1.0f) continue;
                        const int dpx = (int)((1.0f - q) * rEff + 0.5f);
                        if (paintSkin(mrow, row, x, featherRamp(dpx, feNeck), st)) any = true;
                    }
                    if (any) {
                        if (y < st.by0) st.by0 = y;
                        if (y > st.by1) st.by1 = y;
                    }
                    mergePaint(st);
                }
            } else {
            for (int y = nyLo; y < nyHi; y++) {
                const unsigned char* row = src + ((size_t)y * w) * 4;
                unsigned char* mrow = &mask[(size_t)y * w];
                const int fBot = featherRamp(nyFar > y ? nyFar - y : y - nyFar, feNeck);
                bool any = false;
                PaintStat st;
                for (int x = nx0; x < nx1; x++) {
                    const int dl = x - nx0 + 1, dr = nx1 - x;
                    int feather = fBot;
                    const int fx = featherRamp(dl < dr ? dl : dr, feNeck);
                    if (fx < feather) feather = fx;
                    if (paintSkin(mrow, row, x, feather, st)) any = true;
                }
                if (any) {
                    if (y < st.by0) st.by0 = y;
                    if (y > st.by1) st.by1 = y;
                }
                mergePaint(st);
            }
            }
            neckPx += (int)(pstat.px - pxBeforeNeck);
        }

        // 五官保护区：**只画眼睛，不画嘴巴**。
        // 参考版的掩码只有 .g = eyes 一个保护通道，没有 lip —— 嘴唇在参考版那里
        // 是被当皮肤磨的。3.1.14 之前我们多画了一个嘴部椭圆（kps[6..9] 那四行），
        // 那是自创的保护，100 档唇区残差 +25.5 的最可能根因，已删除以对齐结构。
        // 尺寸按脸框比例给（YuNet 只给 5 个点，没有轮廓，只能这样估）。
        memset(protect, 0, (size_t)w * h);
        for (const auto& f : faces) {
            if (!kpsSane(f)) continue;    // 关键点飞出去就跳过这张脸的保护
            const float ex = f.w * 0.12f, ey = f.h * 0.075f;
            paintProtectEllipse(protect, w, h, f.kps[0], f.kps[1], ex, ey);
            paintProtectEllipse(protect, w, h, f.kps[2], f.kps[3], ex, ey);
        }
        tpProt = prof ? nowMsCore() : 0.0;
        if (prof) {
            const double tEnd = nowMsCore();
            char pb[256];
            snprintf(pb, sizeof(pb),
                     "掩码分解 检测=%.2f memset=%.2f 肤色=%.2f 脸框+脖=%.2f 保护=%.2f 合计=%.2f",
                     tpDet - tp0, tpMem - tpDet, tpSkin - tpMem,
                     tpProt - tpSkin, tEnd - tpProt, tEnd - tp0);
            beauty_logv(BEAUTY_LOG_INFO, "%s", pb);
        }

        // 诊断：脸框并集面积。取**并集**而不是求和 —— 两张脸重叠时求和自己就把
        // 面积吹大了，那正好会把「多脸并集拉大 ROI」这个待查原因掩盖掉。
        // 外扩系数用 kFaceBoxGrow，与掩码绘制时用的框保持一致，否则比出来的倍数没意义。
        int fbx0 = w, fby0 = h, fbx1 = -1, fby1 = -1;
        for (const auto& f : faces) {
            const float cx = f.x + f.w * 0.5f, cy = f.y + f.h * 0.5f;
            const float hw = f.w * kFaceBoxGrow * 0.5f, hh = f.h * kFaceBoxGrow * 0.5f;
            int fx0 = (int)(cx - hw), fx1 = (int)(cx + hw);
            int fy0 = (int)(cy - hh), fy1 = (int)(cy + hh);
            if (fx0 < 0) fx0 = 0;
            if (fy0 < 0) fy0 = 0;
            if (fx1 > w) fx1 = w;
            if (fy1 > h) fy1 = h;
            if (fx1 <= fx0 || fy1 <= fy0) continue;
            if (fx0 < fbx0) fbx0 = fx0;
            if (fy0 < fby0) fby0 = fy0;
            if (fx1 > fbx1) fbx1 = fx1;
            if (fy1 > fby1) fby1 = fy1;
        }
        px = (int)pstat.px;
        if (pstat.bx1 >= 0) { bx0 = pstat.bx0; bx1 = pstat.bx1; by0 = pstat.by0; by1 = pstat.by1; }
        stOut.skinPx   = px;
        stOut.neckPx   = neckPx;
        stOut.faceBoxA = (fbx1 > fbx0 && fby1 > fby0) ? (fbx1 - fbx0) * (fby1 - fby0) : 0;

        // ROI 只在这里算一次（掩码每 4 帧才重算），中间帧直接复用。
        // pad 必须 >= radius：ROI 边缘像素的邻域会往外读，切掉了结果就变了。
        if (bx1 < 0) {
            boundsOut = beautycore::RoiRect();          // 全零掩码
        } else {
            const int pad = g_cfg.radius + 1;
            beautycore::RoiRect r;
            r.x0 = (bx0 - pad < 0) ? 0 : bx0 - pad;
            r.y0 = (by0 - pad < 0) ? 0 : by0 - pad;
            r.x1 = (bx1 + pad + 1 > w) ? w : bx1 + pad + 1;
            r.y1 = (by1 + pad + 1 > h) ? h : by1 + pad + 1;
            boundsOut = r;
        }

        // 诊断：五官保护覆盖率按椭圆面积估，不扫全帧
        double prArea = 0.0;
        for (const auto& f : faces) {
            if (!f.hasKps) continue;
            const double ex = f.w * 0.12, ey = f.h * 0.075;
            prArea += 2.0 * 3.14159265 * ex * ey;
            const double mrx = fabsf(f.kps[8] - f.kps[6]) * 0.5 + f.w * 0.05;
            prArea += 3.14159265 * mrx * (f.h * 0.09);
        }
        char b[256];
        // 1.1.2.0：加 neck / faceBox，把「ROI 被谁撑大」写进同一行。
        // 脖= 与 脸= 相加可能大于 mask 总数以外的口径差异不在这里体现 ——
        // 两个数都取自同一个 px 累加器，neck 是其中一部分，脸 = mask - neck。
        snprintf(b, sizeof(b),
                 "mask=%d px (%.2f%%) neck=%d (%.2f%%) faces=%d roi=%.2f%% facebox=%.2f%% protect=%.2f%% kps=%d",
                 px, 100.0 * px / (double)((size_t)w * h),
                 neckPx, 100.0 * neckPx / (double)((size_t)w * h),
                 facesOut,
                 100.0 * (double)boundsOut.area() / (double)((size_t)w * h),
                 100.0 * (double)stOut.faceBoxA / (double)((size_t)w * h),
                 100.0 * prArea / (double)((size_t)w * h),
                 (faces.empty() || !faces[0].hasKps) ? 0 : 1);
        msgOut = b;
        return true;
    }

    // 检不出脸时的统一处理（同步/异步两条路共用）：
    // 丢掉一次检测结果 ≠ 脸消失了，先沿用旧掩码最多 kMaskHold 次。
    bool onDetectFailed(const std::string& ferr, int inputSize, float bestScore) {
        lastFaces_ = 0;
        if (maskValid_ && maskHold_ < kMaskHold) {
            maskHold_++;
            char b[128];
            snprintf(b, sizeof(b), "detect lost, holding mask %d/%d",
                     maskHold_, kMaskHold);
            faceErr_ = b;
            return true;
        }
        maskValid_ = false;
        bounds_ = beautycore::RoiRect();     // 空矩形 = 无 ROI，调用方退回全帧
        // 诊断：把「推理输入边长」和「最高置信度」留下来。检不出脸时这两个
        // 数决定了该调尺寸还是该调阈值，缺了就只能瞎猜。
        char b[160];
        snprintf(b, sizeof(b), "no face x%d (input=%d best=%.3f%s%s)",
                 kMaskHold + 1, inputSize, bestScore,
                 ferr.empty() ? "" : " err=", ferr.c_str());
        faceErr_ = b;
        return false;
    }

    // ---- 异步检测 ------------------------------------------------
    //
    // 模型：一个 worker 线程 + 一个「输入槽」+ 一个「输出槽」。
    //   · 渲染线程：把结果 swap 走（O(1)），把本帧 memcpy 进输入槽（空闲时）。
    //   · worker：等输入槽有货 → 跑 buildMaskInto → 结果放进输出槽。
    // 渲染线程**永远不 wait**，所以检测再慢也只是「掩码晚一帧到位」。
    //
    // 为什么不做多缓冲/队列：检测只要 15ms，而提交周期是 4 帧（133ms），
    // 一个槽位绰绰有余。排队的意义是「一帧都不能丢」，但掩码本来就只需要最新
    // 那份 —— 中间结果没有价值（同类实现也都是「只保留最新帧」）。
    struct AsyncState {
        std::mutex m;
        std::condition_variable cv;
        std::thread th;
        bool quit = false;
        bool hasJob = false;      // 输入槽有货
        bool working = false;     // worker 正在算
        bool ready = false;       // 输出槽有结果待取
        std::vector<unsigned char> frame;
        int w = 0, h = 0;
        // 提交这一帧时的行序（v3.0.4）。存进任务里而不是让 worker 读成员：
        // worker 与渲染线程只通过本结构通信。
        bool flipY = true;
        std::vector<unsigned char> mask;
        std::vector<unsigned char> protect;
        beautycore::RoiRect bounds;
        std::vector<beautycore::FaceBox> fbox;   // 本帧人脸（含关键点）
        MaskStat pixels;
        int faces = 0;
        bool valid = false;
        int inputSize = 0;
        float bestScore = 0.0f;
        std::string msg;
        double ms = 0.0;
        // BUG4：这个任务是谁提交的。只有提交者本人能取走结果 —— 否则
        // 「B 收走 A 的掩码」会把 A 的掩码装进 B 的状态里（串台的另一半）。
        int slot = -1;
        unsigned long long readyMs = 0;   // 结果就绪时刻（防孤儿任务把队列堵死）
    };
    AsyncState async_;
    bool asyncOn_ = true;         // 由 g_cfg.asyncDetect 初始化
    bool asyncStarted_ = false;
    double asyncMs_ = 0.0;        // 最近一次 worker 的耗时（诊断用）

    // ---- 3.1.10 掩码 ↔ 帧对齐（修「羽化压不住的拖影」）----------------------
    //
    // 3.1.9 之前，`mask_` 与它描述的帧是**脱钩**的：worker 在帧 k 上算掩码，
    // 渲染线程在帧 k+1~k+2 上用它。皮肤在动，掩码就永远落后 1~2 帧，边界每一帧
    // 都在脸后扫一条带 —— 羽化只能把这条带从"硬边"软成"淡边"，消不掉。
    //
    // 修法：把「掩码 + 它描述的那一帧」作为**一个整体**扣下来，磨皮直接处理那一帧。
    // 数据本来就在：worker 的输入槽 `async_.frame` 里存着它算掩码用的原始帧，而且
    // 结果收走时（collectAsync）worker 一定已经空闲 —— 交换出去零拷贝、无竞态。
    // 代价是输出整体落后 1 帧（30fps 下 33ms，摄像头链路里本来就不可见），
    // 收益是**掩码与画面逐像素同源**：脸后的背景不再被扫出一条带，脸的前缘也不再
    // 有一条"没磨"的粗边。
    //
    // 只在「结果刚到」的那一帧用（maskFresh_）：worker 偶尔慢了一帧，就退回旧的
    // 「当前帧 + 旧掩码」，**不重复输出已显示过的帧** —— 持续停顿比一次性对齐
    // 更显眼。30fps 下 worker 15ms < 33ms 帧预算，结果每帧都到，对齐是常态。
    std::vector<unsigned char> heldFrame_;   // 与 mask_ 同源的原始帧（RGBA）
    bool maskFresh_ = false;                 // 本帧刚收到一份新的有效结果
    bool alignOn_ = true;                    // BEAUTY_MASK_ALIGN=0 关闭（A/B 用）

    // ---- BUG4（2026-11-06）：per-slot 状态 -----------------------------------
    //
    // 引擎是进程级单例，但下面这些量是**每实例**的：皮肤掩码、五官保护区、
    // 人脸位置、包围盒、掩码新鲜度、对齐用的整帧缓存、帧号与 EMA 历史。
    // 以前它们直接挂在 CoreEngine 上，两个实例同帧渲染时会互相覆盖。
    // 最严重的一种表现就是「掩码↔帧对齐」把**对方的帧**当成自己的输入
    //（heldFrame_ 被另一个实例换走），整层画面串台：A 输出 B 的画面。
    // 实测（03_测试夹具/bug4_repro.py）两次 render 间隔 >=5ms 必然发生。
    //
    // 做法：收进 SlotState，按 slot 存一份；process() 进入时把本实例的状态
    // swap 进成员、退出时 swap 回去。全程 O(1)，不拷贝像素 —— 所以 process()
    // 里一百多处成员访问一行都不用改，行为也只在「多实例同帧」这一种情况下
    // 变化；单实例时 swap 的是同一份，逐字节不变。
    struct SlotState {
        std::string faceErr_;
        std::vector<unsigned char> mask_;
        std::vector<unsigned char> protect_;
        std::vector<beautycore::FaceBox> faces_;
        beautycore::RoiRect bounds_;
        bool maskValid_ = false;
        bool everHadMask_ = false;
        int maskAge_ = 999;
        int maskHold_ = 0;
        int lastFaces_ = 0;
        float lastScore_ = 0.0f;
        MaskStat maskStat_;
        std::vector<unsigned char> heldFrame_;
        bool maskFresh_ = false;
        // 时序 EMA：历史必须逐帧连续，跨实例混用等于把别人的画面掺进来。
        std::vector<unsigned char> motionPrev_;
        bool motionHave_ = false;
        std::vector<unsigned char> detailLumT_;
        bool detailEmaReset_ = true;
        uint64_t frameNo_ = 0;
        uint64_t detailEmaGen_ = 0;
    };

    std::mutex slotMx_;
    std::map<int, std::unique_ptr<SlotState>> slots_;

    SlotState& slotOf(int slot) {
        std::lock_guard<std::mutex> lk(slotMx_);
        auto it = slots_.find(slot);
        if (it == slots_.end())
            it = slots_.emplace(slot, std::unique_ptr<SlotState>(new SlotState())).first;
        return *it->second;
    }

    // 整帧状态锁（P0 修复，2026-11-06）。
    //
    // swapSlotIn/Out 把本实例的状态搬进 CoreEngine 的**共享成员**，而 OFX 规范
    // 并不保证 render 串行 —— 本仓库 gpu_beauty.cpp 里讲 thread_local 的那段注释
    // 也明写「Resolve 可能多线程并发 render 多个实例」。没有这把锁时：
    //   · A 换入的状态会被 B 换走，A 全程读到的是 B 的掩码 / 对齐帧 / EMA 历史；
    //   · 换回时 B 的状态被写进 A 的 slot —— 串台被固化进状态表，比不修还难查；
    //   · 并发 mask_.assign() 是直接的堆破坏（释放后再写）。
    // 代价是两个实例的 process() 串行化（1080p 约 20ms 一帧）。正确性优先：
    // 真要并行，得把状态改成按 slot 传参，那属于引擎层重写，不在本次范围。
    //
    // 顺带解决 slotMx_ 的悬垂：releaseSlot / release 也必须先拿这把锁，因此
    // 不可能在某个 SlotScope 还活着时 erase / clear 掉它引用的 SlotState。
    //
    // 用函数内 static 而不是文件级对象：避免静态初始化顺序问题（插件可能在
    // 别的翻译单元的静态初始化期间就被调用）。recursive 是因为 slotOf() 内部
    // 还要再拿 slotMx_，而 releaseSlot 走的是同一层嵌套。
    static std::recursive_mutex& slotStateMx() {
        static std::recursive_mutex m;
        return m;
    }

    // 下面两组 swap 必须严格成对：进一次、出一次，成员回到原样。
    // 每个 swap 都是指针级的（vector/string 的 swap 只换三根指针）。
    void swapSlotIn(SlotState* s) {
        faceErr_.swap(s->faceErr_);
        mask_.swap(s->mask_);
        protect_.swap(s->protect_);
        faces_.swap(s->faces_);
        std::swap(bounds_, s->bounds_);
        std::swap(maskValid_, s->maskValid_);
        std::swap(everHadMask_, s->everHadMask_);
        std::swap(maskAge_, s->maskAge_);
        std::swap(maskHold_, s->maskHold_);
        std::swap(lastFaces_, s->lastFaces_);
        std::swap(lastScore_, s->lastScore_);
        std::swap(maskStat_, s->maskStat_);
        heldFrame_.swap(s->heldFrame_);
        std::swap(maskFresh_, s->maskFresh_);
        motionPrev_.swap(s->motionPrev_);
        std::swap(motionHave_, s->motionHave_);
        detailLumT_.swap(s->detailLumT_);
        std::swap(detailEmaReset_, s->detailEmaReset_);
        std::swap(frameNo_, s->frameNo_);
        std::swap(detailEmaGen_, s->detailEmaGen_);
    }
    void swapSlotOut(SlotState* s) { swapSlotIn(s); }

    // 作用域守卫：保证 process() 的每一条提前返回路径都会把状态换回去。
    //
    // lk_ 必须声明在 s_ 之前：成员按声明顺序初始化，锁要先于 slotOf() 拿到，
    // 才能保证全工程统一的加锁顺序是「整帧状态锁 → slotMx_」。
    // 析构时函数体先跑（swap 回去），之后才轮到成员析构（解锁）—— 正好。
    struct SlotScope {
        CoreEngine* e_;
        std::unique_lock<std::recursive_mutex> lk_;
        SlotState* s_;
        SlotScope(CoreEngine* e, int slot)
            : e_(e), lk_(slotStateMx()), s_(&e->slotOf(slot)) {
            e_->swapSlotIn(s_);
        }
        ~SlotScope() {
            // 出图后 OFX 层还要读这三样（日志用），换回去之前先留一份快照。
            // 快照是 thread_local 的：只属于本渲染线程，不会被另一个实例的
            // process 覆盖（以前是进程级成员，日志会把结果记到邻居头上）。
            t_lastFacesOut = e_->lastFaces_;
            t_lastScoreOut = e_->lastScore_;
            t_lastErrOut   = e_->faceErr_;
            e_->swapSlotOut(s_);
        }
    };

    void startAsyncIfNeeded() {
        if (!asyncOn_ || asyncStarted_) return;
        asyncStarted_ = true;
        async_.th = std::thread([this] { asyncWorker(); });
        // 把 worker 降到「低于正常」：它晚一两帧出结果完全没关系（掩码本来就
        // 4 帧才更新一次），但渲染线程晚一帧就掉帧。让 OS 优先保渲染线程。
        SetThreadPriority(async_.th.native_handle(), THREAD_PRIORITY_BELOW_NORMAL);
    }

    void stopAsync() {
        if (!asyncStarted_) return;
        {
            std::lock_guard<std::mutex> lk(async_.m);
            async_.quit = true;
        }
        async_.cv.notify_all();
        if (async_.th.joinable()) async_.th.join();
        asyncStarted_ = false;
    }

    // 把本帧交给 worker。worker 忙就直接放弃这一轮（下一个周期再说）——
    // 排队没有意义，掩码只需要最新的那份。
    //
    // BUG4：多实例之后这里多了两条纪律：
    //   ① 任务记下提交者 slot，只有提交者能取走结果（见 collectAsync）；
    //   ② 结果还没被取走时**不准**提交新帧 —— 否则 async_.frame 被覆盖，
    //      而「掩码描述的原始帧」还是旧的那份，配对就错了。
    //      孤儿结果（提交者再没来过）超过 1.5 秒就丢弃，避免堵死队列。
    void submitAsync(const unsigned char* src, int w, int h, int slot) {
        {
            std::lock_guard<std::mutex> lk(async_.m);
            if (async_.hasJob || async_.working) return;
            if (async_.ready) {
                if (GetTickCount64() - async_.readyMs < 1500) return;
                async_.ready = false;   // 提交者已经消失，丢掉这份孤儿结果
            }
            if (async_.frame.size() != (size_t)w * h * 4)
                async_.frame.resize((size_t)w * h * 4);
            async_.w = w;
            async_.h = h;
            async_.flipY = imgBottomUp_;
            async_.slot = slot;
        }
        // 拷贝放在锁外：1080p 下这是一次 8MB memcpy（约 0.7ms），
        // 持锁做会把 worker 的收尾也堵住。
        memcpy(&async_.frame[0], src, (size_t)w * h * 4);
        {
            std::lock_guard<std::mutex> lk(async_.m);
            async_.hasJob = true;
            g_maskSubmits++;      // 诊断计数：只在**真的提交了**时才加
        }
        async_.cv.notify_one();
    }

    // 收走 worker 的结果。返回 true = 这一轮确实消费了一个结果。
    //
    // BUG4：只取「本实例提交的」那份。别人的结果原样留在槽里，等它自己来取；
    // 这保证拿到的掩码和 heldFrame_ 一定同源、且都属于当前实例。
    bool collectAsync(int slot) {
        beautycore::RoiRect b;
        MaskStat st;
        int faces = 0, inputSize = 0;
        float best = 0.0f;
        bool valid = false, got = false;
        std::string msg;
        double ms = 0.0;
        {
            std::lock_guard<std::mutex> lk(async_.m);
            if (async_.ready && async_.slot == slot) {
                // 只有「有脸」时才换缓冲：没脸时 worker 的 mask 内容无意义，
                // 换进来会把当前正在用的掩码冲掉（丢帧保持就失效了）。
                if (async_.valid) {
                    mask_.swap(async_.mask);
                    protect_.swap(async_.protect);
                    faces_.swap(async_.fbox);
                    // 3.1.10：把**这份掩码描述的原始帧**一起扣下来。此时 worker
                    // 必然空闲（ready=working 结束），帧槽没人读写，swap 无竞态；
                    // swap 之后帧槽里是被换出的旧 heldFrame_，下一次 submitAsync
                    // 的 memcpy（同样要求 worker 空闲）会先把它覆盖掉再交活。
                    if (async_.frame.size() ==
                        (size_t)async_.w * async_.h * 4) {
                        heldFrame_.swap(async_.frame);
                        maskFresh_ = true;   // 渲染线程自己在 process 里消费
                    }
                }
                b = async_.bounds;
                st = async_.pixels;
                faces = async_.faces;
                valid = async_.valid;
                inputSize = async_.inputSize;
                best = async_.bestScore;
                msg = async_.msg;
                ms = async_.ms;
                async_.ready = false;
                got = true;
            }
        }
        if (!got) return false;
        asyncMs_ = ms;
        lastScore_ = best;   // v3.0.4：worker 的分数搬到渲染线程可读的位置
        if (valid) {
            bounds_ = b;
            maskStat_ = st;
            lastFaces_ = faces;
            maskHold_ = 0;
            maskValid_ = true;
            faceErr_ = msg;
        } else {
            onDetectFailed(msg, inputSize, best);
        }
        return true;
    }

    void asyncWorker() {
        for (;;) {
            std::vector<unsigned char> mask, protect;
            int w = 0, h = 0;
            bool flipY = true;
            {
                std::unique_lock<std::mutex> lk(async_.m);
                async_.cv.wait(lk, [this] { return async_.quit || async_.hasJob; });
                if (async_.quit) return;
                mask.swap(async_.mask);
                protect.swap(async_.protect);
                w = async_.w;
                h = async_.h;
                flipY = async_.flipY;
                async_.hasJob = false;
                async_.working = true;
            }

            beautycore::RoiRect b;
            std::vector<beautycore::FaceBox> fbox;
            MaskStat st;
            int faces = 0, inputSize = 0;
            float best = 0.0f;
            std::string msg;
            const double t0 = nowMsCore();
            const bool ok = buildMaskInto(&async_.frame[0], w, h, mask, protect,
                                          b, fbox, st, faces, msg, flipY);
            inputSize = face_.inputSize();
            best = face_.lastBestScore();
            const double ms = nowMsCore() - t0;

            {
                std::lock_guard<std::mutex> lk(async_.m);
                async_.mask.swap(mask);
                async_.protect.swap(protect);
                async_.fbox.swap(fbox);
                async_.bounds = b;
                async_.pixels = st;
                async_.faces = faces;
                async_.valid = ok;
                async_.inputSize = inputSize;
                async_.bestScore = best;
                async_.msg = msg;
                async_.ms = ms;
                async_.working = false;
                async_.ready = true;
                async_.readyMs = GetTickCount64();
            }
        }
    }

    // 亮度 Sobel 梯度 → 0..256 的「边缘强度」（256 = 强边缘，完全不磨）。
    //
    // 为什么需要它：双边滤波靠「颜色差大 → 权重小」隐式保护边缘，对高频纹理
    // 有效，但对**同色系的硬边**（黑眉毛压在皮肤上、眼镜框边缘）保护不足，
    // 会把边缘磨成渐变。显式梯度门控把「真边缘」和「皮肤瑕疵」分开：
    // 前者梯度大、后者梯度小。
    //
    // 用 |gx|+|gy| 代替 sqrt(gx²+gy²)：边缘检测本来就容忍近似，L1 省掉一次
    // 开方，在 1080p 十几万像素上是实打实的差别。
    //
    // 阈值为什么是「死区 + 斜坡」而不是那份公开实现的「线性 ×10」：
    // 实测这张图的梯度分布（归一化到 0..1）是
    //     脸颊平坦处  中位 0.020 / 90 分位 0.082
    //     眉毛横带    中位 0.017 / 90 分位 0.111   ← 中位数比脸颊还低！
    // 眉毛横带的**中位数**和脸颊一样，因为那个区域里大部分像素是眉间的皮肤，
    // 只有眉毛本体是高梯度。所以 Sobel 标的是「边缘线」而不是「区域」，
    // 线性放大 ×10 会让皮肤纹理（0.02×10=0.2）也吃到 20% 门控 —— 实测确实
    // 如此：低梯度区保留率被从 84% 抬到 94%，等于整体少磨了一大截。
    // 正确做法是给死区：低于 0.10 完全不门控，高于 0.30 完全门控。
    // 阈值按 G 通道尺度标定（G 0..255，Sobel 系数和 4 → gx 上界 ±1020）。
    // 原 luma*1000 口径下的 0.10/0.30 归一化，等价换算到这里是 102/306。
    static const int kEdgeLo = 102;
    static const int kEdgeHi = 306;

    static int sobelEdge(const unsigned char* src, int w, int h,
                         int x, int y, int k) {
        const int xm = (x > 0) ? (x - 1) : 0;
        const int xp = (x + 1 < w) ? (x + 1) : (w - 1);
        const int ym = (y > 0) ? (y - 1) : 0;
        const int yp = (y + 1 < h) ? (y + 1) : (h - 1);
        const unsigned char* r0 = src + (size_t)ym * w * 4;
        const unsigned char* r1 = src + (size_t)y * w * 4;
        const unsigned char* r2 = src + (size_t)yp * w * 4;

        // 用 G 通道当亮度（人眼对绿最敏感，梯度性质与 luma 基本一致）。
        // 完整 luma 要 3 次乘加 × 8 邻居 = 24 次/像素，实测这占了门控开销的
        // 大头；换成直接读 G 通道后少掉 2/3 的算术，阈值按同一口径重新标定过。
        const int l00 = r0[xm * 4 + 1];
        const int l01 = r0[x * 4 + 1];
        const int l02 = r0[xp * 4 + 1];
        const int l10 = r1[xm * 4 + 1];
        const int l12 = r1[xp * 4 + 1];
        const int l20 = r2[xm * 4 + 1];
        const int l21 = r2[x * 4 + 1];
        const int l22 = r2[xp * 4 + 1];

        const int gx = (l02 + 2 * l12 + l22) - (l00 + 2 * l10 + l20);
        const int gy = (l20 + 2 * l21 + l22) - (l00 + 2 * l01 + l02);
        const int mag = ((gx < 0 ? -gx : gx) + (gy < 0 ? -gy : gy)) / 2;

        // k 是灵敏度倍数（越大越容易判为边缘）：k=10 用标定好的阈值，
        // k=5 把上阈值翻倍（更难门控），k=20 减半（更激进）。
        int hi = (k > 0) ? (kEdgeHi * 10 / k) : kEdgeHi;
        if (hi <= kEdgeLo + 1) hi = kEdgeLo + 1;
        if (mag <= kEdgeLo) return 0;
        if (mag >= hi) return 256;
        return (int)((long long)(mag - kEdgeLo) * 256 / (hi - kEdgeLo));
    }

    // 线程数决策。行数太少时线程创建开销大于收益，直接单线程。
    static int blendThreads(int threads, int h) {
        int n = threads;
        if (n <= 0) {
            const unsigned hc = std::thread::hardware_concurrency();
            n = hc ? (int)hc : 4;
            if (n > 8) n = 8;
        }
        if (h < 32) return 1;
        const int byRows = h / 16;
        if (n > byRows) n = byRows;
        return (n < 1) ? 1 : n;
    }

    // 只在掩码内混合：掩码外逐字节保持原图。
    // 按行多线程：掩码外虽然是简单拷贝，1080p 也是 200 万像素，单线程要 2~3ms。
    static void blendWithMask(const unsigned char* src, const unsigned char* sm,
                              unsigned char* dst, int w, int h, float t,
                              const unsigned char* mask,
                              const unsigned char* protect, int protectK) {
        const int ti = (int)(t * 256.0f + 0.5f);
        const int inv = 256 - ti;
        const int kEdge = g_cfg.edgeK;
        const int kLimitThr = g_cfg.limitThr;

        auto runRows = [&](int yBegin, int yEnd) {
        for (int y = yBegin; y < yEnd; y++) {
            const size_t rowOff = (size_t)y * w;
            for (int x = 0; x < w; x++) {
                const size_t o = (rowOff + x) * 4;
                if (mask[rowOff + x] == 0) {    // 掩码外：逐字节还原
                    dst[o + 0] = src[o + 0];
                    dst[o + 1] = src[o + 1];
                    dst[o + 2] = src[o + 2];
                    dst[o + 3] = src[o + 3];
                    continue;
                }
                int k = (ti * mask[rowOff + x]) >> 8;   // 掩码值也参与权重
                // 五官保护：眼睛、嘴巴上的磨皮按 protectK 减弱
                if (protect && protectK > 0) {
                    const int p = (int)protect[rowOff + x] * protectK / 100;
                    k = (k * (256 - p)) >> 8;
                }
                if (kEdge > 0) {
                    const int e = sobelEdge(src, w, h, x, y, kEdge);
                    // e=0 时与上面的公式逐位相同，保证「不门控」与旧行为一致
                    k = (k * (256 - e)) >> 8;
                }
                // 阈值软膝（立体感）：只抹「小幅度起伏」（坑洼、斑点、毛孔），
                // 鼻梁两侧、下颌线这类**大幅度明暗过渡**按比例保住，
                // 否则过渡带被抹平后脸会显得塌、出现台阶。
                // 公式同该实现：limit = 1 - smoothstep(thr, thr*2.5, |src-sm|)
                if (kLimitThr > 0 && k > 0) {
                    int dmax = 0;
                    for (int c = 0; c < 3; c++) {
                        int dv = (int)src[o + c] - (int)sm[o + c];
                        if (dv < 0) dv = -dv;
                        if (dv > dmax) dmax = dv;
                    }
                    const int hi = kLimitThr * 5 / 2;
                    int limit;
                    if (dmax <= kLimitThr) {
                        limit = 256;
                    } else if (dmax >= hi) {
                        limit = 0;
                    } else {
                        limit = 256 * (hi - dmax) / (hi - kLimitThr);
                    }
                    k = (k * limit) >> 8;
                }
                const int ki = 256 - k;
                dst[o + 0] = (unsigned char)(((int)src[o + 0] * ki + (int)sm[o + 0] * k + 128) >> 8);
                dst[o + 1] = (unsigned char)(((int)src[o + 1] * ki + (int)sm[o + 1] * k + 128) >> 8);
                dst[o + 2] = (unsigned char)(((int)src[o + 2] * ki + (int)sm[o + 2] * k + 128) >> 8);
                dst[o + 3] = src[o + 3];
            }
        }
        };

        const int nt = blendThreads(g_cfg.threads, h);
        if (nt <= 1) {
            runRows(0, h);
            return;
        }
        // 持久线程池（3.1.21）。这里原来每帧 new 一组 std::thread 再 join，
        // 实测光建线程就要 0.29 ms × nt，一帧 21 趟下来 6 ms 上下。
        beautycore::parallelRows(0, h, nt, runRows);
        (void)inv;
    }

    // **同步**路径的重算周期（帧）。异步路径用 g_cfg.maskEveryAsync（默认 0 = 每帧）。
    // 两条路必须分开：同步时检测压在渲染线程上，每帧跑就是每帧 +14ms（掉帧）；
    // 异步时它跑在 worker 上，每帧跑只多烧 42% 的一个核。
    // 这个 4 也决定了离线台架的口径 —— cpu_bench / verify_roi / detail_test 全走同步，
    // 所以它们报的每帧毫秒里，检测成本是按 1/5 摊的。别拿它直接对生产值。
    static const int kMaskEverySync = 4;
    // 连续丢检多少次之后才真正放弃掩码。按 4 帧一次算，6 次 ≈ 0.8 秒 @30fps：
    // 够撑过挥手、低头、眨眼这类短暂遮挡，又不会让「人真的走开了」一直磨着空气。
    static const int kMaskHold = 6;

    beautycore::FaceDetector face_;
    // v3.0.4：源缓冲行序。true = bottom-up（OFX/Resolve 的约定，默认）。
    // 只影响人脸检测的读行方向与框/关键点的 y 映射；对称类处理不依赖它。
    bool imgBottomUp_ = true;
    bool faceTried_ = false;
    bool faceOk_ = false;
    std::string faceErr_;
    std::vector<unsigned char> mask_;
    std::vector<unsigned char> protect_;   // 五官保护区（眼睛/嘴巴），0..255
    // 本帧检出的人脸（含 5 个关键点）。五官局部五项全靠它定位；
    // 与掩码同一生命周期（异步时由 worker 一起交回），所以不会出现
    // 「掩码是这一帧的、关键点是上一帧的」这种错配。
    std::vector<beautycore::FaceBox> faces_;
    // 1.2.9：人脸框复用缓存（见 buildMaskInto）
    std::vector<beautycore::FaceBox> detFaces_;
    std::vector<unsigned char> detThumb_;
    int detW_ = 0, detH_ = 0, detAge_ = 0;
    bool detValid_ = false;
    beautycore::RoiRect bounds_;   // 掩码包围盒（外扩 radius），随掩码一起更新
    bool maskValid_ = false;
    // 3.1.21：本次「启用以来」是否成功产出过掩码。只看第一次 —— 用它区分
    // 「刚开开关、检测还没回来」（这几帧透传）和「人不在画面里」（全帧兜底）。
    bool everHadMask_ = false;
    int maskAge_ = 999;
    int maskHold_ = 0;   // 连续丢检次数（用于沿用旧掩码）
    int lastFaces_ = 0;
    // 本帧（掩码那次检测）的最高候选分。v3.0.4：由 updateMask / collectAsync
    // 写入，供渲染线程日志用 —— 直接读 face_.lastBestScore() 在异步下恒为 0。
    float lastScore_ = 0.0f;
    MaskStat maskStat_;
    std::vector<unsigned char> smooth_;
    std::vector<unsigned char> warpScratch_;   /* 3.1.27：美形的 ROI 采样源 */
    std::vector<unsigned char> lowBuf_;   // 高低频的低频层
    std::vector<unsigned char> scratch_;  // 可分离高斯的中间缓冲
    std::vector<float> guideScratch_;     // 引导滤波的 float 缓冲
    std::vector<float> clarityScratch_;   // clarity 的灰度 box 缓冲
    std::vector<float> detailScratch_;    // 锐化的 4 个亮度平面
    std::vector<float> spotScratch_;      // 祛斑痘的两个浮点平面
    std::vector<float> s3ScratchA_;       // surface3 的 b1 平面（6 个 ROI 平面）
    std::vector<float> s3ScratchB_;       // surface3 的 b2 平面（同上，不能共用）
    double lastMs_ = 0.0;

    // ---- 细节层时序 EMA（0.7.2）-------------------------------------------
    // motionPrev_ 是 1/4 分辨率的亮度快照，只用来算「帧间差均值」这个标量；
    // detailLumT_ 是全帧亮度历史，喂给 detailRefill 做 EMA。
    // 两个都按帧尺寸增长后复用，不每帧分配。
    std::vector<unsigned char> motionPrev_;
    bool motionHave_ = false;
    std::vector<unsigned char> detailLumT_;
    bool detailEmaReset_ = true;
    // 帧序号 + 「历史最后一次被写入的帧序号」。
    // 为什么需要这个：EMA 的历史必须**逐帧连续**推进，中间断一帧（面板全 0 提前
    // 返回、脸丢了导致 detailRefill 早退、只有脸内锐化但这一帧没检出脸）之后，
    // 历史就是陈旧的 —— 再拿它混会把几帧前那个位置的亮度渗进当前帧，观感是
    // 「重新开磨皮的第一帧闪一下」。所以只要 detailEmaGen_ + 1 != frameNo_
    // 就当作重置。用序号比较而不是布尔标志，是因为提前返回的路径太多，
    // 逐个去置标志一定会漏。
    uint64_t frameNo_ = 0;
    uint64_t detailEmaGen_ = 0;   // 0 = 从未写过

    // 运动判据：全帧亮度帧间差的均值（4×4 抽样，单位=灰阶，0..255）。
    // 顺手把快照推进一帧。首帧返回一个「大值」，让调用方按运动处理（beta=1），
    // 避免从空历史起步时把画面拉向黑。
    double updateMotion(const unsigned char* src, int w, int h) {
        const int sw = w / 4, sh = h / 4;
        if (!src || sw <= 0 || sh <= 0) return 255.0;
        const size_t n = (size_t)sw * (size_t)sh;
        if (motionPrev_.size() != n) {
            motionPrev_.assign(n, 0);
            motionHave_ = false;
        }
        uint64_t acc = 0;
        for (int y = 0; y < sh; y++) {
            const unsigned char* row = src + (size_t)(y * 4) * w * 4;
            unsigned char* pr = &motionPrev_[(size_t)y * (size_t)sw];
            for (int x = 0; x < sw; x++) {
                const unsigned char* q = row + (size_t)(x * 4) * 4;
                // 77/151/28 是整数版 0.299/0.587/0.114，和 detailRefill 的浮点
                // 权重略有出入，但这里只用来判「动没动」，不需要与它逐位一致。
                const int l = (q[0] * 77 + q[1] * 151 + q[2] * 28) >> 8;
                const int d = l - (int)pr[x];
                acc += (uint64_t)(d < 0 ? -d : d);
                pr[x] = (unsigned char)l;
            }
        }
        if (!motionHave_) {
            motionHave_ = true;
            return 255.0;
        }
        return (double)acc / (double)n;
    }

    // 帧间差 → 新帧权重 beta。<= still 视为静止（强平滑），>= full 完全不平滑。
    // 中间线性过渡，避免在阈值附近抖动导致「时有时无」的观感。
    float emaBeta(double motion) const {
        if (!g_cfg.emaOn) return 1.0f;
        if (motion <= (double)g_cfg.emaStill) return g_cfg.emaBetaStill;
        if (motion >= (double)g_cfg.emaFull) return 1.0f;
        const double t = (motion - (double)g_cfg.emaStill) /
                         ((double)g_cfg.emaFull - (double)g_cfg.emaStill);
        return (float)(g_cfg.emaBetaStill + (1.0 - g_cfg.emaBetaStill) * t);
    }

    // ---- 人脸局部五项 ------------------------------------------------------
    // 五官立体 / 亮眼 / 去黑眼圈 / 去法令纹 / 祛斑痘。
    //
    // 几何全部相对**脸框 + 5 个关键点**，没有一个绝对像素值 —— 理由见
    // beauty_region.h：绝对像素在 720p 会移到瞳孔上、在 4K 又跑到下眼睑。
    //
    // 为什么必须放在磨皮与美白之后：
    //   · 放磨皮之前，刚提亮的眼白会被磨皮平均掉，变成一片雾；
    //   · 放美白之前，两次抬亮叠加容易把眼白/牙齿推过 255，出现死白斑。
    //
    // 五项共用一次 applyEllipses（增益相加），因为它们的贡献互不干扰：
    // 面板为 0 的那项贡献恒为 0，所以「只开一项」和「五项全开」都能各自对拍。
    void applyFaceRegions(unsigned char* dst, int w, int h, const BeautySettings& s) {
        const float threed = clampi01(s.panel[P_THREED] / 100.0);
        const float eye = clampi01(s.panel[P_EYEBRIGHT] / 100.0);
        const float pouch = clampi01(s.panel[P_POUCH] / 100.0);
        const float naso = clampi01(s.panel[P_NASOLABIAL] / 100.0);
        const float spot = clampi01(s.panel[P_DELSPOT] / 100.0);
        if (threed <= 0.0f && eye <= 0.0f && pouch <= 0.0f && naso <= 0.0f &&
            spot <= 0.0f)
            return;
        if (getenv("BEAUTY_REGION_DEBUG")) {
            for (const auto& f : faces_) {
                fprintf(stderr, "[dbg] face x=%.1f y=%.1f w=%.1f h=%.1f kps=%d "
                        "kps=(%.1f,%.1f)(%.1f,%.1f)(%.1f,%.1f)(%.1f,%.1f)(%.1f,%.1f)\n",
                        f.x, f.y, f.w, f.h, (int)f.hasKps,
                        f.kps[0], f.kps[1], f.kps[2], f.kps[3], f.kps[4], f.kps[5],
                        f.kps[6], f.kps[7], f.kps[8], f.kps[9]);
            }
        }
        if (faces_.empty() || mask_.size() != (size_t)w * h) return;

        // v3.0.9：帧行序（开发指南 约束⑧）。
        //
        // 下面每个椭圆的 cy 与「眼下方 k 脸高」这类偏移，都是按**视觉 top-down**
        // 标定的（参照实现的差分图就是 top-down）。faces_ 里的 y 是缓冲行号，
        // bottom-up 时整块几何要沿 y 镜像，否则：
        //   · 五官立体的「额头提亮」落到鼻底/嘴、「下颌压暗」落到发际线以上；
        //   · 去黑眼圈落到**上眼睑**（该去的是眼袋）；
        //   · 去法令纹落到**眉弓**（实测峰值 y 与正确位置差 40~60 px）。
        // 大眼/亮眼是径向各向同性的椭圆，看不出来 —— 和 v3.0.5 的瘦脸是同一个坑。
        // sgn = +1 表示「缓冲行号增大 = 视觉向下」，top-down 即如此。
        const float sgn = imgBottomUp_ ? -1.0f : 1.0f;

        const unsigned char* mk = &mask_[0];
        std::vector<beautycore::Ellipse> els;
        els.reserve(48);
        // 法令纹走的是「暗度填平」模型，区域椭圆与其它三项分开收集，
        // 因为它的作用方式不是「加常量」而是「min(封顶, 斜率×暗差)」。
        std::vector<beautycore::Ellipse> nasoEls;
        nasoEls.reserve(4);
        // 法令纹**下瓣**（常量增益椭圆）：在 applyFoldRemoval 之后叠加，
        // 免得影响上瓣的邻域最大 LR 计算。
        std::vector<beautycore::Ellipse> nasoTailEls;
        nasoTailEls.reserve(2);
        float nasoSlope = 0.0f;


        for (const auto& f : faces_) {
            if (f.w < 8.0f || f.h < 8.0f) continue;
            const float fx = f.x, fy = f.y, fw = f.w, fh = f.h;
            // 关键点不可信时只保留「不需要位置」的祛斑痘，其余四项跳过：
            // 位置错了会把亮度糊到脸颊/背景上，比不做更糟。
            const bool kp = kpsSane(f);

            if (kp) {
                // 归一化坐标（相对脸框）
                auto nx = [&](float px) { return (px - fx) / fw; };
                auto ny = [&](float py) { return (py - fy) / fh; };
                const float e0x = nx(f.kps[0]), e0y = ny(f.kps[1]);
                const float e1x = nx(f.kps[2]), e1y = ny(f.kps[3]);
                const float nsx = nx(f.kps[4]), nsy = ny(f.kps[5]);
                const float m0x = nx(f.kps[6]), m0y = ny(f.kps[7]);
                const float m1x = nx(f.kps[8]), m1y = ny(f.kps[9]);

                if (threed > 0.0f) {
                    // 3.1.22：五官立体 = 8 个锚在关键点上的宽椭圆。
                    //
                    // 三条实测结论（数据见 06_离线验证/全项对齐_20261030/
                    // _grid_seg.py 的分段输出 与 fit_threed.json）：
                    //  1. 参照是**解剖/网格驱动**的：额头恒亮、脖子恒暗，与画面
                    //     自身明暗无关。之前的「低频明暗场」路线实测 corr≈0.05，
                    //     已废弃。
                    //  2. 参照的**面部中线 = 双眼中点**，不是 YuNet 的鼻尖 ——
                    //     侧脸时鼻尖会偏 0.12 脸宽，参照中线不偏。鼻梁/鼻底/
                    //     额头/下颌全部锚在 midX/midY 上。
                    //  3. 提亮区是**大片平顶**、压暗区是**窄带**，所以 feather
                    //     分两组（亮 0.705 / 暗 0.378）。
                    //
                    // 拟合集（m00/m03/m06）评分 79.67，留出集（m01/m02/m04/
                    // m05/m07）78.17 —— 没有过拟合。
                    //
                    // 3.1.23：重新拟合时把**引擎自己的皮肤软掩码**也模拟进去了
                    // （applyEllipses 会 acc *= mw）。不模拟这一层会高估增益。
                    const float g = g_cfg.regThreedGain * threed;
                    const float midX = (e0x + e1x) * 0.5f;
                    const float midY = (e0y + e1y) * 0.5f;
                    const float mouthMidX = (m0x + m1x) * 0.5f;
                    const float mouthMidY = (m0y + m1y) * 0.5f;
                    const float feB = 0.705f;   // 亮部 feather（大平顶）
                    const float feD = 0.378f;   // 暗部 feather（窄带）
                    const float sh = 0.15f;
                    // 3.1.23：8 帧**统一**重拟合（_fit_region.py threed --all），
                    // 模拟器评分 80.36（chg 2.491 / 参照 2.479，corr 0.798）。
                    // 绝对值 = THREED_SPEC 基准 + 拟合偏移（fit_threed.json）。
                    // 额头（宽、浅、在眉线上方）
                    els.push_back({midX - 0.010000f, midY - 0.210000f * sgn, 0.350f,
                                   0.115f, 0.0f, g * 8.525f, sh, feB});
                    // 眉弓 / 眼窝（压暗）
                    els.push_back({midX - 0.030000f, midY - 0.060000f * sgn, 0.352500f,
                                   0.145f, 0.0f, g * -2.150f, sh, feD});
                    // 鼻梁（窄竖条）
                    els.push_back({midX - 0.025000f, midY, 0.050f,
                                   0.180f, 0.0f, g * 6.900f, sh, feB});
                    // 左 / 右颧骨
                    els.push_back({e0x - 0.045000f, e0y + 0.117500f * sgn, 0.152500f,
                                   0.100f, 0.0f, g * 7.650f, sh, feB});
                    els.push_back({e1x - 0.012500f, e1y + 0.125000f * sgn, 0.150f,
                                   0.112500f, 0.0f, g * 10.6375f, sh, feB});
                    // 鼻底 / 上唇阴影（横向压暗）
                    els.push_back({midX - 0.005000f, midY + 0.195000f * sgn, 0.290f,
                                   0.092500f, 0.0f, g * -7.250f, sh, feD});
                    // 下巴
                    els.push_back({mouthMidX - 0.020000f, mouthMidY + 0.155000f * sgn,
                                   0.110f, 0.070f, 0.0f, g * 9.650f,
                                   sh, feB});
                    // 下颌 / 脖子（大面积压暗）
                    els.push_back({midX + 0.067500f, midY + 0.500000f * sgn, 0.507500f,
                                   0.190f, 0.0f, g * -4.800f, sh, feD});
                }
                if (eye > 0.0f) {
                    const float g = g_cfg.regEyeGain * eye;
                    // 眼白提亮。shade 给 0.35：眼白本来就接近饱和，按亮度加权
                    // 之后高光不会继续被推（推过去就是一块死白，观感比不提更差）。
                    // rx 从 0.075 放到 0.10：参考版的亮眼 bbox 宽 0.65 脸宽
                    // （从内眼角一直盖到外眼角），0.075 只盖住瞳孔那一小块，
                    // 眼白提亮不完整，观感是「两个亮点」而不是「眼睛变亮」。
                    els.push_back({e0x, e0y, 0.100f, 0.055f, 0.0f, g, 0.35f, 0.35f});
                    els.push_back({e1x, e1y, 0.100f, 0.055f, 0.0f, g, 0.35f, 0.35f});
                }
                if (pouch > 0.0f) {
                    // 3.1.22：去黑眼圈 = 双层椭圆（亮核 + 淡晕）。
                    //
                    // 实测：参照的两个眼袋与**暗度无关**（corr(原亮度,Δ) ≈ +0.1），
                    // 是纯几何区域 —— 所以用固定形状的椭圆是对的。
                    // 3.1.23：改成**斜向窄脊**（_pouch_map.py 实测）。
                    //
                    // 参照的响应不是水平椭圆带，而是一条斜向窄脊：
                    // m00 眼R 从 (x=0.66,y=0.43) 斜向右下到 (x=0.74,y=0.50)，
                    // 宽约 12 px、长约 62 px、方向约 41°。
                    // 椭圆能表达这条脊：rx = 脊长（脸宽单位）、ry = 脊宽（脸高单位）、
                    // rot = 脊方向。注意 rx 按 faceW 缩放、ry 按 faceH 缩放。
                    //
                    //  · 核   ：rx 0.0950 / ry 0.0300，增益 10.25，平台窄
                    //  · 晕   ：rx 0.2050 / ry 0.0600，增益 3.00，铺开
                    //  · 位置 ：眼下方 0.064 脸高
                    //  · 转头时近侧眼袋在画面上被拉宽 → rx *= 1 − yawK·yaw·side
                    //    （yaw = 鼻尖相对眼中点的横向偏移 / 眼距）
                    const float g = g_cfg.regPouchScale * pouch;
                    const float eyeDist = e1x - e0x;
                    const float yaw = (eyeDist > 0.01f)
                                          ? (nsx - (e0x + e1x) * 0.5f) / eyeDist
                                          : 0.0f;
                    struct Side { float ex, ey, dx, dy, rot; int sgn; };
                    const Side sides[2] = {
                        {e0x, e0y, -0.0532f, 0.0638f, 0.0633f, -1},
                        {e1x, e1y, 0.0094f, 0.0666f, 0.5817f, +1},
                    };
                    for (int i = 0; i < 2; i++) {
                        const Side& s = sides[i];
                        float w = 1.0f - g_cfg.regPouchYawK * yaw * (float)s.sgn;
                        if (w < 0.25f) w = 0.25f;
                        // 斜率也要跟着镜像（y 翻转后旋转角取反）
                        const float r = s.rot * (float)s.sgn * sgn;
                        // 核（窄而强）
                        els.push_back({s.ex + s.dx, s.ey + s.dy * sgn,
                                       0.0750f * w, 0.0220f, r, g * 10.2500f,
                                       0.4000f, 0.3187f});
                        // 晕（宽而弱）
                        els.push_back({s.ex + s.dx, s.ey + s.dy * sgn,
                                       0.2050f * w, 0.0600f, r, g * 3.0000f,
                                       0.2000f, 0.4600f});
                    }
                }
                if (naso > 0.0f) {
                    // 3.1.22：去法令纹 = **暗度驱动**（不是固定形状 + 固定亮度）。
                    //
                    // 实测（_region_model.py / _naso_fill.py）：
                    //  · 参照的峰值像素 Δ **恒等于 +52**（封顶）
                    //  · corr(原亮度, Δ) = −0.37 ~ −0.44 → 越暗提得越多
                    //  · 输出值可以超过邻域最大值 → 不是「填到邻域均值」
                    // 所以模型是：Δ = min(maxLift, slope·max(0, LR − Y))，
                    // LR = 邻域最大亮度（半径 6）。区域用两个椭圆限定（哪条线
                    // 是法令纹由「暗线在哪」自己决定，脸一转也跟得上）。
                    //
                    // 拟合集 60.18，留出集 54.65。
                    //
                    // 3.1.23：模型升级（8 帧统一拟合）
                    //  1. 区域改成**窄椭圆**，锚在「眼正下方 0.13 脸高」——
                    //     实测参照的法令纹响应就在这个位置（沿眼下方竖线扫描，
                    //     峰值 Δ 恒为 51~52，且只有 5~8 px 宽）。
                    //  2. 限制因子从「皮肤掩码」改成 max(皮肤掩码, 区域权重)：
                    //     皮肤掩码是肤色椭圆，褶皱越深掩码越低，引擎恰好在
                    //     最该提亮的线上把效果掐掉（corr(掩码,参照Δ) = −0.66）。
                    //  3. 加**暗线闸门** gate(d)，把响应宽度收到褶皱线上。
                    //
                    // 3.1.23 第二轮：改成**双瓣**模型（naso6，8 帧统一拟合）
                    //
                    // 依据（_naso_line.py，沿眼下方竖线扫描的峰值剖面，m00 眼R）：
                    //     dy(眼下方脸高)  参照 Δ   旧模型(naso5) Δ
                    //          0.13          11.7        23.9
                    //          0.15          18.3        60.0（封顶，过冲）
                    //          0.17          48.0（峰）  60.0（封顶）
                    //          0.19           5.2        14.0
                    //          0.23           6.7         3.8
                    //          0.25           8.0         0    ← 下瓣从这开始
                    //          0.31          12.0         0    ← 下瓣自己的峰
                    //          0.45           0.7         0
                    // 参照的响应有**两个瓣**：
                    //   · 上瓣 = 真正的褶皱线（窄、强，峰在眼下方 0.17 脸高）
                    //   · 下瓣 = 褶皱朝嘴角延伸的那一段（宽、弱，dy 0.25~0.45）
                    // 旧模型只有上瓣，而且上瓣还过冲到封顶 60（参照峰才 48）。
                    //
                    // 下瓣的幅度只有 3~12 灰阶，且和亮度无关（那儿并不暗），
                    // 所以**不能**用暗度驱动表达 —— 改用**常量增益椭圆**
                    // （和五官立体同一套原语）。下瓣在 applyFoldRemoval **之后**
                    // 叠加，避免影响上瓣的邻域最大 LR 计算。
                    const float g = naso;
                    const float slope = g_cfg.regNasoSlope * g;
                    struct Side { float ex, ey, dx, dy, rot; };
                    const Side sides[2] = {
                        //   dx      dy      rot      rx     ry
                        {e0x, e0y, -0.0085f, 0.1496f, 0.2766f},
                        {e1x, e1y, -0.0295f, 0.1148f, 0.3266f},
                    };
                    for (int i = 0; i < 2; i++) {
                        const Side& s = sides[i];
                        nasoEls.push_back({s.ex + s.dx, s.ey + s.dy * sgn, 0.0800f,
                                           0.1500f, s.rot * sgn, 0.0f, 0.0f, 0.4000f});
                    }
                    // 下瓣：常量增益椭圆（fit_naso6.json，8 帧统一拟合）
                    struct Tail { float ex, ey, dx, rot; int sgn; };
                    const Tail tails[2] = {
                        {e0x, e0y, -0.0631f, 0.2294f, -1},
                        {e1x, e1y, -0.0332f, -0.2007f, +1},
                    };
                    const float eyeDist = e1x - e0x;
                    const float yaw = (eyeDist > 0.01f)
                                          ? (nsx - (e0x + e1x) * 0.5f) / eyeDist
                                          : 0.0f;
                    for (int i = 0; i < 2; i++) {
                        const Tail& t = tails[i];
                        float w = 1.0f - 1.9508f * yaw * (float)t.sgn;
                        if (w < 0.25f) w = 0.25f;
                        nasoTailEls.push_back({t.ex + t.dx, t.ey + 0.2843f * sgn,
                                               0.1000f * w, 0.1200f,
                                               t.rot * (float)t.sgn * sgn,
                                               g * 4.0000f, 0.15f, 0.5500f});
                    }
                    nasoSlope = slope;
                }
            }

            if (!els.empty()) {
                // 每个椭圆坐标都是**相对这张脸的归一化值**，所以这里传脸框，
                // 一次调用把这五项的贡献一起加掉（增益相加，互不干扰）。
                beautycore::applyEllipses(dst, w, h, mk, fx, fy, fw, fh,
                                          &els[0], (int)els.size(), g_cfg.threads);
                els.clear();
            }
            if (!nasoEls.empty()) {
                // 法令纹：Δ = min(maxLift, slope·max(0, LR − Y)) × 区域权重
                beautycore::applyFoldRemoval(dst, w, h, mk, fx, fy, fw, fh,
                                             &nasoEls[0], (int)nasoEls.size(),
                                             nasoSlope, g_cfg.regNasoMaxLift,
                                             g_cfg.regNasoLrRadius,
                                             g_cfg.regNasoGateLo,
                                             g_cfg.regNasoGateHi,
                                             g_cfg.threads);
                nasoEls.clear();
            }
            if (!nasoTailEls.empty()) {
                // 法令纹**下瓣**：常量增益椭圆（参照的褶皱朝嘴角延伸的那一段，
                // 幅度 3~12 灰阶，与亮度无关）。必须在 applyFoldRemoval 之后，
                // 否则上瓣的邻域最大 LR 会被下瓣抬起来。
                beautycore::applyEllipses(dst, w, h, mk, fx, fy, fw, fh,
                                          &nasoTailEls[0],
                                          (int)nasoTailEls.size(),
                                          g_cfg.threads);
                nasoTailEls.clear();
            }
        }

        if (spot > 0.0f && !bounds_.empty()) {
            beautycore::applySpotFix(dst, w, h, mk, bounds_,
                                     g_cfg.regSpotK * spot, g_cfg.regSpotRadius,
                                     g_cfg.regSpotThrLo, g_cfg.regSpotThrHi,
                                     g_cfg.threads, spotScratch_);
        }
    }

    // 高低频参数在两个分支里各拼一次，抽出来避免两处漂移（以前就是改一处忘一处）。
    beautycore::FreqParams makeFreqParams() const {
        beautycore::FreqParams fp;
        fp.radius = g_cfg.radius;          // 沿用标定好的 r=2
        fp.iters = g_cfg.freqIters;        // 迭代次数决定低频尺度
        fp.sigmaSpace = g_cfg.sigmaSpace;
        fp.sigmaColor = g_cfg.sigmaColor;
        fp.magLo = g_cfg.freqMagLo;
        fp.magHi = g_cfg.freqMagHi;
        fp.grain = g_cfg.freqGrain;
        fp.strength = 100;                 // 面板档位由 blendWithMask 按 t 缩放
        fp.lowMode = g_cfg.freqLowMode;
        fp.guideRadius = g_cfg.guideRadius;
        fp.guideEps = g_cfg.guideEps;
        fp.lowPath = g_cfg.lowPath;
        fp.refRadius = g_cfg.refRadius;
        fp.boxRadius = g_cfg.boxRadius;
        fp.boxIters = g_cfg.boxIters;
        return fp;
    }
};

// 选择实现。只解析一次环境变量。
//
// 3.1.7 起**默认是 core**（自研引擎）。离线基准 DLL 里可用 BEAUTY_ENGINE_IMPL=legacy 切回旧链路。
// 为什么现在敢切：
//   · 面板 11/11 项已覆盖（3.1.2 起）；
//   · 真机 3.1.5 日志确认 engine=core/region=on/faces=1（3.1.4 修好的人脸模型加载）；
//   · 最后一个已知画质缺口（锐化放大的噪点闪烁）在 3.1.6 用时序 EMA 补掉。
//
// 未知值（比如写错成 "cor"）当作「没设置」处理，走默认。为什么不静默退回旧链路：
// 那样写错一个字母会悄悄跑回旧引擎，用户以为自己在测新引擎。
// 写错是可见的 —— init OK 那行会打出 engine=core/0.7.2，一眼能看出来。
IBeautyEngine* createEngine() {
    const char* v = getenv("BEAUTY_ENGINE_IMPL");
    if (v && v[0]) {
#ifdef BEAUTY_WITH_LEGACY_ENGINE
        if (strcmp(v, "legacy") == 0) return new LegacyAdapter();
#endif
        if (strcmp(v, "passthrough") == 0) return new PassthroughEngine();
        if (strcmp(v, "core") == 0) return new CoreEngine();
    }
    return new CoreEngine();
}

}  // namespace

// 皮肤判据快照（C20，只用于日志）。定义放在匿名命名空间外面 —— 否则外部
// （旧宿主的插件实现）链接不到，与上面 beautyCoreSetSmoothParams 同一规则。
// 内部的 g_skin* 仍在匿名空间里，同一 TU 内可见。
const char* beautyCoreSkinInfo() {
    static char buf[192];
    snprintf(buf, sizeof(buf),
             "skin=%s cb=%+.4f cr=%+.4f r=%.4f/%.4f ema=%.2f%s",
             g_skinAdapt ? "adapt" : "fixed",
             g_skinCb, g_skinCr, g_skinRu, g_skinRv, g_skinAdaptEma,
             g_skinAdaptValid ? "" : " (center=default, not yet estimated)");
    return buf;
}

// 估计首次收效后返回 true（并清标志），渲染线程按它决定要不要补打一行
// skin= —— 与 beautyCoreSkinInfo() 连用，两步之间没有别的线程会再改 g_skin*。
// 注意：g_skinAdaptValid 一旦置位整个会话不会再翻回 false（估计连续失败也
// 只沿用上一帧中心），所以这个标志整个生命周期至多被置位一次，取走即完。
bool beautyCoreSkinAnnounce() {
    return g_skinAnnounce.exchange(false, std::memory_order_relaxed);
}

// 自研核参数（离线调参入口）。定义必须放在匿名命名空间**外面**，否则外部
// （core_probe.cpp）链接不到；g_cfg 本身仍在匿名空间内，只给 CoreEngine 用。
void beautyCoreSetSmoothParams(int radius, float sigmaSpace,
                               float sigmaColor, int down) {
    g_cfg.radius = radius;
    g_cfg.sigmaSpace = sigmaSpace;
    g_cfg.sigmaColor = sigmaColor;
    g_cfg.down = (down < 1) ? 1 : down;
}

// 性能相关开关（离线调参 / 对照实验入口）。定义同样要放在匿名命名空间外面。
//   threads <= 0 自动；1 强制单线程
//   useRoi  0 = 关掉 ROI 裁剪（只在测「全帧成本」时用，生产必须开）
void beautyCoreSetPerf(int threads, int useRoi) {
    g_cfg.threads = (threads < 1) ? 0 : threads;
    g_cfg.roi = (useRoi != 0);
}

// Sobel 边缘门控灵敏度（离线调参入口）。0 = 关闭。
void beautyCoreSetEdgeK(int k) {
    g_cfg.edgeK = (k < 0) ? 0 : k;
}

void beautyCoreSetClarity(float maxGain, int radius, float kneeLo, float kneeHi) {
    if (maxGain >= 0.0f) g_cfg.clarityMax = maxGain;
    if (radius > 0) g_cfg.clarityRadius = radius;
    if (kneeHi > kneeLo && kneeLo >= 0.0f) {
        g_cfg.clarityKneeLo = kneeLo;
        g_cfg.clarityKneeHi = kneeHi;
    }
}

void beautyCoreSetLowPath(int path, int refRadius) {
    g_cfg.lowPath = (path < 0) ? 0 : (path > 3 ? 3 : path);
    if (refRadius >= 0) g_cfg.refRadius = refRadius;
}

void beautyCoreSetGuide(int mode, int radius, float eps) {
    g_cfg.freqLowMode = (mode != 0) ? 1 : 0;
    if (radius > 0) g_cfg.guideRadius = radius;
    if (eps > 0.0f) g_cfg.guideEps = eps;
}

void beautyCoreSetFreqGrain(float g) {
    g_cfg.freqGrain = (g < 0.0f) ? 0.0f : ((g > 1.0f) ? 1.0f : g);
}

void beautyCoreSetFreq(int mode, float sigma, float magLo, float magHi) {
    g_cfg.freqMode = (mode != 0) ? 1 : 0;
    // sigma 参数在这里复用为「迭代次数」（1..6），保持导出签名不变
    if (sigma >= 1.0f) {
        g_cfg.freqIters = (int)(sigma + 0.5f);
        if (g_cfg.freqIters < 1) g_cfg.freqIters = 1;
        if (g_cfg.freqIters > 8) g_cfg.freqIters = 8;
    }
    if (magHi > magLo) {
        g_cfg.freqMagLo = magLo;
        g_cfg.freqMagHi = magHi;
    }
}

void beautyCoreSetLimitThr(int thr) {
    g_cfg.limitThr = (thr < 0) ? 0 : thr;
}

void beautyCoreSetProtectK(int k) {
    if (k < 0) k = 0;
    if (k > 100) k = 100;
    g_cfg.protectK = k;
}

// 掩码导出（离线对拍用）。同样必须放在匿名命名空间外面。
bool beautyCoreComputeMask(const unsigned char* src, int w, int h,
                           unsigned char* outMask) {
    if (!src || !outMask || w <= 0 || h <= 0) return false;
    CoreEngine* ce = dynamic_cast<CoreEngine*>(&beautyEngine());
    if (!ce) return false;
    return ce->computeMaskForDump(src, w, h, outMask);
}

IBeautyEngine& beautyEngine() {
    // 与 LegacyEngine::inst() 同一套路：魔法静态保证只初始化一次且线程安全；
    // 对象刻意不析构，避免宿主 FreeLibrary 时静态析构与渲染线程赛跑。
    static IBeautyEngine* s = createEngine();
    return *s;
}

const char* beautyEngineId() {
    return beautyEngine().id();
}

int beautyCoreMaskCadence() {
    return g_cfg.maskEveryAsync;
}

uint64_t beautyCoreMaskSubmits() {
    return g_maskSubmits;
}

uint64_t beautyCoreAlignUsed() {
    return g_alignUsed;
}

// 3.1.10 探针：一次调用带回 (对齐帧数, 处理总帧数)，供日志算比率。
int beautyCoreAlignStat(uint64_t* used, uint64_t* total) {
    if (used) *used = g_alignUsed;
    if (total) *total = g_alignTotal;
    return 0;
}
