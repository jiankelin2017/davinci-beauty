// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Jian
#pragma once
// core_glue.h —— BeautyEngine 与自研 CoreEngine 之间的桥接层。
//
// 为什么需要这一层：beauty_engine.h 自带一份面板参数枚举（P_*），而 beauty_params.h
// 也有一份同名枚举；两者若同时进入同一个翻译单元会重定义。本桥接层只传原始数据
// （double panel[14] + 三个 int），不暴露任何一方的枚举/结构体，从而让
// beauty_engine.cpp 完全不必 include beauty_params.h / beauty_core.h，从根本上避开冲突。
//
// 唯一真正接触 CoreEngine 的翻译单元是 beauty_engine_glue.cpp。

#ifdef __cplusplus
extern "C" {
#endif

// 初始化引擎。ortDir 非空时先设为 onnxruntime.dll 搜索目录（自测用）；
// 返回 true 表示成功，失败原因写入 errBuf。
bool coreBeautyEnsureReady(const char* ortDir, char* errBuf, int errCap);

// 预热（同 ensureReady，仅语义不同）。didInit 回填是否真的做了初始化（1/0）。
bool coreBeautyPreheat(const char* ortDir, char* errBuf, int errCap, int* didInit);

bool coreBeautyReady();
int  coreBeautyAcquireSlot(char* errBuf, int errCap);
void coreBeautyReleaseSlot(int slot);
void coreBeautySetDetectOptions(int rotate, int landmarkQuality, int maxFaces);

// 帧行序（v3.0.4）。bottomUp=1 表示源缓冲第 0 行是画面最下面一行 ——
// OFX/Resolve 的约定，也是引擎默认值；BeautySelfTest / 离线夹具喂的是
// top-down 的 raw 文件，必须传 0，否则人脸检测看到的是上下颠倒的画面。
void coreBeautySetImageBottomUp(int bottomUp);

// 异步人脸检测开关（v3.0.4）。自测 / 逐字节对拍必须传 0：
// 异步下掩码来自 worker 线程，同一输入连跑两次可能得到不同输出。
void coreBeautySetAsyncDetect(int on);

// 逐帧处理。panel 长度必须 >= 14。
bool coreBeautyProcess(int slot,
                       const unsigned char* src, unsigned char* dst,
                       int w, int h,
                       const double panel[14],
                       int blur_type, int skin_detect, int smooth_mode,
                       bool advanceFrame);

int         coreBeautyLastFaces();
double      coreBeautyLastFaceScore();   // v3.0.1：本帧最高候选置信度
int         coreBeautyItemId();
const char* coreBeautyVersion();
const char* coreBeautyLastErr();
double      coreBeautyLastCoreMs();
bool        coreBeautyRegionActive();
const char* coreBeautyGpuProvider();
void        coreBeautyRelease();

// 诊断（BUG4）：上一次 process 是否把输入整帧换成了引擎缓存的 heldFrame_。
// 1 = 换了（本帧处理的是「掩码所描述的那一帧」，可能来自另一个实例）。
// 只读计数器，不参与任何渲染决策。
int         coreBeautyDiagFrameSubst();

#ifdef __cplusplus
}
#endif
