# -*- coding: utf-8 -*-
"""验证「美白 v2 通道」能否把美白限制在人脸。

> ⚠️ **2026-10-21 更正：下面 A5 的结论已被推翻，不要再引用。**
> 用「画面里放一块非肤色蓝块」这个更硬的判据复测（`probe_whiten_region.py`，
> 见 `PHASE0_SDK区域探测报告.md` 表 4）：
>   `use_new_whiten=1;color_level_v2=1` → 蓝块改动 **100%**（整帧），
>   且与 `color_level_v2=1` **逐字节完全相同** → `use_new_whiten` 是空开关。
> 当年 A5 之所以读成「只在人脸」，是因为拿 `skin_detect=1 vs 0` 的磨皮差分当人脸掩码，
> 而磨皮的掩码其实是**肤色**不是人脸（同报告表 1/2）；再叠加当时对照里
> 面板 `color_level=100` 走的是**我们自己的 CPU 掩码**（本来就限在肤色上）。
> 结论：SDK 美白没有任何区域化路径，`color_level_v2`/`use_new_whiten`/`red_level`
> 全部整帧 —— 插件把 SDK 美白恒关、自己按肤色掩码在 CPU 侧做，是正确选择。

已测到的事实（见 sweep_whiten.py 的输出）：
  A2  color_level 0→100              → 100% 像素变化，四角 +8   = 全局（用户报的 bug）
  A3  color_level_v2=1               → 99.74% 变化，四角 +4      = 仍全局
  A5  use_new_whiten=1;color_level_v2=1 → 40.68%，bbox=人脸框    = **已被推翻**（见上）
  A11 v2=1+use_new_whiten 下 color_level 0→100 → 0 变化 = v2 接管后 legacy 参数失效

本脚本进一步确认：
  * v2 通道的强度曲线（color_level_v2 取不同值时人脸内改动量）
  * 人脸内**颜色方向**（是提亮=美白，还是偏色）
  * 是否真的只落在人脸内（人脸外平均改动应≈0）
  * 多张照片上是否稳定

用法：
    py probe_whiten_v2.py <照片> [<照片2> ...]
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

MASK_FILE = os.path.join(HERE, "_face_mask.bin")
EXTRA_FILE = os.path.join(HERE, "_sweep_extra.txt")

BASE = {"blur_level": 100, "skin_detect": 1}


def load_frame(photo, crop=None):
    from PIL import Image
    im = Image.open(photo).convert("RGB")
    if crop:
        x, y, w, h = crop
        im = im.crop((x, y, x + w, y + h))
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


def set_extra(text):
    with open(EXTRA_FILE, "w", encoding="utf-8") as f:
        f.write(text or "")


def analyze(ref, test, face):
    """人脸内/外的改动量 + 颜色方向（RGB 各通道的有符号平均差）。"""
    n = len(ref) // 4
    if face is None or len(face) != n:
        return None
    fin = [0, 0, 0]
    fn = 0
    fout = [0, 0, 0]
    on = 0
    fchg = ochg = 0
    for i in range(0, len(ref) - 3, 4):
        p = i // 4
        d = [test[i] - ref[i], test[i + 1] - ref[i + 1], test[i + 2] - ref[i + 2]]
        mx = max(abs(d[0]), abs(d[1]), abs(d[2]))
        if face[p]:
            fin[0] += d[0]; fin[1] += d[1]; fin[2] += d[2]; fn += 1
            if mx >= 3:
                fchg += 1
        else:
            fout[0] += d[0]; fout[1] += d[1]; fout[2] += d[2]; on += 1
            if mx >= 3:
                ochg += 1
    return {
        "face_changed": fchg, "face_px": fn,
        "face_drgb": [round(fin[0] / max(fn, 1), 2), round(fin[1] / max(fn, 1), 2),
                      round(fin[2] / max(fn, 1), 2)],
        "out_changed": ochg, "out_px": on,
        "out_drgb": [round(fout[0] / max(on, 1), 2), round(fout[1] / max(on, 1), 2),
                     round(fout[2] / max(on, 1), 2)],
    }


def build_face_mask(photo):
    """人脸掩码 = 磨皮 skin_detect=1 与 skin_detect=0 的差分（两帧都跑 SDK）。"""
    dll = os.environ.get(
        "BEAUTY_OFX_DLL",
        r"C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle\Contents\Win64\DaVinciBeauty.ofx")
    set_extra("")
    src = load_frame(photo)
    drv = Driver(dll)
    drv.set_params(blur_level=100, skin_detect=1)
    _, a = drv.render(src)
    drv.set_params(blur_level=100, skin_detect=0)
    _, b = drv.render(src)
    m = bytearray(len(src) // 4)
    for i in range(len(m)):
        d = max(abs(a[i * 4] - b[i * 4]), abs(a[i * 4 + 1] - b[i * 4 + 1]),
                abs(a[i * 4 + 2] - b[i * 4 + 2]))
        m[i] = 1 if d >= 3 else 0
    return m


def run(photo, extra, panel):
    dll = os.environ.get(
        "BEAUTY_OFX_DLL",
        r"C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle\Contents\Win64\DaVinciBeauty.ofx")
    set_extra(extra)
    src = load_frame(photo)
    face = build_face_mask(photo)
    drv = Driver(dll)
    p = dict(BASE)
    p.update(panel)
    drv.set_params(**p)
    _, out = drv.render(src)
    # 参照帧：同一额外参数、color_level=0、color_level_v2=0
    set_extra(extra)
    drv.set_params(blur_level=100, skin_detect=1)
    _, ref = drv.render(src)
    return analyze(ref, out, face)


def main():
    photos = sys.argv[1:]
    if not photos:
        print(__doc__)
        return 2
    # v2 强度曲线（extra 里写死 color_level_v2 的话不便做差分，
    # 所以这里改成：ref 全 0，test 逐步加大）
    tests = [
        ("legacy color_level=100 (全局)", "", {"color_level": 100}),
        ("v2=0.25", "use_new_whiten=1;color_level_v2=0.25", {}),
        ("v2=0.5", "use_new_whiten=1;color_level_v2=0.5", {}),
        ("v2=1.0", "use_new_whiten=1;color_level_v2=1", {}),
        ("v2=2.0", "use_new_whiten=1;color_level_v2=2", {}),
        ("v2=1.0 无 use_new_whiten", "color_level_v2=1", {}),
    ]
    for photo in photos:
        print("=== %s ===" % os.path.basename(photo))
        for name, extra, panel in tests:
            r = run(photo, extra, panel)
            if r is None:
                print("  %-32s  (无人脸掩码)" % name)
                continue
            print("  %-32s 人脸内改动 %6d/%-6d dRGB=%-22s | 人脸外改动 %6d/%-6d dRGB=%s"
                  % (name, r["face_changed"], r["face_px"], r["face_drgb"],
                     r["out_changed"], r["out_px"], r["out_drgb"]))
        print("")
    return 0


if __name__ == "__main__":
    sys.exit(main())
