// gpu_move_bench.cpp -- GPU 磨皮「搬运」分项台架（1.0.7.0）
//
// 目的：把 gpuSmoothRgbBand 的耗时拆到能被单独优化的粒度，用实测决定改哪一行。
// 背景数字（PROGRESS「P1」）：4K 上传 2.9 + 读回 3.2ms，而算法本身 0.15ms。
// 但那两个数是按**整帧**量的，生产走 ROI。所以必须按真实 ROI 重测一次，
// 否则会去优化一个并不存在的瓶颈。
//
// 分项口径（与 GpuSmoothTiming 一致）：
//   pack    Map(WRITE_DISCARD) + 逐行 memcpy 源图
//   mask    打包 mask/protect（旧实现：逐像素 4 次字节写）
//   copyUp  两次 CopyResource（DYNAMIC -> DEFAULT）
//   compute 5 个 kernel 的提交（CPU 侧耗时，不含 GPU 执行）
//   copyDn  CopyResource 到 staging（拷的是整块桶化尺寸）
//   map     Map(READ) 阻塞等 GPU + 拷回输出矩形
//
// 同时做逐字节对拍：搬运怎么改都不能让画面变。差异不是 0 就判失败。
//
// 2026-11-07 追加「运动序列判据」（见文件下半部的 motionCase）：上面 5 个 case 都是
// 静态图，帧 N 与帧 N-1 完全相同，对「用了上一帧的结果」这种缺陷**零分辨力**。
// 新增一组帧间带位移的序列，并报出灵敏度（上一帧结果与本帧结果的差），
// 让将来做 2c 双缓冲延迟取回时，缺陷不会静默通过。第二参数传 nomotion 可跳过。
//
// 编译：同目录 build_gpu_move.ps1（与生产 build.ps1 同参数）。

#include "beauty_smooth.h"
#include "gpu_smooth.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

namespace {

// 墙钟毫秒。不能用 clock()：它统计的是**进程 CPU 时间**，8 线程跑出来
// 是墙钟的 8 倍，跟 GPU 侧的墙钟数没法对比。
double nowMs() {
    using clk = std::chrono::steady_clock;
    static const clk::time_point t0 = clk::now();
    return std::chrono::duration<double, std::milli>(clk::now() - t0).count();
}

// 一张「难算」的图：低频渐变（大累加值）+ 细噪声（取整临界）+ 硬边（窗口跳变）。
// 与 gpu_smooth_test 的合成图同一思路 —— 盒式滤波的误差只在这三处暴露。
void fillImage(std::vector<unsigned char>& buf, int w, int h) {
    buf.resize((size_t)w * h * 4);
    unsigned int s = 0x9E3779B9u;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            s = s * 1664525u + 1013904223u;
            unsigned char* p = &buf[((size_t)y * w + x) * 4];
            const int gx = x * 255 / (w > 1 ? w - 1 : 1);
            const int gy = y * 255 / (h > 1 ? h - 1 : 1);
            const int n  = (int)((s >> 24) & 0x1F) - 16;
            p[0] = (unsigned char)((gx + n + 256) & 0xFF);
            p[1] = (unsigned char)((gy + n + 256) & 0xFF);
            p[2] = (unsigned char)(((gx + gy) / 2 + n + 256) & 0xFF);
            p[3] = 255;
            if (((x / 37) + (y / 37)) & 1) p[0] = (unsigned char)(255 - p[0]);
        }
    }
}

// 掩码 = 软边椭圆，包围盒面积约占画面 areaPct%。真机上人脸框就是这个形状。
// protect 是两个小圆（模拟五官保护）。
void buildMask(std::vector<unsigned char>& mask, std::vector<unsigned char>& prot,
               int w, int h, double areaPct, beautycore::RoiRect* bbox) {
    mask.assign((size_t)w * h, 0);
    prot.assign((size_t)w * h, 0);

    // 椭圆面积 = pi*a*b，令其等于 areaPct% 的画面面积。
    // 脸的高宽比取 1.3（真机人脸框典型值）。
    const double target = (double)w * (double)h * areaPct / 100.0;
    const double aspect = 1.30;
    const double a = std::sqrt(target / (3.14159265358979 * aspect));
    const double b = a * aspect;

    const float cx = w * 0.50f, cy = h * 0.45f;
    const float rx = (float)a, ry = (float)b;

    int x0 = w, y0 = h, x1 = 0, y1 = 0;
    for (int y = 0; y < h; ++y) {
        const float dy = ((float)y - cy) / ry;
        if (dy * dy >= 1.0f) continue;
        for (int x = 0; x < w; ++x) {
            const float dx = ((float)x - cx) / rx;
            const float d2 = dx * dx + dy * dy;
            if (d2 >= 1.0f) continue;
            float v = (1.0f - d2) * 3.0f;
            if (v > 1.0f) v = 1.0f;
            const int m = (int)(v * 255.0f + 0.5f);
            if (m <= 0) continue;
            mask[(size_t)y * w + x] = (unsigned char)m;
            if (x < x0) x0 = x;
            if (y < y0) y0 = y;
            if (x > x1) x1 = x;
            if (y > y1) y1 = y;
        }
    }

    const int exList[2] = { (int)(cx - rx * 0.42f), (int)(cx + rx * 0.42f) };
    const int ey = (int)(cy - ry * 0.28f);
    const int er = (int)(rx * 0.16f);
    for (int k = 0; k < 2; ++k) {
        for (int y = ey - er; y <= ey + er; ++y) {
            if (y < 0 || y >= h) continue;
            for (int x = exList[k] - er; x <= exList[k] + er; ++x) {
                if (x < 0 || x >= w) continue;
                const int ddx = x - exList[k], ddy = y - ey;
                if (ddx * ddx + ddy * ddy > er * er) continue;
                prot[(size_t)y * w + x] = 255;
            }
        }
    }

    bbox->x0 = x0;
    bbox->y0 = y0;
    bbox->x1 = x1 + 1;
    bbox->y1 = y1 + 1;
}

long long countDiff(const unsigned char* a, const unsigned char* b, size_t n, int* maxd) {
    long long nd = 0;
    int md = 0;
    for (size_t i = 0; i < n; ++i) {
        const int d = (int)a[i] - (int)b[i];
        if (d) {
            ++nd;
            const int ad = d < 0 ? -d : d;
            if (ad > md) md = ad;
        }
    }
    if (maxd) *maxd = md;
    return nd;
}

struct Row {
    int w, h;
    double areaPct;
    int bboxW, bboxH;
    int imgW, imgH;          // 实际搬运的图像矩形
    int outW, outH;          // 输出矩形
    double mbPack, mbMask, mbCopyUp, mbCopyDn, mbMap;   // 各段搬运量（MB）
    double pack, mask, copyUp, compute, copyDn, map, fill, total, cpu;
    double mapWait, mapCopy;   // map 的两个组成：等 GPU / CPU 读 staging
    long long diff; int maxd;
};

void oneCase(int w, int h, double areaPct, int reps, Row* out) {
    std::vector<unsigned char> src;
    fillImage(src, w, h);
    std::vector<unsigned char> mask, prot;
    beautycore::RoiRect bbox{};
    buildMask(mask, prot, w, h, areaPct, &bbox);

    beautycore::BandParams p;   // 生产默认：halfRes=1 passes=3 gwp=2

    // ---- CPU 参考（同时是基准时间）----
    std::vector<unsigned char> outCpu(src);
    std::vector<float> scratch;
    // 先热身一次：scratch 首次会按画幅分配几十 MB，那笔分配不能算进 CPU 基准，
    // 否则 CPU 侧被系统性高估，GPU 的相对收益会被夸大。
    beautycore::surface3RgbBand(&src[0], &outCpu[0], w, h,
                                &mask[0], &prot[0], p, bbox, 8, scratch);
    // 取 3 次最小值：多线程 + 机器有其它负载时，最小值最接近「这台的算力下限」。
    double cpuMs = 1e9;
    for (int i = 0; i < 3; ++i) {
        const double c0 = nowMs();
        beautycore::surface3RgbBand(&src[0], &outCpu[0], w, h,
                                    &mask[0], &prot[0], p, bbox, 8, scratch);
        const double dt = nowMs() - c0;
        if (dt < cpuMs) cpuMs = dt;
    }

    // ---- GPU：先热身 2 次，再取 reps 次均值 ----
    std::vector<unsigned char> outGpu(src);
    GpuSmoothTiming t{};
    for (int i = 0; i < 2; ++i) {
        gpuSmoothRgbBand(&src[0], &outGpu[0], w, h, &mask[0], &prot[0], p, bbox, &t);
    }

    double sp = 0, sm = 0, scu = 0, scomp = 0, scd = 0, smap = 0, sfill = 0, stot = 0;
    double swait = 0, scopy = 0;
    for (int i = 0; i < reps; ++i) {
        std::memcpy(&outGpu[0], &src[0], (size_t)w * h * 4);   // 清掉上一轮痕迹
        GpuSmoothTiming g{};
        if (!gpuSmoothRgbBand(&src[0], &outGpu[0], w, h, &mask[0], &prot[0], p, bbox, &g)) {
            std::printf("  [FAIL] gpuSmoothRgbBand returned false: %s\n", gpuSmoothLastError());
            return;
        }
        sp += g.packMs; sm += g.maskMs; scu += g.copyUpMs;
        scomp += g.computeMs; scd += g.copyDnMs; smap += g.mapMs; sfill += g.fillMs;
        swait += g.mapWaitMs; scopy += g.mapCopyMs;
        stot += g.totalMs;
        if (i == 0) t = g;
    }

    out->w = w; out->h = h; out->areaPct = areaPct;
    out->bboxW = bbox.x1 - bbox.x0;
    out->bboxH = bbox.y1 - bbox.y0;
    out->imgW = t.imgW; out->imgH = t.imgH;
    out->outW = out->bboxW + 16; out->outH = out->bboxH + 16;
    out->pack = sp / reps; out->mask = sm / reps; out->copyUp = scu / reps;
    out->compute = scomp / reps; out->copyDn = scd / reps; out->map = smap / reps;
    out->fill = sfill / reps;
    out->mapWait = swait / reps; out->mapCopy = scopy / reps;
    out->total = stot / reps;
    // cpu 是 3 次单次耗时里的**最小值**，不是累加值 —— 与上面那批「sum / reps」
    // 的均值口径本来就不同。1.0.7.0 第一版在这里多除了一次 reps：上面对 cpuMs
    // 取的是 min 而不是 sum，再除 reps 等于把 CPU 基准压到真实值的 1/reps
    // （默认 reps=5 时只剩两成），「GPU 比 CPU 慢 2~3 倍」的对比整段失真。
    out->cpu = cpuMs;

    const double px4 = 4.0 / 1048576.0;
    out->mbPack   = (double)t.imgW * t.imgH * px4;      // RGBA8：4 字节/像素
    // mask 与 protect 各是一张 R8_UINT（1 字节/像素），上传段分两次 Map、各传一遍，
    // 合计 2 字节/像素。1.0.7.0 把掩码从 RGBA8 拆成两张 R8 时这行没跟着改，
    // 仍按 mbPack 的 4 字节记，报出的搬运量是真实值的 2 倍 ——
    // 拿它除出来的带宽只有真实带宽的一半。
    out->mbMask   = (double)t.imgW * t.imgH * 2.0 / 1048576.0;
    out->mbCopyUp = out->mbPack * 2;
    out->mbCopyDn = (double)t.imgW * t.imgH * px4;
    out->mbMap    = (double)out->outW * out->outH * px4;

    out->diff = countDiff(&outCpu[0], &outGpu[0], (size_t)w * h * 4, &out->maxd);
}

// ==================== 运动序列判据（计划一 2c 的防护基座，2026-11-07）====================
//
// 为什么必须有这一组：上面 5 个 case 与 `baseline.py` 的 18 个画幅，输入都是**静态图**。
// 同一张图连喂多次，帧 N 与帧 N-1 逐字节相同 —— 在这种输入下，「返回本帧结果」与
// 「返回上一帧结果」的输出完全一致，逐字节判据**没有任何分辨力**。
//
// 后果很具体：计划一里的 2c（双缓冲延迟取回，把 Map 的等待挪到下一帧）若照现有台架
// 验证，会轻松通过全部判据，却把「dst 的 ROI 被写进上一帧像素」的缺陷带进生产。
//
// 所以这里补一组帧间有位移的序列，并额外报一个**灵敏度**数字：把上一帧的 CPU 结果
// 拿去和本帧的 CPU 结果比，得到的 diff 就是「一个按上帧返回的实现会被判成多少错」。
// 灵敏度恒为 0 说明这组序列白跑（编不出可分辨的两帧），此时直接判失败，不允许静默失明。
void shiftRgbaWrap(std::vector<unsigned char>& buf, int w, int h, int dx) {
    if (dx <= 0) return;
    dx %= w;
    if (dx == 0) return;
    std::vector<unsigned char> tmp((size_t)w * 4);
    for (int y = 0; y < h; ++y) {
        unsigned char* row = &buf[(size_t)y * w * 4];
        std::memcpy(&tmp[0], row, (size_t)w * 4);
        std::memcpy(row, &tmp[(size_t)dx * 4], (size_t)(w - dx) * 4);
        std::memcpy(row + (size_t)(w - dx) * 4, &tmp[0], (size_t)dx * 4);
    }
}

void shiftR8Wrap(std::vector<unsigned char>& buf, int w, int h, int dx) {
    if (dx <= 0) return;
    dx %= w;
    if (dx == 0) return;
    std::vector<unsigned char> tmp((size_t)w);
    for (int y = 0; y < h; ++y) {
        unsigned char* row = &buf[(size_t)y * w];
        std::memcpy(&tmp[0], row, (size_t)w);
        std::memcpy(row, &tmp[dx], (size_t)(w - dx));
        std::memcpy(row + (w - dx), &tmp[0], (size_t)dx);
    }
}

// 扫非零像素取包围盒（位移后 bbox 会跟着动，必须重算 —— 这也正好覆盖
// 「上一帧的输出矩形与这一帧不同」这个 2c 才会遇到的坑）。
beautycore::RoiRect maskBBox(const std::vector<unsigned char>& mask, int w, int h) {
    beautycore::RoiRect r{};
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; ++y) {
        const unsigned char* row = &mask[(size_t)y * w];
        for (int x = 0; x < w; ++x) {
            if (!row[x]) continue;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
    }
    if (x1 < 0) return r;      // 空掩码 → empty()
    r.x0 = x0; r.y0 = y0; r.x1 = x1 + 1; r.y1 = y1 + 1;
    return r;
}

struct MotionRow {
    int frame, dx, bboxX0;
    long long gpuVsCpu; int gpuMaxd;      // 本实现：本帧 GPU 结果 vs 本帧 CPU 结果
    long long staleVsCpu; int staleMaxd;  // 上帧 CPU 结果 vs 本帧 CPU 结果（= 2c 会报的错）
};

// 返回值：被判为不一致的帧数。degenerate 置 true 表示这组序列没有分辨力。
int motionCase(int w, int h, double areaPct, int frames, int shiftPx,
               std::vector<MotionRow>* rows, bool* degenerate) {
    std::vector<unsigned char> baseSrc, baseMask, baseProt;
    fillImage(baseSrc, w, h);
    beautycore::RoiRect bb0{};
    buildMask(baseMask, baseProt, w, h, areaPct, &bb0);

    beautycore::BandParams p;
    std::vector<float> scratch;

    std::vector<unsigned char> srcF, maskF, protF;
    std::vector<unsigned char> outCpu, outGpu, prevCpu;
    bool hasPrev = false;
    int bad = 0;
    long long maxStale = 0;

    // 热身：scratch 的首次分配 + 设备纹理组的首次建立都不算进判据。
    {
        std::vector<unsigned char> warmS(baseSrc), warmD(baseSrc);
        beautycore::surface3RgbBand(&warmS[0], &warmD[0], w, h,
                                    &baseMask[0], &baseProt[0], p, bb0, 8, scratch);
        std::vector<unsigned char> warmG(baseSrc);
        GpuSmoothTiming tw{};
        gpuSmoothRgbBand(&baseSrc[0], &warmG[0], w, h,
                         &baseMask[0], &baseProt[0], p, bb0, &tw);
    }

    for (int f = 0; f < frames; ++f) {
        const int dx = f * shiftPx;
        srcF  = baseSrc;   shiftRgbaWrap(srcF, w, h, dx);
        maskF = baseMask;  shiftR8Wrap(maskF, w, h, dx);
        protF = baseProt;  shiftR8Wrap(protF, w, h, dx);
        const beautycore::RoiRect bbox = maskBBox(maskF, w, h);

        outCpu.assign((size_t)w * h * 4, 0);
        outGpu.assign((size_t)w * h * 4, 0);

        beautycore::surface3RgbBand(&srcF[0], &outCpu[0], w, h,
                                    &maskF[0], &protF[0], p, bbox, 8, scratch);

        GpuSmoothTiming g{};
        if (!gpuSmoothRgbBand(&srcF[0], &outGpu[0], w, h,
                              &maskF[0], &protF[0], p, bbox, &g)) {
            std::printf("  [FAIL] frame %d: gpuSmoothRgbBand returned false: %s\n",
                        f, gpuSmoothLastError());
            ++bad;
            hasPrev = false;   // 断了链，下一帧不再跨帧比较
            continue;
        }

        MotionRow r{};
        r.frame = f; r.dx = dx; r.bboxX0 = bbox.x0;
        r.gpuVsCpu = countDiff(&outCpu[0], &outGpu[0], (size_t)w * h * 4, &r.gpuMaxd);
        if (hasPrev)
            r.staleVsCpu = countDiff(&outCpu[0], &prevCpu[0], (size_t)w * h * 4, &r.staleMaxd);
        else
            r.staleVsCpu = -1;   // 首帧没有上一帧

        if (r.gpuVsCpu != 0) ++bad;
        if (r.staleVsCpu > maxStale) maxStale = r.staleVsCpu;

        rows->push_back(r);
        prevCpu.swap(outCpu);
        hasPrev = true;
    }

    *degenerate = (maxStale <= 0);
    return bad;
}

struct PipeRow {
    int frame;
    int mode;                       // 见 PipeMode
    long long vsCpu; int maxd;      // 与 CPU 结果之差 —— 唯一判据，必须为 0
    int retrieved;                  // timing.pipeRetrieved
    int deferred;                   // timing.pipeDeferred
    int block;                      // timing.pipeBlock（为什么没走）
    float sigAvg, sigBad;
};

// ---- 2c 延迟取回判据（1.1.1.0 重写）----
//
// ★ 1.1.0.0 那版判据是错的，必须先记下来，别再犯：
//   它把期望值定义成「本帧 src + 上一帧矩形里填上一帧 CPU 结果」，等于预设
//   「贴旧帧内容」就是正确输出。于是 7 行全 0 差异、逐字节 18/18 全是真数字，
//   验的却是一个错的设计 —— 真机上直接表现为 ROI 整块错位
//   （用户原话：以脸为中心的长方形透明遮罩、闪烁、像放大）。
//   教训：判据只能以「本帧的正确输出」为准，绝不能以「实现怎么拼」为准。
//   一个判据只要写出了「实现应该产生什么」，就已经放弃了验正确性的能力。
//
// 新判据只有一条：**每一帧都必须与 CPU 结果逐字节一致**。
// 再叠三条「该走 / 不该走」的断言，防止用例空转（跑了但根本没进延迟路径）：
//   ① 全静止（PM_STATIC）    → f>=1 必须走延迟（取=1），且与 CPU 一致
//   ② 只有 ROI 内内容动       → 必须退回同步（阻=motion），且与 CPU 一致
//   ③ 整体平移、bbox 跟着变   → 必须退回同步（阻=rect），且与 CPU 一致
// ②是 1.1.0.0 完全缺失的一条，也正是这次真机事故最接近的场景：
// 「人脸框稳定、人在框里动」，靠矩形比对拦不住，只有比画面内容才拦得住。
enum PipeMode { PM_STATIC = 0, PM_ROI_SHIFT = 1, PM_SHIFT_ALL = 2 };

const char* pipeModeName(int m) {
    switch (m) {
        case PM_ROI_SHIFT: return "仅ROI内动";
        case PM_SHIFT_ALL: return "整体平移";
        default:           return "全静止";
    }
}
const char* pipeBlockName(int b) {
    switch (b) {
        case 1: return "noprev";
        case 2: return "jump";
        case 3: return "param";
        case 4: return "rect";
        case 5: return "motion";
        default: return "ok";
    }
}
int pipelineCase(int w, int h, double areaPct, int frames, PipeMode mode,
                 std::vector<PipeRow>* rows, long long* maxdOut) {
    std::vector<unsigned char> baseSrc, baseMask, baseProt;
    fillImage(baseSrc, w, h);
    beautycore::RoiRect bb0{};
    buildMask(baseMask, baseProt, w, h, areaPct, &bb0);

    beautycore::BandParams p;
    std::vector<float> scratch;
    static int kOwner = 0;          // 实例身份：真机传引擎 this，台架给个稳定地址

    std::vector<unsigned char> srcF, maskF, protF, outCpu, outGpu;
    int bad = 0;
    long long worst = 0;

    // 热身：首次分配与纹理组建立不算判据
    {
        std::vector<unsigned char> warmG(baseSrc);
        GpuSmoothTiming tw{};
        gpuSmoothRgbBand(&baseSrc[0], &warmG[0], w, h,
                         &baseMask[0], &baseProt[0], p, bb0, &tw);
    }

    for (int f = 0; f < frames; ++f) {
        srcF  = baseSrc;
        maskF = baseMask;
        protF = baseProt;
        // 三种模式只在「谁跟着动」上有区别：
        //   STATIC     都不动
        //   ROI_SHIFT  只 src 动（mask 不动 → bbox 不动 → 矩形不变，只剩画面在变）
        //   SHIFT_ALL  三个都动（bbox 跟着变 → 矩形变，走 PB_RECT 那条路）
        const int srcStep = (mode == PM_STATIC) ? 0 : ((mode == PM_ROI_SHIFT) ? 2 : 3);
        const int mskStep = (mode == PM_SHIFT_ALL) ? 3 : 0;
        if (srcStep) shiftRgbaWrap(srcF, w, h, f * srcStep);
        if (mskStep) {
            shiftR8Wrap(maskF, w, h, f * mskStep);
            shiftR8Wrap(protF, w, h, f * mskStep);
        }
        const beautycore::RoiRect bbox = maskBBox(maskF, w, h);

        outCpu.assign((size_t)w * h * 4, 0);
        outGpu.assign((size_t)w * h * 4, 0);

        beautycore::surface3RgbBand(&srcF[0], &outCpu[0], w, h,
                                    &maskF[0], &protF[0], p, bbox, 8, scratch);

        GpuSmoothTiming g{};
        if (!gpuSmoothRgbBand(&srcF[0], &outGpu[0], w, h,
                              &maskF[0], &protF[0], p, bbox, &g,
                              &kOwner, (long long)f, /*allowPipeline=*/true)) {
            std::printf("  [FAIL] pipeline %s frame %d: %s\n",
                        pipeModeName(mode), f, gpuSmoothLastError());
            ++bad;
            continue;
        }

        PipeRow r{};
        r.frame = f;
        r.mode  = mode;
        r.retrieved = g.pipeRetrieved;
        r.deferred  = g.pipeDeferred;
        r.block     = g.pipeBlock;
        r.sigAvg    = g.pipeSigAvg;
        r.sigBad    = g.pipeSigBad;

        // 唯一判据：与 CPU 逐字节一致。
        // 延迟取回只在「输入与上一帧相同」时才成立，而那种情况下它输出的必然等于本帧
        // CPU 结果 —— 所以这条判据对三种模式一视同仁，不需要为延迟路径另写期望值。
        r.vsCpu = countDiff(&outCpu[0], &outGpu[0], (size_t)w * h * 4, &r.maxd);
        if (r.vsCpu != 0) ++bad;
        if (r.vsCpu > worst) worst = r.vsCpu;

        // 该走 / 不该走。f=0 没有上一帧，两种模式都必须是 取=0。
        if (f >= 1) {
            if (mode == PM_STATIC) {
                if (r.retrieved != 1) ++bad;   // 静止却不走延迟 → 优化形同虚设
            } else {
                if (r.retrieved != 0) ++bad;   // 动了还走延迟 → 正是真机那个 bug
            }
        }
        rows->push_back(r);
    }

    // 关掉流水线：必须立刻回到「取=0 且与 CPU 一致」。
    // 这是 ini 里 gpu_smooth_pipeline=off 的逃生通道，不能只是「大致正确」。
    {
        std::vector<unsigned char> s(baseSrc), m(baseMask), pr(baseProt);
        const beautycore::RoiRect bb = maskBBox(m, w, h);
        outCpu.assign((size_t)w * h * 4, 0);
        outGpu.assign((size_t)w * h * 4, 0);
        beautycore::surface3RgbBand(&s[0], &outCpu[0], w, h, &m[0], &pr[0], p, bb, 8, scratch);
        GpuSmoothTiming g{};
        if (!gpuSmoothRgbBand(&s[0], &outGpu[0], w, h, &m[0], &pr[0], p, bb, &g,
                              &kOwner, (long long)frames, /*allowPipeline=*/false)) {
            std::printf("  [FAIL] pipeline drain: %s\n", gpuSmoothLastError());
            ++bad;
        } else {
            int md = 0;
            PipeRow r{};
            r.frame = frames;
            r.mode  = mode;
            r.vsCpu = countDiff(&outCpu[0], &outGpu[0], (size_t)w * h * 4, &md);
            r.maxd  = md;
            r.retrieved = g.pipeRetrieved;
            r.deferred  = g.pipeDeferred;
            r.block     = g.pipeBlock;
            if (r.vsCpu != 0) ++bad;
            if (r.retrieved != 0) ++bad;   // 关了还取上一帧 → 开关没生效
            rows->push_back(r);
        }
    }

    *maxdOut = worst;
    return bad;
}
// ==================== growP 消融（1.1.1.3 取样后新增，2026-11-07）====================
//
// 为什么要有这一组：真机取样把一个数量级更大的浪费挖出来了 —— GPU 实际搬运的
// 矩形不是掩码包围盒，而是包围盒外每边再扩 growP 平面像素：
//
//     growP = passes*32 + passes*(r2+ra) + 2  =  116 平面像素 = 232 图像像素/边
//
// 1080p 上 bbox=971x672 被扩到 1488x1016，边距占了整个矩形的 57%。而 GPU 计算
// 只要 0.14ms、搬运占时 92% —— 也就是说优化对象是这个常量，不是脸框。
//
// `passes*32` 那一项的依据是**一次实验**（「内缩 64 处还剩 1613 字节、96 处归零」），
// 不是几何必然。本组把 growP 逐档往下压，用与其它用例同一条判据（与 CPU 整帧结果
// 逐字节一致）找出「还能保持 0 差异的最小 growP」。
//
// 判据怎么读：
//   diff == 0  → 这一档边距够用，可以照此缩小
//   diff  > 0  → 边缘污染已经吃到有效输出区，这一档不行
// 注意 diff 恒为 0 也可能只是「污染还没到有效区」，所以**最小可用档再减一档必须
// 翻红**才有说服力 —— 否则说明这组用例的分辨力不够（掩码太居中、对齐余量太大）。
struct GrowRow {
    int gp;                 // 平面像素边距
    int imgW, imgH;         // 实际搬运矩形
    int r1, r2, ra, passes; // 本档实际的盒式半径与趟数（r 随画幅缩放，必须记）
    int effM;               // 有效边距（平面像素，取 x/y 较小者）
    double mb;              // 单程搬运量（RGBA8）
    double move, total;     // 搬图耗时 / 合计耗时
    long long diff; int maxd;
};

// 返回「这一档不达标」的用例数（diff != 0）。
int growpOneCase(int w, int h, double areaPct, int reps,
                 const int* gps, int nGps, std::vector<GrowRow>* out) {
    std::vector<unsigned char> src;
    fillImage(src, w, h);
    std::vector<unsigned char> mask, prot;
    beautycore::RoiRect bbox{};
    buildMask(mask, prot, w, h, areaPct, &bbox);

    beautycore::BandParams p;
    std::vector<float> scratch;

    // CPU 整帧结果是唯一参考，只算一次（本组不比 CPU 耗时，比的是画面对不对）。
    std::vector<unsigned char> outCpu(src);
    beautycore::surface3RgbBand(&src[0], &outCpu[0], w, h,
                                &mask[0], &prot[0], p, bbox, 8, scratch);

    int bad = 0;
    for (int gi = 0; gi < nGps; ++gi) {
        const int gp = gps[gi];
        gpuSmoothSetGrowPForTest(gp);

        // 每换一档纹理组都要重建（尺寸变了），所以每档各自热身 2 次：
        // 不热身的话 CreateTexture2D 的一次性开销会混进这一档的耗时里。
        std::vector<unsigned char> outGpu(src);
        GpuSmoothTiming t{};
        for (int i = 0; i < 2; ++i) {
            std::memcpy(&outGpu[0], &src[0], (size_t)w * h * 4);
            gpuSmoothRgbBand(&src[0], &outGpu[0], w, h, &mask[0], &prot[0], p, bbox, &t);
        }

        double smove = 0.0, stot = 0.0;
        for (int i = 0; i < reps; ++i) {
            std::memcpy(&outGpu[0], &src[0], (size_t)w * h * 4);
            GpuSmoothTiming g{};
            if (!gpuSmoothRgbBand(&src[0], &outGpu[0], w, h,
                                  &mask[0], &prot[0], p, bbox, &g)) {
                std::printf("  [FAIL] growp %d: %s\n", gp, gpuSmoothLastError());
                ++bad;
                break;
            }
            smove += g.packMs + g.maskMs + g.copyUpMs + g.copyDnMs + g.mapMs + g.fillMs;
            stot  += g.totalMs;
            if (i == 0) t = g;
        }

        GrowRow r{};
        r.gp = gp;
        r.imgW = t.imgW; r.imgH = t.imgH;
        r.r1 = t.r1; r.r2 = t.r2; r.ra = t.ra; r.passes = t.passes;

        // 有效边距：growP 只是**名义值**，plx0 = alignDown32(pxLo - growP)
        // 会把它推得更远（最多 +31 平面像素），而且 x/y 各自独立。
        // 判阈值只能看这个数，否则「32 通过、40 也通过」会被读成「32 就够」。
        const int outX0 = (bbox.x0 - 8 < 0) ? 0 : (bbox.x0 - 8);
        const int outY0 = (bbox.y0 - 8 < 0) ? 0 : (bbox.y0 - 8);
        const int effX = outX0 - t.imgX0;         // 图像像素
        const int effY = outY0 - t.imgY0;
        r.effM = (effX < effY ? effX : effY) / 2; // 取小的那个，换成平面像素
        r.mb = (double)t.imgW * t.imgH * 4.0 / 1048576.0;
        r.move = smove / reps;
        r.total = stot / reps;
        r.diff = countDiff(&outCpu[0], &outGpu[0], (size_t)w * h * 4, &r.maxd);
        if (r.diff != 0) ++bad;
        out->push_back(r);
    }

    gpuSmoothSetGrowPForTest(-1);   // 恢复生产公式，别把状态漏给后面的用例
    return bad;
}
}  // namespace

int main(int argc, char** argv) {
    const bool ablate = (argc > 1 && std::strcmp(argv[1], "growp") == 0);
    const int reps = (ablate || argc <= 1) ? 5 : atoi(argv[1]);

    if (!gpuSmoothInit()) {
        std::printf("[FAIL] gpuSmoothInit: %s\n", gpuSmoothLastError());
        return 2;
    }

    struct Spec { int w, h; double pct; const char* note; };
    const Spec specs[] = {
        { 1920, 1080, 22.0, "1080p typical face" },
        { 1920, 1080,  6.0, "1080p small face" },
        { 3840, 2160,  6.0, "4K small face" },
        { 3840, 2160, 22.0, "4K typical face" },
        { 640,  394,  22.0, "small frame" },
    };

    // ---- growp 子命令：只跑边距消融，不跑其它用例 ----
    if (ablate) {
        // 档位密一点：有效边距 = alignDown32(pxLo - growP) 的结果，
        // 每档实际落点会被 32 对齐推走 0~31 像素，稀疏采点会读错阈值。
        static const int gps[] = { 0, 8, 16, 24, 32, 40, 48, 64, 96, 116 };
        const int nGps = (int)(sizeof(gps) / sizeof(gps[0]));

        // 消融专用画幅：比主台架更极端 —— 脸越小，边距占搬运量的比例越大，
        // 阈值也越容易被逼近。3% 的脸在 4K 上只有巴掌大，是边距开销的最坏情形。
        const Spec ablSpecs[] = {
            { 1920, 1080, 22.0, "1080p typical face" },
            { 1920, 1080,  6.0, "1080p small face" },
            { 1920, 1080,  3.0, "1080p tiny face" },
            { 3840, 2160,  6.0, "4K small face" },
            { 3840, 2160, 22.0, "4K typical face" },
            { 3840, 2160,  3.0, "4K tiny face" },
            { 1280,  720, 22.0, "720p typical face" },
            { 640,  394,  22.0, "small frame" },
        };

        std::printf("=== growP 消融：边距逐档下压，判据=与 CPU 整帧逐字节一致 ===\n");
        std::printf("生产公式 growP = passes*32 + passes*(r2+ra) + 2 = 116 平面像素（232 图像像素/边）\n");
        std::printf("「多搬」= 实际搬运矩形面积 / 有效输出区面积 - 1\n\n");

        int totalBad = 0;
        for (size_t i = 0; i < sizeof(ablSpecs) / sizeof(ablSpecs[0]); ++i) {
            std::vector<GrowRow> rows;
            const int bad = growpOneCase(ablSpecs[i].w, ablSpecs[i].h, ablSpecs[i].pct,
                                         reps, gps, nGps, &rows);
            totalBad += bad;

            // 有效输出区 = bbox + 8*2（gpu_smooth 里的 outGrow）
            std::printf("[GROWP] %-20s %dx%d 脸面积=%.1f%%", ablSpecs[i].note,
                        ablSpecs[i].w, ablSpecs[i].h, ablSpecs[i].pct);
            if (!rows.empty())
                std::printf("  r=%d/%d/%d P=%d  生产 growP=%d",
                            rows[0].r1, rows[0].r2, rows[0].ra, rows[0].passes,
                            rows[0].passes * 32 + rows[0].passes * (rows[0].r2 + rows[0].ra) + 2);
            std::printf("\n");
            std::printf("  %-8s %-8s %-13s %-9s %-9s %-9s %-6s %s\n",
                        "growP", "有效", "搬运矩形", "搬运MB", "搬图ms", "合计ms", "diff", "判定");
            for (size_t k = 0; k < rows.size(); ++k) {
                const GrowRow& r = rows[k];
                std::printf("  %-8s %-8d %-13s %-9.2f %-9.2f %-9.2f %-6lld %s\n",
                            (r.gp == 116 || r.gp == 140) ? (std::to_string(r.gp) + "*").c_str()
                                                         : std::to_string(r.gp).c_str(),
                            r.effM,
                            (std::to_string(r.imgW) + "x" + std::to_string(r.imgH)).c_str(),
                            r.mb, r.move, r.total, r.diff,
                            r.diff == 0 ? "OK" : (std::string("MISMATCH max=") +
                                                  std::to_string(r.maxd)).c_str());
            }
            std::printf("\n");
        }
        gpuSmoothShutdown();
        std::printf("RESULT: %s\n", totalBad ? "见 MISMATCH 行" : "全部档位逐字节一致");
        return 0;
    }

    std::vector<Row> rows;
    for (size_t i = 0; i < sizeof(specs) / sizeof(specs[0]); ++i) {
        Row r{};
        oneCase(specs[i].w, specs[i].h, specs[i].pct, reps, &r);
        rows.push_back(r);
        std::printf("[CASE] %-20s %dx%d bbox=%dx%d upload=%dx%d (%.1f MB) out=%dx%d\n",
                    specs[i].note, specs[i].w, specs[i].h,
                    r.bboxW, r.bboxH, r.imgW, r.imgH, r.mbPack, r.outW, r.outH);
    }

    std::printf("\n%-20s %8s %8s %8s %8s %8s %8s %8s | %8s %8s | %8s\n",
                "case", "pack", "mask", "copyUp", "compute", "copyDn", "map", "fill",
                "total", "move%", "cpu");
    long long badDiff = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        const double move = r.pack + r.mask + r.copyUp + r.copyDn + r.map + r.fill;
        std::printf("%-20s %8.2f %8.2f %8.2f %8.2f %8.2f %8.2f %8.2f | %8.2f %7.1f%% | %8.2f\n",
                    specs[i].note, r.pack, r.mask, r.copyUp, r.compute, r.copyDn, r.map, r.fill,
                    r.total, move * 100.0 / (r.total > 0 ? r.total : 1.0), r.cpu);
        if (r.diff != 0) ++badDiff;
    }

    // map 的两半：等 GPU 与 CPU 读 staging。优化手段完全相反，必须分开看。
    std::printf("\n%-20s %10s %10s %10s %10s\n",
                "case", "mapWait", "mapCopy", "sum", "map");
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        std::printf("%-20s %10.2f %10.2f %10.2f %10.2f\n",
                    specs[i].note, r.mapWait, r.mapCopy,
                    r.mapWait + r.mapCopy, r.map);
    }

    // 机器可读：一行一份，便于多轮取中位数（上面的表格是给人看的）。
    std::printf("\n");
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        std::printf("CSV,%dx%d,bbox=%dx%d,img=%dx%d,pack=%.3f,mask=%.3f,copyUp=%.3f,"
                    "compute=%.3f,copyDn=%.3f,map=%.3f,fill=%.3f,total=%.3f,cpu=%.3f,"
                    "mapWait=%.3f,mapCopy=%.3f\n",
                    r.w, r.h, r.bboxW, r.bboxH, r.imgW, r.imgH,
                    r.pack, r.mask, r.copyUp, r.compute, r.copyDn, r.map, r.fill,
                    r.total, r.cpu, r.mapWait, r.mapCopy);
    }

    std::printf("\n[EXACT] byte-exact vs CPU:\n");
    for (size_t i = 0; i < rows.size(); ++i) {
        std::printf("  %-20s diff=%lld maxdiff=%d  %s\n",
                    specs[i].note, rows[i].diff, rows[i].maxd,
                    rows[i].diff == 0 ? "OK" : "MISMATCH");
    }

    // ---- 运动序列：唯一能发现「用错帧」的判据（2c 上线前必须先有它）----
    const bool skipMotion = (argc > 2 && std::strcmp(argv[2], "nomotion") == 0);
    int  mBad = 0;
    bool mDegen = false;
    std::vector<MotionRow> mrows;
    if (!skipMotion) {
        // 1080p、22% 人脸、6 帧、每帧左移 3 px（累计 15 px，约半张脸的宽度）。
        // 位移量要够大：太小则相邻两帧的磨皮结果几乎相同，灵敏度会被磨没。
        mBad = motionCase(1920, 1080, 22.0, 6, 3, &mrows, &mDegen);

        std::printf("\n[MOTION] 运动序列判据 1080p 22%% 6 帧 每帧左移 3px\n");
        std::printf("  说明：gpuVsCpu 是本实现的错（应为 0）；staleVsCpu 是「上一帧 CPU 结果」\n"
                    "        与本帧 CPU 结果的差，即「若改成返回上一帧结果」会报的错。\n");
        std::printf("  %-6s %-5s %-8s %-21s %-21s\n",
                    "frame", "dx", "bboxX0", "gpuVsCpu(diff/max)", "staleVsCpu(diff/max)");
        for (size_t i = 0; i < mrows.size(); ++i) {
            const MotionRow& m = mrows[i];
            char s1[32], s2[32];
            std::snprintf(s1, sizeof(s1), "%lld/%d", m.gpuVsCpu, m.gpuMaxd);
            if (m.staleVsCpu < 0) std::snprintf(s2, sizeof(s2), "(首帧)");
            else                  std::snprintf(s2, sizeof(s2), "%lld/%d", m.staleVsCpu, m.staleMaxd);
            std::printf("  %-6d %-5d %-8d %-21s %-21s %s\n",
                        m.frame, m.dx, m.bboxX0, s1, s2,
                        m.gpuVsCpu == 0 ? "OK" : "MISMATCH");
        }
        if (mDegen) {
            std::printf("  [FAIL] 灵敏度为 0：本组序列相邻帧的 CPU 结果完全相同，\n"
                        "         说明它分辨不出「用错帧」，判据失效。请加大 shiftPx。\n");
        } else {
            std::printf("  [灵敏度] 有效：一个「返回上一帧结果」的实现会在上面每帧非首帧处\n"
                        "           报出 staleVsCpu 那列的量级，本判据足以拦住 2c 的缺陷。\n");
        }
    }

    // ---- 2c 延迟取回：判据只有一条 —— 每帧都必须等于 CPU 结果（1.1.1.0 重写）----
    // 上一版（1.1.0.0）拿「本条流水线会拼出什么」当期望值，等于自己给自己出题，
    // 7 行全过但真机 ROI 整块错位。这里改成三种模式各自跟 CPU 比，同时断言该走/不该走。
    int  pBad = 0;
    long long pWorst = 0;
    std::vector<PipeRow> prows;
    if (!skipMotion) {
        static const PipeMode kModes[] = { PM_STATIC, PM_ROI_SHIFT, PM_SHIFT_ALL };
        for (int mi = 0; mi < 3; ++mi) {
            long long w0 = 0;
            pBad += pipelineCase(1920, 1080, 22.0, 6, kModes[mi], &prows, &w0);
            if (w0 > pWorst) pWorst = w0;
        }

        std::printf("\n[PIPELINE] 延迟取回判据 1080p 22%% 6 帧 x 3 模式\n");
        std::printf("  判据：每行 vsCPU 必须为 0（与 CPU 结果逐字节一致，不设第二套期望值）。\n");
        std::printf("  断言：全静止 取=1；仅ROI内动 / 整体平移 取=0；末行 drain 关流水线，取=0。\n");
        std::printf("  %-7s %-12s %-15s %-4s %-4s %-8s %s\n",
                    "frame", "mode", "vsCPU(diff/max)", "取", "管", "阻断", "sig(avg/bad)");
        for (size_t i = 0; i < prows.size(); ++i) {
            const PipeRow& r = prows[i];
            char s[40], sg[40];
            std::snprintf(s, sizeof(s), "%lld/%d", r.vsCpu, r.maxd);
            std::snprintf(sg, sizeof(sg), "%.2f/%.4f", r.sigAvg, r.sigBad);
            std::printf("  %-7d %-12s %-15s %-4d %-4d %-8s %s %s\n",
                        r.frame, pipeModeName(r.mode), s,
                        r.retrieved, r.deferred, pipeBlockName(r.block),
                        sg, r.vsCpu == 0 ? "OK" : "MISMATCH");
        }
        std::printf("  最大差异 %lld 字节；判据失败帧数 %d\n", pWorst, pBad);
    }

    gpuSmoothShutdown();
    const long long totalBad = badDiff + (long long)mBad + (mDegen ? 1LL : 0LL) + (long long)pBad;
    if (totalBad) {
        std::printf("\nRESULT: FAIL (%lld static + %d motion case(s) + %d pipeline; %s)\n",
                    badDiff, mBad, pBad, mDegen ? "灵敏度为 0" : "见上");
        return 1;
    }
    std::printf("\nRESULT: PASS (静态 %d 例 + 运动 %d 帧 + 延迟取回 %d 帧 全部逐字节一致，运动判据灵敏度有效)\n",
                (int)rows.size(), (int)mrows.size(), (int)prows.size());
    return 0;
}
