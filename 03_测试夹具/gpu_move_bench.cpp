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
// 编译：同目录 build_gpu_move.ps1（与生产 build.ps1 同参数）。

#include "beauty_smooth.h"
#include "gpu_smooth.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
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
    for (int i = 0; i < reps; ++i) {
        std::memcpy(&outGpu[0], &src[0], (size_t)w * h * 4);   // 清掉上一轮痕迹
        GpuSmoothTiming g{};
        if (!gpuSmoothRgbBand(&src[0], &outGpu[0], w, h, &mask[0], &prot[0], p, bbox, &g)) {
            std::printf("  [FAIL] gpuSmoothRgbBand returned false: %s\n", gpuSmoothLastError());
            return;
        }
        sp += g.packMs; sm += g.maskMs; scu += g.copyUpMs;
        scomp += g.computeMs; scd += g.copyDnMs; smap += g.mapMs; sfill += g.fillMs;
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
    out->total = stot / reps; out->cpu = cpuMs / reps;

    const double px4 = 4.0 / 1048576.0;
    out->mbPack   = (double)t.imgW * t.imgH * px4;
    out->mbMask   = out->mbPack;
    out->mbCopyUp = out->mbPack * 2;
    out->mbCopyDn = (double)t.imgW * t.imgH * px4;
    out->mbMap    = (double)out->outW * out->outH * px4;

    out->diff = countDiff(&outCpu[0], &outGpu[0], (size_t)w * h * 4, &out->maxd);
}

}  // namespace

int main(int argc, char** argv) {
    const int reps = (argc > 1) ? atoi(argv[1]) : 5;

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

    // 机器可读：一行一份，便于多轮取中位数（上面的表格是给人看的）。
    std::printf("\n");
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        std::printf("CSV,%dx%d,bbox=%dx%d,img=%dx%d,pack=%.3f,mask=%.3f,copyUp=%.3f,"
                    "compute=%.3f,copyDn=%.3f,map=%.3f,fill=%.3f,total=%.3f,cpu=%.3f\n",
                    r.w, r.h, r.bboxW, r.bboxH, r.imgW, r.imgH,
                    r.pack, r.mask, r.copyUp, r.compute, r.copyDn, r.map, r.fill,
                    r.total, r.cpu);
    }

    std::printf("\n[EXACT] byte-exact vs CPU:\n");
    for (size_t i = 0; i < rows.size(); ++i) {
        std::printf("  %-20s diff=%lld maxdiff=%d  %s\n",
                    specs[i].note, rows[i].diff, rows[i].maxd,
                    rows[i].diff == 0 ? "OK" : "MISMATCH");
    }

    gpuSmoothShutdown();
    if (badDiff) {
        std::printf("\nRESULT: FAIL (%lld case(s) not byte-exact)\n", badDiff);
        return 1;
    }
    std::printf("\nRESULT: PASS (all cases byte-exact)\n");
    return 0;
}
