// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Jian
#pragma once
// BeautyEngine —— DaVinci Resolve OFX 美颜插件的引擎适配层。
//
// v3.0.0 起：本层不再加载旧版引擎，而是把请求转给自研的纯 CPU
// 美颜引擎（beauty_core 里的 CoreEngine，对应旧宿主 3.1.39）。人脸检测用 ONNX 人脸检测模型，
// 在引擎内部**自动检测 GPU**（仅 Windows DirectML）：只要把 onnxruntime.dll 换成
// onnxruntime-directml 构建版，人脸推理就自动走 GPU；否则自动退回 CPU。本文件
// 只做「BeautyEngineSettings → 原始参数 → 转调 CoreEngine」（经 core_glue 桥接层），
// 不持有任何渲染资源、不依赖任何闭源 DLL。
//
// 公开接口（ofx_plugin.cpp 依赖）完全保留，宿主一行都不用改。
//
// 面板参数枚举（P_*）来自 beauty_params.h（**唯一出处**），与 CoreEngine 的
// BeautySettings::panel[] 下标逐位一致（美型三项索引 11/12/13 在两边含义相同：
// 瘦脸 / 大眼 / 下巴），因此参数可逐字段直通，无需任何映射表。
// 1.3.6.0 之前这里另写了一份同名不同名的枚举，与 beauty_params.h 同时包含会报
// C2365；现已合并，本文件只是 `#include` 它。

#include <string>

#include "beauty_params.h"

// ---------------------------------------------------------------------------
// 构建版本号 —— 唯一出处
// ---------------------------------------------------------------------------
// 四段式 `主.功能.修订.构建`（2026-11-06 起，见 PROGRESS「版本号规则 v2」）。
//
//   MAJOR  不兼容变更（旧工程失效 / 参数编号变 / 引擎接口重写）
//   MINOR  用户可见的新功能（新滑块、新配置项、新渲染路径）
//   PATCH  修 bug / 调默认值 / 提速，但不加新东西
//   REV    无行为变化（日志、注释、文档、打包、开关挪位置）
//
// **低位不进位**：PATCH 可以从 9 涨到 10、20，不必攒够就顶到 MINOR。
// 这条是「版本号升太快」的解药 —— 日常改动全由后两位消化。
//
// 一次改动跨多档时取最高那档，低位归零，不叠加。
//
// 消费方：
//   · BEAUTY_BUILD_STR  → 日志、面板「版本」参数（四段全显示）
//   · MAJOR/MINOR     → OfxPlugin 结构体的 versionMajor/versionMinor
//                        **OFX 结构体只有两段**，宿主列表里永远显示 "1.0"，
//                        后两位只在日志与面板里可见。这是规范限制，不是 bug。
//   · MAJOR/MINOR     → BeautyVersion() 的 ABI 编码
// ---------------------------------------------------------------------------
#define BEAUTY_BUILD_MAJOR 1
#define BEAUTY_BUILD_MINOR 3
#define BEAUTY_BUILD_PATCH 6
#define BEAUTY_BUILD_REV   0
#define BEAUTY_BUILD_STR "1.3.6.0"

// 面板参数顺序（滤镜属性页滑块的顺序）见 beauty_params.h —— 本文件不再重复定义。

struct BeautyEngineSettings {
    double panel[P_COUNT];   // 面板刻度 0..100
    // 磨皮算法。顺序必须与 ofx_plugin.cpp 的 kBlurTypeChoices 一致：
    //   0 双边滤波（= 保边表面模糊，v3.0.5 起的默认档，也是历史行为）
    //   1 高斯（纯空间高斯，不保边）
    //   2 弱磨皮（同一套保边算法，强度对折）
    // v3.0.5 之前这个字段**从未被读取**，三档输出逐位相同；旧注释写的
    // 「1=双边 0=高斯」是旧版引擎时代的顺序，已作废。
    int blur_type;
    int skin_detect;         // 0/1 只磨皮肤区域（默认 1：不要把背景一起磨糊）
    // 3.0.17：磨皮模式（纯 CPU 侧，不是 SDK 参数）。
    //   0 人像磨皮 = 历史行为，逐位不变
    //   1 清晰磨皮 = 磨皮后多回填细频（皮肤更透）
    //   2 精细磨皮 = 回填更多 + 补一点中频（结构最完整）
    //   3 轻磨皮   = 降低磨皮强度（整体最轻）
    int smooth_mode;

    BeautyEngineSettings() : blur_type(0), skin_detect(1), smooth_mode(0) {
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

class BeautyEngine {
public:
    static BeautyEngine& inst();

    // 资源目录（旧宿主的插件资源目录）里必须含自研 CPU 引擎
    // 的运行时资产：onnxruntime.dll（ONNX Runtime，普通 CPU 版即可；如需 GPU
    // 推理可换成 onnxruntime-directml 构建版）与 yunet.onnx（人脸检测模型）。
    // 成功返回 true，失败时 err 描述原因。首次调用耗时主要在 ONNX 会话构建与
    // 人脸模型加载，所以调用方应当在「模块加载 / 滤镜创建」时预热，而不是等
    // 第一次渲染。线程安全：内部加锁并复检状态。
    bool ensureReady(std::string& err);
    // 滤镜创建时专用：加锁判断「是否需要预热」，并把「本次是否真的做了初始化」
    // 通过 didInit 报回（仅用于日志）。
    bool preheat(std::string& err, bool& didInit);
    bool ready() const { return ready_; }

    // 初始化失败次数；达到上限后 ensureReady 直接失败，不再重付那 2 秒。
    int failCount() const { return failCount_; }
    // 创建 GL 上下文的线程 id（诊断用）
    unsigned long ownerThread() const { return ownerThread_; }

    // 逐帧处理。src / dst 都是 RGBA8，长度 w*h*4。允许 src == dst。
    bool process(const unsigned char* src, unsigned char* dst,
                 int w, int h, const BeautyEngineSettings& s);

    // ---- 槽位 API（保留给未来按实例隔离用）----
    //
    // ⚠️ 现状（v3.0.8 实测）：CoreEngine::acquireSlot 恒返回 0、releaseSlot 是空操作、
    // process() 忽略 slot 与 advanceFrame —— 全进程只有一份引擎状态（含时序 EMA /
    // 混合历史 / 上一帧参数缓存）。因此**不要**把「每实例独立」当成保证：
    // 两个滤镜实例交替渲染时会共享同一份时序历史。参数本身逐帧全量下发，
    // 不会像 v2.x 旧版引擎的缓存那样「A 的滑块改到 B」；受影响的是跨帧平滑量。
    // 真要隔离，得让 CoreEngine 持有 per-slot 状态，属于引擎层改造。
    int  acquireSlot(std::string& err);
    void releaseSlot(int slot);
    // 进程级检测选项（fuSetDefaultRotationMode / 关键点质量 / 最大人脸数）。
    // SDK 这三个 setter 是进程全局的，多个滤镜实例共用同一份状态，最后一次
    // 调用生效；内部加锁 + SEH，只能在渲染线程调用。
    void setDetectOptions(int rotate, int landmarkQuality, int maxFaces);
    // advanceFrame 目前被 CoreEngine 忽略（帧序总是自增）—— 保留参数是为了让
    // 宿主侧「非顺序渲染」的意图留在签名里，将来引擎实现按实例时序时直接接上。
    bool process(int slot, const unsigned char* src, unsigned char* dst,
                 int w, int h, const BeautyEngineSettings& s, bool advanceFrame = true);

    int lastFaces() const { return faces_; }
    // v3.0.1：本帧人脸检测的最高候选置信度（不受阈值影响），供日志判断
    // 「模型没看出脸」还是「看出了没到阈值」。
    double lastFaceScore() const { return faceScore_; }
    int itemId() const;     // 实现见 beauty_engine.cpp（经 coreBeauty 桥接）
    const char* version() const;  // 实现见 beauty_engine.cpp（含 GPU provider）
    // 上一次失败的原因（成功时为空串）。供插件层把原因报给宿主。
    const char* lastErr() const { return failReason_.c_str(); }
    // 上一次 process 内 SDK 自身的耗时（毫秒，不含调用方的像素格式转换）
    double lastSdkMs() const { return lastSdkMs_; }
    // 诊断（BUG4）：上一次 process 是否把输入整帧换成了引擎缓存的 heldFrame_。
    // 只读，不参与渲染决策；OFX 层把它写进 DIAG 行。
    bool frameSubstituted() const;

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
    BeautyEngine();
    ~BeautyEngine() {}
    BeautyEngine(const BeautyEngine&) = delete;
    BeautyEngine& operator=(const BeautyEngine&) = delete;

    // v3.0.8：删掉 v2.x（旧版引擎）遗留的整组成员与方法声明 —— GL 隐藏窗口、
    // SDK 道具池 Slot、渲染临时缓冲、语义区域映射表。v3.0.0 起这些全部由
    // CoreEngine 内部持有，本层只剩「参数搬运 + 取回诊断」。
    bool ready_ = false;
    int  failCount_ = 0;
    unsigned long ownerThread_ = 0;
    std::string failReason_;
    int faces_ = 0;
    double faceScore_ = 0.0;   // v3.0.1：本帧最高候选置信度
    // 上一次 process 内引擎自身的耗时（毫秒，不含调用方的像素格式转换）
    double lastSdkMs_ = 0.0;

    // 语义区域状态。CoreEngine 不暴露覆盖率 / 推理耗时，只保留「是否可用 +
    // 初始化失败原因」两项。
    bool regionActive_ = false;
    std::string regionErr_;
};

// ---------------------------------------------------------------------------
// 自测入口：不经旧宿主，直接跑通 加载→初始化→渲染→落盘 全链路。
// 输入是裸 RGBA8（宽×高×4 字节），输出同尺寸的裸 RGBA8（便于像素比对）。
// 返回 0 表示成功。故意用 C ABI，方便用 ctypes / LoadLibrary 调用
// （blur_test 下的探针脚本用的就是它）。
// ---------------------------------------------------------------------------
#ifdef __cplusplus
extern "C" {
#endif
__declspec(dllexport) int __stdcall BeautySelfTest(const char* engineDir,
                                                 const char* rawRgbaIn,
                                                 const char* rawRgbaOut,
                                                 int width, int height,
                                                 double blur, double whiten,
                                                 char* msgBuf, int msgCap);
__declspec(dllexport) unsigned long __stdcall BeautyVersion(void);
#ifdef __cplusplus
}
#endif
