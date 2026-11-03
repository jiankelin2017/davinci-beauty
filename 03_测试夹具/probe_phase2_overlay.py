# -*- coding: utf-8 -*-
"""v1.4 vs v1.51 同图对照：美白实际改动区域 + 蕾丝上衣局部放大。

回答的问题
----------
「非皮肤区不再被提亮」到底长什么样。Phase 1 的 `_phase1_overlay.png` 只画了
v1.4；这张图把同一素材在**两种行为下**各渲染一遍并排摆：

    第一行  原图 / v1.4 输出 / v1.51 输出
    第二行  v1.4 被提亮的区域(红) / v1.51 被提亮的区域(红) / 被修复掉的部分(黄)
    第三行  上面两块放大 3 倍：原图 / v1.4 / v1.51（纹理丢失一眼可见）

因为 `NAMA_REGION_OFF` 必须在插件加载前就设好，两个渲染各起一个子进程。

用法：
    python probe_phase2_overlay.py <照片> <输出png> [--whiten 100]
"""
import os
import subprocess
import sys

import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
DEC = os.path.abspath(os.path.join(HERE, "..", "05_解密模型数据"))
sys.path.insert(0, os.path.join(DEC, "_tools"))

import probe_whiten_scope as pws      # noqa: E402
import probe_phase1_whiten_scope as p1  # noqa: E402
from extract_face_assets import FuAI  # noqa: E402

W, H = pws.W, pws.H


def tint(base, mask, rgb, alpha=0.6):
    a = np.asarray(base).astype(np.float32)
    m = (mask.astype(np.float32) * alpha)[:, :, None]
    c = np.array(rgb, dtype=np.float32)[None, None, :]
    return Image.fromarray(np.clip(a * (1 - m) + c * m, 0, 255).astype(np.uint8))


def label(im, text, sub=None):
    im = im.copy()
    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, im.width, 22], fill=(20, 20, 20))
    d.text((8, 6), text, fill=(255, 255, 255))
    if sub:
        d.rectangle([0, im.height - 20, im.width, im.height], fill=(20, 20, 20))
        d.text((8, im.height - 16), sub, fill=(230, 230, 120))
    return im


def render_child(photo, raw_out, region_off):
    """子进程：只负责渲染并存盘，环境变量在进程启动前设好。"""
    env = dict(os.environ)
    if region_off:
        env["NAMA_REGION_OFF"] = "1"
    else:
        env.pop("NAMA_REGION_OFF", None)
    me = os.path.abspath(__file__)
    cmd = [sys.executable, me, "--child", photo, raw_out]
    r = subprocess.run(cmd, env=env, capture_output=True)
    if r.returncode != 0:
        sys.stderr.write(r.stdout.decode("utf-8", "replace"))
        sys.stderr.write(r.stderr.decode("utf-8", "replace"))
        raise SystemExit("子进程渲染失败 region_off=%s" % region_off)
    return raw_out


def child_render(photo, raw_out):
    im = p1.fit(Image.open(photo).convert("RGB"), W, H)
    src = im.convert("RGBA").tobytes()
    dll = os.environ.get("NAMA_OFX_DLL", p1.DLL_PLUGIN)
    # blur=0、skin_detect=1：只留美白，画面上的任何改动都只能来自美白。
    # 这样两张图的红色区域才严格等于「被提亮的范围」，不掺磨皮的噪声 ——
    # Phase 1 那张图用 blur_level=1 且 skin_detect 被 Driver 置 0，红色里
    # 混了全帧磨皮，反而把「误提亮」这个结论冲淡了。
    out = p1.run_panel(dict(blur_level=0, color_level=100.0, skin_detect=1), src, dll)
    with open(raw_out, "wb") as f:
        f.write(out)
    return 0


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "--child":
        return child_render(sys.argv[2], sys.argv[3])
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    photo, out_png = sys.argv[1], sys.argv[2]

    im = p1.fit(Image.open(photo).convert("RGB"), W, H)
    arr = np.asarray(im)
    src = im.convert("RGBA").tobytes()

    fu = FuAI(p1.DLL_FUAI, p1.BUNDLE)
    seg = p1.seg_mask(fu, arr)

    raw_old = os.path.join(HERE, "_v14_whiten.raw")
    raw_new = os.path.join(HERE, "_v151_whiten.raw")
    render_child(photo, raw_old, region_off=True)
    render_child(photo, raw_new, region_off=False)

    o_old = np.frombuffer(open(raw_old, "rb").read(), dtype=np.uint8)
    o_new = np.frombuffer(open(raw_new, "rb").read(), dtype=np.uint8)
    o_old = o_old.reshape(H, W, 4)[:, :, :3].astype(np.int16)
    o_new = o_new.reshape(H, W, 4)[:, :, :3].astype(np.int16)
    a = arr.astype(np.int16)

    d_old = np.abs(o_old - a).max(axis=2)
    d_new = np.abs(o_new - a).max(axis=2)
    act_old = d_old >= 3
    act_new = d_new >= 3
    fixed = act_old & ~act_new          # v1.4 改了、v1.51 不改了
    skin = seg > 0.5
    wrongly = act_old & ~skin           # v1.4 改了的非皮肤像素

    # ---- 选放大区域：v1.4 误伤最集中的那一块「非皮肤」区域 ----
    # 两个坑都踩过：
    #  * 取「差值最大的单个像素」→ 命中 (0,0) 墙角，一个离群点带偏整个框；
    #  * 取「差值累加最大的网格」→ 命中裸肩（那里本来是真皮肤，v1.51 也在改，
    #    平均差 164，演示不了任何东西）。
    # 现在：网格按「被修复的像素数」打分，且要求该格**平均语义概率 < 0.35**，
    # 也就是必须是「颜色像皮肤、语义说不是」的布料。
    if wrongly.any():
        gw, gh = 8, 6
        best, bxy = -1, (H // 2, W // 2)
        for gy in range(gh):
            for gx in range(gw):
                ys = slice(gy * H // gh, (gy + 1) * H // gh)
                xs = slice(gx * W // gw, (gx + 1) * W // gw)
                if float(seg[ys, xs].mean()) >= 0.35:
                    continue
                n = int(fixed[ys, xs].sum())
                if n > best:
                    best, bxy = n, (gy, gx)
        gy, gx = bxy
        cy = (gy + 0.5) * H // gh
        cx = (gx + 0.5) * W // gw
    else:
        cy, cx = H // 2, W // 2
    zw, zh = 200, 132
    x0 = max(0, min(W - zw, int(cx - zw // 2)))
    y0 = max(0, min(H - zh, int(cy - zh // 2)))
    box = (x0, y0, x0 + zw, y0 + zh)
    Z = 3

    def zoom(img):
        return img.crop(box).resize((zw * Z, zh * Z), Image.NEAREST)

    tiles = [
        label(im, "input", "素材 %dx%d" % (W, H)),
        label(Image.fromarray(o_old.astype(np.uint8)), "v1.4 输出 (美白100)",
              "非皮肤区被提亮 %.1f%%  平均 +%.1f/255"
              % (100.0 * float((act_old & ~skin).sum()) / max((~skin).sum(), 1),
                 float(d_old[act_old & ~skin].mean()) if (act_old & ~skin).any() else 0.0)),
        label(Image.fromarray(o_new.astype(np.uint8)), "v1.51 输出 (美白100)",
              "非皮肤区被提亮 %.1f%%  平均 +%.1f/255"
              % (100.0 * float((act_new & ~skin).sum()) / max((~skin).sum(), 1),
                 float(d_new[act_new & ~skin].mean()) if (act_new & ~skin).any() else 0.0)),

        label(tint(im, act_old, (255, 60, 60)), "v1.4 被提亮 %.1f%%" % (100.0 * act_old.mean()),
              "红=逐字节差>=3"),
        label(tint(im, act_new, (255, 60, 60)), "v1.51 被提亮 %.1f%%" % (100.0 * act_new.mean()),
              "红=逐字节差>=3"),
        label(tint(im, fixed, (255, 210, 40)), "被修复的面积 %.1f%%" % (100.0 * fixed.mean()),
              "黄=v1.4改了、v1.51不改了"),

        label(zoom(im), "放大3x  原图", "框选自 v1.4 误伤最严重处"),
        label(zoom(Image.fromarray(o_old.astype(np.uint8))), "放大3x  v1.4", "纹理被推到接近255"),
        label(zoom(Image.fromarray(o_new.astype(np.uint8))), "放大3x  v1.51", "逐字节保持原样"),
    ]

    gap = 6
    cols, rows = 3, 3
    sheet = Image.new("RGB", (cols * W + (cols - 1) * gap,
                              rows * H + (rows - 1) * gap), (24, 24, 24))
    for i, t in enumerate(tiles):
        x = (i % cols) * (W + gap)
        y = (i // cols) * (H + gap)
        sheet.paste(t, (x, y))
    sheet.save(out_png)

    print("误伤（v1.4 改了的非皮肤像素）  占非皮肤区 %.2f%%  (%d px)"
          % (100.0 * float(wrongly.sum()) / max((~skin).sum(), 1), int(wrongly.sum())))
    print("v1.51 同区误伤               占非皮肤区 %.2f%%  (%d px)"
          % (100.0 * float((act_new & ~skin).sum()) / max((~skin).sum(), 1),
             int((act_new & ~skin).sum())))
    print("被修复的面积                 占全图 %.2f%%" % (100.0 * float(fixed.mean())))
    print("放大区域 box=%s" % (box,))
    print("saved", out_png)
    return 0


if __name__ == "__main__":
    sys.exit(main())
