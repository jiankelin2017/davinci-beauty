# -*- coding: utf-8 -*-
"""color_level_v2 通道的强度/作用域扫描 + ASCII 密度图。

R6 已证：use_new_whiten=1 后 color_level_v2 才生效，且改动集中在人脸列
但有背景渗出（out_av 5.88）。本脚本回答两个问题：
  1. V 强度下降时，人脸/背景比例如何变化（找 out≈0 的可用强度）？
  2. skin_detect / lower_quality 能否收紧背景渗出？

每行 = 独立的 ref(V=0) vs test(V=v) 差分，两边面板完全相同（blur=100
保证非全 0、跑 SDK，且 blur 效果在差分里精确抵消）。

用法: py sweep_v2.py [照片]
"""
import ctypes
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import mock_host as mh  # noqa: E402

mh.DEBUG = False

W = 640
H = 394
mh.W = W
mh.H = H

GW, GH = 80, 30
RAMP = " .:-=+*#%@"
EXTRA_FILE = os.path.join(HERE, "_sweep_extra.txt")
PHOTO = os.environ.get("AB_PHOTO",
                       os.path.expanduser("~") + r"\Pictures\A7300538.jpg")

CANDS = ["use_new_whiten", "color_level_v2", "whiten_v2", "beauty_mask_v",
         "lower_quality", "manual_scale"]


def extra_text(**kw):
    vals = {k: 0 for k in CANDS}
    vals.update(kw)
    parts = []
    for k in CANDS:
        v = vals[k]
        parts.append("%s=%s" % (k, ("%g" % v) if isinstance(v, float) else str(int(v))))
    return ";".join(parts)


def set_extra(text):
    with open(EXTRA_FILE, "w", encoding="utf-8") as f:
        f.write(text or "")


def load_frame(photo):
    from PIL import Image
    im = Image.open(photo).convert("RGB")
    sw, sh = im.size
    s = min(W / float(sw), H / float(sh))
    nw, nh = max(1, int(sw * s)), max(1, int(sh * s))
    im = im.resize((nw, nh), Image.LANCZOS)
    canvas = Image.new("RGBA", (W, H), (0, 0, 0, 255))
    canvas.paste(im.convert("RGBA"), ((W - nw) // 2, (H - nh) // 2))
    return canvas.tobytes()


class Driver(object):
    def __init__(self, dll):
        self.lib = ctypes.WinDLL(dll)
        getn = self.lib.OfxGetNumberOfPlugins
        getn.restype = ctypes.c_int
        getn.argtypes = []
        getp = self.lib.OfxGetPlugin
        getp.restype = ctypes.POINTER(mh.OfxPlugin)
        getp.argtypes = [ctypes.c_int]
        p = getp(0).contents
        SETHOST = ctypes.CFUNCTYPE(None, ctypes.POINTER(mh.OfxHost))
        SETHOST(p.setHost)(ctypes.byref(mh.HOST))
        MAIN = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_char_p, ctypes.c_void_p,
                                ctypes.c_void_p, ctypes.c_void_p)
        self.main = MAIN(p.mainEntry)
        self.eff = mh.Effect()
        self.eff_h = mh.newh(self.eff)
        self.main(mh.kOfxActionLoad.encode(), None, None, None)
        self.main(mh.kOfxActionDescribe.encode(), self.eff_h, None, None)
        self.main(mh.kOfxImageEffectActionDescribeInContext.encode(),
                  self.eff_h, None, None)
        self.main(mh.kOfxActionCreateInstance.encode(), self.eff_h, None, None)
        sclip = self.eff.clips.get("Source")
        if sclip:
            sclip.props.set("OfxImageEffectPropRegionOfDefinition", 0, [0, 0, W, H])
            sclip.props.set("OfxImageEffectPropPixelDepth", 0, "OfxBitDepthByte")
            sclip.props.set("OfxImageEffectPropComponents", 0, "OfxImageComponentRGBA")
            sclip.props.set("OfxImagePropPixelAspectRatio", 0, 1.0)
        self.inargs = mh.PropSet("inargs")
        self.inargs.set("OfxImageEffectPropRenderWindow", 0, [0, 0, W, H])
        self.inargs.set("OfxImageEffectPropRenderScale", 0, [1.0, 1.0])
        self.inargs_h = mh.newh(self.inargs)
        self.frame = 0

    def set_params(self, **kw):
        for nm, pm in self.eff.params.items():
            if pm.ptype == "OfxParamTypePage":
                continue
            mh.pset(pm, kw.get(nm, 0))

    def render(self, src):
        self.frame += 1
        self.inargs.set("OfxPropTime", 0, 10.0 + self.frame * 0.25)
        mh.SRC_BUF = ctypes.create_string_buffer(src, len(src))
        mh.DST_BUF = ctypes.create_string_buffer(b"\x5a" * len(src), len(src))
        mh.DEPTH = "OfxBitDepthByte"
        st = self.main(mh.kOfxImageEffectActionRender.encode(), self.eff_h,
                       self.inargs_h, None)
        return st, mh.DST_BUF.raw

    def render_settled(self, src, n=3):
        out = None
        for _ in range(n):
            _, out = self.render(src)
        return out


def delta_map(a, b):
    out = bytearray(len(a) // 4)
    for i in range(0, len(a) - 3, 4):
        d = max(abs(a[i] - b[i]), abs(a[i + 1] - b[i + 1]),
                abs(a[i + 2] - b[i + 2]))
        out[i // 4] = min(255, d)
    return out


def split_stats(dm, mask, thr=3):
    fi = fo = 0
    fn = on = 0
    fchg = ochg = 0
    changed = 0
    minx, miny, maxx, maxy = 10 ** 9, 10 ** 9, -1, -1
    for p, d in enumerate(dm):
        if d >= thr:
            changed += 1
            x, y = p % W, p // W
            if x < minx: minx = x
            if x > maxx: maxx = x
            if y < miny: miny = y
            if y > maxy: maxy = y
        if mask[p]:
            fi += d; fn += 1
            if d >= thr: fchg += 1
        else:
            fo += d; on += 1
            if d >= thr: ochg += 1
    corners = []
    for (cx, cy) in ((2, 2), (W - 3, 2), (2, H - 3), (W - 3, H - 3)):
        corners.append(dm[cy * W + cx])
    n = len(dm)
    bb = None if maxx < 0 else [minx, miny, maxx, maxy]
    return {"changed": changed, "pct": round(100.0 * changed / n, 2),
            "bbox": bb, "corners": corners,
            "face_av": round(fi / max(fn, 1), 2), "face_pct": round(100.0 * fchg / max(fn, 1), 1),
            "out_av": round(fo / max(on, 1), 2), "out_pct": round(100.0 * ochg / max(on, 1), 1)}


def show_grid(dm, face, title):
    print(title)
    print("  +" + "-" * GW + "+")
    for gy in range(GH):
        y0 = gy * H // GH
        y1 = max(y0 + 1, (gy + 1) * H // GH)
        line = []
        for gx in range(GW):
            x0 = gx * W // GW
            x1 = max(x0 + 1, (gx + 1) * W // GW)
            tot = n = fc = 0
            for y in range(y0, y1):
                base = y * W
                for x in range(x0, x1):
                    tot += dm[base + x]; n += 1
                    if face[base + x]:
                        fc += 1
            avg = tot / max(n, 1)
            ch = RAMP[min(len(RAMP) - 1, int(avg / 4.0))]
            if fc > n * 0.5 and ch == " ":
                ch = "F"
            line.append(ch)
        print("  |" + "".join(line) + "|")
    print("  +" + "-" * GW + "+")


# 面板：blur=100 保证跑 SDK（并保证两边完全相同 → 差分抵消）
PANEL = {"blur_level": 100, "blur_type": 0, "color_level": 0}

COMBOS = [
    ("V=0.10 skin=1", dict(use_new_whiten=1, color_level_v2=0.10), 1, 0),
    ("V=0.20 skin=1", dict(use_new_whiten=1, color_level_v2=0.20), 1, 0),
    ("V=0.30 skin=1", dict(use_new_whiten=1, color_level_v2=0.30), 1, 0),
    ("V=0.50 skin=1", dict(use_new_whiten=1, color_level_v2=0.50), 1, 0),
    ("V=0.80 skin=1", dict(use_new_whiten=1, color_level_v2=0.80), 1, 0),
    ("V=1.00 skin=1", dict(use_new_whiten=1, color_level_v2=1.00), 1, 0),
    ("V=0.50 skin=0", dict(use_new_whiten=1, color_level_v2=0.50), 0, 0),
    ("V=0.50 skin=1 lower_q=1", dict(use_new_whiten=1, color_level_v2=0.50, lower_quality=1), 1, 1),
    ("V=0.50 skin=1+mask_v", dict(use_new_whiten=1, color_level_v2=0.50, beauty_mask_v=1), 1, 0),
    # —— 找全局化拐点 + manual_scale 是否收紧背景渗出 + U 单独是否惰性 ——
    ("V=0.55", dict(use_new_whiten=1, color_level_v2=0.55), 1, 0),
    ("V=0.60", dict(use_new_whiten=1, color_level_v2=0.60), 1, 0),
    ("V=0.65", dict(use_new_whiten=1, color_level_v2=0.65), 1, 0),
    ("V=0.70", dict(use_new_whiten=1, color_level_v2=0.70), 1, 0),
    ("V=0.75", dict(use_new_whiten=1, color_level_v2=0.75), 1, 0),
    ("V=0.50 mscale=0.5", dict(use_new_whiten=1, color_level_v2=0.50, manual_scale=0.5), 1, 0),
    ("V=0.50 mscale=0.7", dict(use_new_whiten=1, color_level_v2=0.50, manual_scale=0.7), 1, 0),
    ("V=0.50 mscale=1.3", dict(use_new_whiten=1, color_level_v2=0.50, manual_scale=1.3), 1, 0),
    ("U=1 alone (V=0)", dict(use_new_whiten=1), 1, 0),
]

# ref 侧的 extra 覆盖：普通行 ref = test 把 V 归 0（隔离 V 的作用）；
# "U=1 alone" 行 ref = 全 0（测 use_new_whiten 自身是否惰性）
REF_OVERRIDE = {
    "U=1 alone (V=0)": dict(),
}


def build_mask(drv, src):
    set_extra(extra_text())
    drv.set_params(**PANEL, skin_detect=1)
    a = drv.render_settled(src)
    drv.set_params(**PANEL, skin_detect=0)
    b = drv.render_settled(src)
    m = bytearray(len(src) // 4)
    for i in range(len(m)):
        d = max(abs(a[i * 4] - b[i * 4]), abs(a[i * 4 + 1] - b[i * 4 + 1]),
                abs(a[i * 4 + 2] - b[i * 4 + 2]))
        m[i] = 1 if d >= 3 else 0
    return bytes(m)


def main():
    argv = [a for a in sys.argv[1:] if not a.startswith("--")]
    photo = argv[0] if argv else PHOTO
    do_map = "--map" in sys.argv
    dll = os.environ.get(
        "BEAUTY_OFX_DLL",
        r"C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle"
        r"\Contents\Win64\DaVinciBeauty.ofx")
    src = load_frame(photo)
    drv = Driver(dll)
    mask = build_mask(drv, src)
    print("MASK face_pixels=%d (%.2f%%)" % (sum(mask), 100.0 * sum(mask) / (W * H)))

    print("%-30s %8s %7s %-16s %7s %7s %8s %8s %s"
          % ("combo", "changed", "pct", "bbox", "face%", "out%", "face_av", "out_av", "corners"))
    results = []
    for name, ref_kw, skin, lowq in COMBOS:
        pan = dict(PANEL); pan["skin_detect"] = skin
        if name in REF_OVERRIDE:
            ref_extra = dict(REF_OVERRIDE[name])
        else:
            ref_extra = dict(ref_kw); ref_extra["color_level_v2"] = 0
        set_extra(extra_text(**ref_extra))
        drv.set_params(**pan)
        ref = drv.render_settled(src)
        set_extra(extra_text(**ref_kw))              # test
        test = drv.render_settled(src)
        dm = delta_map(ref, test)
        r = split_stats(dm, mask)
        r["combo"] = name
        results.append(r)
        print("%-30s %8d %6.2f%% %-16s %6.1f%% %6.1f%% %8.2f %8.2f %s"
              % (name, r["changed"], r["pct"], r["bbox"], r["face_pct"],
                 r["out_pct"], r["face_av"], r["out_av"], r["corners"]))
        if do_map and "V=0.50 skin=1" == name:
            show_grid(dm, mask, "\n== %s 密度图 (F=人脸掩码) ==" % name)

    print("")
    print("SWEEP_JSON " + json.dumps(results, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
