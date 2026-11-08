// beauty_freq.cpp —— 高低频分离磨皮实现

#include "beauty_freq.h"
#include "beauty_pool.h"

#include "beauty_guide.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

namespace beautycore {

namespace {

int effThreads(int threads, int rows) {
    if (threads == 1) return 1;
    int n = threads;
    if (n <= 0) {
        const unsigned hc = std::thread::hardware_concurrency();
        n = hc ? (int)hc : 4;
        if (n > 8) n = 8;
    }
    if (n < 1) n = 1;
    if (rows < 32) return 1;
    const int byRows = rows / 16;
    if (n > byRows) n = byRows;
    return (n < 1) ? 1 : n;
}

void buildKernel(std::vector<float>& k, int radius, float sigma) {
    const int n = radius * 2 + 1;
    k.assign((size_t)n, 0.0f);
    const float inv = (sigma > 0.0f) ? 1.0f / (2.0f * sigma * sigma) : 0.0f;
    float sum = 0.0f;
    for (int i = 0; i < n; i++) {
        const int d = i - radius;
        const float v = std::exp(-(float)(d * d) * inv);
        k[(size_t)i] = v;
        sum += v;
    }
    if (sum > 0.0f) {
        for (int i = 0; i < n; i++) k[(size_t)i] /= sum;
    }
}

// 并行按行跑一段区间
template <typename Fn>
void runRows(int y0, int y1, int threads, Fn fn) {
    const int rows = y1 - y0;
    const int nt = effThreads(threads, rows);
    if (nt <= 1) {
        fn(y0, y1);
        return;
    }
    beautycore::parallelRows(y0, y1, nt, [&fn](int b, int e) { fn(b, e); });
}

inline int clampi(int v, int lo, int hi) {
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

}  // namespace

void gaussBlurRgba(const unsigned char* src, unsigned char* dst,
                   int w, int h, int radius, float sigma,
                   const RoiRect& roi, int threads,
                   std::vector<unsigned char>& scratch) {
    if (!src || !dst || w <= 0 || h <= 0) return;
    if (roi.empty()) return;
    if (radius < 1) {
        // 半径 0：低频 = 原图
        for (int y = roi.y0; y < roi.y1; y++) {
            const size_t off = (size_t)y * w * 4;
            memcpy(dst + off + (size_t)roi.x0 * 4, src + off + (size_t)roi.x0 * 4,
                   (size_t)roi.width() * 4);
        }
        return;
    }

    std::vector<float> ker;
    buildKernel(ker, radius, sigma);
    const int taps = radius * 2 + 1;

    const size_t need = (size_t)w * h * 4;
    if (scratch.size() < need) scratch.resize(need);
    unsigned char* mid = &scratch[0];

    // 水平 pass：只做「垂直 pass 会读到」的那些行（roi 上下各外扩 radius）。
    // 不做整图 —— 1080p 整图水平模糊要多花 3~4 倍时间，而 ROI 外的低频没人用。
    const int hy0 = clampi(roi.y0 - radius, 0, h);
    const int hy1 = clampi(roi.y1 + radius, 0, h);
    runRows(hy0, hy1, threads, [&](int yBegin, int yEnd) {
        for (int y = yBegin; y < yEnd; y++) {
            const unsigned char* row = src + (size_t)y * w * 4;
            unsigned char* out = mid + (size_t)y * w * 4;
            for (int x = 0; x < w; x++) {
                float a0 = 0, a1 = 0, a2 = 0;
                for (int t = 0; t < taps; t++) {
                    const int sx = clampi(x + t - radius, 0, w - 1);
                    const float kv = ker[(size_t)t];
                    const unsigned char* p = row + (size_t)sx * 4;
                    a0 += p[0] * kv;
                    a1 += p[1] * kv;
                    a2 += p[2] * kv;
                }
                unsigned char* o = out + (size_t)x * 4;
                o[0] = (unsigned char)(a0 + 0.5f);
                o[1] = (unsigned char)(a1 + 0.5f);
                o[2] = (unsigned char)(a2 + 0.5f);
                o[3] = row[(size_t)x * 4 + 3];
            }
        }
    });

    // 垂直 pass：只写 roi 内的行，x 覆盖 roi 宽度
    runRows(roi.y0, roi.y1, threads, [&](int yBegin, int yEnd) {
        for (int y = yBegin; y < yEnd; y++) {
            unsigned char* out = dst + (size_t)y * w * 4;
            for (int x = roi.x0; x < roi.x1; x++) {
                float a0 = 0, a1 = 0, a2 = 0;
                for (int t = 0; t < taps; t++) {
                    const int sy = clampi(y + t - radius, 0, h - 1);
                    const float kv = ker[(size_t)t];
                    const unsigned char* p = mid + ((size_t)sy * w + x) * 4;
                    a0 += p[0] * kv;
                    a1 += p[1] * kv;
                    a2 += p[2] * kv;
                }
                unsigned char* o = out + (size_t)x * 4;
                o[0] = (unsigned char)(a0 + 0.5f);
                o[1] = (unsigned char)(a1 + 0.5f);
                o[2] = (unsigned char)(a2 + 0.5f);
                o[3] = src[((size_t)y * w + x) * 4 + 3];
            }
        }
    });
}

void freqComposite(const unsigned char* src, const unsigned char* low,
                   unsigned char* dst, int w, int h,
                   float strength, float magLo, float magHi, float grain,
                   const RoiRect& roi, int threads) {
    if (!src || !low || !dst || w <= 0 || h <= 0 || roi.empty()) return;
    if (strength <= 0.0f) {
        for (int y = roi.y0; y < roi.y1; y++) {
            const size_t off = (size_t)y * w * 4;
            memcpy(dst + off + (size_t)roi.x0 * 4, src + off + (size_t)roi.x0 * 4,
                   (size_t)roi.width() * 4);
        }
        return;
    }
    if (magHi <= magLo) magHi = magLo + 1.0f;

    runRows(roi.y0, roi.y1, threads, [&](int yBegin, int yEnd) {
        for (int y = yBegin; y < yEnd; y++) {
            for (int x = roi.x0; x < roi.x1; x++) {
                const size_t o = ((size_t)y * w + x) * 4;
                int d0 = (int)src[o + 0] - (int)low[o + 0];
                int d1 = (int)src[o + 1] - (int)low[o + 1];
                int d2 = (int)src[o + 2] - (int)low[o + 2];

                // 颗粒回填（该实现的 smoothGrain）。
                // 双边迭代会留下"阶梯感"——平坦区被压成平台，平台之间出现新台阶，
                // 实测「小幅度高频」反而从 105% 涨到 113%。做法是把 4 邻域尺度的
                // 极细颗粒从要磨掉的量里扣回来：(src-low) - (src-around)*grain。
                // 这样只磨中频瑕疵，最细的皮肤纹理原样留着。
                if (grain > 0.0f) {
                    const int xm = (x > 0) ? (x - 1) : 0;
                    const int xp = (x + 1 < w) ? (x + 1) : (w - 1);
                    const int ym = (y > 0) ? (y - 1) : 0;
                    const int yp = (y + 1 < h) ? (y + 1) : (h - 1);
                    const size_t oxm = ((size_t)y * w + xm) * 4;
                    const size_t oxp = ((size_t)y * w + xp) * 4;
                    const size_t oym = ((size_t)ym * w + x) * 4;
                    const size_t oyp = ((size_t)yp * w + x) * 4;
                    const int g0 = (src[oxm] + src[oxp] + src[oym] + src[oyp]) / 4;
                    const int g1 = (src[oxm+1] + src[oxp+1] + src[oym+1] + src[oyp+1]) / 4;
                    const int g2 = (src[oxm+2] + src[oxp+2] + src[oym+2] + src[oyp+2]) / 4;
                    d0 -= (int)((src[o+0] - g0) * grain);
                    d1 -= (int)((src[o+1] - g1) * grain);
                    d2 -= (int)((src[o+2] - g2) * grain);
                }
                int mag = d0 < 0 ? -d0 : d0;
                int t = d1 < 0 ? -d1 : d1;
                if (t > mag) mag = t;
                t = d2 < 0 ? -d2 : d2;
                if (t > mag) mag = t;

                // atten = strength · (1 - smoothstep(magLo, magHi, mag))
                float a;
                if ((float)mag <= magLo) {
                    a = strength;
                } else if ((float)mag >= magHi) {
                    a = 0.0f;
                } else {
                    const float u = ((float)mag - magLo) / (magHi - magLo);
                    a = strength * (1.0f - u * u * (3.0f - 2.0f * u));
                }
                if (a <= 0.0f) {
                    dst[o + 0] = src[o + 0];
                    dst[o + 1] = src[o + 1];
                    dst[o + 2] = src[o + 2];
                } else {
                    dst[o + 0] = (unsigned char)(src[o + 0] - d0 * a + 0.5f);
                    dst[o + 1] = (unsigned char)(src[o + 1] - d1 * a + 0.5f);
                    dst[o + 2] = (unsigned char)(src[o + 2] - d2 * a + 0.5f);
                }
                dst[o + 3] = src[o + 3];
            }
        }
    });
}

// clarity：大半径局部对比回填。
//
// out = dst + (L - box(L, r)) × gain × gate(|L - box(L,r)|) × mask
//
// 三个刻意的选择：
//   1) 只在**亮度**上算，把同一个增量加到 RGB 三通道 —— 保持色相与饱和度不变，
//      三通道各算各的会把颜色边缘放大成彩边。
//   2) gate 用软膝而不是硬阈值：硬阈值会在阈值边界处留下「加/不加」的分界线，
//      看起来像一圈圈的等高线。
//   3) 输入是 dst（磨皮之后的图），不是 src —— 我们要增强的是最终画面的结构，
//      拿原图算会把刚磨掉的瑕疵重新加回来一半。
static inline float claritySmoothStep(float a, float b, float x) {
    if (b <= a) return (x < a) ? 0.0f : 1.0f;
    float t = (x - a) / (b - a);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

void clarityRgb(unsigned char* dst, int w, int h, const unsigned char* mask,
                const RoiRect& roi, const ClarityParams& p, int threads,
                std::vector<float>& scratch) {
    if (!dst || !mask || w <= 0 || h <= 0 || roi.empty()) return;
    if (p.gain <= 0.0f) return;

    LowFreqBuf lf = boxLumaRoi(dst, w, h, p.radius, roi, threads, scratch);
    if (!lf.valid()) return;

    const float lo = p.kneeLo;
    const float hi = (p.kneeHi > lo) ? p.kneeHi : (lo + 1.0f);

    runRows(roi.y0, roi.y1, threads, [&](int yBegin, int yEnd) {
        for (int y = yBegin; y < yEnd; y++) {
            const int ly = y - lf.y0;
            for (int x = roi.x0; x < roi.x1; x++) {
                const unsigned char m = mask[(size_t)y * w + x];
                if (m == 0) continue;                 // 掩码外一个字节都不动
                const int lx = x - lf.x0;
                if (lx < 0 || ly < 0 || lx >= lf.bw || ly >= lf.bh) continue;
                const size_t o = ((size_t)y * w + x) * 4;

                const float L = 0.299f * dst[o + 0] + 0.587f * dst[o + 1] +
                                0.114f * dst[o + 2];
                const float mid = L - lf.r[(size_t)ly * lf.bw + lx];
                const float am = std::fabs(mid);
                const float gate = 1.0f - claritySmoothStep(lo, hi, am);
                if (gate <= 0.0f) continue;

                const float k = p.gain * gate * ((float)m * (1.0f / 255.0f));
                const float add = mid * k;
                if (add > -0.5f && add < 0.5f) continue;   // 增量小于半个灰阶：不动

                for (int c = 0; c < 3; c++) {
                    float v = (float)dst[o + c] + add;
                    if (v < 0.0f) v = 0.0f;
                    if (v > 255.0f) v = 255.0f;
                    dst[o + c] = (unsigned char)(v + 0.5f);
                }
            }
        }
    });
}

// 与上面同构，但低频来自浮点平面。合成的算术一个字都没改 —— 变的只是
// `src - low` 里的 low 保持浮点精度（不再先压回 uint8，少一次量化）。
void freqCompositePlanes(const unsigned char* src, unsigned char* dst,
                         int w, int h, const LowFreqBuf& low,
                         float strength, float magLo, float magHi, float grain,
                         const RoiRect& roi, int threads) {
    if (!src || !dst || w <= 0 || h <= 0 || roi.empty() || !low.valid()) return;
    if (strength <= 0.0f) {
        for (int y = roi.y0; y < roi.y1; y++) {
            const size_t off = (size_t)y * w * 4;
            memcpy(dst + off + (size_t)roi.x0 * 4, src + off + (size_t)roi.x0 * 4,
                   (size_t)roi.width() * 4);
        }
        return;
    }
    if (magHi <= magLo) magHi = magLo + 1.0f;

    runRows(roi.y0, roi.y1, threads, [&](int yBegin, int yEnd) {
        for (int y = yBegin; y < yEnd; y++) {
            const int ly = y - low.y0;
            for (int x = roi.x0; x < roi.x1; x++) {
                const int lx = x - low.x0;
                if (lx < 0 || ly < 0 || lx >= low.bw || ly >= low.bh) continue;
                const size_t li = (size_t)ly * low.bw + lx;
                const size_t o = ((size_t)y * w + x) * 4;

                float d0 = (float)src[o + 0] - low.r[li];
                float d1 = (float)src[o + 1] - low.g[li];
                float d2 = (float)src[o + 2] - low.b[li];

                // 颗粒回填：与 freqComposite 完全一致（在 uint8 源上取 4 邻域）
                if (grain > 0.0f) {
                    const int xm = (x > 0) ? (x - 1) : 0;
                    const int xp = (x + 1 < w) ? (x + 1) : (w - 1);
                    const int ym = (y > 0) ? (y - 1) : 0;
                    const int yp = (y + 1 < h) ? (y + 1) : (h - 1);
                    const size_t oxm = ((size_t)y * w + xm) * 4;
                    const size_t oxp = ((size_t)y * w + xp) * 4;
                    const size_t oym = ((size_t)ym * w + x) * 4;
                    const size_t oyp = ((size_t)yp * w + x) * 4;
                    const int g0 = (src[oxm] + src[oxp] + src[oym] + src[oyp]) / 4;
                    const int g1 = (src[oxm+1] + src[oxp+1] + src[oym+1] + src[oyp+1]) / 4;
                    const int g2 = (src[oxm+2] + src[oxp+2] + src[oym+2] + src[oyp+2]) / 4;
                    d0 -= ((float)src[o+0] - (float)g0) * grain;
                    d1 -= ((float)src[o+1] - (float)g1) * grain;
                    d2 -= ((float)src[o+2] - (float)g2) * grain;
                }

                float mag = std::fabs(d0);
                const float t1 = std::fabs(d1), t2 = std::fabs(d2);
                if (t1 > mag) mag = t1;
                if (t2 > mag) mag = t2;

                float a;
                if (mag <= magLo) {
                    a = strength;
                } else if (mag >= magHi) {
                    a = 0.0f;
                } else {
                    const float u = (mag - magLo) / (magHi - magLo);
                    a = strength * (1.0f - u * u * (3.0f - 2.0f * u));
                }
                if (a <= 0.0f) {
                    dst[o + 0] = src[o + 0];
                    dst[o + 1] = src[o + 1];
                    dst[o + 2] = src[o + 2];
                } else {
                    const float v0 = (float)src[o + 0] - d0 * a;
                    const float v1 = (float)src[o + 1] - d1 * a;
                    const float v2 = (float)src[o + 2] - d2 * a;
                    dst[o + 0] = (unsigned char)(v0 < 0 ? 0 : (v0 > 255 ? 255 : v0) + 0.5f);
                    dst[o + 1] = (unsigned char)(v1 < 0 ? 0 : (v1 > 255 ? 255 : v1) + 0.5f);
                    dst[o + 2] = (unsigned char)(v2 < 0 ? 0 : (v2 > 255 ? 255 : v2) + 0.5f);
                }
                dst[o + 3] = src[o + 3];
            }
        }
    });
}

void highLowSmooth(const unsigned char* src, unsigned char* dst,
                   int w, int h, const FreqParams& p,
                   const RoiRect& roi, int threads,
                   std::vector<unsigned char>& lowBuf,
                   std::vector<unsigned char>& scratch,
                   std::vector<float>& guideScratch) {
    if (!src || !dst || w <= 0 || h <= 0 || roi.empty()) return;

    // 路线 C（lowPath == 3）：盒式迭代低通，不保边。
    //
    // 与上面两条路线的根本差别：低频层**不保边**。边缘处 (src − low) 幅度自然变大，
    // 由后面的幅度门控统一拦住；而痘印/鼻翼阴影/唇纹幅度中等，会被削掉。
    // 之前「双边保边 + 门控保边」两道叠在一起，等于把瑕疵也一起放过了 —— 实测
    // 脸颊细纹理只削 14%，参考版削 37%（C14）。
    if (p.lowPath == 3) {
        BoxLowParams bp;
        bp.radius = (p.boxRadius > 0) ? p.boxRadius : 4;
        bp.iters = (p.boxIters > 0) ? p.boxIters : 3;
        LowFreqBuf lf = boxRgbRoi(src, w, h, bp, roi, threads, guideScratch);
        if (lf.valid()) {
            freqCompositePlanes(src, dst, w, h, lf,
                                (float)p.strength / 100.0f, p.magLo, p.magHi,
                                p.grain, roi, threads);
            return;
        }
    }

    // 路线 A（lowPath > 0）：浮点双边 + 可选低频参照。
    // 上面 lowMode 那条是「换算法」，这条是「换权重判据 + 换精度」，
    // 两者互斥，这里优先走新路线。
    if (p.lowPath > 0) {
        BilateralRefParams bp;
        bp.radius = (p.radius > 0) ? p.radius : 2;
        bp.sigmaSpace = p.sigmaSpace;
        bp.sigmaColor = p.sigmaColor;
        bp.iters = (p.iters < 1) ? 1 : p.iters;
        bp.refRadius = (p.lowPath >= 2) ? p.refRadius : 0;
        // guideScratch 复用为浮点平面缓冲（9 个 bw×bh 平面），避免再加一个成员
        LowFreqBuf lf = lowFreqRefRoi(src, w, h, bp, roi, threads, guideScratch);
        if (lf.valid()) {
            freqCompositePlanes(src, dst, w, h, lf,
                                (float)p.strength / 100.0f, p.magLo, p.magHi,
                                p.grain, roi, threads);
            return;
        }
        // 分配失败等极端情况：落回下面的旧路线，宁可比原来丑也不能不出效果
    }

    const size_t need = (size_t)w * h * 4;
    if (lowBuf.size() < need) lowBuf.resize(need);
    if (scratch.size() < need) scratch.resize(need);

    // 低频层用**双边迭代**而不是大半径高斯。两个原因：
    //   1) 保边：高斯是各向同性的，会把边缘也糊进低频，导致 (src-low) 在边缘
    //      处出现大差值，幅度门控只能部分救回来（实测大幅度区掉到 92%）；
    //      双边在边缘处几乎不平均，(src-low) 本来就接近 0。
    //   2) 便宜：r=2 的双边已经优化过（ROI + 多线程），迭代几次仍比 σ=3 的
    //      19-tap 可分离高斯便宜得多（实测高斯 1080p 要 34 ms，超预算）。
    // 迭代 N 次把等效半径推到 N×r，同时每次都不跨边缘。
    if (p.lowMode == 1) {
        // 引导滤波低频：O(1)/像素，半径与成本无关，且输出是局部线性的
        // （q = a·I + b），不会像双边迭代那样把平坦区压成平台再出现台阶。
        GuideParams gp;
        gp.radius = p.guideRadius;
        gp.eps = p.guideEps;
        guidedSmoothRoi(src, &lowBuf[0], w, h, gp, roi, threads, guideScratch);
        freqComposite(src, &lowBuf[0], dst, w, h,
                      (float)p.strength / 100.0f, p.magLo, p.magHi, p.grain,
                      roi, threads);
        return;
    }

    const int iters = (p.iters < 1) ? 1 : p.iters;
    const int r = (p.radius > 0) ? p.radius : 2;

    // 每一轮迭代都会把「有效区域」向外扩 r 像素（边缘像素的邻域要读到外面）。
    // 所以迭代 N 轮必须用外扩 N×r 的 ROI 跑，否则第 2 轮开始就会在边界读到
    // 上一轮没写过的残留数据 —— 实测 ROI 内边界会出现 maxdiff=7 的偏差。
    // 最后只在原始 roi 内合成，多算的那一圈对外不可见。
    RoiRect wide = roi;
    const int grow = r * iters;
    wide.x0 = (roi.x0 - grow < 0) ? 0 : roi.x0 - grow;
    wide.y0 = (roi.y0 - grow < 0) ? 0 : roi.y0 - grow;
    wide.x1 = (roi.x1 + grow > w) ? w : roi.x1 + grow;
    wide.y1 = (roi.y1 + grow > h) ? h : roi.y1 + grow;

    bilateralRgbaRoi(src, &lowBuf[0], w, h, r, p.sigmaSpace,
                     p.sigmaColor, wide, threads);
    unsigned char* cur = &lowBuf[0];
    unsigned char* alt = &scratch[0];
    for (int i = 1; i < iters; i++) {
        bilateralRgbaRoi(cur, alt, w, h, r, p.sigmaSpace, p.sigmaColor,
                         wide, threads);
        std::swap(cur, alt);
    }
    freqComposite(src, cur, dst, w, h,
                  (float)p.strength / 100.0f, p.magLo, p.magHi, p.grain,
                  roi, threads);
}

}  // namespace beautycore

namespace beautycore {

// ---------------------------------------------------------------------------
// 清晰 / 美白 / 红润：照抄原生参考版的逐通道传递函数（2026-10-30）
// ---------------------------------------------------------------------------
// 数据来源：06_离线验证\全项对齐_20261030\tone_curve.py
//   把 panel 全置 0，用 BEAUTY_EXTRA_PARAMS_FILE 直接下发单个参考版参数
//   （绕开本工程 3.x 用 CPU 顶替的那几项），在 3 张真机帧的肤色掩码内
//   按输入灰阶分箱统计输出。箱内 σ：clarity 0.08~0.27、color_level
//   0.36~1.24、red_level 0.73 —— 只有零点几灰阶，说明它们是**纯颜色变换**。
//
// 结论（推翻两条旧认知）：
//   ① 「清晰」不是局部对比/中频增益，是 S 形对比曲线 + 轻微暖调（96→92、
//      160→170、200→210）。原来那套 12px 中频回填在语义上就是错的。
//   ② 「红润」不是加红，是**只压绿通道**（峰值 -5 @96~128 灰阶，
//      224 灰阶以上归零），R 基本不动、B 微升。旧实现加 R 压 B 是另一种观感。
//
// 表：5 档（面板 0/25/50/75/100）× 3 通道 × 17 锚点（输入 0,16,...,240,255）。
// 档位与输入值都线性插值。锚点外的区间夹到端点。
static const unsigned char kClarityLut5[5][3][17] = {
    { /* panel 0 = identity */
      {  0, 16, 32, 48, 64, 80, 96,112,128,144,160,176,192,208,224,240,255},
      {  0, 16, 32, 48, 64, 80, 96,112,128,144,160,176,192,208,224,240,255},
      {  0, 16, 32, 48, 64, 80, 96,112,128,144,160,176,192,208,224,240,255},
    },
    { /* 0.25 */
      {  0, 16, 32, 47, 62, 78, 95,112,129,146,162,179,195,210,225,241,255},
      {  0, 16, 31, 46, 62, 79, 95,112,129,146,163,179,194,209,224,240,255},
      {  0, 16, 31, 46, 62, 79, 95,112,129,146,163,179,195,209,225,240,255},
    },
    { /* 0.5 */
      {  0, 16, 32, 46, 60, 77, 94,112,129,147,165,182,198,212,227,241,255},
      {  0, 16, 29, 44, 60, 77, 95,113,131,149,166,182,195,211,225,240,255},
      {  0, 16, 29, 44, 60, 77, 95,112,131,149,166,182,197,211,226,240,255},
    },
    { /* 0.75 */
      {  0, 16, 32, 45, 58, 75, 93,111,130,149,167,185,200,215,229,241,255},
      {  0, 16, 28, 42, 58, 76, 94,113,133,151,169,185,197,212,225,241,255},
      {  0, 16, 28, 42, 58, 76, 94,113,132,151,169,185,200,212,227,241,255},
    },
    { /* 1.0 */
      {  0, 16, 32, 43, 56, 74, 92,111,131,151,170,188,203,217,230,242,255},
      {  0, 16, 26, 40, 56, 74, 94,114,134,154,172,188,199,214,226,241,255},
      {  0, 16, 26, 41, 56, 74, 93,113,133,153,171,188,203,214,228,241,255},
    },
};

static const unsigned char kWhitenLut5[5][3][17] = {
    { /* panel 0 = identity */
      {  0, 16, 32, 48, 64, 80, 96,112,128,144,160,176,192,208,224,240,255},
      {  0, 16, 32, 48, 64, 80, 96,112,128,144,160,176,192,208,224,240,255},
      {  0, 16, 32, 48, 64, 80, 96,112,128,144,160,176,192,208,224,240,255},
    },
    { /* 0.25 */
      {  0, 16, 32, 49, 65, 82, 99,115,132,148,164,179,195,210,225,241,255},
      {  0, 16, 33, 49, 66, 82, 99,116,132,148,163,179,194,209,225,241,255},
      {  0, 16, 32, 49, 66, 83,100,116,133,149,164,179,195,210,225,240,255},
    },
    { /* 0.5 */
      {  0, 16, 32, 50, 67, 85,102,119,136,152,167,182,197,212,227,241,255},
      {  0, 16, 33, 51, 68, 85,103,119,136,152,167,182,196,211,225,241,255},
      {  0, 16, 32, 51, 68, 86,104,121,137,153,168,182,198,211,227,241,255},
    },
    { /* 0.75 */
      {  0, 16, 32, 50, 68, 87,105,122,140,156,171,185,200,214,228,242,254},
      {  0, 16, 34, 52, 69, 87,106,123,140,156,170,185,197,212,226,242,255},
      {  0, 16, 32, 52, 70, 89,108,125,142,158,172,186,201,213,228,241,255},
    },
    { /* 1.0 */
      {  0, 16, 32, 51, 70, 89,108,126,144,160,174,188,203,216,230,242,254},
      {  0, 16, 35, 54, 71, 90,109,126,143,159,174,187,199,214,227,242,255},
      {  0, 16, 32, 53, 72, 92,112,129,146,162,176,189,204,215,230,242,255},
    },
};

// 红润：只有绿通道被压（面板 100 档实测），R 不动、B 微升。
static const signed char kRosyG17[17] = {0,0,-4,-4,-5,-5,-5,-5,-4,-4,-3,-3,-1,-1,0,0,0};
static const signed char kRosyB17[17] = {0,0, 2, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1,0,0};

namespace {

// 17 锚点表（步长 16，最后一点 255）线性插值。
inline float lut17(const unsigned char* t, float v) {
    const float p = v * (1.0f / 16.0f);
    if (p >= 16.0f) return (float)t[16];
    if (p <= 0.0f) return (float)t[0];
    const int i = (int)p;
    const float f = p - (float)i;
    return (float)t[i] + ((float)t[i + 1] - (float)t[i]) * f;
}

inline float dlt17(const signed char* t, float v) {
    const float p = v * (1.0f / 16.0f);
    if (p >= 16.0f) return (float)t[16];
    if (p <= 0.0f) return (float)t[0];
    const int i = (int)p;
    const float f = p - (float)i;
    return (float)t[i] + ((float)t[i + 1] - (float)t[i]) * f;
}

// 档位插值：0..1 → 表索引 0..4
struct Lv5 {
    int i0;
    float f;
    explicit Lv5(float a) {
        float x = a * 4.0f;
        if (x < 0.0f) x = 0.0f;
        if (x > 3.999f) x = 3.999f;
        i0 = (int)x;
        f = x - (float)i0;
    }
    inline float at(const unsigned char t[5][3][17], int c, float v) const {
        const float a = lut17(t[i0][c], v);
        const float b = lut17(t[i0 + 1][c], v);
        return a + (b - a) * f;
    }
};

}  // namespace

// 色调权重低通半径（1.2.0 引入；1.2.1 曾提到 12，1.2.2 退回 8 —— 见下方注释）。
// 只作用在 toneColor 的权重上：r=8 → 17×17，r=12 → 25×25 盒式均值。
// 为什么需要：掩码在内容边界（头发丝、阴影、项链）上 1~3 像素内从 255 掉到 0，
// 磨皮在平坦皮肤上看不出来，美白「整片加常数」就会把它照成一道暗线。
const int kToneWeightRadius = 24;

// 色调边缘门控阈值（1.2.4），与 beauty_core.cpp 的 kEdgeLo/kEdgeHi 同一口径。
static const int kToneEdgeLo = 102;
static const int kToneEdgeHi = 306;

void toneColor(unsigned char* dst, int w, int h,
               const unsigned char* mask, const RoiRect& roi,
               float clarity, float whiten, float rosy, int threads) {
    if (!dst || !mask || roi.empty()) return;
    if (clarity < 0.0f) clarity = 0.0f;
    if (clarity > 1.0f) clarity = 1.0f;
    if (whiten < 0.0f) whiten = 0.0f;
    if (whiten > 1.0f) whiten = 1.0f;
    if (rosy < 0.0f) rosy = 0.0f;
    if (rosy > 1.0f) rosy = 1.0f;
    if (clarity <= 0.0f && whiten <= 0.0f && rosy <= 0.0f) return;

    const bool doClar = (clarity > 0.0f);
    const bool doWhiten = (whiten > 0.0f);
    const bool doRosy = (rosy > 0.0f);
    const Lv5 lc(clarity), lw(whiten);

    // 抖动（1.1.9.0，修「开美白就出一道横线」）。
    //
    // 起因：真机复验「线还在，把美白关掉就没有了，只开磨皮没有」。
    // 对照实测（同一帧，磨皮 98 固定，美白 0 / 64 各跑一次，两张输出相减就是
    // 美白本身的增量）：增量值域 0..11，而在脖子上能测到 **47~67 列连续的
    // 同向 ≥0.5 级台阶** —— 那就是一条横线的量级。
    //
    // 根因不是掩码，是**量化**：美白曲线在中间调斜率约 1.19（17 锚点表
    // panel 1.0 的 R 行 32→51→70→89→108，每 16 级涨 19），斜率 >1 就必然
    // 每隔十来级出现一次「涨 2 级」的跳变；而脖子是一大片被磨平的暗部渐变，
    // 这种 2 级跳变沿着等亮度线（在脖子上基本是横的）连成一条线，肉眼就是
    // 一道细细的台阶。磨皮不改变亮度曲线，所以只开磨皮看不到。
    //
    // 解法：在**量化之前**加 4×4 Bayer 有序抖动，幅度 ±0.5 级。跳变没有变，
    // 但它不再落在同一行上 —— 沿等亮度线摊成两行各一半，眼睛积分回原来的
    // 亮度，线就散了。幅度正好半个 LSB，在平坦区看不出来，也不会改变均值。
    static const int kBayer4[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    const bool doDither = doWhiten || doClar;   // 红润只有 ±1 级，单独不需要

    // 色调权重先做空间低通（1.2.0，1.2.1 把半径 8 → 12）。
    //
    // 1.1.9 的抖动在真机上没消掉那条线，说明它不是「量化台阶」而是**权重里有结构**：
    // 磨皮看到同一份掩码却不出线，是因为在有纹理的地方磨皮会把掩码的细节一起抹掉，
    // 而磨皮对**平坦皮肤**几乎是恒等映射（没什么可磨），于是掩码里 1~3 行/列的
    // 小结构只有美白这种"整片加常数"的算子才暴露出来 —— 一行漏加 8 级就是一道暗线。
    //
    // 美白/清晰本来就是大范围色调操作，权重没有必要保留 1~3 像素级的细节。这里把
    // 权重过一次盒式低通（先横后竖，O(1)/像素）：R=8 是 17×17，2 像素宽的结构在
    // 均值里剩 2/17 ≈ 12%（8 级的落差降到 1 级以内，真机复验「肉眼基本看不出」）；
    // R=12 是 25×25，同样 2 像素的结构只剩 2/25 = 8%，3 像素的从 18% 降到 12% ——
    // 代价是色调边界再往外扩约 4 px，最坏会轻微吃到衣领上沿。真正的边界（掩码
    // 自己的 64 px 羽化）两种半径下都几乎不受影响。
    //
    // 半径只影响**色调**权重，磨皮用的仍是原掩码，所以不会把磨皮范围一起放大。
    //
    // 1.2.3：改成「长斜坡 + 内部门控」两道。
    // 真机复验 R=8 / R=12 都还剩「右边锁骨处极少量白色条纹」。看截图，那条白边的形状是
    // **一段竖着的直边**：皮肤和外套/头发的交界处，掩码 1~2 像素内从 255 掉到 0，
    // 低通只能把它从 1 px 摊到 17 px（R=8）—— 8 级的提亮摊在 17 px 上仍然是一条边，
    // 而继续加大半径会把提亮**外溢到外套上**（R=12 实测溢出 +46%，真机上就是那条白边更明显）。
    //
    // 所以拆成两件事：
    //   wlong  = 半径 24 的盒式均值 —— 边界处的斜坡拉到 ~48 px，单像素落差降到 1/3；
    //   wsmall = 半径 3 的小均值 —— 只用来做「内外门控」：wsmall 掉到 0 说明已经出了皮肤，
    //            把 wlong 按 wsmall/64 压回 0，于是**长斜坡只向皮肤内侧长，不往外溢**。
    // 两个半径都作用在色调权重上，磨皮完全不受影响。
    static thread_local std::vector<unsigned char> wbuf, wtmp, wsmall;
    const int bw = roi.width(), bh = roi.height();
    const unsigned char* wsrc = mask;
    if (bw > 0 && bh > 0 && doDither) {
        const size_t n = (size_t)bw * bh;
        if (wbuf.size() < n) { wbuf.resize(n); wtmp.resize(n); wsmall.resize(n); }
        auto box2 = [&](int R, unsigned char* out) {
            // 横向：mask -> wtmp
            for (int yy = 0; yy < bh; yy++) {
                const unsigned char* src = mask + (size_t)(roi.y0 + yy) * w + roi.x0;
                unsigned char* o = &wtmp[(size_t)yy * bw];
                int sum = 0, cnt = 0;
                for (int x = 0; x <= R && x < bw; x++) { sum += src[x]; cnt++; }
                for (int x = 0; x < bw; x++) {
                    o[x] = (unsigned char)(sum / cnt);
                    const int add = x + R + 1, sub = x - R;
                    if (add < bw) { sum += src[add]; cnt++; }
                    if (sub >= 0) { sum -= src[sub]; cnt--; }
                }
            }
            // 竖向：wtmp -> out
            for (int xx = 0; xx < bw; xx++) {
                int sum = 0, cnt = 0;
                for (int y = 0; y <= R && y < bh; y++) { sum += wtmp[(size_t)y * bw + xx]; cnt++; }
                for (int y = 0; y < bh; y++) {
                    out[(size_t)y * bw + xx] = (unsigned char)(sum / cnt);
                    const int add = y + R + 1, sub = y - R;
                    if (add < bh) { sum += wtmp[(size_t)add * bw + xx]; cnt++; }
                    if (sub >= 0) { sum -= wtmp[(size_t)sub * bw + xx]; cnt--; }
                }
            }
        };
        // 1.3.0：两遍盒式均值改成并行（按行切 H 遍、按列切 V 遍）。
        // 实测 1080p 的 ROI 上这一段串行要 2~3 ms，占了色调段的一半以上。
        {
            int nth = (threads > 0) ? threads : (int)std::thread::hardware_concurrency();
            if (nth > 16) nth = 16;
            // wtmp 是 thread_local：**必须在本线程取好指针再并行**，
            // 否则工作线程拿到的是各自那份未 resize 的副本（实测直接 AV 崩溃）。
            unsigned char* const wtp = &wtmp[0];
            auto box2p = [&](int R, unsigned char* out) {
                // 横向：每行独立
                auto hRow = [&](int yb, int ye) {
                    for (int yy = yb; yy < ye; yy++) {
                        const unsigned char* src = mask + (size_t)(roi.y0 + yy) * w + roi.x0;
                        unsigned char* o = wtp + (size_t)yy * bw;
                        int sum = 0, cnt = 0;
                        for (int x = 0; x <= R && x < bw; x++) { sum += src[x]; cnt++; }
                        for (int x = 0; x < bw; x++) {
                            o[x] = (unsigned char)(sum / cnt);
                            const int add = x + R + 1, sub = x - R;
                            if (add < bw) { sum += src[add]; cnt++; }
                            if (sub >= 0) { sum -= src[sub]; cnt--; }
                        }
                    }
                };
                if (nth > 1 && bh >= 64) beautycore::parallelRows(0, bh, nth, hRow);
                else hRow(0, bh);
                // 竖向：每列独立（把 x 当遍历轴交给池子）
                auto vCol = [&](int xb, int xe) {
                    for (int xx = xb; xx < xe; xx++) {
                        int sum = 0, cnt = 0;
                        for (int y = 0; y <= R && y < bh; y++) { sum += wtp[(size_t)y * bw + xx]; cnt++; }
                        for (int y = 0; y < bh; y++) {
                            out[(size_t)y * bw + xx] = (unsigned char)(sum / cnt);
                            const int add = y + R + 1, sub = y - R;
                            if (add < bh) { sum += wtp[(size_t)add * bw + xx]; cnt++; }
                            if (sub >= 0) { sum -= wtp[(size_t)sub * bw + xx]; cnt--; }
                        }
                    }
                };
                if (nth > 1 && bw >= 64) beautycore::parallelRows(0, bw, nth, vCol);
                else vCol(0, bw);
            };
            box2p(kToneWeightRadius, &wbuf[0]);
            box2p(3, &wsmall[0]);
        }
        for (size_t i = 0; i < n; i++) {
            int gate = (int)wsmall[i] * 4;        // 64 以上算「在皮肤里面」
            if (gate > 255) gate = 255;
            wbuf[i] = (unsigned char)(((int)wbuf[i] * gate) / 255);
        }
        wsrc = &wbuf[0];
    }
    const bool wRoi = (wsrc != mask);

    // 边缘门控（1.2.4）：和磨皮的 g_cfg.edgeK 同一个理由 ——
    // 细而强的结构（压在锁骨上的碎发、链子、衣纹）不该被"整片加常数"的
    // 美白抬亮。磨皮本来就有这道门（blendWithMask 里的 sobelEdge），
    // 所以只开磨皮看不见；美白没有门，碎发被抬成浅色细条。
    //
    // 1.3.6.0：Sobel 必须**在写回之前单独算一遍**。
    // 原实现把它写在写回循环里，读的是 dst 自身 —— 而 dst 正被其它线程按行并行
    // 写美白结果。行块边界那两行读到的邻域究竟是"已美白"还是"未美白"取决于
    // 线程调度：实测 2100 次有 33 次输出差 1 级（1.57%），异常行全部落在
    // 261 + 35k（35 = 块高）以及其 y±1，即 Sobel 的上下邻域。
    // 这是数据竞争（C++ 未定义行为），不是测量噪声，也是"逐字节回归"偶发
    // 假失配的真因。改成先算到 ebuf 再读，顺带省掉写回时的九宫格重算。
    static thread_local std::vector<unsigned char> ebuf;
    const unsigned char* eb = nullptr;
    if (doWhiten || doClar) {
        const size_t en = (size_t)bw * bh;
        if (ebuf.size() < en) ebuf.resize(en);
        unsigned char* const ebp = &ebuf[0];
        runRows(roi.y0, roi.y1, threads, [&](int yBegin, int yEnd) {
            for (int y = yBegin; y < yEnd; y++) {
                for (int x = roi.x0; x < roi.x1; x++) {
                    const int xm = (x > 0) ? (x - 1) : x;
                    const int xp = (x + 1 < w) ? (x + 1) : x;
                    const int ym = (y > 0) ? (y - 1) : y;
                    const int yp = (y + 1 < h) ? (y + 1) : y;
                    const unsigned char* r0 = dst + (size_t)ym * w * 4;
                    const unsigned char* r1 = dst + (size_t)y * w * 4;
                    const unsigned char* r2 = dst + (size_t)yp * w * 4;
                    const int l00 = r0[xm * 4 + 1], l01 = r0[x * 4 + 1], l02 = r0[xp * 4 + 1];
                    const int l10 = r1[xm * 4 + 1], l12 = r1[xp * 4 + 1];
                    const int l20 = r2[xm * 4 + 1], l21 = r2[x * 4 + 1], l22 = r2[xp * 4 + 1];
                    const int gx = (l02 + 2 * l12 + l22) - (l00 + 2 * l10 + l20);
                    const int gy = (l20 + 2 * l21 + l22) - (l00 + 2 * l01 + l02);
                    int mag = ((gx < 0 ? -gx : gx) + (gy < 0 ? -gy : gy)) / 2;
                    int e = 0;
                    if (mag >= kToneEdgeHi) e = 192;
                    else if (mag > kToneEdgeLo)
                        e = (int)((long long)(mag - kToneEdgeLo) * 192 / (kToneEdgeHi - kToneEdgeLo));
                    ebp[(size_t)(y - roi.y0) * bw + (x - roi.x0)] = (unsigned char)e;
                }
            }
        });
        eb = ebp;
    }

    runRows(roi.y0, roi.y1, threads, [&](int yBegin, int yEnd) {
        for (int y = yBegin; y < yEnd; y++) {
            const size_t rowOff = (size_t)y * w;
            const unsigned char* wrow = wRoi ? (wsrc + (size_t)(y - roi.y0) * bw) : nullptr;
            for (int x = roi.x0; x < roi.x1; x++) {
                const unsigned char mv = wRoi ? wrow[x - roi.x0] : mask[rowOff + x];
                if (mv == 0) continue;          // 非皮肤：一个字节都不动
                const size_t o = (rowOff + x) * 4;
                const int edge = eb ? (int)eb[(size_t)(y - roi.y0) * bw + (x - roi.x0)] : 0;
                const float sk = (float)mv * (1.0f / 255.0f) * (float)(256 - edge) * (1.0f / 256.0f);

                float r = (float)dst[o + 0];
                float g = (float)dst[o + 1];
                float b = (float)dst[o + 2];

                // 顺序：清晰 → 美白 → 红润。参考版侧没取到三段的确切顺序，
                // 这个顺序保证「清晰抬对比不会把美白抬亮压平」（先结构后提亮）。
                if (doClar) {
                    r += (lc.at(kClarityLut5, 0, r) - r) * sk;
                    g += (lc.at(kClarityLut5, 1, g) - g) * sk;
                    b += (lc.at(kClarityLut5, 2, b) - b) * sk;
                }
                if (doWhiten) {
                    r += (lw.at(kWhitenLut5, 0, r) - r) * sk;
                    g += (lw.at(kWhitenLut5, 1, g) - g) * sk;
                    b += (lw.at(kWhitenLut5, 2, b) - b) * sk;
                }
                if (doRosy) {
                    g += dlt17(kRosyG17, g) * rosy * sk;
                    b += dlt17(kRosyB17, b) * rosy * sk;
                }

                // 有序抖动：把「涨 2 级」的等亮度台阶摊到相邻行/列上（见上面
                // kBayer4 的注释）。没开清晰/美白时保持逐位不变。
                float dth = 0.0f;
                if (doDither) {
                    dth = ((float)kBayer4[((y & 3) << 2) | (x & 3)] + 0.5f) * (1.0f / 16.0f) - 0.5f;
                }
                dst[o + 0] = (unsigned char)(r < 0 ? 0 : (r > 255.0f ? 255.0f : r + 0.5f + dth));
                dst[o + 1] = (unsigned char)(g < 0 ? 0 : (g > 255.0f ? 255.0f : g + 0.5f + dth));
                dst[o + 2] = (unsigned char)(b < 0 ? 0 : (b > 255.0f ? 255.0f : b + 0.5f + dth));
            }
        }
    });
}

void toneWhitenRosy(unsigned char* dst, int w, int h,
                    const unsigned char* mask, const RoiRect& roi,
                    float whiten, float rosy, int threads) {
    toneColor(dst, w, h, mask, roi, 0.0f, whiten, rosy, threads);
}

}  // namespace beautycore
