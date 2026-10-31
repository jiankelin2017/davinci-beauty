# -*- coding: utf-8 -*-
"""把「美白实际提亮区域 / 颜色掩码 / 语义皮肤掩码」画成一张四联对比图。

用法
    python probe_phase1_overlay.py <照片> <输出png> [--whiten 100]
"""
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
DEC = os.path.abspath(os.path.join(HERE, "..", "05_解密模型数据"))
sys.path.insert(0, os.path.join(DEC, "_tools"))

import probe_whiten_scope as pws  # noqa: E402
import probe_phase1_whiten_scope as p1  # noqa: E402
from extract_face_assets import FuAI  # noqa: E402

W, H = pws.W, pws.H


def tint(base, mask, rgb, alpha=0.55):
    a = np.asarray(base).astype(np.float32)
    m = (mask.astype(np.float32) * alpha)[:, :, None]
    c = np.array(rgb, dtype=np.float32)[None, None, :]
    return Image.fromarray(np.clip(a * (1 - m) + c * m, 0, 255).astype(np.uint8))


def label(im, text):
    im = im.copy()
    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, im.width, 22], fill=(20, 20, 20))
    d.text((8, 6), text, fill=(255, 255, 255))
    return im


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    photo, out_png = sys.argv[1], sys.argv[2]
    whiten = 100.0
    if "--whiten" in sys.argv:
        whiten = float(sys.argv[sys.argv.index("--whiten") + 1])

    im = p1.fit(Image.open(photo).convert("RGB"), W, H)
    arr = np.asarray(im)
    src = im.convert("RGBA").tobytes()

    fu = FuAI(p1.DLL_FUAI, p1.BUNDLE)
    seg = p1.seg_mask(fu, arr)
    col = p1.color_mask(arr)

    dll = os.environ.get("NAMA_OFX_DLL", p1.DLL_PLUGIN)
    out = p1.run_panel(dict(blur_level=1, color_level=whiten), src, dll)
    o = np.frombuffer(out, dtype=np.uint8).reshape(H, W, 4)[:, :, :3]
    diff = np.abs(o.astype(np.int16) - arr.astype(np.int16)).max(axis=2)
    actual = diff >= 3

    tiles = [
        label(im, "input"),
        label(Image.fromarray(o), "whiten %g (actual output)" % whiten),
        label(tint(im, actual, (255, 60, 60)), "actually brightened %.1f%%" % (100.0 * actual.mean())),
        label(tint(im, col, (60, 200, 90)), "color heuristic mask %.1f%%" % (100.0 * col.mean())),
        label(tint(im, seg > 0.6, (60, 120, 255)), "semantic skin mask %.1f%%" % (100.0 * (seg > 0.6).mean())),
    ]
    gap = 6
    cols, rows = 3, 2
    sheet = Image.new("RGB", (cols * W + (cols - 1) * gap, rows * H + (rows - 1) * gap),
                      (28, 28, 28))
    for i, t in enumerate(tiles):
        x = (i % cols) * (W + gap)
        y = (i // cols) * (H + gap)
        sheet.paste(t, (x, y))
    sheet.save(out_png)
    print("saved", out_png)
    return 0


if __name__ == "__main__":
    sys.exit(main())
