# -*- coding: utf-8 -*-
"""逐帧稳定性探针：确认「第 1 帧」和「第 2 帧以后」到底差在哪。

**为什么必须做这个**
我用「同一实例连续渲两帧」测美白作用域，测到「100% 像素变化、四角 +8」。
但改用「先热身 3 帧再测」时，差异变成 0。两种测法矛盾，说明量到的**不是**
美白的净效果，而是 SDK 的**首帧热身**（人脸跟踪在第 1 帧还没建立）。

这个脚本把每一帧都留下来，两两做差，并同时抓插件日志里的 faces= 读数，
从而分清「参数效果」和「热身效果」。

用法：
    py probe_seq.py <照片> ["<额外参数>"] ["<面板json>"]
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


def diff(a, b, thr=3):
    n = len(a) // 4
    changed = 0
    tot = 0
    minx, miny, maxx, maxy = 10 ** 9, 10 ** 9, -1, -1
    for i in range(0, len(a) - 3, 4):
        m = max(abs(a[i] - b[i]), abs(a[i + 1] - b[i + 1]), abs(a[i + 2] - b[i + 2]))
        tot += m
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
            "avg": round(tot / max(n, 1), 2), "bbox": bb, "corners": corners}


def main():
    photo = sys.argv[1]
    extra = sys.argv[2] if len(sys.argv) > 2 else ""
    panel = json.loads(sys.argv[3]) if len(sys.argv) > 3 else {
        "blur_level": 100, "skin_detect": 1, "color_level": 0}
    dll = os.environ.get(
        "BEAUTY_OFX_DLL",
        r"C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle\Contents\Win64\DaVinciBeauty.ofx")
    with open(EXTRA_FILE, "w", encoding="utf-8") as f:
        f.write(extra)
    src = load_frame(photo)
    drv = Driver(dll)
    drv.set_params(**panel)
    frames = []
    for i in range(6):
        st, out = drv.render(src)
        frames.append(out)
        print("frame %d status=%d sentinel_left=%d" % (i + 1, st, out.count(b"\x5a")))
    print("")
    print("相邻帧差（判断热身）:")
    for i in range(1, len(frames)):
        d = diff(frames[i - 1], frames[i])
        print("  f%d vs f%d: changed=%-7d pct=%6.2f%% avg=%7.2f corners=%s bbox=%s"
              % (i, i + 1, d["changed"], d["pct"], d["avg"], d["corners"], d["bbox"]))
    print("")
    print("首帧 vs 末帧: %s" % diff(frames[0], frames[-1]))
    print("f2 vs f6   : %s" % diff(frames[1], frames[-1]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
