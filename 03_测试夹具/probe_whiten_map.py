# -*- coding: utf-8 -*-
"""把「美白」造成的逐像素改动画成 ASCII 密度图，直观判断是全局还是人脸。

同时输出：
  * 逐像素改动量热力图（每个字符 = 8x20 像素块）
  * 直方图：改动量的分布
  * 人脸掩码（由只开磨皮得到）叠加显示
用法：
    py probe_whiten_map.py <照片> [额外参数]
"""
import ctypes
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

GW, GH = 80, 30          # ASCII 网格
RAMP = " .:-=+*#%@"      # 密度由低到高


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
        self.inargs.set("OfxPropTime", 0, 10.0)
        self.inargs.set("OfxImageEffectPropRenderScale", 0, [1.0, 1.0])
        self.inargs_h = mh.newh(self.inargs)

    def set_params(self, **kw):
        for nm, pm in self.eff.params.items():
            if pm.ptype == "OfxParamTypePage":
                continue
            mh.pset(pm, kw.get(nm, 0))

    def render(self, src):
        mh.SRC_BUF = ctypes.create_string_buffer(src, len(src))
        mh.DST_BUF = ctypes.create_string_buffer(b"\x5a" * len(src), len(src))
        mh.DEPTH = "OfxBitDepthByte"
        st = self.main(mh.kOfxImageEffectActionRender.encode(), self.eff_h,
                       self.inargs_h, None)
        return st, mh.DST_BUF.raw


def delta(a, b):
    """每像素最大通道差。"""
    out = bytearray(len(a) // 4)
    for i in range(0, len(a) - 3, 4):
        d = max(abs(a[i] - b[i]), abs(a[i + 1] - b[i + 1]), abs(a[i + 2] - b[i + 2]))
        out[i // 4] = min(255, d)
    return out


def show_grid(dm, face=None, title=""):
    print(title)
    print("  +" + "-" * GW + "+")
    for gy in range(GH):
        y0 = gy * H // GH
        y1 = max(y0 + 1, (gy + 1) * H // GH)
        line = []
        for gx in range(GW):
            x0 = gx * W // GW
            x1 = max(x0 + 1, (gx + 1) * W // GW)
            tot = 0
            n = 0
            fc = 0
            for y in range(y0, y1):
                base = y * W
                for x in range(x0, x1):
                    v = dm[base + x]
                    tot += v
                    n += 1
                    if face is not None and face[base + x]:
                        fc += 1
            avg = tot / max(n, 1)
            lvl = min(len(RAMP) - 1, int(avg / 4.0))
            ch = RAMP[lvl]
            if face is not None and fc > n * 0.5:
                ch = "F" if ch == " " else ch
            line.append(ch)
        print("  |" + "".join(line) + "|")
    print("  +" + "-" * GW + "+")
    print("  ('F' = 该块大部分像素落在人脸掩码里；密度字符 = 平均改动量/4)")


def main():
    photo = sys.argv[1]
    extra = sys.argv[2] if len(sys.argv) > 2 else ""
    dll = os.environ.get(
        "NAMA_OFX_DLL",
        r"C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle\Contents\Win64\DaVinciBeauty.ofx")
    extra_file = os.path.join(HERE, "_sweep_extra.txt")
    with open(extra_file, "w", encoding="utf-8") as f:
        f.write(extra)
    src = load_frame(photo)
    drv = Driver(dll)
    drv.set_params(color_level=0, blur_level=0, skin_detect=1)
    _, out0 = drv.render(src)
    # 人脸掩码：只开磨皮（但这会污染道具状态，所以单独一个进程做）
    drv.set_params(color_level=0, blur_level=100, skin_detect=1)
    _, outB = drv.render(src)
    face = bytearray(len(src) // 4)
    for i in range(len(face)):
        if max(abs(out0[i * 4] - outB[i * 4]), abs(out0[i * 4 + 1] - outB[i * 4 + 1]),
               abs(out0[i * 4 + 2] - outB[i * 4 + 2])) >= 3:
            face[i] = 1
    # 目标：只开美白
    drv.set_params(color_level=100, blur_level=0, skin_detect=1)
    _, out1 = drv.render(src)
    dm = delta(out0, out1)

    show_grid(dm, None, "== 美白改动密度（无参考） ==")
    show_grid(dm, face, "== 美白改动密度（F=人脸掩码） ==")

    hist = [0] * 6
    for v in dm:
        if v == 0:
            hist[0] += 1
        elif v < 3:
            hist[1] += 1
        elif v < 8:
            hist[2] += 1
        elif v < 16:
            hist[3] += 1
        elif v < 32:
            hist[4] += 1
        else:
            hist[5] += 1
    n = len(dm)
    print("改动量直方图: 0=%d  1-2=%d  3-7=%d  8-15=%d  16-31=%d  >=32=%d" % tuple(hist))
    print("非零像素: %d / %d = %.2f%%" % (n - hist[0], n, 100.0 * (n - hist[0]) / n))
    # 人脸内外的平均改动
    fi = fo = 0
    fn = on = 0
    for i in range(n):
        if face[i]:
            fi += dm[i]; fn += 1
        else:
            fo += dm[i]; on += 1
    print("人脸内平均改动=%.2f (n=%d)   人脸外平均改动=%.2f (n=%d)" %
          (fi / max(fn, 1), fn, fo / max(on, 1), on))
    return 0


if __name__ == "__main__":
    sys.exit(main())
