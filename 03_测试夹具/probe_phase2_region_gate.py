# -*- coding: utf-8 -*-
"""Phase 2 验收：语义掩码是否真的把效果关在皮肤里。

和 Phase 1 的 probe_phase1_whiten_scope.py 的区别
--------------------------------------------------
那个脚本用 `set_params(blur_level=1, color_level=100)`，而 Driver.set_params
会把**没指定的参数一律置 0** —— 包括 skin_detect。所以它测的是
「limitToSkin 关掉、只靠掩码做美白」这一种组合，看不到合成阶段的效果。

这个脚本按真实使用姿势测三种组合：
  A. 只美白（blur=0, whiten=100, skin_detect=1）
     → 掩码外必须逐字节不动，掩码内必须明显提亮
  B. 只磨皮（blur=60, whiten=0, skin_detect=1）
     → 掩码外必须逐字节不动（这正是 v1.4 做不到的：双边档下非皮肤区 27~47% 被磨）
  C. 美白+磨皮同时开（默认使用姿势）

掩码用 fuai 现算，并且**按插件一样的 RGBA 格式**（fmt=3, stride=w*4）算一遍，
和 RGB（fmt=1）对比，确认两种取法结果一致。

用法：
    python probe_phase2_region_gate.py <照片> [--dll <插件dll>] [--report out.txt]
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

import probe_whiten_scope as pws          # noqa: E402
import probe_phase1_whiten_scope as p1    # noqa: E402
from extract_face_assets import FuAI      # noqa: E402

W, H = p1.W, p1.H
BUNDLE = p1.BUNDLE
DLL_FUAI = p1.DLL_FUAI

P = ctypes.c_void_p
I = ctypes.c_int


def seg_rgba(fu, rgba_bytes):
    """按插件的取法（RGBA / stride=w*4）算一遍语义掩码，返回 (h,w) float32。"""
    buf = ctypes.create_string_buffer(rgba_bytes, len(rgba_bytes))
    fu.InitView(fu._view, 3, W, H, 0, None, W * 4, None, 0, None, 0)
    fu.SetViewData(fu._view, 0, ctypes.cast(buf, P))
    fu.Process(fu.proc, fu._view, fu._res)
    n = fu.GetNum(fu._res)
    w, h = I(0), I(0)
    extra = (ctypes.c_int * 4)()
    p = fu.GetSkinSeg(fu.proc, fu._view, ctypes.byref(w), ctypes.byref(h),
                      ctypes.cast(extra, P))
    if not p or not w.value or not h.value:
        return None, n
    a = np.ctypeslib.as_array(ctypes.cast(p, ctypes.POINTER(ctypes.c_float)),
                              shape=(w.value * h.value,))
    return a.reshape(h.value, w.value).copy(), n


def render(dll, src, **panel):
    drv = pws.Driver(dll)
    drv.set_params(**panel)
    drv.render(src)                 # 第一帧：让引擎预热 / 出掩码
    _, out = drv.render(src)        # 第二帧：取结果
    return out


def measure(tag, out, src_bytes, arr, seg, rows):
    o = np.frombuffer(out, dtype=np.uint8).reshape(H, W, 4)[:, :, :3].astype(np.int16)
    a = arr.astype(np.int16)
    diff = np.abs(o - a).max(axis=2)
    inside = seg >= 0.5
    outside = seg < 0.1
    # 掩码外：改动像素占比 / 最大逐字节差
    n_out = int(outside.sum())
    changed_out = int((diff[outside] >= 1).sum())
    changed_out3 = int((diff[outside] >= 3).sum())
    pct1 = 100.0 * changed_out / max(n_out, 1)
    pct3 = 100.0 * changed_out3 / max(n_out, 1)
    max_out = int(diff[outside].max()) if n_out else 0
    n_in = int(inside.sum())
    mean_in = float(diff[inside].mean()) if n_in else 0.0
    max_in = int(diff[inside].max()) if n_in else 0
    rows.append("  %-26s 掩码内 %5.1f%% 平均差 %5.2f 最大 %3d | "
                "掩码外改动>=1 %5.2f%%  >=3 %5.2f%%  最大 %3d"
                % (tag, 100.0 * n_in / diff.size, mean_in, max_in, pct1, pct3, max_out))
    return pct1, pct3, max_out, mean_in


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    photo = sys.argv[1]
    dll = os.environ.get("NAMA_OFX_DLL", p1.DLL_PLUGIN)

    im = p1.fit(Image.open(photo).convert("RGB"), W, H)
    arr = np.asarray(im)
    rgba = im.convert("RGBA").tobytes()
    rows = []
    rows.append("素材：%s  (%dx%d)" % (os.path.basename(photo), W, H))

    fu = FuAI(DLL_FUAI, BUNDLE)
    seg_rgb = p1.seg_mask(fu, arr)                     # 已放大到 (H,W)
    faces_rgb = fu.GetNum(fu.process(np.ascontiguousarray(arr)))
    seg_gba, faces_rgba = seg_rgba(fu, rgba)
    if seg_gba is None:
        rows.append("fuai 没给出皮肤掩码（faces=%d）—— 无法验收" % faces_rgba)
        txt = "\n".join(rows)
        print(txt)
        return 1
    # 插件是把 192×256 双线性放大到全分辨率，这里照做，才能和插件对齐
    seg_full = np.asarray(
        Image.fromarray(seg_gba.astype(np.float32), mode="F")
             .resize((W, H), Image.BILINEAR))
    rows.append("fuai faces: RGB取法=%d RGBA取法=%d  掩码原始 %dx%d"
                % (faces_rgb, faces_rgba, seg_gba.shape[1], seg_gba.shape[0]))
    # 两种取法必须一致，否则说明插件的 fmt=3 有问题
    d = np.abs(seg_rgb - seg_full)
    rows.append("两种取法掩码最大差=%.4f 平均差=%.5f  (>0.5 覆盖 RGB %.1f%% / RGBA %.1f%%)"
                % (float(d.max()), float(d.mean()),
                   100.0 * float((seg_rgb >= 0.5).mean()),
                   100.0 * float((seg_full >= 0.5).mean())))

    seg = seg_full
    rows.append("")
    rows.append("== A. 只美白（blur=0 whiten=100 skin_detect=1）==")
    o = render(dll, rgba, blur_level=0, color_level=100, skin_detect=1)
    a1 = measure("whiten=100", o, rgba, arr, seg, rows)

    rows.append("")
    rows.append("== B. 只磨皮（blur=60 whiten=0 skin_detect=1）==")
    o = render(dll, rgba, blur_level=60, color_level=0, skin_detect=1)
    b1 = measure("blur=60", o, rgba, arr, seg, rows)

    rows.append("")
    rows.append("== C. 美白+磨皮（blur=60 whiten=100 skin_detect=1）==")
    o = render(dll, rgba, blur_level=60, color_level=100, skin_detect=1)
    c1 = measure("blur=60+whiten=100", o, rgba, arr, seg, rows)

    rows.append("")
    rows.append("== 判定 ==")
    # 掩码外允许极少量改动：掩码边界经双线性放大后会有过渡带，
    # 且 seg<0.1 与 >=0.5 之间留了 0.1~0.5 的缓冲。>=3 的改动应接近 0。
    ok = True
    for tag, r in (("A", a1), ("B", b1), ("C", c1)):
        good = (r[1] <= 3.0)
        ok = ok and good
        rows.append("  %s 掩码外改动>=3 占比 %.2f%%  -> %s（阈值 <=3%%）"
                    % (tag, r[1], "PASS" if good else "FAIL"))
    good = a1[3] >= 3.0
    if faces_rgba <= 0:
        # 没人脸时插件按 v1.4 的门槛「不提亮」，掩码内平均提亮 0 是**预期**，
        # 不是失败。这里只记录，不参与判定。
        rows.append("  A 掩码内平均提亮 %.2f  -> N/A（faces=0，按设计不提亮）" % a1[3])
    else:
        ok = ok and good
        rows.append("  A 掩码内平均提亮 %.2f  -> %s（阈值 >=3）"
                    % (a1[3], "PASS" if good else "FAIL"))
    rows.append("总判定：%s" % ("PASS" if ok else "FAIL"))

    txt = "\n".join(rows)
    print(txt)
    rp = os.path.join(HERE, "_phase2_gate_report.txt")
    if "--report" in sys.argv:
        rp = sys.argv[sys.argv.index("--report") + 1]
    with open(rp, "w", encoding="utf-8") as f:
        f.write(txt + "\n")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
