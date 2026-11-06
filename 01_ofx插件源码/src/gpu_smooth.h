#pragma once
// ---------------------------------------------------------------------------
// gpu_smooth —— 磨皮生产路径（surface3RgbBand）的 D3D11 Compute 移植
//
// 目标：把 CPU 上 4K 71.5ms / 1080p 24.6ms 的磨皮压到个位数毫秒。
//
// ## 为什么敢要求「逐字节一致」
//
// 生产路径 surface3RgbBand 的四段全是可分离的局部算子：
//   ① 2×2 盒式下采样   (int 求和) × 0.25f          —— 整数和 ≤1020，float 精确
//   ② B1/B2 盒式模糊   滑动窗 sum += a − b，每 32 像素重算
//   ③ amp = |C.g − B2.g| 再盒式模糊
//   ④ 合成             双线性 + 六步标量表达式
//
// ② 的滑动窗在数值上是「整数范围 float 的加减」，2^24 以内精确无舍入；
// 只有 × inv 是一次除法。所以只要 GPU 用 32 位 float、不复用近似函数、
// 不做 FMA 融合，结果与 CPU 位位相同。FMA 融合是唯一风险，用 HLSL 的
// `precise` 限定符压住。
//
// ## ROI 与 kFlush 相位（这是最容易踩的坑）
//
// bandBoxH 每 32 个像素重算一次窗口和，bandBoxV 每 32 行重算一次。重算点
// 落在**全局平面坐标**上。所以 ROI 的平面起点必须对齐到 32 的倍数，
// 否则两边累加相位不同，四舍五入后差 1 个灰阶 —— 真机上就是掩码边缘一圈接缝。
//
// 平面起点 px0 对齐 32 ⇒ 图像起点 X0 = 2·px0 对齐 64。
// 这也是 gpuBeauty 的 alignRoi 只对到 2 的倍数、不能直接复用的原因。
//
// ## 外扩（halo）
//
// 平面外扩 = passes×32 + passes×(r2 + ra) + 2，图像外扩 = 2 × 平面外扩。
// 三项各有来处：
//
//   · passes×32   ← 硬需求，不是保守估计。bandBoxH/V 每 32 像素从零重算
//                   窗口和（kFlush=32）纠正累加误差。ROI 最外一块重算时左边
//                   没数据、只能 clamp 到自身，整帧版读到真实邻居 → 该块整块
//                   不同；下一块重算要读左邻 r 个像素，污染向内传染一块，
//                   共传 passes 次。实测内缩 96 像素处差异归零，恰等于 32×3。
//                   少了这一项会稳定差 1~2 灰阶（1.0.1.1 曾误判为 1 ULP 精度问题）。
//   · passes×(r2 + ra)  ← 盒式每趟扩散 r；amp 那一路要先经 B2 再经 ra。
//   · +2          ← 合成段双线性的足迹。
//
// 外圈只是为了让内圈算得对，不参与输出。
//
// ## 资源分配桶化（1.0.3.0）
//
// 纹理按 64 像素向上取整分配，判重键用「桶尺寸」而不是「本帧真实尺寸」。
// 真机人脸框每帧抖 1~3 像素，旧实现会每帧拆建 24 个 D3D11 对象；桶化后
// 只有跨桶边界才重建。两套尺寸严格分开：
//   · cbuffer / dispatch / clamp / 读回矩形  → 真实尺寸
//   · CreateTexture2D / CopyResource / RowPitch → 桶尺寸
// 桶外像素不参与计算，所以对拍结果必须仍是逐字节一致。
// ---------------------------------------------------------------------------

#include "beauty_smooth.h"   // beautycore::BandParams / RoiRect

struct GpuSmoothTiming {
    double uploadMs   = 0.0;
    double computeMs  = 0.0;
    double downloadMs = 0.0;
    double totalMs    = 0.0;

    // 细分（1.0.7.0 加）。upload/download 三个粗项只能说明「搬运占大头」，
    // 但不能回答「该改哪一行」，所以把搬运拆到能被单独优化的粒度：
    //   packMs   Map(WRITE_DISCARD) + 逐行 memcpy 源图（CPU→映射内存）
    //   maskMs   打包 mask/protect（此前是逐像素 4 次字节写）
    //   copyUpMs 每次 CopyResource（DYNAMIC→DEFAULT）；GPU 内部拷贝
    //   copyDnMs CopyResource 到 staging（拷的是整块桶化尺寸，不是输出矩形）
    //   mapMs    Map(READ) 阻塞等 GPU 完成 + 拷回 dst
    // 生产日志只打前三项（保持原有格式），这三项供台架定位瓶颈。
    double packMs     = 0.0;
    double maskMs     = 0.0;
    double copyUpMs   = 0.0;
    double copyDnMs   = 0.0;
    double mapMs      = 0.0;
    double fillMs     = 0.0;   // ROI 外 dst=src 补齐（只有 dst!=src 时才有）

    // mapMs 再拆（1.0.8.0 加）。mapMs 混了「等 GPU 拷完」和「CPU 读 staging」两件事：
    //   mapWaitMs  Map(READ) 调用本身耗时 —— PCIe 往返 + 等待同步点
    //   mapCopyMs  拿到 pData 之后逐行 memcpy 到 dst 的耗时 —— 纯内存带宽
    // 两者的优化手段完全相反（等待要靠重叠隐藏，搬运只能减少字节数），
    // 不拆开就不知道该动哪一头。mapWaitMs + mapCopyMs 恒等于 mapMs。
    double mapWaitMs  = 0.0;
    double mapCopyMs  = 0.0;

    // uploadMs 再拆（1.0.10.1 加）。同一套逻辑反过来用在上传侧：
    //   upMapMs  三次 Map(WRITE_DISCARD) 调用本身的耗时
    //            —— 驱动要么重命名缓冲（便宜），要么等 GPU 用完上一帧这张 DYNAMIC
    //            纹理（贵）。真机 1920x1080 上 uploadMs=2.90ms 而写入量只有
    //            2.33MB（≈800MB/s），memcpy 不可能这么慢，所以怀疑大头在这里。
    //   upMemMs  逐行 memcpy / memset 的纯内存耗时（= packMs + maskMs − upMapMs）
    // 两者差一个数量级就说明该上双缓冲（轮流两张 pack 纹理），否则只能减字节数。
    double upMapMs    = 0.0;
    double upMemMs    = 0.0;

    int    imgX0 = 0, imgY0 = 0, imgW = 0, imgH = 0;   // 实际上传的图像矩形
    int    planeX0 = 0, planeY0 = 0, planeW = 0, planeH = 0;
    int    pw = 0, ph = 0;                             // 半分辨率平面尺寸
    int    r1 = 0, r2 = 0, ra = 0, passes = 0;
};

// 懒初始化。返回 false 表示无可用 GPU，调用方应回落 CPU。
bool gpuSmoothInit();
bool gpuSmoothReady();
void gpuSmoothShutdown();
const char* gpuSmoothLastError();

// 与 beautycore::surface3RgbBand 同签名、同语义。
// 成功返回 true 且把结果写进 dst；失败返回 false（调用方走 CPU）。
// bbox 是掩码包围盒（bounds_），本函数自行外扩到 halo。
//
// 「同语义」包括 dst 的完整性：CPU 版是整帧逐像素写 dst（mask=0 处写 src），
// 本函数同样保证调用返回后 **dst 全帧都是有效内容** —— 输出矩形外由本函数
// 从 src 补四条带。所以调用方不需要预先 memcpy(dst, src) 就能直接替换 CPU 调用。
bool gpuSmoothRgbBand(const unsigned char* src, unsigned char* dst,
                      int w, int h,
                      const unsigned char* mask, const unsigned char* protect,
                      const beautycore::BandParams& p,
                      const beautycore::RoiRect& bbox,
                      GpuSmoothTiming* timing);

// 累计统计（供日志节流用）
void gpuSmoothStats(long long* frames, double* avgTotal, double* avgPlane);

// 资源生命周期诊断（台架断言用，生产路径不调用）：
// rebuilds = 纹理组重建次数，deviceCreates = 设备创建次数，
// alloc* = 当前桶化分配尺寸。桶化生效时 rebuilds 应远小于帧数。
void gpuSmoothResourceStats(long long* rebuilds, long long* deviceCreates,
                            int* allocImgW, int* allocImgH,
                            int* allocPw, int* allocPh);

// 调试：把内部某个平面回读到 host（0=C 1=B1 2=B2 3=amp）。
// 只给离线对拍台架用，生产路径不调用。回读矩形是真实 pw×ph，
// 桶外的填充像素自动裁掉。
bool gpuSmoothDebugPlane(int which, float* out, int capacity, int* pw, int* ph);
