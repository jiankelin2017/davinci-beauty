// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Jian
// gpu_smooth_test.cpp —— P2 离线对拍台架
//
// 验收标准（用户 2026-11-06 拍板 A 案）：**逐字节一致**，0 字节差异。
//
// 做法：同一张合成图，CPU 跑 surface3RgbBand，GPU 跑 gpuSmoothRgbBand，
// 全帧比字节。合成图刻意覆盖宽动态范围（渐变 + 硬边 + 随机噪声），
// 因为盒式滤波的误差只在「累加值很大」和「四舍五入临界点」两处暴露。
//
// 为什么在离线台上做：接进 Resolve 之后，「算法不对」和「宿主没跑通」
// 两类故障会混在一起，排查成本高一个数量级。

#include "beauty_smooth.h"
#include "gpu_smooth.h"
#include "beauty_log.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <vector>
#include <string>

namespace {

void fillImage(std::vector<unsigned char>& buf, int w, int h) {
    buf.resize((size_t)w * h * 4);
    unsigned int s = 0x9E3779B9u;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            s = s * 1664525u + 1013904223u;
            unsigned char* p = &buf[((size_t)y * w + x) * 4];
            // 低频渐变打底（大累加值），叠一层细噪声（四舍五入临界点）
            const int gx = x * 255 / (w > 1 ? w - 1 : 1);
            const int gy = y * 255 / (h > 1 ? h - 1 : 1);
            const int n  = (int)((s >> 24) & 0x1F) - 16;
            p[0] = (unsigned char)((gx + n + 256) & 0xFF);
            p[1] = (unsigned char)((gy + n + 256) & 0xFF);
            p[2] = (unsigned char)(((gx + gy) / 2 + n + 256) & 0xFF);
            p[3] = 255;
            // 每 37 像素一条硬边，制造盒式窗口内的大跳变
            if (((x / 37) + (y / 37)) & 1) { p[0] = (unsigned char)(255 - p[0]); }
        }
    }
}

// 掩码：软边椭圆；protect：两个小圆斑（模拟五官保护）
void buildMask(std::vector<unsigned char>& mask, std::vector<unsigned char>& prot,
               int w, int h, beautycore::RoiRect* bbox) {
    mask.assign((size_t)w * h, 0);
    prot.assign((size_t)w * h, 0);

    const float cx = w * 0.5f, cy = h * 0.45f;
    const float rx = w * 0.17f, ry = h * 0.20f;
    int x0 = w, y0 = h, x1 = 0, y1 = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float dx = ((float)x - cx) / rx;
            const float dy = ((float)y - cy) / ry;
            const float d2 = dx * dx + dy * dy;
            if (d2 >= 1.0f) continue;
            // 软边：靠近边缘线性衰减到 0
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

    // 五官保护：两个小圆，位置在掩码内部偏上
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

    bbox->x0 = x0; bbox->y0 = y0; bbox->x1 = x1 + 1; bbox->y1 = y1 + 1;
}

// 复刻 beauty_smooth.cpp 的 bandBoxW（那边是 static，台架取不到）。
// 只用来在日志里说明「裁剪之后半径变成了多少」，不参与任何计算。
static int boxRof(int ww, float frac, int minW) {
    int k = (int)(frac * (float)ww + 0.5f);
    if (k < minW) k = minW;
    if ((k & 1) == 0) k++;
    return (k - 1) / 2;
}

struct DiffStat {
    long long n = 0;
    int maxd = 0;
    int fx = -1, fy = -1, fc = -1, fa = 0, fb = 0;
    int minx = 0, maxx = 0, miny = 0, maxy = 0;
    int grid[8][8] = {};          // 粗略分布：把画面切成 8×8 看差异落在哪
    int gw = 0, gh = 0;
};

DiffStat compare(const std::vector<unsigned char>& a, const std::vector<unsigned char>& b,
                 int w, int h) {
    DiffStat st;
    st.minx = w; st.miny = h;
    st.gw = w; st.gh = h;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t o = ((size_t)y * w + x) * 4;
            for (int c = 0; c < 4; ++c) {
                const int d = (int)a[o + c] - (int)b[o + c];
                const int ad = d < 0 ? -d : d;
                if (ad) {
                    ++st.n;
                    if (ad > st.maxd) st.maxd = ad;
                    if (x < st.minx) st.minx = x;
                    if (x > st.maxx) st.maxx = x;
                    if (y < st.miny) st.miny = y;
                    if (y > st.maxy) st.maxy = y;
                    st.grid[(y * 8) / (h > 1 ? h : 1)][(x * 8) / (w > 1 ? w : 1)] += 1;
                    if (st.fx < 0) {
                        st.fx = x; st.fy = y; st.fc = c;
                        st.fa = a[o + c]; st.fb = b[o + c];
                    }
                }
            }
        }
    }
    return st;
}

void printGrid(const DiffStat& st) {
    if (!st.n) return;
    std::printf("\n      差异分布（8x8 网格，画面范围 x[%d..%d] y[%d..%d]）\n",
                st.minx, st.maxx, st.miny, st.maxy);
    for (int r = 0; r < 8; ++r) {
        std::printf("      ");
        for (int c = 0; c < 8; ++c) std::printf("%7d", st.grid[r][c]);
        std::printf("\n");
    }
}

bool runCase(int w, int h, int threads) {
    std::vector<unsigned char> src, mask, prot;
    fillImage(src, w, h);
    beautycore::RoiRect bbox;
    buildMask(mask, prot, w, h, &bbox);

    beautycore::BandParams p;   // 全默认：f1=5/960 f2=9/960 fa=5/960 passes=3 gwp=2 halfRes=1

    // 三段隔离：先证明管道通（I=0 时输出 == 输入），再单独验 B1/B2，
    // 最后跑全量。哪一段先崩，问题就在哪一段。
    struct Mode { const char* name; float I, e, T; };
    const Mode modes[] = {
        { "A 管道（I=0，输出应等于源）", 0.0f, 0.3f, 6.5f },
        { "B 双尺度（e=0, T=1e6 关闭门控）", 1.0f, 0.0f, 1.0e6f },
        { "C 全量（生产默认）", 1.0f, 0.3f, 6.5f },
    };

    bool allOk = true;
    for (const auto& md : modes) {
        beautycore::BandParams pm = p;
        pm.I = md.I; pm.e = md.e; pm.T = md.T;

        std::vector<unsigned char> ref = src;
        std::vector<float> scratch;
        beautycore::surface3RgbBand(src.data(), ref.data(), w, h,
                                    mask.data(), prot.data(), pm, bbox, threads, scratch);

        // dst 故意填垃圾而不是 src 的副本：gpuSmoothRgbBand 只写输出矩形，
        // 若它没有把矩形外补成 src，这里会立刻暴露出「ROI 外残影」。
        // 生产调用点（beauty_core）同样不预拷 dst，台架必须同构才能测到那条路径。
        std::vector<unsigned char> gpu((size_t)w * h * 4, 0xAB);
        GpuSmoothTiming tm{};
        if (!gpuSmoothRgbBand(src.data(), gpu.data(), w, h,
                              mask.data(), prot.data(), pm, bbox, &tm)) {
            std::printf("  [%s] GPU 拒绝执行：%s\n", md.name, gpuSmoothLastError());
            allOk = false;
            continue;
        }

        const DiffStat st = compare(ref, gpu, w, h);
        std::printf("  %-34s 差异=%lld 最大差=%d  %dx%d@%d,%d 平面%dx%d@%d,%d r=%d/%d/%d P=%d  "
                    "上传%.2f 算%.2f 读回%.2f 合计%.2f ms",
                    md.name, st.n, st.maxd,
                    tm.imgW, tm.imgH, tm.imgX0, tm.imgY0, tm.pw, tm.ph,
                    tm.planeX0, tm.planeY0, tm.r1, tm.r2, tm.ra, tm.passes,
                    tm.uploadMs, tm.computeMs, tm.downloadMs, tm.totalMs);
        if (st.n) {
            std::printf("  首差@(%d,%d)ch=%d ref=%d gpu=%d", st.fx, st.fy, st.fc, st.fa, st.fb);
            if (md.I > 0.5f && md.e < 0.1f) printGrid(st);   // 只在 B 段打分布
            allOk = false;
        }
        std::printf("\n");

        // ---- 裁剪对拍：把 GPU 实际处理的矩形裁出来交给 CPU 再跑一遍 ----
        // 【只作参考，不计入判定】因为 bandBoxW 的半径是「画面宽度 × 比例」算的，
        // 图裁小之后 ww 变小，算出的 r 可能与全帧不同 —— 那时裁剪版 CPU 跑的
        // 就是另一组参数，跟 GPU 比没有意义。实测：半径恰好没变的两个画幅
        // （1080p 竖 1/2、640×394 1/2）本项为 0，半径变了的两个
        // （1080p 横 2/4→1/2、4K 5/9→2/4）必不为 0，与算法正确性无关。
        // 真正的判据只有上面那条「整帧 CPU vs GPU」。
        // 之所以成立：裁剪后 CPU 的平面尺寸恰是 pw×ph，起点也是 planeX0，
        // 与 GPU 的 ROI 完全同构，clamp 边界也一致。
        {
            const int cw = tm.imgW, ch = tm.imgH;
            std::vector<unsigned char> cs((size_t)cw * ch * 4), cm((size_t)cw * ch), cp((size_t)cw * ch);
            for (int y = 0; y < ch; ++y) {
                std::memcpy(&cs[(size_t)y * cw * 4],
                            &src[((size_t)(tm.imgY0 + y) * w + tm.imgX0) * 4], (size_t)cw * 4);
                std::memcpy(&cm[(size_t)y * cw], &mask[(size_t)(tm.imgY0 + y) * w + tm.imgX0], (size_t)cw);
                std::memcpy(&cp[(size_t)y * cw], &prot[(size_t)(tm.imgY0 + y) * w + tm.imgX0], (size_t)cw);
            }
            beautycore::RoiRect cb;
            cb.x0 = bbox.x0 - tm.imgX0; cb.y0 = bbox.y0 - tm.imgY0;
            cb.x1 = bbox.x1 - tm.imgX0; cb.y1 = bbox.y1 - tm.imgY0;

            std::vector<unsigned char> cref = cs;
            std::vector<float> sc2;
            beautycore::surface3RgbBand(cs.data(), cref.data(), cw, ch,
                                        cm.data(), cp.data(), pm, cb, threads, sc2);

            // 只在 ROI 内比：GPU 只写 ROI，ROI 外的裁剪图上 CPU 改了而 GPU 没改，
            // 那是「本来就不该比」的区域。早期把这段算进差异，导致修好之后
            // 仍打出几十万的假差异（首差恰好落在 ROI 边界外第一行）。
            long long nd = 0, ndOut = 0; int md2 = 0;
            int fx = -1, fy = -1;
            for (int y = 0; y < ch; ++y) {
                for (int x = 0; x < cw; ++x) {
                    const bool inRoi = (x >= cb.x0 && x < cb.x1 && y >= cb.y0 && y < cb.y1);
                    const size_t o = ((size_t)y * cw + x) * 4;
                    const size_t go = ((size_t)(tm.imgY0 + y) * w + (tm.imgX0 + x)) * 4;
                    for (int c = 0; c < 4; ++c) {
                        const int d = (int)cref[o + c] - (int)gpu[go + c];
                        const int ad = d < 0 ? -d : d;
                        if (ad) {
                            if (!inRoi) { ++ndOut; continue; }
                            ++nd;
                            if (ad > md2) md2 = ad;
                            if (fx < 0) { fx = x; fy = y; }
                        }
                    }
                }
            }
            std::printf("  %-34s 差异=%lld 最大差=%d  ROI外(不计)=%lld",
                        "↳ 裁剪对拍（ROI内）", nd, md2, ndOut);
            if (nd) std::printf("  首差@局部(%d,%d)", fx, fy);
            std::printf("\n");

            // 一刀切开「裁剪版 CPU 与 GPU 不同」到底是哪一方的锅：
            // 拿全帧 CPU 的结果（ref）在 ROI 内裁出来，与裁剪版 CPU（cref）比。
            long long ns = 0; int ms2 = 0;
            for (int y = 0; y < ch; ++y) {
                for (int x = 0; x < cw; ++x) {
                    const bool inRoi = (x >= cb.x0 && x < cb.x1 && y >= cb.y0 && y < cb.y1);
                    if (!inRoi) continue;
                    const size_t o = ((size_t)y * cw + x) * 4;
                    const size_t ro = ((size_t)(tm.imgY0 + y) * w + (tm.imgX0 + x)) * 4;
                    for (int c = 0; c < 4; ++c) {
                        const int d = (int)cref[o + c] - (int)ref[ro + c];
                        const int ad = d < 0 ? -d : d;
                        if (ad) { ++ns; if (ad > ms2) ms2 = ad; }
                    }
                }
            }
            // ---- 本项为何不能当判据 ----
            // bandBoxW 的半径是「画面宽度 × 比例」算出来的：
            //     k = (int)(frac * ww + 0.5f)，再抬到 minW、再抬成奇数
            // 所以把图裁小之后 ww 跟着变小，算出的半径可能与全帧不同 ——
            // 裁剪版 CPU 跑的就是另一组 r，拿它跟 GPU 比毫无意义。
            // 实测四个画幅只有「半径恰好没变」的那两个（1080p 竖、640×394）
            // 裁剪对拍为 0，另外两个（1080p 横 2/4→1/2、4K 5/9→2/4）必不为 0。
            // 结论：本项仅作参考，不计入判定；真正的判据是上面的「整帧 CPU vs GPU」。
            const int r1c = boxRof((cw + 1) / 2, pm.f1, 3);
            const int r2c_ = boxRof((cw + 1) / 2, pm.f2, 5);
            const int rac = boxRof((cw + 1) / 2, pm.fa, 3);
            std::printf("  %-34s 差异=%lld 最大差=%d   (裁剪 r=%d/%d/%d 全帧 r=%d/%d/%d)\n",
                        "↳ 裁剪CPU vs 全帧CPU（ROI内）", ns, ms2, r1c, r2c_, rac,
                        tm.r1, tm.r2, tm.ra);
        }
    }
    return allOk;
}

// ---------------------------------------------------------------- 平面级对拍
// 把「不一致」切在 box 段和 compose 段之间，这是唯一能一刀切开的地方：
//   · 平面全 0 差异、输出不一致 → compose（双线性 / gw 除法 / 舍入）
//   · 平面就有差异             → box / 下采样（累加顺序 / 相位 / halo）
// 只看 ROI 内的重叠区：CPU 算全帧，GPU 只算 ROI，区域外本就该不同。
void diagnosePlanes(int w, int h, int threads) {
    std::vector<unsigned char> src, mask, prot;
    fillImage(src, w, h);
    beautycore::RoiRect bbox;
    buildMask(mask, prot, w, h, &bbox);

    beautycore::BandParams pm;
    pm.I = 1.0f; pm.e = 0.3f; pm.T = 6.5f;

    std::vector<unsigned char> ref = src;
    std::vector<float> sc;
    beautycore::surface3RgbBand(src.data(), ref.data(), w, h,
                                mask.data(), prot.data(), pm, bbox, threads, sc);

    std::vector<unsigned char> gpu = src;
    GpuSmoothTiming tm{};
    if (!gpuSmoothRgbBand(src.data(), gpu.data(), w, h,
                          mask.data(), prot.data(), pm, bbox, &tm)) {
        std::printf("  诊断：GPU 拒绝执行\n");
        return;
    }

    const int cw = (w + 1) / 2, ch = (h + 1) / 2;
    const size_t npC = (size_t)cw * (size_t)ch;

    struct PL { const char* name; int block; int which; int comp; };
    const PL pl[] = {
        { "C.r ", 0, 0, 0 }, { "C.g ", 1, 0, 1 }, { "C.b ", 2, 0, 2 },
        { "B1.r", 3, 1, 0 }, { "B1.g", 4, 1, 1 }, { "B1.b", 5, 1, 2 },
        { "B2.r", 6, 2, 0 }, { "B2.g", 7, 2, 1 }, { "B2.b", 8, 2, 2 },
        { "AMP ", 9, 3, 0 },
    };

    // ---------------------------------------------------------------
    // 判定谁偏离：两路 CPU 参考都用「GPU 回读的 C 平面」当输入。
    // C 平面已证逐位一致，所以唯一变量就是 box 的算术序列本身。
    //   参考 G = 照抄 HLSL（先输出后更新，add=y+r+1 / rem=y-r）
    //   参考 P = 照抄 CPU bandBoxV（先更新后输出，add=y+r / rem=y-1-r）
    // 谁跟 GPU 对得上，谁就是「能复现的那一侧」。
    // ---------------------------------------------------------------
    {
        std::vector<float> gC((size_t)tm.pw * tm.ph * 4, 0.0f), gB1((size_t)tm.pw * tm.ph * 4, 0.0f);
        int w1_ = 0, h1_ = 0, w2_ = 0, h2_ = 0;
        if (gpuSmoothDebugPlane(0, gC.data(), (int)gC.size(), &w1_, &h1_) &&
            gpuSmoothDebugPlane(1, gB1.data(), (int)gB1.size(), &w2_, &h2_)) {

            const int pw = tm.pw, ph = tm.ph, r = tm.r1, passes = tm.passes;
            const float inv = 1.0f / (float)(2 * r + 1);
            auto cl = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };

            std::vector<float> src((size_t)pw * ph);
            for (int y = 0; y < ph; ++y)
                for (int x = 0; x < pw; ++x)
                    src[(size_t)y * pw + x] = gC[((size_t)y * pw + x) * 4 + 0];

            std::vector<float> aG(src), bG(src), aP(src), bP(src);

            // 参考 G：先输出后更新
            for (int p = 0; p < passes; ++p) {
                for (int y = 0; y < ph; ++y) {
                    float s = 0.0f;
                    for (int x = 0; x < pw; ++x) {
                        if ((x & 31) == 0) {
                            s = 0.0f;
                            for (int k = -r; k <= r; ++k)
                                s += aG[(size_t)y * pw + cl(x + k, 0, pw - 1)];
                        }
                        bG[(size_t)y * pw + x] = s * inv;
                        s += aG[(size_t)y * pw + cl(x + r + 1, 0, pw - 1)]
                           - aG[(size_t)y * pw + cl(x - r, 0, pw - 1)];
                    }
                }
                for (int x = 0; x < pw; ++x) {
                    float s = 0.0f;
                    for (int y = 0; y < ph; ++y) {
                        if ((y & 31) == 0) {
                            s = 0.0f;
                            for (int k = -r; k <= r; ++k)
                                s += bG[(size_t)cl(y + k, 0, ph - 1) * pw + x];
                        }
                        aG[(size_t)y * pw + x] = s * inv;
                        s += bG[(size_t)cl(y + r + 1, 0, ph - 1) * pw + x]
                           - bG[(size_t)cl(y - r, 0, ph - 1) * pw + x];
                    }
                }
            }

            // 参考 P：先更新后输出（CPU bandBoxV 的写法）
            for (int p = 0; p < passes; ++p) {
                for (int y = 0; y < ph; ++y) {
                    float s = 0.0f;
                    for (int x = 0; x < pw; ++x) {
                        if ((x % 32) == 0) {
                            s = 0.0f;
                            for (int k = -r; k <= r; ++k)
                                s += aP[(size_t)y * pw + cl(x + k, 0, pw - 1)];
                        }
                        bP[(size_t)y * pw + x] = s * inv;
                        s += aP[(size_t)y * pw + cl(x + r + 1, 0, pw - 1)]
                           - aP[(size_t)y * pw + cl(x - r, 0, pw - 1)];
                    }
                }
                for (int x = 0; x < pw; ++x) {
                    float s = 0.0f;
                    for (int y = 0; y < ph; ++y) {
                        if ((y % 32) == 0) {
                            s = 0.0f;
                            for (int k = -r; k <= r; ++k)
                                s += bP[(size_t)cl(y + k, 0, ph - 1) * pw + x];
                        } else {
                            s += bP[(size_t)cl(y + r, 0, ph - 1) * pw + x]
                               - bP[(size_t)cl(y - 1 - r, 0, ph - 1) * pw + x];
                        }
                        aP[(size_t)y * pw + x] = s * inv;
                    }
                }
            }

            // 三个对象两两比：GPU 侧 B1.r / 参考G / 参考P / CPU 全帧 B1.r
            const float* cpuB1 = sc.data() + (size_t)3 * npC;
            const int rMax2 = std::max(std::max(tm.r1, tm.r2), tm.ra);
            const int hh = tm.passes * rMax2 + 2;

            auto cmpPlane = [&](const char* label, const std::vector<float>& gpuPlane,
                                const std::vector<float>& refPlane) {
                long long nd = 0, nc = 0; int mu = 0;
                for (int ly = hh; ly < ph - hh; ++ly)
                    for (int lx = hh; lx < pw - hh; ++lx) {
                        const float x1 = gpuPlane[(size_t)ly * pw + lx];
                        const float x2 = refPlane[(size_t)ly * pw + lx];
                        ++nc;
                        if (std::memcmp(&x1, &x2, 4) == 0) continue;
                        ++nd;
                        unsigned int u1, u2;
                        std::memcpy(&u1, &x1, 4); std::memcpy(&u2, &x2, 4);
                        const int d = (u1 > u2) ? (int)(u1 - u2) : (int)(u2 - u1);
                        if (d > mu) mu = d;
                    }
                std::printf("    [CHK] %-22s bitdiff=%-7lld / %-7lld maxULP=%d\n", label, nd, nc, mu);
            };

            std::vector<float> gpuB1((size_t)pw * ph);
            for (int y = 0; y < ph; ++y)
                for (int x = 0; x < pw; ++x)
                    gpuB1[(size_t)y * pw + x] = gB1[((size_t)y * pw + x) * 4 + 0];

            cmpPlane("gpu vs refG", gpuB1, aG);
            cmpPlane("gpu vs refP", gpuB1, aP);
            cmpPlane("refG vs refP", aG, aP);

            // ---- 关键隔离：把 CPU 的 box 函数喂「从全帧裁出的 ROI 局部」 ----
            // 输入与 GPU 完全同构、坐标也同构，跑的就是生产那份 bandBoxN。
            //   · 结果 == aP  → 算法与输入都一致，差异只可能来自坐标原点/尺寸
            //   · 结果 != aP  → 输入或坐标就有问题（尽管 C 平面比过 0 差异）
            {
                const int pw2 = tm.pw, ph2 = tm.ph;
                std::vector<float> crop((size_t)pw2 * ph2), ctmp((size_t)pw2 * ph2, 0.0f), gcv((size_t)pw2 * ph2);
                for (int ly = 0; ly < ph2; ++ly) {
                    const int hy = tm.planeY0 + ly;
                    for (int lx = 0; lx < pw2; ++lx) {
                        const int hx = tm.planeX0 + lx;
                        const bool in = (hy >= 0 && hy < ch && hx >= 0 && hx < cw);
                        crop[(size_t)ly * pw2 + lx] = in ? sc[(size_t)hy * cw + hx] : 0.0f;
                        gcv [(size_t)ly * pw2 + lx] = gC[((size_t)ly * pw2 + lx) * 4 + 0];
                    }
                }
                // 输入先比一次
                {
                    long long nd = 0; int mu = 0;
                    for (int i = 0; i < pw2 * ph2; ++i) {
                        if (std::memcmp(&crop[i], &gcv[i], 4) == 0) continue;
                        ++nd;
                        unsigned int u1, u2;
                        std::memcpy(&u1, &crop[i], 4); std::memcpy(&u2, &gcv[i], 4);
                        const int d = (u1 > u2) ? (int)(u1 - u2) : (int)(u2 - u1);
                        if (d > mu) mu = d;
                    }
                    std::printf("    [CHK] %-22s bitdiff=%-7lld / %-7lld maxULP=%d\n",
                                "crop(C) vs gpuC", nd, (long long)pw2 * ph2, mu);
                }
                // ---- passes=1 切分：单趟都不一致 → 单趟算法差异；单趟一致 → 多趟结构差异 ----
                {
                    const int r_ = tm.r1;
                    const float inv_ = 1.0f / (float)(2 * r_ + 1);
                    std::vector<float> ra(gcv), rb(gcv.size());
                    // refP 单趟：H: ra->rb, V: rb->ra
                    for (int y = 0; y < ph2; ++y) {
                        float sx = 0.0f;
                        for (int x = 0; x < pw2; ++x) {
                            if ((x % 32) == 0) {
                                sx = 0.0f;
                                for (int k = -r_; k <= r_; ++k)
                                    sx += ra[(size_t)y * pw2 + cl(x + k, 0, pw2 - 1)];
                            }
                            rb[(size_t)y * pw2 + x] = sx * inv_;
                            sx += ra[(size_t)y * pw2 + cl(x + r_ + 1, 0, pw2 - 1)]
                                - ra[(size_t)y * pw2 + cl(x - r_, 0, pw2 - 1)];
                        }
                    }
                    for (int x = 0; x < pw2; ++x) {
                        float sy = 0.0f;
                        for (int y = 0; y < ph2; ++y) {
                            if ((y % 32) == 0) {
                                sy = 0.0f;
                                for (int k = -r_; k <= r_; ++k)
                                    sy += rb[(size_t)cl(y + k, 0, ph2 - 1) * pw2 + x];
                            } else {
                                sy += rb[(size_t)cl(y + r_, 0, ph2 - 1) * pw2 + x]
                                    - rb[(size_t)cl(y - 1 - r_, 0, ph2 - 1) * pw2 + x];
                            }
                            ra[(size_t)y * pw2 + x] = sy * inv_;
                        }
                    }

                    std::vector<float> ba(gcv), bt(gcv.size(), 0.0f);
                    beautycore::beautyDebugBox1(ba.data(), bt.data(), pw2, ph2, r_, 1, 1);

                    long long nd = 0, nc = 0; int mu = 0;
                    for (int ly = hh; ly < ph2 - hh; ++ly)
                        for (int lx = hh; lx < pw2 - hh; ++lx) {
                            const float x1 = ba[(size_t)ly * pw2 + lx];
                            const float x2 = ra[(size_t)ly * pw2 + lx];
                            ++nc;
                            if (std::memcmp(&x1, &x2, 4) == 0) continue;
                            ++nd;
                            unsigned int u1, u2;
                            std::memcpy(&u1, &x1, 4); std::memcpy(&u2, &x2, 4);
                            const int d = (u1 > u2) ? (int)(u1 - u2) : (int)(u2 - u1);
                            if (d > mu) mu = d;
                        }
                    std::printf("    [CHK] %-22s bitdiff=%-7lld / %-7lld maxULP=%d\n",
                                "passes=1 refP vs cpuBox", nd, nc, mu);

                    // 逐趟扫描：找出从第几趟开始分叉
                    for (int pc = 2; pc <= tm.passes; ++pc) {
                        std::vector<float> xa(gcv), xb(gcv.size());
                        for (int p = 0; p < pc; ++p) {
                            for (int y = 0; y < ph2; ++y) {
                                float sx = 0.0f;
                                for (int x = 0; x < pw2; ++x) {
                                    if ((x % 32) == 0) {
                                        sx = 0.0f;
                                        for (int k = -r_; k <= r_; ++k)
                                            sx += xa[(size_t)y * pw2 + cl(x + k, 0, pw2 - 1)];
                                    }
                                    xb[(size_t)y * pw2 + x] = sx * inv_;
                                    sx += xa[(size_t)y * pw2 + cl(x + r_ + 1, 0, pw2 - 1)]
                                        - xa[(size_t)y * pw2 + cl(x - r_, 0, pw2 - 1)];
                                }
                            }
                            for (int x = 0; x < pw2; ++x) {
                                float sy = 0.0f;
                                for (int y = 0; y < ph2; ++y) {
                                    if ((y % 32) == 0) {
                                        sy = 0.0f;
                                        for (int k = -r_; k <= r_; ++k)
                                            sy += xb[(size_t)cl(y + k, 0, ph2 - 1) * pw2 + x];
                                    } else {
                                        sy += xb[(size_t)cl(y + r_, 0, ph2 - 1) * pw2 + x]
                                            - xb[(size_t)cl(y - 1 - r_, 0, ph2 - 1) * pw2 + x];
                                    }
                                    xa[(size_t)y * pw2 + x] = sy * inv_;
                                }
                            }
                        }
                        std::vector<float> ya(gcv), yb(gcv.size(), 0.0f);
                        beautycore::beautyDebugBox1(ya.data(), yb.data(), pw2, ph2, r_, pc, 1);
                        long long nd3 = 0, nc3 = 0; int mu3 = 0;
                        for (int ly = hh; ly < ph2 - hh; ++ly)
                            for (int lx = hh; lx < pw2 - hh; ++lx) {
                                const float x1 = ya[(size_t)ly * pw2 + lx];
                                const float x2 = xa[(size_t)ly * pw2 + lx];
                                ++nc3;
                                if (std::memcmp(&x1, &x2, 4) == 0) continue;
                                ++nd3;
                                unsigned int u1, u2;
                                std::memcpy(&u1, &x1, 4); std::memcpy(&u2, &x2, 4);
                                const int d = (u1 > u2) ? (int)(u1 - u2) : (int)(u2 - u1);
                                if (d > mu3) mu3 = d;
                            }
                        char lbl[64];
                        std::snprintf(lbl, sizeof(lbl), "passes=%d refP vs cpuBox", pc);
                        std::printf("    [CHK] %-22s bitdiff=%-7lld / %-7lld maxULP=%d\n", lbl, nd3, nc3, mu3);
                    }
                }

                {
                    const size_t i0 = (size_t)(ph2 / 2) * pw2 + pw2 / 2;
                    std::printf("    [DBG] pre  call: crop=%.7g gcv=%.7g refP=%.7g\n",
                                (double)crop[i0], (double)gcv[i0], (double)aP[i0]);
                }
                beautycore::beautyDebugBox1(crop.data(), ctmp.data(), pw2, ph2, tm.r1, tm.passes, 1);
                {
                    const size_t i0 = (size_t)(ph2 / 2) * pw2 + pw2 / 2;
                    std::printf("    [DBG] post call: crop=%.7g ctmp=%.7g gcv=%.7g\n",
                                (double)crop[i0], (double)ctmp[i0], (double)gcv[i0]);
                }

                long long nd = 0, nc = 0; int mu = 0;
                for (int ly = hh; ly < ph2 - hh; ++ly)
                    for (int lx = hh; lx < pw2 - hh; ++lx) {
                        // acc 才是最终结果：bandBoxN 是 H:acc→tmp, V:tmp→acc，
                        // tmp 里只剩最后一趟 H 的中间态。
                        const float x1 = crop[(size_t)ly * pw2 + lx];
                        const float x2 = aP[(size_t)ly * pw2 + lx];
                        ++nc;
                        if (std::memcmp(&x1, &x2, 4) == 0) continue;
                        ++nd;
                        unsigned int u1, u2;
                        std::memcpy(&u1, &x1, 4); std::memcpy(&u2, &x2, 4);
                        const int d = (u1 > u2) ? (int)(u1 - u2) : (int)(u2 - u1);
                        if (d > mu) mu = d;
                    }
                std::printf("    [CHK] %-22s bitdiff=%-7lld / %-7lld maxULP=%d\n",
                            "cpuBox(crop) vs refP", nd, nc, mu);

                // 取几个内缩区内的点，把「原始输入 / refP / cpuBox / gpu」并排打出来
                const int vy = ph2 / 2, vx = pw2 / 2;
                for (int d = 0; d < 3; ++d) {
                    const int yy = vy + d * 7, xx = vx + d * 11;
                    const size_t i = (size_t)yy * pw2 + xx;
                    std::printf("    [VAL] (%d,%d) in=%.7g refP=%.7g cpuBox=%.7g gpu=%.7g\n",
                                xx, yy, (double)src[i], (double)aP[i],
                                (double)crop[i], (double)gpuB1[i]);
                }
            }

            // 与 CPU 生产实现的 B1.r 比（坐标要换算到全帧）
            {
                long long nd = 0, nc = 0; int mu = 0;
                for (int ly = hh; ly < ph - hh; ++ly) {
                    const int hy = tm.planeY0 + ly;
                    if (hy < 0 || hy >= ch) continue;
                    for (int lx = hh; lx < pw - hh; ++lx) {
                        const int hx = tm.planeX0 + lx;
                        if (hx < 0 || hx >= cw) continue;
                        const float x1 = gpuB1[(size_t)ly * pw + lx];
                        const float x2 = cpuB1[(size_t)hy * cw + hx];
                        ++nc;
                        if (std::memcmp(&x1, &x2, 4) == 0) continue;
                        ++nd;
                        unsigned int u1, u2;
                        std::memcpy(&u1, &x1, 4); std::memcpy(&u2, &x2, 4);
                        const int d = (u1 > u2) ? (int)(u1 - u2) : (int)(u2 - u1);
                        if (d > mu) mu = d;
                    }
                }
                std::printf("    [CHK] %-22s bitdiff=%-7lld / %-7lld maxULP=%d\n",
                            "gpu vs cpuProd", nd, nc, mu);
            }

            // 内缩扫描：差异若随内缩增大而消失 → 边界/halo 问题；
            // 若始终存在且比例不变 → 算法或舍入问题。
            {
                std::printf("    [SCAN] inset:");
                for (int inset : { 32, 44, 64, 96, 128, 160 }) {
                    long long nd = 0, nc = 0;
                    for (int ly = inset; ly < ph - inset; ++ly) {
                        const int hy = tm.planeY0 + ly;
                        if (hy < 0 || hy >= ch) continue;
                        for (int lx = inset; lx < pw - inset; ++lx) {
                            const int hx = tm.planeX0 + lx;
                            if (hx < 0 || hx >= cw) continue;
                            ++nc;
                            const float x1 = gpuB1[(size_t)ly * pw + lx];
                            const float x2 = cpuB1[(size_t)hy * cw + hx];
                            if (std::memcmp(&x1, &x2, 4) != 0) ++nd;
                        }
                    }
                    std::printf(" %d:%lld/%lld", inset, nd, nc);
                }
                std::printf("\n");
            }
        }
    }

    // 直调 CPU 自己的 box：输入用 CPU 自己的 C 平面（全帧），输出与生产的 b1r 比。
    // 全帧对全帧，坐标完全同构，没有任何映射假设。
    {
        const int bt = ((size_t)cw * ch >= 150000) ? threads : 1;
        std::vector<float> acc(sc.begin(), sc.begin() + (std::ptrdiff_t)npC);
        std::vector<float> tmp((size_t)cw * ch, 0.0f);
        beautycore::beautyDebugBox1(acc.data(), tmp.data(), cw, ch, tm.r1, tm.passes, bt);

        const float* cpuB1 = sc.data() + (size_t)3 * npC;
        const int hh0 = tm.passes * tm.r1 + 2;
        long long nd = 0, nc = 0; int mu = 0;
        for (int y = hh0; y < ch - hh0; ++y)
            for (int x = hh0; x < cw - hh0; ++x) {
                const float x1 = acc[(size_t)y * cw + x];
                const float x2 = cpuB1[(size_t)y * cw + x];
                ++nc;
                if (std::memcmp(&x1, &x2, 4) == 0) continue;
                ++nd;
                unsigned int u1, u2;
                std::memcpy(&u1, &x1, 4); std::memcpy(&u2, &x2, 4);
                const int d = (u1 > u2) ? (int)(u1 - u2) : (int)(u2 - u1);
                if (d > mu) mu = d;
            }
        std::printf("    [CHK] %-22s bitdiff=%-7lld / %-7lld maxULP=%d  (threads=%d)\n",
                    "cpuBox(direct) vs prod", nd, nc, mu, bt);

        // 同一份输入，只改线程数。若两者不同 → CPU 的并行 box 有竞态，
        // 生产结果的「真值」本身就不确定，GPU 再怎么复刻也不可能稳定对上。
        std::vector<float> a1(sc.begin(), sc.begin() + (std::ptrdiff_t)npC);
        std::vector<float> t1((size_t)cw * ch, 0.0f);
        std::vector<float> a2 = a1;
        std::vector<float> t2((size_t)cw * ch, 0.0f);
        beautycore::beautyDebugBox1(a1.data(), t1.data(), cw, ch, tm.r1, tm.passes, 1);
        beautycore::beautyDebugBox1(a2.data(), t2.data(), cw, ch, tm.r1, tm.passes, 12);
        {
            long long nd2 = 0; int mu2 = 0;
            for (size_t i = 0; i < a1.size(); ++i) {
                if (std::memcmp(&a1[i], &a2[i], 4) == 0) continue;
                ++nd2;
                unsigned int u1, u2;
                std::memcpy(&u1, &a1[i], 4); std::memcpy(&u2, &a2[i], 4);
                const int d = (u1 > u2) ? (int)(u1 - u2) : (int)(u2 - u1);
                if (d > mu2) mu2 = d;
            }
            std::printf("    [CHK] %-22s bitdiff=%-7lld / %-7lld maxULP=%d\n",
                        "cpuBox t=1 vs t=12", nd2, (long long)a1.size(), mu2);
        }
    }

    std::printf("[PLANE] cpu_full=%dx%d  gpu=%dx%d @%d,%d\n",
                cw, ch, tm.pw, tm.ph, tm.planeX0, tm.planeY0);
    for (const auto& q : pl) {
        std::vector<float> gp((size_t)tm.pw * tm.ph * 4, 0.0f);
        int gpw = 0, gph = 0;
        if (!gpuSmoothDebugPlane(q.which, gp.data(), (int)gp.size(), &gpw, &gph)) {
            std::printf("    %s readback-failed\n", q.name);
            continue;
        }
        const float* cp = sc.data() + (size_t)q.block * npC;

        long long nDiff = 0, nCmp = 0;
        int maxUlp = 0;
        float maxAbs = 0.0f;
        int fx = -1, fy = -1; float fa = 0, fb = 0;
        int byx[32] = {}, byy[32] = {};
        // 必须内缩 halo 再比：GPU 的外扩区是「从 ROI 边界 clamp 出来的」，
        // CPU 全帧在同样位置是真实邻域，两者本来就不同 —— 比了只会得到一堆假差异。
        // 需求外扩 = passes × 最大半径（盒式扩散）+ 2（双线性采样 + 保险）。
        const int rMax = std::max(std::max(tm.r1, tm.r2), tm.ra);
        const int halo = tm.passes * rMax + 2;
        for (int ly = halo; ly < gph - halo; ++ly) {
            const int hy = tm.planeY0 + ly;
            if (hy < 0 || hy >= ch) continue;
            for (int lx = halo; lx < gpw - halo; ++lx) {
                const int hx = tm.planeX0 + lx;
                if (hx < 0 || hx >= cw) continue;
                const float a = cp[(size_t)hy * cw + hx];
                const float b = gp[((size_t)ly * gpw + lx) * 4 + q.comp];
                ++nCmp;
                if (std::memcmp(&a, &b, 4) == 0) continue;
                ++nDiff;
                unsigned int ua, ub;
                std::memcpy(&ua, &a, 4); std::memcpy(&ub, &b, 4);
                const int ulp = (ua > ub) ? (int)(ua - ub) : (int)(ub - ua);
                if (ulp > maxUlp) maxUlp = ulp;
                const float ad = std::fabs(a - b);
                if (ad > maxAbs) maxAbs = ad;
                byx[hx & 31]++; byy[hy & 31]++;
                if (fx < 0) { fx = hx; fy = hy; fa = a; fb = b; }
            }
        }
        std::printf("    %s bitdiff=%-8lld / %-9lld maxULP=%-4d maxAbs=%.6g",
                    q.name, nDiff, nCmp, maxUlp, (double)maxAbs);
        if (nDiff) std::printf("  first@(%d,%d) cpu=%.9g gpu=%.9g", fx, fy, (double)fa, (double)fb);
        std::printf("\n");
        if (nDiff) {
            // flush 相位诊断：CPU 每 32 像素/行重算窗口，若差异在该相位为 0、
            // 块内递增，就直接证明「滑窗累加顺序」是元凶。
            std::printf("        by x%%32:");
            for (int i = 0; i < 32; ++i) std::printf(" %d", byx[i]);
            std::printf("\n        by y%%32:");
            for (int i = 0; i < 32; ++i) std::printf(" %d", byy[i]);
            std::printf("\n");
        }
    }
}

}  // namespace

// ---------------------------------------------------------------- 生命周期回归
// mock_host 抓出来的 bug（2026-11-06）：gpuSmoothRgbBand 里
//     if (!g_ready && !createDevice()) return false;
// 忘了给 g_ready 赋值，于是**每次调用都重建一个设备**，纹理却留在旧设备上，
// 第二次 Map 就返回 DXGI_ERROR_DEVICE_REMOVED（0x887A0005），
// 表现是「首帧好用、之后每帧静默回退 CPU」，日志里只有一句 Map 失败。
//
// 台架原来每个画幅只调一次、且画幅各不相同（尺寸一变就重建纹理，把问题盖住了），
// 所以这条必须单独测：
//   ① 同一尺寸连续调 3 次（不 shutdown）—— 抓「重复建设备」
//   ② shutdown 之后再调 2 次              —— 抓「重建路径漏资源」
// 两种都必须逐字节一致。
bool lifecycleCheck(int threads) {
    const int w = 640, h = 394;
    std::vector<unsigned char> src, mask, prot;
    fillImage(src, w, h);
    beautycore::RoiRect bbox;
    buildMask(mask, prot, w, h, &bbox);

    beautycore::BandParams pm;   // 全默认：f1=5/960 f2=9/960 fa=5/960 passes=3
    std::vector<unsigned char> ref = src;
    std::vector<float> sc;
    beautycore::surface3RgbBand(src.data(), ref.data(), w, h,
                                mask.data(), prot.data(), pm, bbox, threads, sc);

    bool ok = true;
    for (int phase = 0; phase < 2; ++phase) {
        if (phase == 1) gpuSmoothShutdown();
        const int reps = (phase == 0) ? 3 : 2;
        for (int i = 0; i < reps; ++i) {
            std::vector<unsigned char> gpu((size_t)w * h * 4, 0xAB);
            GpuSmoothTiming tm{};
            if (!gpuSmoothRgbBand(src.data(), gpu.data(), w, h,
                                  mask.data(), prot.data(), pm, bbox, &tm)) {
                std::printf("  [LIFE] %-12s 第 %d 次：拒绝执行 %s\n",
                            phase ? "shutdown 后" : "同尺寸连续", i + 1, gpuSmoothLastError());
                ok = false;
                continue;
            }
            const DiffStat st = compare(ref, gpu, w, h);
            std::printf("  [LIFE] %-12s 第 %d 次  差异=%lld 最大差=%d  合计=%.2f ms\n",
                        phase ? "shutdown 后" : "同尺寸连续", i + 1, st.n, st.maxd, tm.totalMs);
            if (st.n) ok = false;
        }
    }
    return ok;
}

// ---------------------------------------------------------------- 1.0.3.0 新增回归
// 台架原来的三个盲区，恰好对应 06:15 真机闪退时「台架通过、真机崩」的差异：
//   ① 每个 case 只调一次、bbox 恒定 → 掩盖 ROI 逐帧抖动引发的资源重建
//   ② 始终单实例                    → 掩盖第二个实例接入后的全局状态切换
//   ③ 只跑几帧                      → 掩盖长时 churn 之后的资源/设备状态
// 这里用「可平移掩码」造出逐帧变化的 bbox，并把重建次数做成硬断言。
//
// 注意：离线台架无法复现真机「连续跑 1138 帧后静置 26 分钟再放第二个实例」
// 的完整时序，只能覆盖「连续帧 churn」与「实例交替」两个维度。

static void buildMaskAt(std::vector<unsigned char>& mask,
                        std::vector<unsigned char>& prot,
                        int w, int h, int dx, int dy,
                        beautycore::RoiRect* bbox) {
    mask.assign((size_t)w * h, 0);
    prot.assign((size_t)w * h, 0);

    const float cx = w * 0.5f + (float)dx, cy = h * 0.45f + (float)dy;
    const float rx = w * 0.17f, ry = h * 0.20f;
    int x0 = w, y0 = h, x1 = 0, y1 = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float ddx = ((float)x - cx) / rx;
            const float ddy = ((float)y - cy) / ry;
            const float d2 = ddx * ddx + ddy * ddy;
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
    bbox->x0 = x0; bbox->y0 = y0; bbox->x1 = x1 + 1; bbox->y1 = y1 + 1;
}

// ROI 逐帧抖动 + 长时连续运行：验证桶化分配确实止住了每帧重建。
// ---------------------------------------------------------------------------
// 1.0.4.0 / D1：compose 段 1 灰阶的「零 GPU 改动」定位。
//
// churn 用例在 400 帧里抓到 2 帧、各 1 个像素、单通道差 1 灰阶。平面全 0
// 差异（C/B1/B2/AMP 各 63040 值逐位相同）说明差异不在 box 段；
// T=1e6 让 gw 恒等于 1 后差异归零，只能说明落在
//   amp 双线性 → araw → ap → 1/(1+ap)
// 这条链上，**不能**区分是 amp 双线性的 1 ULP 还是 SM5 rcp 的近似误差：
// invT=1e-6 会把前者的 ULP 压到 1e-14 量级，同样不影响 gw。
// 这里用 surface3RgbBand 留下的平面快照在 CPU 上逐句复算差异像素，再对每个
// 中间量做 ±1/±2 ULP 扰动，看谁的 1 ULP 足以跨过 +0.5 取整边界。
// 只动台架，不碰生产 shader，也不改 CPU 基线。
//
// 【1.0.4.0 结论】复算值恰好 156.500000000，只有 gw 的 +1 ULP 翻成 156，
// 其余七个量 ±2 ULP 全不翻。据此把 GPU 的 1/(1+ap) 改成「mad 残差 + 两次
// Newton 精化」，400 帧 churn 差异归零、台架 7/7。本函数保留为回归哨兵：
// 将来若再出现 compose 段 1 灰阶，设 P2_TRACE=1 即可重跑这条链。
// ---------------------------------------------------------------------------
static inline float traceBilinear(const float* p, int w, int h, float x, float y) {
    const float fx = x - 0.5f, fy = y - 0.5f;
    int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
    const float tx = fx - (float)x0, ty = fy - (float)y0;
    int x1 = x0 + 1, y1 = y0 + 1;
    const auto ci = [](int v, int hi) { return v < 0 ? 0 : (v > hi ? hi : v); };
    x0 = ci(x0, w - 1); x1 = ci(x1, w - 1);
    y0 = ci(y0, h - 1); y1 = ci(y1, h - 1);
    const float a = p[(size_t)y0 * w + x0], b = p[(size_t)y0 * w + x1];
    const float c = p[(size_t)y1 * w + x0], d = p[(size_t)y1 * w + x1];
    const float ab = a + (b - a) * tx, cd = c + (d - c) * tx;
    return ab + (cd - ab) * ty;
}
static inline float bumpUlp(float v, int n) {
    unsigned int b; std::memcpy(&b, &v, 4);
    b = (unsigned int)((int)b + n);
    float r; std::memcpy(&r, &b, 4); return r;
}
static inline unsigned int bitsOf(float v) {
    unsigned int b; std::memcpy(&b, &v, 4); return b;
}

// sel: -1=不扰动 0=ampB 1=ap 2=gw 3=v1 4=v2 5=skin 6=se 7=keepEye
void composeTrace(int w, int h,
                  const std::vector<unsigned char>& src,
                  const std::vector<unsigned char>& mask,
                  const std::vector<unsigned char>& prot,
                  const beautycore::BandParams& pm,
                  const std::vector<float>& sc,
                  int fx, int fy, int ch, int refVal, int gpuVal) {
    const int cw = (w + 1) / 2, chh = (h + 1) / 2;
    const size_t np = (size_t)cw * chh;
    if (sc.size() < np * 10) { std::printf("  [TRACE] scratch too small\n"); return; }
    const float* amp = sc.data() + 9 * np;
    const float* b1  = sc.data() + (3 + ch) * np;
    const float* b2  = sc.data() + (6 + ch) * np;
    const float inv255 = 1.0f / 255.0f;
    const float I    = (pm.I < 0.0f) ? 0.0f : ((pm.I > 1.0f) ? 1.0f : pm.I);
    const float e    = (pm.e < 0.0f) ? 0.0f : ((pm.e > 1.0f) ? 1.0f : pm.e);
    const float eyeK = (pm.protectK < 0.0f) ? 0.0f
                     : ((pm.protectK > 100.0f) ? 1.0f : pm.protectK / 100.0f);
    const float invT = (pm.T > 1e-3f) ? (1.0f / pm.T) : 0.0f;
    const float sxw = (float)cw / (float)w, syw = (float)chh / (float)h;
    const float vx = ((float)fx + 0.5f) * sxw, vy = ((float)fy + 0.5f) * syw;
    const int mi = (int)mask[(size_t)fy * w + fx];
    const int pi = (int)prot[(size_t)fy * w + fx];
    const float skin = (float)mi * inv255;
    const float eye  = (float)pi * inv255 * eyeK;
    const float keepEye = 1.0f - eye, se = skin * e;
    const float c = (float)src[((size_t)fy * w + fx) * 4 + ch];

    const float ampB = traceBilinear(amp, cw, chh, vx, vy);
    const float araw = ampB * invT;
    const float ap   = araw * araw;
    const float den  = 1.0f + ap;
    const float gw   = 1.0f / den;
    const float v1   = traceBilinear(b1, cw, chh, vx, vy);
    const float v2   = traceBilinear(b2, cw, chh, vx, vy);

    // 逐句照抄 surface3RgbBand 的 compose（含 ① ② ③ 的运算顺序）。
    auto calc = [&](int sel, float nv) -> float {
        float aa = ampB, pp = ap, gg = gw, q1 = v1, q2 = v2;
        float sk = skin, ee = se, ke = keepEye;
        switch (sel) {
            case 0: { aa = nv; const float rr = aa * invT; pp = rr * rr;
                      gg = 1.0f / (1.0f + pp); break; }
            case 1: pp = nv; gg = 1.0f / (1.0f + pp); break;
            case 2: gg = nv; break;
            case 3: q1 = nv; break;
            case 4: q2 = nv; break;
            case 5: sk = nv; break;
            case 6: ee = nv; break;
            case 7: ke = nv; break;
            default: break;
        }
        float even = c - q1 + q2;
        even = c + (even - c) * sk;
        even = even + (q2 - even) * ee;
        even = c + (even - c) * gg;
        const float res = c + (even - c) * I;
        float outv = c + (res - c) * ke;
        if (outv < 0.0f) outv = 0.0f;
        if (outv > 255.0f) outv = 255.0f;
        return outv;
    };

    const float base = calc(-1, 0.0f);
    const int baseRound = (int)(base + 0.5f);
    std::printf("  [TRACE] px=(%d,%d)ch=%d ref=%d gpu=%d  cpuRecalc=%.9f -> %d\n",
                fx, fy, ch, refVal, gpuVal, base, baseRound);
    std::printf("  [TRACE] m=%d prot=%d c=%.1f skin=%.9f(0x%08X) se=%.9f keepEye=%.9f\n",
                mi, pi, c, skin, bitsOf(skin), se, keepEye);
    std::printf("  [TRACE] vx=%.9f vy=%.9f ampB=%.9f(0x%08X) araw=%.9f"
                " ap=%.9f(0x%08X) den=%.9f gw=%.9f(0x%08X)\n",
                vx, vy, ampB, bitsOf(ampB), araw, ap, bitsOf(ap), den,
                gw, bitsOf(gw));
    std::printf("  [TRACE] v1=%.9f v2=%.9f  outv-156.5=%.9f\n",
                v1, v2, base - 156.5f);

    struct N { const char* name; int sel; float v; };
    const N names[] = {
        {"ampB", 0, ampB}, {"ap", 1, ap}, {"gw", 2, gw},
        {"v1", 3, v1}, {"v2", 4, v2}, {"skin", 5, skin},
        {"se", 6, se}, {"keepEye", 7, keepEye},
    };
    for (const auto& n : names) {
        int r[5];
        for (int d = -2; d <= 2; ++d) {
            r[d + 2] = (d == 0) ? baseRound
                       : (int)(calc(n.sel, bumpUlp(n.v, d)) + 0.5f);
        }
        const bool flips = r[0] != baseRound || r[1] != baseRound ||
                           r[3] != baseRound || r[4] != baseRound;
        std::printf("  [TRACE] %-8s -2/-1/+1/+2 ULP -> %d/%d/%d/%d %s\n",
                    n.name, r[0], r[1], r[3], r[4], flips ? "<<< FLIPS" : "");
    }
}

bool churnCheck(int threads) {
    int frames = 400;
    if (const char* e = std::getenv("P2_LONG_FRAMES")) {
        const int v = std::atoi(e);
        if (v > 0) frames = (v > 5000) ? 5000 : v;
    }
    const int w = 640, h = 394;
    std::vector<unsigned char> src, mask, prot;
    fillImage(src, w, h);
    beautycore::BandParams pm;
    // 排查 compose 段 1 灰阶残留时的判别开关：
    //   NO_GATE → T=1e6，gw≈1，隔离 amp 双线性与门控除法
    //   NO_PROT → protect 全 0，隔离 eye/keepEye 一路
    const bool noGate = std::getenv("P2_CHURN_NO_GATE") != nullptr;
    const bool noProt = std::getenv("P2_CHURN_NO_PROT") != nullptr;
    if (noGate) pm.T = 1.0e6f;

    long long rb0 = 0, dc0 = 0;
    int aW = 0, aH = 0, aPw = 0, aPh = 0;
    gpuSmoothResourceStats(&rb0, &dc0, &aW, &aH, &aPw, &aPh);

    long long totalDiff = 0;
    int rejects = 0, reuse = 0, sizeChanges = 0, printed = 0;
    double sumMs = 0.0;
    int minPw = 1 << 30, maxPw = 0, minPh = 1 << 30, maxPh = 0;
    int prevPw = -1, prevPh = -1, prevAllocPw = -1, prevAllocPh = -1;

    std::vector<unsigned char> ref, gpu((size_t)w * h * 4, 0xAB);
    for (int f = 0; f < frames; ++f) {
        const double ph = (double)f * 0.137;
        const int dx = (int)(20.0 * std::sin(ph)) + ((f % 7) - 3);
        const int dy = (int)(11.0 * std::cos(ph * 0.71)) + ((f % 5) - 2);
        beautycore::RoiRect bbox;
        buildMaskAt(mask, prot, w, h, dx, dy, &bbox);
        if (noProt) std::fill(prot.begin(), prot.end(), (unsigned char)0);

        // mask 每帧都在动，CPU 参考必须每帧重算，不能复用第一帧。
        ref = src;
        std::vector<float> sc;
        beautycore::surface3RgbBand(src.data(), ref.data(), w, h,
                                    mask.data(), prot.data(), pm, bbox, threads, sc);

        std::fill(gpu.begin(), gpu.end(), 0xAB);
        GpuSmoothTiming tm{};
        if (!gpuSmoothRgbBand(src.data(), gpu.data(), w, h,
                              mask.data(), prot.data(), pm, bbox, &tm)) {
            ++rejects;
            continue;
        }
        const DiffStat st = compare(ref, gpu, w, h);
        totalDiff += st.n;
        if (st.n && printed < 5) {
            // [KNOWN-1LSB] 历史遗留项的回归哨兵：1.0.2.0 曾在这里稳定差 1 灰阶，
            // 1.0.4.0 用 mad 残差 + 两次 Newton 精化修掉了。若这行再次打印，
            // 说明 compose 的除法精度出现回归；下面的平面级对拍会立刻切开
            // 「box 段」与「compose 段」。
            std::printf("  [KNOWN-1LSB] diff frame=%d dx=%d dy=%d bbox=(%d,%d)-(%d,%d)"
                        " pw=%d ph=%d n=%lld first@(%d,%d)ch=%d ref=%d gpu=%d\n",
                        f, dx, dy, bbox.x0, bbox.y0, bbox.x1, bbox.y1,
                        tm.pw, tm.ph, st.n, st.fx, st.fy, st.fc, st.fa, st.fb);
            // 一刀切开：平面全 0 差异却输出不一致 → compose 段；
            // 平面就有差异 → 下采样/box 段。差异帧恰好是全帧平面（pw==ww），
            // 坐标可直接对齐，不需要 halo 内缩。
            const int cw = (w + 1) / 2, ch = (h + 1) / 2;
            const size_t npC = (size_t)cw * ch;
            const int blocks[4] = { 0, 3, 6, 9 };   // C.r / B1.r / B2.r / amp
            if (tm.pw == cw && tm.ph == ch) {
                for (int q = 0; q < 4; ++q) {
                    std::vector<float> gp((size_t)tm.pw * tm.ph * 4, 0.0f);
                    int gpw = 0, gph = 0;
                    if (!gpuSmoothDebugPlane(q, gp.data(), (int)gp.size(), &gpw, &gph)) {
                        std::printf("    [CHK] plane %d readback FAILED\n", q);
                        continue;
                    }
                    long long nd = 0;
                    for (int y = 0; y < gph; ++y) {
                        for (int x = 0; x < gpw; ++x) {
                            const float a = sc[(size_t)blocks[q] * npC + (size_t)y * cw + x];
                            const float b = gp[((size_t)y * gpw + x) * 4 + 0];
                            if (std::memcmp(&a, &b, 4) != 0) ++nd;
                        }
                    }
                    std::printf("    [CHK] plane %d bitdiff=%lld / %lld\n",
                                q, nd, (long long)gpw * gph);
                }
            } else {
                std::printf("    [CHK] skip plane check: pw/ph %dx%d != full %dx%d\n",
                            tm.pw, tm.ph, cw, ch);
            }
            // D1 定点定位：用本帧 CPU 平面快照逐句复算 + ULP 敏感度扫描。
            if (std::getenv("P2_TRACE") != nullptr) {
                composeTrace(w, h, src, mask, prot, pm, sc,
                             st.fx, st.fy, st.fc, st.fa, st.fb);
            }
            ++printed;
        }
        sumMs += tm.totalMs;
        minPw = (std::min)(minPw, tm.pw); maxPw = (std::max)(maxPw, tm.pw);
        minPh = (std::min)(minPh, tm.ph); maxPh = (std::max)(maxPh, tm.ph);

        long long rb = 0, dc = 0;
        int cW = 0, cH = 0, cPw = 0, cPh = 0;
        gpuSmoothResourceStats(&rb, &dc, &cW, &cH, &cPw, &cPh);
        // 「真实尺寸变了但桶没变」= 桶化命中，这正是本用例要证明的事。
        const bool changed = prevPw >= 0 && (tm.pw != prevPw || tm.ph != prevPh);
        if (changed) ++sizeChanges;
        if (changed && cPw == prevAllocPw && cPh == prevAllocPh) ++reuse;
        prevPw = tm.pw; prevPh = tm.ph;
        prevAllocPw = cPw; prevAllocPh = cPh;
    }

    long long rb1 = 0, dc1 = 0;
    gpuSmoothResourceStats(&rb1, &dc1, &aW, &aH, &aPw, &aPh);
    const long long rebuilds = rb1 - rb0;
    const long long devCreates = dc1 - dc0;
    std::printf("  [CHURN] frames=%d rejects=%d diff=%lld  pw=%d..%d ph=%d..%d"
                "  rebuild=%lld deviceCreate=%lld reuse=%d/%d sizeChanges  avg=%.2f ms\n",
                frames, rejects, totalDiff, minPw, maxPw, minPh, maxPh,
                rebuilds, devCreates, reuse, sizeChanges,
                frames ? sumMs / frames : 0.0);
    // A 方案本身的目标单列一行，避免被「逐字节遗留项」盖住：
    // 资源不再随 ROI 抖动重建、设备不被反复创建、无回落。
    std::printf("  [A-OK] bucket goal: rebuild=%lld deviceCreate=%lld"
                " reuse=%d/%d rejects=%d\n",
                rebuilds, devCreates, reuse, sizeChanges, rejects);

    // 容量式复用生效时：整个序列最多重建 1~2 次（首帧建桶 / 大幅收缩），
    // 每一次真实尺寸变化都应该命中已有纹理。桶化前 rebuild ≈ frames，
    // 那才是 06:15 真机上 1138 帧约 2.7 万次创建销毁的根源。
    const bool ok = rejects == 0 && totalDiff == 0 && devCreates == 0 &&
                    rebuilds <= 2 && sizeChanges > 0 && reuse == sizeChanges;
    if (!ok) {
        std::printf("  [CHURN] FAIL criteria: rejects=0 diff=0 devCreate=0"
                    " rebuild<=2 sizeChanges>0 reuse==sizeChanges\n");
    }
    return ok;
}

// 多实例交替：插件的 D3D 状态是进程级单例，两个 OFX 实例只能共用同一套
// 设备与纹理组。台架里模拟成「两组画幅/人脸框交替调用」，覆盖第二实例
// 接入后全局状态被切换的路径。
struct Inst {
    int w = 0, h = 0;
    std::vector<unsigned char> src, mask, prot, ref;
    beautycore::RoiRect bbox;
    beautycore::BandParams pm;
};

static void prepareInst(Inst& in, int w, int h, int dx, int dy, int threads) {
    in.w = w; in.h = h;
    fillImage(in.src, w, h);
    buildMaskAt(in.mask, in.prot, w, h, dx, dy, &in.bbox);
    in.ref = in.src;
    std::vector<float> sc;
    beautycore::surface3RgbBand(in.src.data(), in.ref.data(), w, h,
                                in.mask.data(), in.prot.data(), in.pm,
                                in.bbox, threads, sc);
}

// 返回 -1 = 拒绝执行，否则返回差异字节数。
static long long checkInst(const Inst& in) {
    std::vector<unsigned char> gpu(in.src.size(), 0xAB);
    GpuSmoothTiming tm{};
    if (!gpuSmoothRgbBand(in.src.data(), gpu.data(), in.w, in.h,
                          in.mask.data(), in.prot.data(), in.pm,
                          in.bbox, &tm)) {
        return -1;
    }
    return compare(in.ref, gpu, in.w, in.h).n;
}

bool multiInstanceCheck(int threads) {
    Inst a, b, c;
    prepareInst(a, 640, 394, 0, 0, threads);
    prepareInst(b, 644, 398, 3, -2, threads);   // 相近画幅 + 略不同的人脸框
    prepareInst(c, 800, 600, 0, 0, threads);    // 跨桶画幅

    long long rb0 = 0, dc0 = 0;
    int tW = 0, tH = 0, tPw = 0, tPh = 0;
    gpuSmoothResourceStats(&rb0, &dc0, &tW, &tH, &tPw, &tPh);

    long long diff = 0;
    int rejects = 0;
    // 阶段①：同桶内交替。预期 0~2 次重建（首次建桶 + 可能跨一次边界）。
    for (int i = 0; i < 20; ++i) {
        const long long da = checkInst(a), db = checkInst(b);
        if (da < 0 || db < 0) ++rejects;
        else { diff += da + db; }
    }
    long long rb1 = 0, dc1 = 0;
    gpuSmoothResourceStats(&rb1, &dc1, &tW, &tH, &tPw, &tPh);
    const long long rebuildsSame = rb1 - rb0;

    // 阶段②：跨桶交替。容量式复用会先扩到较大那组桶，之后小画幅复用大纹理，
    // 所以不该每轮重建；这条正好抓「两个实例交替导致 churn」的回归。
    for (int i = 0; i < 6; ++i) {
        const long long da = checkInst(a), dc = checkInst(c);
        if (da < 0 || dc < 0) ++rejects;
        else { diff += da + dc; }
    }
    long long rb2 = 0, dc2 = 0;
    gpuSmoothResourceStats(&rb2, &dc2, &tW, &tH, &tPw, &tPh);
    const long long devCreates = dc2 - dc0;

    std::printf("  [MULTI] sameBucket 20x2 calls: rebuild=%lld ;"
                " crossBucket 6x2 calls: rebuild=%lld ; rejects=%d diff=%lld"
                " deviceCreate=%lld\n",
                rebuildsSame, rb2 - rb1, rejects, diff, devCreates);

    // 三个阶段都不该出现「资源随调用来回重建」：同桶 ≤2、跨桶 ≤2、设备 0。
    const bool ok = rejects == 0 && diff == 0 && devCreates == 0 &&
                    rebuildsSame <= 2 && (rb2 - rb1) <= 2 && gpuSmoothReady();
    if (!ok) std::printf("  [MULTI] FAIL criteria: rejects=0 diff=0 devCreate=0"
                         " sameBucketRebuild<=2 crossBucketRebuild<=2 ready=true\n");
    return ok;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (!gpuSmoothInit()) {
        std::printf("[FATAL] GPU 初始化失败：%s\n", gpuSmoothLastError());
        return 2;
    }

    struct Case { int w, h; const char* name; };
    const Case cases[] = {
        { 1920, 1080, "1080p 横" },
        { 1080, 1920, "1080p 竖" },
        { 3840, 2160, "4K" },
        {  640,  394, "小图" },
    };

    int pass = 0, total = 0;
    // P2_QUICK：调试用，跳过四个画幅对拍与生命周期，只跑新增的 churn/多实例。
    // shader 相关实验（BEAUTY_SMOOTH_OD=1）在 4K 上极慢，需要这条快捷路径。
    const bool quick = std::getenv("P2_QUICK") != nullptr;
    if (!quick) {
    for (const auto& c : cases) {
        ++total;
        std::printf("[CASE] %s  %dx%d\n", c.name, c.w, c.h);
        if (runCase(c.w, c.h, 12)) {
            ++pass;
            std::printf("  => 逐字节一致 ✔\n");
        } else {
            std::printf("  => 不一致 ✘\n");
            if (!std::getenv("P2_NO_DIAG")) diagnosePlanes(c.w, c.h, 12);
        }
    }

    // 生命周期单独一项：画幅四个 case 各自只调一次且尺寸不同，
    // 天然绕开了「同尺寸连续调用」和「shutdown 后重建」两条路径。
    ++total;
    std::printf("[CASE] 生命周期（同尺寸连续 / shutdown 后重建）\n");
    if (lifecycleCheck(12)) {
        ++pass;
        std::printf("  => 逐字节一致 ✔\n");
    } else {
        std::printf("  => 不一致 ✘\n");
    }
    }  // !quick

    // 1.0.3.0：补上台架原本缺失的三个维度。
    ++total;
    std::printf("[CASE] ROI 逐帧抖动 + 长时连续运行（桶化分配）\n");
    if (churnCheck(12)) {
        ++pass;
        std::printf("  => 通过 ✔\n");
    } else {
        std::printf("  => 失败 ✘\n");
    }

    ++total;
    std::printf("[CASE] 多实例交替（同桶 / 跨桶）\n");
    if (multiInstanceCheck(12)) {
        ++pass;
        std::printf("  => 通过 ✔\n");
    } else {
        std::printf("  => 失败 ✘\n");
    }

    {
        long long rb = 0, dc = 0, fr = 0;
        double av = 0.0, ap = 0.0;
        int iw = 0, ih = 0, pw = 0, ph = 0;
        gpuSmoothResourceStats(&rb, &dc, &iw, &ih, &pw, &ph);
        gpuSmoothStats(&fr, &av, &ap);
        std::printf("[STATS] frames=%lld rebuilds=%lld deviceCreates=%lld"
                    " lastAlloc=%dx%d/%dx%d avgTotal=%.2f ms\n",
                    fr, rb, dc, iw, ih, pw, ph, av);
    }

    gpuSmoothShutdown();
    std::printf("\n结果：%d/%d 通过\n", pass, total);
    return (pass == total) ? 0 : 1;
}
