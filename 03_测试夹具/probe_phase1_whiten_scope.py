# -*- coding: utf-8 -*-
"""Phase 1b：美白/磨皮的实际作用区域 vs 真·语义皮肤区域。

要回答的问题
------------
插件的 `applyWhiten()` 用 Cr/Cb + R>G>B 判肤色（`nama_engine.cpp`），SDK 的磨皮
掩码也被 Phase 0 判为「只看颜色」。但「颜色像皮肤」和「是皮肤」是两回事 ——
薄饼、木桌、米色墙、皮革都可能落在同一个色域里。

所以本脚本不再用合成的纯色块，而是直接拿**真实照片**量三件事：

  1. 插件实际改动了哪些像素（`|out-in| >= 3`）—— 真值
  2. 颜色启发式认为哪些是皮肤 —— 现在的掩码
  3. fuai 语义分割认为哪些是皮肤 —— 理想的掩码

然后算「非皮肤区域被误改的比例」：这就是用户能看见的错。

用法
    python probe_phase1_whiten_scope.py <照片> [--panel whiten=100] [--dll ...]
"""
import ctypes
import os
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
DEC = os.path.abspath(os.path.join(HERE, "..", "05_解密模型数据"))
sys.path.insert(0, os.path.join(DEC, "_tools"))

import probe_whiten_scope as pws  # noqa: E402
from extract_face_assets import FuAI  # noqa: E402

DLL_PLUGIN = (r"C:\Program Files\Common Files\OFX\Plugins"
              r"\DaVinciBeauty.ofx.bundle\Contents\Win64\DaVinciBeauty.ofx")
DLL_FUAI = (r"C:\Program Files\Common Files\OFX\Plugins"
            r"\DaVinciBeauty.ofx.bundle\Contents\Win64\fuai.dll")
BUNDLE = os.path.join(DEC, "ai_face_processor_pc", "ai_face_processor_pc.bundle")

W, H = pws.W, pws.H          # 夹具固定画幅，与其它探针一致


def fit(im, w, h):
    """cover 裁剪到 w×h，避免拉伸变形影响人脸检测。"""
    s = max(w / float(im.width), h / float(im.height))
    im2 = im.resize((max(1, int(im.width * s + 0.5)),
                     max(1, int(im.height * s + 0.5))), Image.LANCZOS)
    l = (im2.width - w) // 2
    t = (im2.height - h) // 2
    return im2.crop((l, t, l + w, t + h))


def color_mask(arr):
    """复刻 applyWhiten 第一 pass 的判据。"""
    R = arr[:, :, 0].astype(np.int32)
    G = arr[:, :, 1].astype(np.int32)
    B = arr[:, :, 2].astype(np.int32)
    Y = (66 * R + 129 * G + 25 * B + 128) >> 8
    Cb = ((-38 * R - 74 * G + 112 * B + 128) >> 8) + 128
    Cr = ((112 * R - 94 * G - 18 * B + 128) >> 8) + 128
    return ((Cr >= 133) & (Cr <= 173) & (Cb >= 77) & (Cb <= 127) &
            (R > G) & (G > B) & (R > 80) & (Y > 60) & (Y < 235))


def seg_mask(fu, arr):
    """fuai 皮肤分割回放到 arr 尺寸。"""
    fu.process(np.ascontiguousarray(arr))
    sk = fu.skin_mask()                                  # (256,192)
    return np.asarray(Image.fromarray((sk * 255).astype(np.uint8))
                      .resize((arr.shape[1], arr.shape[0]), Image.BILINEAR)) / 255.0


def run_panel(panel, src, dll):
    drv = pws.Driver(dll)
    drv.set_params(**panel)
    drv.render(src)
    _, out = drv.render(src)
    return out


def stats(name, actual, mask):
    """mask 命中的像素里有多少真被改了；以及非 mask 区被改了多少。"""
    inside = float((actual & mask).sum())
    outside = float((actual & ~mask).sum())
    m = float(mask.sum())
    tot = float(actual.size)
    print("  %-22s 命中区 %6.2f%% | 命中区内改动 %5.1f%% | 命中区外改动 %5.1f%% (占全图 %.2f%%)"
          % (name, 100.0 * m / tot,
             100.0 * inside / max(m, 1.0),
             100.0 * outside / max(tot - m, 1.0),
             100.0 * outside / tot))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    photo = sys.argv[1]
    dll = os.environ.get("NAMA_OFX_DLL", DLL_PLUGIN)
    whiten = 100.0
    if "--whiten" in sys.argv:
        whiten = float(sys.argv[sys.argv.index("--whiten") + 1])

    im = fit(Image.open(photo).convert("RGB"), W, H)
    arr = np.asarray(im)
    src = im.convert("RGBA").tobytes()

    print("素材：%s  (%dx%d)" % (os.path.basename(photo), W, H))
    fu = FuAI(DLL_FUAI, BUNDLE)
    seg = seg_mask(fu, arr)
    faces = fu.GetNum(fu.process(np.ascontiguousarray(arr)))
    print("  fuai 检出人脸 = %d" % faces)

    col = color_mask(arr)
    skin_seg = seg > 0.6
    nonskin = seg < 0.3

    print("")
    print("== 掩码覆盖对比 ==")
    print("  颜色启发式命中        %6.2f%%" % (100.0 * col.mean()))
    print("  语义分割命中(>0.6)    %6.2f%%" % (100.0 * skin_seg.mean()))
    inter = float((col & skin_seg).sum())
    print("  两者交集              %6.2f%%" % (100.0 * inter / col.size))
    print("  颜色说是、语义说不是  %6.2f%%   <-- 现在的误判面积"
          % (100.0 * float((col & ~skin_seg).sum()) / col.size))

    # ---- 美白：面板 whitening=100，磨皮压到 1 只为让管线跑起来 ----
    print("")
    print("== 美白 面板=%g ==" % whiten)
    out = run_panel(dict(blur_level=1, color_level=whiten), src, dll)
    o = np.frombuffer(out, dtype=np.uint8).reshape(H, W, 4)[:, :, :3].astype(np.int16)
    a = arr.astype(np.int16)
    diff = np.abs(o - a).max(axis=2)
    actual = diff >= 3
    print("  实际改动像素          %6.2f%%" % (100.0 * actual.mean()))
    stats("颜色启发式掩码", actual, col)
    stats("语义皮肤掩码", actual, skin_seg)
    # 真值落在非皮肤区多少 —— 这就是「木桌被提亮」的量
    ns = float(nonskin.sum())
    print("  非皮肤区被提亮        %6.2f%%  (面积占全图 %.1f%%)"
          % (100.0 * float((actual & nonskin).sum()) / max(ns, 1.0),
             100.0 * ns / actual.size))
    if (actual & nonskin).any():
        d_ns = diff[actual & nonskin]
        print("  非皮肤区平均提亮      %6.1f / 255" % float(d_ns.mean()))

    # ---- 磨皮：看 SDK 掩码是不是也只认颜色 ----
    print("")
    print("== 磨皮 面板=100（双边，只磨皮肤）==")
    out2 = run_panel(dict(blur_level=100, skin_detect=1), src, dll)
    o2 = np.frombuffer(out2, dtype=np.uint8).reshape(H, W, 4)[:, :, :3].astype(np.int16)
    d2 = np.abs(o2 - a).max(axis=2)
    act2 = d2 >= 3
    print("  实际改动像素          %6.2f%%" % (100.0 * act2.mean()))
    stats("颜色启发式掩码", act2, col)
    stats("语义皮肤掩码", act2, skin_seg)
    print("  非皮肤区被磨          %6.2f%%  (面积占全图 %.1f%%)"
          % (100.0 * float((act2 & nonskin).sum()) / max(ns, 1.0),
             100.0 * ns / act2.size))
    return 0


if __name__ == "__main__":
    sys.exit(main())
