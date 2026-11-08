# -*- coding: utf-8 -*-
"""严格的双帧 A/B 差分器（针对 SDK 的「首帧热身」陷阱）。

**为什么需要它**
SDK 的美颜道具是人脸跟踪的：第 1 帧渲染时人脸处理器可能还没建立跟踪，
输出与第 2 帧以后不同。直接拿「第 1 帧」和「第 2 帧」做 A/B，量到的全是
热身差异，不是参数差异（我在这上面踩过一次：误以为 color_level_v2 生效了）。

**做法**
  * 每个配置先渲 WARMUP 帧丢弃，再渲 A 帧、B 帧。
  * 同一配置下 A、B 两帧的参数完全相同 → 它们的差必须为 0（自检）。
  * 真正的 A/B 差 = 「配置 P 下渲一帧」与「配置 Q 下渲一帧」，各自都先热身。

用法：
    py probe_ab.py <照片> "<额外A>" "<额外B>" "<面板A json>" "<面板B json>"
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

EXTRA_FILE = os.path.join(HERE, "_sweep_extra.txt")
WARMUP = 3


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
        self.main(mh.kOfxImageEffectActionDescribeInContext.encode(), self.eff_h, None, None)
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


def set_extra(text):
    with open(EXTRA_FILE, "w", encoding="utf-8") as f:
        f.write(text or "")


def diff(a, b, thr=3):
    n = len(a) // 4
    changed = 0
    tot = 0
    dr = dg = db = 0
    minx, miny, maxx, maxy = 10 ** 9, 10 ** 9, -1, -1
    for i in range(0, len(a) - 3, 4):
        d0 = b[i] - a[i]
        d1 = b[i + 1] - a[i + 1]
        d2 = b[i + 2] - a[i + 2]
        m = max(abs(d0), abs(d1), abs(d2))
        tot += m
        dr += d0; dg += d1; db += d2
        if m >= thr:
            changed += 1
            p = i // 4
            x, y = p % W, p // W
            if x < minx: minx = x
            if x > maxx: maxx = x
            if y < miny: miny = y
            if y > maxy: maxy = y
    corners = []
    for (cx, cy) in ((2, 2), (W - 3, 2), (2, H - 3), (W - 3, H - 3)):
        i = (cy * W + cx) * 4
        corners.append(max(abs(a[i] - b[i]), abs(a[i + 1] - b[i + 1]),
                           abs(a[i + 2] - b[i + 2])))
    bb = None if maxx < 0 else [minx, miny, maxx, maxy]
    return {"changed": changed, "pct": round(100.0 * changed / n, 2),
            "avg": round(tot / max(n, 1), 2),
            "drgb": [round(dr / n, 2), round(dg / n, 2), round(db / n, 2)],
            "bbox": bb, "corners": corners,
            "verdict": "GLOBAL" if max(corners) >= 3 else ("local" if changed else "none")}


def render_stable(dll, src, extra, panel, warmup=WARMUP):
    """在同一实例上：热身若干帧后取最后一帧。"""
    set_extra(extra)
    drv = Driver(dll)
    drv.set_params(**panel)
    for _ in range(warmup):
        drv.render(src)
    return drv.render(src)[1]


def main():
    photo = sys.argv[1]
    extra_a = sys.argv[2] if len(sys.argv) > 2 else ""
    extra_b = sys.argv[3] if len(sys.argv) > 3 else ""
    panel_a = json.loads(sys.argv[4]) if len(sys.argv) > 4 else {}
    panel_b = json.loads(sys.argv[5]) if len(sys.argv) > 5 else {}
    dll = os.environ.get(
        "BEAUTY_OFX_DLL",
        r"C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle\Contents\Win64\DaVinciBeauty.ofx")
    src = load_frame(photo)

    # 自检：同一配置连续两帧应完全一致（否则跟踪状态在漂，测不了）
    s1 = render_stable(dll, src, extra_a, panel_a)
    s2 = render_stable(dll, src, extra_a, panel_a)
    selfcheck = diff(s1, s2)
    # 真正的 A/B
    a = render_stable(dll, src, extra_a, panel_a)
    b = render_stable(dll, src, extra_b, panel_b)
    ab = diff(a, b)
    print("SELFCHECK " + json.dumps(selfcheck, ensure_ascii=False))
    print("AB " + json.dumps(ab, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
