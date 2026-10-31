# -*- coding: utf-8 -*-
"""挑「肤色但不是皮肤」的素材 —— 用来判定磨皮/美白掩码是颜色还是语义。

为什么要这一步
--------------
Phase 0 用「纯色噪点块」当假肤色（PALE / SKINTONE），但那两块**本身就是肤色**，
语义分割网络同样会判成皮肤 —— 所以那组实验无法区分「颜色判据」和「语义分割」。
能区分两者的只有一种素材：**颜色像皮肤、但语义上不是皮肤的东西**，比如木桌、
米色墙、皮革沙发。

本脚本用两条判据给每张照片打分：
  color_skin%  插件现在用的 Cr/Cb + R>G>B 启发式命中的像素占比
  seg_skin%    fuai 皮肤分割（语义）命中 >0.6 的像素占比
两者的差值就是「颜色说像皮肤、语义说不是」的面积 —— 差值越大，素材越有判别力。

用法
    python find_skinlike_material.py <照片目录> [--top 5]
"""
import ctypes
import os
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
DEC = os.path.abspath(os.path.join(HERE, "..", "_archive", "05_解密模型数据"))
sys.path.insert(0, os.path.join(DEC, "_tools"))
from extract_face_assets import FuAI  # noqa: E402

DLL = (r"C:\Program Files\Common Files\OFX\Plugins"
       r"\DaVinciBeauty.ofx.bundle\Contents\Win64\fuai.dll")
BUNDLE = os.path.join(DEC, "ai_face_processor_pc", "ai_face_processor_pc.bundle")

LONG_SIDE = 720   # 只做素材筛选，降采样足够，且让两条判据在同一尺度上比


def color_mask(arr):
    """复刻 nama_engine.cpp 的 applyWhiten 第一 pass 判据（Cr/Cb + R>G>B）。"""
    R = arr[:, :, 0].astype(np.int32)
    G = arr[:, :, 1].astype(np.int32)
    B = arr[:, :, 2].astype(np.int32)
    Y = (66 * R + 129 * G + 25 * B + 128) >> 8
    Cb = ((-38 * R - 74 * G + 112 * B + 128) >> 8) + 128
    Cr = ((112 * R - 94 * G - 18 * B + 128) >> 8) + 128
    return ((Cr >= 133) & (Cr <= 173) & (Cb >= 77) & (Cb <= 127) &
            (R > G) & (G > B) & (R > 80) & (Y > 60) & (Y < 235))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    root = sys.argv[1]
    top = 5
    if "--top" in sys.argv:
        top = int(sys.argv[sys.argv.index("--top") + 1])

    fu = FuAI(DLL, BUNDLE)
    files = sorted(f for f in os.listdir(root)
                   if f.lower().endswith((".jpg", ".jpeg", ".png")))
    rows = []
    for i, fn in enumerate(files):
        path = os.path.join(root, fn)
        try:
            im = Image.open(path).convert("RGB")
        except Exception:
            continue
        s = LONG_SIDE / float(max(im.size))
        if s < 1.0:
            im = im.resize((max(1, int(im.width * s)), max(1, int(im.height * s))),
                           Image.BILINEAR)
        arr = np.asarray(im)
        res = fu.process(np.ascontiguousarray(arr))
        n = fu.GetNum(res)
        sk = fu.skin_mask()                     # (256,192) float32
        # 皮肤分割是整图各向异性拉伸，这里按同比例回放到当前尺寸再算占比
        sk_full = np.asarray(Image.fromarray((sk * 255).astype(np.uint8))
                             .resize((im.width, im.height), Image.BILINEAR)) / 255.0
        seg_pct = 100.0 * float((sk_full > 0.6).mean())
        col_pct = 100.0 * float(color_mask(arr).mean())
        rows.append((fn, n, col_pct, seg_pct, col_pct - seg_pct))
        if (i + 1) % 10 == 0:
            print("  ... %d/%d" % (i + 1, len(files)), flush=True)

    print("")
    print("%-16s %5s %10s %10s %10s" % ("文件", "faces", "颜色命中%", "语义命中%", "差值"))
    print("-" * 56)
    for fn, n, c, sg, d in sorted(rows, key=lambda r: -r[4])[:top]:
        print("%-16s %5d %9.1f%% %9.1f%% %9.1f" % (fn, n, c, sg, d))
    print("")
    print("差值大 = 「颜色像皮肤、语义说不是」面积大 = 判别力强的素材")
    return 0


if __name__ == "__main__":
    sys.exit(main())
