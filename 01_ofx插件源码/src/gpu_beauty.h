#pragma once
// gpu_beauty.h —— D3D11 GPU 后端（P1：ROI 优先的传输管道）
//
// == 为什么是「ROI 优先」而不是「全帧上 GPU」================================
// P1 的搬运基准（_p1bench/）在本机实测（32F 口径，宁可低估）：
//
//   4K 全帧：上传 22.4ms + 读回 48.2ms = 70.7ms   ← 与 CPU 磨皮 70.7ms 持平
//   4K 人脸：上传  4.8ms + 读回 11.2ms = 16.0ms   ← 比 CPU 快 4.4 倍
//
// 瓶颈是 PCIe 往返而不是 GPU 算力（本机 x8 通道实测 6~9 GB/s）。只传人脸
// 包围盒，收益全部来自这里。所以本模块**只接受 ROI**，纹理尺寸 = ROI 尺寸，
// 上传按行从全帧缓冲抽取、下载按行写回，连全帧纹理都不分配。
//
// == 像素格式：8bit，不是 float ============================================
// 引擎内 process() 的帧缓冲是 **RGBA8（w*h*4 字节）**，不是 float32 ——
// 宿主给的 f32 由 OFX 层的 blitFrame/f32_to_u8_row 先降到 8bit 才进引擎。
// 因此 GPU 侧必须用 R8G8B8A8_UINT + Texture2D<uint4>：
//   · 读 SRV 拿 0..255 原始整数，UAV 写 0..255 原始整数；
//   · 全程没有 float↔UNORM 的归一化，恒等路径**逐字节精确**，不需要容差。
// 这一点与视频链路 shader 的「值域固定 0..255」是同一个道理，只是原因更硬：
// 不是精度权衡，而是引擎本来就跑在 8bit 上。
//
// == P1 的职责边界 =========================================================
// P1 只做管道 + 逐字节验证，不做任何画面改动：
//   · GPU_BEAUTY_IDENTITY：上传 → CS 恒等 → 下载，必须与源逐字节相同
//   · GPU_BEAUTY_SWAP_RB：交换 R/B（证明 dispatch 真的执行了，同时用来验证
//     「校验函数本身抓得住差异」—— 恒等全过也可能是没跑，这一步排除该可能）
// 真正的磨皮 21 趟在 P2 追加 stage，管道不变。
//
// == 计时口径 =============================================================
// 本机（Optimus 混合输出）实测：每次 End(QUERY)+Flush+GetData 自旋固定要
// 11~15ms，与帧大小无关（1.84MP 与 8.3MP 都是 15.5ms）。因此管道里
// **不允许插任何显式 fence**，只保留 Map(READ) 这一次自然阻塞，
// 代价是「GPU 执行时间」被并进 downloadMs。分段精度交给离线探针。
// ===========================================================================

enum GpuBeautyStage {
    GPU_BEAUTY_IDENTITY = 0,   // 恒等：验证往返逐字节一致
    GPU_BEAUTY_SWAP_RB  = 1,   // 交换 R/B：证明 CS 真的执行
    GPU_BEAUTY_STAGE_COUNT = 2,
};

struct GpuBeautyRoi {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool empty() const { return x1 <= x0 || y1 <= y0; }
};

struct GpuBeautyTiming {
    double uploadMs   = 0.0;   // Map(WRITE_DISCARD) + 逐行 memcpy + Unmap
    double submitMs   = 0.0;   // CopyResource + Dispatch 的提交耗时（不含执行）
    double downloadMs = 0.0;   // Map(READ) 阻塞（**含 GPU 执行**）+ 逐行 memcpy
    double totalMs    = 0.0;
    int    roiW = 0, roiH = 0;
};

// 懒初始化：创建设备、编译 CS。失败后不再重试，保证不会每帧重试拖慢渲染。
bool gpuBeautyInit();

bool gpuBeautyReady();

// 释放全部资源。仅用于离线工具与单元测试；插件里不调用。
void gpuBeautyShutdown();

// 核心：ROI 从 src 上传 → CS(stage) → 下载到 dst。
//   src / dst：全帧缓冲，RGBA8 交错，每行 rowPixels 个像素（通常 = w）
//   dst 可与 src 相同（原地写回 ROI）
bool gpuBeautyApplyRoi(const unsigned char* src, unsigned char* dst,
                       int w, int h, int rowPixels,
                       GpuBeautyRoi roi, int stage, GpuBeautyTiming* timing);

// 跑一次 ApplyRoi 后与源逐字节比对。outMismatch = 不一致的字节数。
// 返回 true 表示逐字节相同。
bool gpuBeautyVerifyRoi(const unsigned char* src, int w, int h, int rowPixels,
                        GpuBeautyRoi roi, int stage,
                        GpuBeautyTiming* timing,
                        int* outMismatch, int* outMaxDiff);

// 累计统计：帧数、平均上传达/提交/读回、最后一次不一致字节数与最大差。
void gpuBeautyStats(long long* frames, double* avgUpload, double* avgSubmit,
                    double* avgDownload, int* lastMismatch, int* lastMaxDiff);

const char* gpuBeautyLastError();
