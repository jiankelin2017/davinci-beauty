# -*- coding: utf-8 -*-
"""dump_analyze.py —— 给真机「白块」做像素级定性（1.1.2.1 建立，1.1.3.0 支持暂停帧）。

配套插件里的 BEAUTY_DUMP_OUT：它把 src / dst / mask 三张图落盘成 PPM/PGM，
本脚本读回来回答四个问题 —— 这四个问题靠日志和推理都答不出来：

  1) 变化区域（dst−src）的形状：矩形 / 椭圆 / 不规则轮廓？
     → 填充率 = 变化像素 ÷ bbox 面积。矩形≈1.00，椭圆≈0.785，轮廓会更低。
  2) 变化区域内的掩码分布：全 255（硬边）还是有梯度（羽化已生效）？
  3) 变化区域在 src 里是什么颜色：肤色（R>G>B、有一定饱和）还是中性衣物？
  4) 提亮幅度：中位 +N 级。美白 LUT 中调只有 8~10 级，超出就不是它。

用法：
    python dump_analyze.py [目录] [seq|re]
    python dump_analyze.py                      # 默认 C:\\ProgramData\\DaVinciBeauty\\dump 的第 0 帧
    python dump_analyze.py D:\\tmp\\dump 1      # 指定目录与序号
    python dump_analyze.py D:\\tmp\\dump re     # 读 dump_re_*（暂停后强制重渲染那一帧）

1.1.3.0 起插件有两组导出：
    dump_N_*   (N=0..5)  播放阶段的前 6 帧，名额常被开头镜头用光
    dump_re_*            暂停后按 ← → 重渲染同一帧时覆写 —— 停在哪帧就是哪帧
分析白块优先用 `re`。

产出：
    终端报告 + <目录>/analysis_<seq>.png（原图 + 掩码红叠 + 变化框）
              <目录>/analysis_<seq>_diff.png（变化幅度热图，8 倍增益）

判读对照（配合 docs/history/history-progress-1x.md 的 1.1.2.1 节）：
    src 有饱和 + R>G>B  → 那块是真皮肤，修法是加长脖子带下缘羽化
    src 接近中性灰/米色 → 那块被误判成肤色，修法是把脖子从色调里剔除
"""

import os
import sys

import numpy as np
from PIL import Image

DEFAULT_DIR = r"C:\ProgramData\DaVinciBeauty\dump"


def load(d, tag):
    # tag 可以是数字（dump_0_*）或字符串 "re"（dump_re_*），文件名一律用 str 拼
    src_p = os.path.join(d, "dump_%s_src.ppm" % tag)
    dst_p = os.path.join(d, "dump_%s_dst.ppm" % tag)
    msk_p = os.path.join(d, "dump_%s_mask.pgm" % tag)
    for p in (src_p, dst_p, msk_p):
        if not os.path.exists(p):
            raise SystemExit("缺少文件: %s" % p)
    src = np.asarray(Image.open(src_p).convert("RGB")).astype(np.int16)
    dst = np.asarray(Image.open(dst_p).convert("RGB")).astype(np.int16)
    msk = np.asarray(Image.open(msk_p).convert("L")).astype(np.int16)
    return src, dst, msk


def pct(a, b):
    return 100.0 * a / b if b else 0.0


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_DIR
    frameTag = sys.argv[2] if len(sys.argv) > 2 else "0"   # "0".."5" 或 "re"
    src, dst, msk = load(d, frameTag)
    h, w = msk.shape

    print("=" * 68)
    print("帧 %s   %dx%d   来源 %s" % (frameTag, w, h, d))
    print("=" * 68)

    diff = dst - src
    mag = np.abs(diff).max(axis=2)
    total = w * h

    # ---- 1. 变化区域总体 ---------------------------------------------------
    print("\n[1] 改动量分布")
    for thr in (1, 2, 4, 8, 16, 32):
        n = int((mag > thr).sum())
        print("    幅度 >%-3d : %8d px  (%6.3f%%)" % (thr, n, pct(n, total)))

    ch = mag > 2
    n_ch = int(ch.sum())
    if n_ch == 0:
        print("\n    本帧没有任何改动 —— 该帧参数全 0，或没跑色调/磨皮。")
        return

    # ---- 2. 形状 -----------------------------------------------------------
    ys, xs = np.nonzero(ch)
    y0, y1, x0, x1 = int(ys.min()), int(ys.max()), int(xs.min()), int(xs.max())
    bw, bh = x1 - x0 + 1, y1 - y0 + 1
    fill = n_ch / float(bw * bh)
    shape = "矩形" if fill > 0.93 else ("椭圆" if fill > 0.70 else "不规则/轮廓")
    print("\n[2] 变化区域形状")
    print("    bbox   : x %d..%d   y %d..%d   (%dx%d)" % (x0, x1, y0, y1, bw, bh))
    print("    像素数 : %d    bbox 面积 %d    填充率 %.3f  → 形状更像「%s」"
          % (n_ch, bw * bh, fill, shape))

    # ---- 3. 掩码与变化的重合度 ---------------------------------------------
    print("\n[3] 掩码 ↔ 变化 重合度")
    mask_on = msk > 0
    n_on = int(mask_on.sum())
    print("    掩码非零     : %8d px (%6.3f%%)" % (n_on, pct(n_on, total)))
    both = mask_on & ch
    print("    掩码内且变化 : %8d px   占掩码内 %6.2f%%" % (int(both.sum()), pct(both.sum(), n_on)))
    print("    掩码内未变化 : %8d px   占掩码内 %6.2f%%"
          % (int((mask_on & ~ch).sum()), pct(int((mask_on & ~ch).sum()), n_on)))
    out_ch = int((~mask_on & ch).sum())
    tag = " ← 掩码外也变了，属异常" if out_ch else "  （掩码外零改动，符合设计）"
    print("    掩码外却变化 : %8d px%s" % (out_ch, tag))

    print("\n    变化区掩码值直方图：")
    hist, edges = np.histogram(msk[ch], bins=[0, 1, 32, 64, 128, 192, 254, 256])
    for i, n in enumerate(hist):
        lo, hi = int(edges[i]), int(edges[i + 1])
        if n == 0:
            continue
        bar = "#" * int(round(40.0 * n / max(hist)))
        print("      %3d..%-3d : %8d  %5.1f%%  %s" % (lo, hi - 1, int(n), pct(n, n_ch), bar))

    full = int((msk[ch] >= 250).sum())
    print("    其中满值(≥250) 占变化区 %.1f%%  → %s"
          % (pct(full, n_ch),
             "边界是硬的（羽化没进到这块）" if pct(full, n_ch) > 80 else "带梯度（羽化已生效）"))

    # ---- 4. 那块到底什么颜色 -----------------------------------------------
    print("\n[4] 变化区在 src 里的颜色（判断皮肤 vs 衣物）")
    s_ch = src[ch]
    mean_rgb = s_ch.mean(axis=0)
    med_rgb = np.median(s_ch, axis=0)
    sat = (s_ch.max(axis=1) - s_ch.min(axis=1)).astype(np.float64)
    y_lum = 0.299 * s_ch[:, 0] + 0.587 * s_ch[:, 1] + 0.114 * s_ch[:, 2]
    print("    src 均值 RGB  : (%.1f, %.1f, %.1f)" % tuple(mean_rgb))
    print("    src 中位 RGB  : (%.1f, %.1f, %.1f)" % tuple(med_rgb))
    print("    饱和度 max-min: 中位 %.1f   均值 %.1f" % (np.median(sat), sat.mean()))
    print("    亮度 Y        : 中位 %.1f" % np.median(y_lum))
    r, g, b = med_rgb
    rg = r - g
    gb = g - b
    print("    R-G = %+.1f   G-B = %+.1f" % (rg, gb))

    # 参照：全图满掩码区（mask≥250）的中位颜色。若它与「变化区」明显不同，
    # 说明色调只在掩码的一部分上真正生效（例如脖子被判成肤色但 skinWeight 很低）。
    full_area = msk >= 250
    if full_area.sum() > 1000:
        ref = np.median(src[full_area], axis=0)
        print("    参照：全图满掩码区中位 RGB = (%.1f, %.1f, %.1f)" % tuple(ref))

    # 判据：真皮肤通常 R>G>B 且饱和度 15~60、亮度 60~220
    skin_like = (rg > 5) and (gb > 2) and (10 <= np.median(sat) <= 90) and (50 <= np.median(y_lum) <= 230)
    print("\n    判定：%s"
          % ("偏「真皮肤」（R>G>B、有饱和、亮度在肤色区间）" if skin_like
             else "偏「非皮肤」（接近中性色或不符合肤色梯度）"))

    # 变化区外一圈的对照（同高度、紧邻外侧 8px）
    pad = 8
    oy0, oy1 = max(0, y0 - pad), min(h, y1 + pad + 1)
    ox0, ox1 = max(0, x0 - pad), min(w, x1 + pad + 1)
    ring = np.zeros_like(ch)
    ring[oy0:oy1, ox0:ox1] = True
    ring[y0:y1 + 1, x0:x1 + 1] = False
    if ring.sum() > 100:
        rr = np.median(src[ring], axis=0)
        print("    对照：bbox 外 8px 环带中位 RGB = (%.1f, %.1f, %.1f)" % tuple(rr))
        print("          ΔR=%+.1f ΔG=%+.1f ΔB=%+.1f （与块内的差异，越小越说明「同一种材质」）"
              % tuple(med_rgb - rr))

    # ---- 5. 提亮幅度 -------------------------------------------------------
    print("\n[5] 改动幅度（dst-src）")
    for c, name in enumerate("RGB"):
        v = diff[..., c][ch]
        print("    %s: 中位 %+6.1f   均值 %+6.1f   最小 %+5d   最大 %+5d"
              % (name, np.median(v), v.mean(), v.min(), v.max()))
    lum_d = 0.299 * diff[..., 0] + 0.587 * diff[..., 1] + 0.114 * diff[..., 2]
    print("    亮度 ΔY: 中位 %+.1f   90 分位 %+.1f"
          % (np.median(lum_d[ch]), np.percentile(lum_d[ch], 90)))

    # ---- 6. 出图 -----------------------------------------------------------
    vis = src.astype(np.float32)
    a = 0.45 * (msk.astype(np.float32) / 255.0)[..., None]
    red = np.array([255.0, 0.0, 0.0], dtype=np.float32)
    vis = vis * (1.0 - a) + red * a
    vis = np.clip(vis, 0, 255).astype(np.uint8)
    vis[y0:y1 + 1, x0, :] = [0, 255, 0]
    vis[y0:y1 + 1, x1, :] = [0, 255, 0]
    vis[y0, x0:x1 + 1, :] = [0, 255, 0]
    vis[y1, x0:x1 + 1, :] = [0, 255, 0]
    out_png = os.path.join(d, "analysis_%s.png" % frameTag)
    Image.fromarray(vis).save(out_png)

    heat = np.clip(mag.astype(np.float32) * 8.0, 0, 255).astype(np.uint8)
    heat_png = os.path.join(d, "analysis_%s_diff.png" % frameTag)
    Image.fromarray(heat).save(heat_png)

    print("\n[6] 出图")
    print("    %s   （原图 + 掩码红叠 + 绿框）" % out_png)
    print("    %s   （变化幅度热图，8 倍增益）" % heat_png)


if __name__ == "__main__":
    main()
