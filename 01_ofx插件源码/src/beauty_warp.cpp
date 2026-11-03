// beauty_warp.cpp —— 美形三项（瘦脸 / 大眼 / 下巴）的位移场实现
//
// 实现要点（每一条都是踩过或算过才写的，改之前先读）：
//
// 1) **反向映射**：对每个输出像素求它的采样源坐标，再双线性取样。
//    正向映射（源→目标）会在目标上留空洞，补洞逻辑比反向映射贵得多。
//
// 2) **ROI 外扩 + 权重归零**：位移场只在脸框附近非零。如果 ROI 边界上位移
//    还有值，ROI 内外的像素就会错位，画面上表现为一条竖直/水平的硬缝。
//    所以每个权重函数都必须在 ROI 边界之前降到 0，ROI 还要比脸框外扩一圈
//    （瘦脸的侧向权重在 |u|≈1 处才最强，不扩就正好切在脸边）。
//
// 3) **坐标钳在 ROI 内**：外扩区已经包含背景，钳住不会读到 ROI 之外的
//    旧像素；不钳的话瘦脸在大幅度下会把采样点推出图像边界。
//
// 4) **位移接近 0 时直拷**：脸心一带三项位移都接近 0，直接 memcpy 一个像素
//    比 4 次双线性快得多，实测能省掉约一半耗时。
//
// 5) 没有关键点时退化：用「脸框比例」估眼位（0.38h / ±0.22w）和嘴线
//    （0.72h）。估出来的位置比关键点差，但至少不会让功能失效 ——
//    检不出关键点的帧在真机上并不罕见（侧脸、低头）。
//
// 6) **y 方向有语义，必须按行序取反**（v3.0.5 修，之前是真机上的硬 bug）：
//    这三项里「上/下」不是装饰 —— 嘴线在眼线之下、下巴在嘴线之下、
//    瘦脸的垂直高斯中心在脸颊。而 OFX 给的缓冲第 0 行是画面**最下面**一行，
//    于是 f.y 是脸框的**视觉下沿**、f.y+f.h 是视觉上沿。
//    之前一律按 top-down 写（chinY=f.y+f.h、eyeY=f.y+0.38h、mouthY=f.y+0.72h），
//    在真机上等于：下巴底取到发际线、嘴线取到额头、嘴线合理性校验恒假
//    （嘴角的缓冲行号比眼小），结果瘦脸的高斯中心跑到眉毛、下巴压的是头发。
//    大眼因为是径向各向同性、且眼心来自关键点（关键点映射本身是对的），
//    所以只有它看起来正常 —— 这就是「三项只有大眼对」的全部原因。
//    修法：所有「视觉往下」的量统一乘 sgn（flipY 时 sgn=-1），比例值改成
//    从缓冲最小行往上数（0.38→0.62、0.72→0.28）。

#include "beauty_warp.h"

#include <cmath>
#include <cstring>

namespace beautycore {

namespace {

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline float smoothstep01(float a, float b, float x) {
    if (b <= a) return x >= b ? 1.0f : 0.0f;
    const float t = clampf((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// 一张脸做位移场需要的全部几何量，先算一次，内层循环只读。
// 所有 y 量都是**缓冲行号**（不是视觉坐标），方向由 makeGeom 的 flipY 决定。
struct Geom {
    float cx = 0, cy = 0;      // 脸框中心
    float fw = 0, fh = 0;      // 脸框宽高
    float eyeRx = 0, eyeRy = 0;  // 右眼（YuNet kps[0]）
    float eyeLx = 0, eyeLy = 0;  // 左眼（YuNet kps[1]）
    float eyeY = 0;            // 双眼平均高度
    float eyeDist = 0;         // 双眼间距，用来定「大眼」的作用半径
    float mouthY = 0;          // 嘴线高度，下巴变形的分界
    float chinY = 0;           // 下巴底（脸框下沿，视觉上的最下）
    bool hasEyes = false;      // 眼睛来自关键点（否则是比例估算）
};

// flipY=true：缓冲第 0 行是画面最下面一行（OFX/Resolve）。
// 此时 f.y 是脸框的**视觉下沿**，f.y+f.h 是视觉上沿，所以：
//   · 下巴底（视觉最下）= f.y，不是 f.y+f.h
//   · 眼线距视觉上沿 0.38h → 距缓冲最小行 0.62h
//   · 嘴线距视觉上沿 0.72h → 距缓冲最小行 0.28h
Geom makeGeom(const FaceBox& f, bool flipY) {
    Geom g;
    g.cx = f.x + f.w * 0.5f;
    g.cy = f.y + f.h * 0.5f;
    g.fw = f.w;
    g.fh = f.h;
    g.chinY = f.y + (flipY ? 0.0f : f.h);

    // 视觉「往下」在缓冲行号里是减小（flipY）还是增大（top-down）。
    const float sgn = flipY ? -1.0f : 1.0f;

    // 比例兜底：三庭五眼的粗估。只在没有关键点时用。
    g.eyeY = f.y + (flipY ? 0.62f : 0.38f) * f.h;
    g.eyeRx = g.cx - 0.22f * f.w;
    g.eyeRy = g.eyeY;
    g.eyeLx = g.cx + 0.22f * f.w;
    g.eyeLy = g.eyeY;
    g.eyeDist = 0.44f * f.w;
    g.mouthY = f.y + (flipY ? 0.28f : 0.72f) * f.h;

    if (f.hasKps) {
        // YuNet 关键点顺序：0 右眼 / 1 左眼 / 2 鼻尖 / 3 右嘴角 / 4 左嘴角
        const float r0x = f.kps[0], r0y = f.kps[1];
        const float l1x = f.kps[2], l1y = f.kps[3];
        const float d = sqrtf((l1x - r0x) * (l1x - r0x) + (l1y - r0y) * (l1y - r0y));
        // 合理性检查：间距太小说明两个点重合（解码失败），太大说明跨了半张图。
        // 这一条与行序无关（用的是距离，不是方向）。
        if (d > 4.0f && d < 3.0f * f.w) {
            g.eyeRx = r0x;
            g.eyeRy = r0y;
            g.eyeLx = l1x;
            g.eyeLy = l1y;
            g.eyeDist = d;
            g.eyeY = (r0y + l1y) * 0.5f;
            g.hasEyes = true;
        }
        const float my = (f.kps[7] + f.kps[9]) * 0.5f;
        // 嘴线必须落在「眼线往下、下巴往上」之间，否则说明嘴角点不可信，
        // 用比例值。方向无关写法：两个差值都乘 sgn 后必须为正。
        // （之前写成 my > eyeY+0.05h && my < chinY，bottom-up 下恒假，
        //   于是真机上关键点的嘴线被白白丢掉，回落到错误的额头位置。）
        if ((my - g.eyeY) * sgn > 0.05f * f.h && (g.chinY - my) * sgn > 0.0f)
            g.mouthY = my;
    }
    return g;
}

}  // namespace

void warpFace(unsigned char* rgba, int w, int h, const FaceBox& f,
              const WarpParams& p, std::vector<unsigned char>& scratch,
              bool flipY) {
    if (!rgba || w <= 0 || h <= 0) return;
    if (p.thin <= 0.0f && p.eye <= 0.0f && p.chin <= 0.0f) return;
    if (!(f.w > 8.0f) || !(f.h > 8.0f)) return;

    const Geom g = makeGeom(f, flipY);
    // 视觉「往下」对应的缓冲行号增量：flipY 时行号减小，故为 -1。
    // 只在「下半脸」这类有方向的判据里用；大眼是径向的，用不到它。
    const float sgn = flipY ? -1.0f : 1.0f;

    // ---- ROI：脸框外扩 30% 宽 / 18% 高，并钳到画面内 ----
    const float mx = 0.30f * g.fw;
    const float my = 0.18f * g.fh;
    int x0 = (int)floorf(f.x - mx);
    int x1 = (int)ceilf(f.x + f.w + mx);
    int y0 = (int)floorf(f.y - my);
    int y1 = (int)ceilf(f.y + f.h + my);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    const int rw = x1 - x0;
    const int rh = y1 - y0;
    if (rw <= 2 || rh <= 2) return;

    // ---- 先把 ROI 拷进 scratch 当采样源（就地写会污染后面的取样）----
    const size_t rowBytes = (size_t)rw * 4;
    const size_t needBytes = rowBytes * (size_t)rh;
    if (scratch.size() < needBytes) scratch.resize(needBytes);
    unsigned char* src = &scratch[0];
    for (int y = 0; y < rh; y++)
        memcpy(src + (size_t)y * rowBytes,
               rgba + ((size_t)(y0 + y) * (size_t)w + (size_t)x0) * 4, rowBytes);

    // ---- 三个场的常量，循环外算好 ----
    // 大眼：作用半径取双眼间距的一半多一点（覆盖眼睛 + 眼皮 + 一点眼袋）。
    float eyeR = 0.52f * g.eyeDist;
    const float eyeMin = 0.10f * g.fw;
    const float eyeMax = 0.35f * g.fw;
    if (eyeR < eyeMin) eyeR = eyeMin;
    if (eyeR > eyeMax) eyeR = eyeMax;
    const float eyeAmp = p.eye * 0.55f;   // 满档中心放大 1.55 倍
    const bool doEye = (p.eye > 0.0f) && (eyeR > 1.0f);

    // 瘦脸：垂直高斯中心放在「眼线 → 嘴线」之间偏下，正好是脸颊。
    const float thinYc = g.eyeY + 0.62f * (g.mouthY - g.eyeY);
    const float thinSig = 0.24f * g.fh;
    const float thinK = p.thin * 0.24f;   // 满档外侧采样点外移 24%
    const bool doThin = (p.thin > 0.0f) && (thinSig > 0.5f);

    // 下巴：从嘴线往下压缩，嘴线以上不动。
    // 「往下」在缓冲里可能是行号减小（flipY）或增大（top-down），统一用 sgn
    // 表达：chinYm 偏移方向、chinSpan 取正、判定与归一化都乘 sgn。
    // 注意最后那步 sy = chinYm + (sy-chinYm)*(1+k) 不用改 —— 它缩放的是
    // **带符号的差值**，与行序无关。
    const float chinYm = g.mouthY + sgn * 0.02f * g.fh;
    const float chinSpan = (g.chinY - chinYm) * sgn;
    const float chinK = p.chin * 0.18f;   // 满档下巴纵向缩 18%
    const bool doChin = (p.chin > 0.0f) && (chinSpan > 1.0f);

    const float halfW = 0.5f * g.fw;

    for (int y = 0; y < rh; y++) {
        const float oy = (float)(y0 + y);
        unsigned char* out = rgba + ((size_t)(y0 + y) * (size_t)w + (size_t)x0) * 4;
        const unsigned char* srow = src + (size_t)y * rowBytes;

        for (int x = 0; x < rw; x++) {
            const float ox = (float)(x0 + x);
            float sx = ox;
            float sy = oy;

            // ---- ① 大眼：以眼心为中心径向放大 ----
            // 采样点向眼心收拢（除以 m>1），内容就被「撑大」。
            if (doEye && g.hasEyes) {
                for (int e = 0; e < 2; e++) {
                    const float ex = e ? g.eyeLx : g.eyeRx;
                    const float ey = e ? g.eyeLy : g.eyeRy;
                    const float dx = sx - ex;
                    const float dy = sy - ey;
                    const float r2 = (dx * dx + dy * dy) / (eyeR * eyeR);
                    if (r2 >= 1.0f) continue;
                    const float q = 1.0f - r2;
                    const float fall = q * q;          // (1−r²)²：中心平、边缘陡
                    const float m = 1.0f + eyeAmp * fall;
                    sx = ex + dx / m;
                    sy = ey + dy / m;
                }
            }

            // ---- ② 瘦脸：向面中线水平压缩 ----
            // 垂直权重是高斯（只动脸颊那一段），侧向权重在 |u|≈0.1 起升、
            // 到 1.55 归零 —— 后者就是 ROI 外扩 30% 的原因。
            if (doThin) {
                const float dyv = (oy - thinYc) / thinSig;
                const float wv = expf(-0.5f * dyv * dyv);
                if (wv > 1e-4f) {
                    const float u = (sx - g.cx) / halfW;
                    const float au = fabsf(u);
                    const float lat = smoothstep01(0.10f, 0.80f, au) *
                                      (1.0f - smoothstep01(0.95f, 1.55f, au));
                    const float k = thinK * wv * lat;
                    if (k > 1e-4f) sx = g.cx + (sx - g.cx) * (1.0f + k);
                }
            }

            // ---- ③ 下巴：下半脸纵向压缩 ----
            // 中线强、两侧弱（lat），否则会连下颌角一起往上提，脸变方。
            // 判据写成 (sy - chinYm)*sgn > 0：bottom-up 下命中视觉**下方**那一半，
            // 之前写成 sy > chinYm，在真机上命中的是视觉最上面 26%（头发）。
            if (doChin && (sy - chinYm) * sgn > 0.0f) {
                const float t = (sy - chinYm) * sgn / chinSpan;
                const float wv = smoothstep01(0.0f, 0.45f, t) *
                                 (1.0f - smoothstep01(0.92f, 1.10f, t));
                if (wv > 1e-4f) {
                    const float u = fabsf((sx - g.cx) / halfW);
                    const float lat = 1.0f - 0.60f * smoothstep01(0.20f, 1.0f, u);
                    const float k = chinK * wv * lat;
                    if (k > 1e-4f) sy = chinYm + (sy - chinYm) * (1.0f + k);
                }
            }

            // ---- 采样 ----
            const float dxo = sx - ox;
            const float dyo = sy - oy;
            unsigned char* dstp = out + (size_t)x * 4;

            if (dxo > -0.02f && dxo < 0.02f && dyo > -0.02f && dyo < 0.02f) {
                memcpy(dstp, srow + (size_t)x * 4, 4);   // 位移可忽略：直拷
                continue;
            }

            float lx = clampf(sx - (float)x0, 0.0f, (float)(rw - 1));
            float ly = clampf(sy - (float)y0, 0.0f, (float)(rh - 1));
            int ix = (int)lx;
            int iy = (int)ly;
            int ix1 = ix + 1;
            int iy1 = iy + 1;
            if (ix1 > rw - 1) ix1 = rw - 1;
            if (iy1 > rh - 1) iy1 = rh - 1;
            const float fx = lx - (float)ix;
            const float fy = ly - (float)iy;

            const unsigned char* p00 = src + (size_t)iy * rowBytes + (size_t)ix * 4;
            const unsigned char* p01 = src + (size_t)iy * rowBytes + (size_t)ix1 * 4;
            const unsigned char* p10 = src + (size_t)iy1 * rowBytes + (size_t)ix * 4;
            const unsigned char* p11 = src + (size_t)iy1 * rowBytes + (size_t)ix1 * 4;

            for (int c = 0; c < 4; c++) {
                const float a = (float)p00[c] * (1.0f - fx) + (float)p01[c] * fx;
                const float b = (float)p10[c] * (1.0f - fx) + (float)p11[c] * fx;
                dstp[c] = (unsigned char)(a * (1.0f - fy) + b * fy + 0.5f);
            }
        }
    }
}

}  // namespace beautycore
