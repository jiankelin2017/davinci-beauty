# -*- coding: utf-8 -*-
"""测量「美白(color_level)」到底作用在**整帧**还是**只作用在人脸**。

方法（不需要外部人脸检测库）：
    1) 基线：所有面板参数为 0，渲染一次 → out0
    2) 只开美白：color_level=100，其余为 0 → outW
       Δ白 = |outW - out0| 逐像素最大通道差
    3) 只开磨皮：blur_level=100 且 skin_detect=1 → outB
       Δ磨 = |outB - out0|
       skin_detect=1 时磨皮**只作用在皮肤/人脸**，所以 Δ磨 的非零区域
       就是「SDK 认为的人脸皮肤区域」，可以当作人脸蒙版的代理。
    4) 判定：
       - Δ白 覆盖了整帧（含四角） → 美白是全局的（用户报的现象）
       - Δ白 集中在 Δ磨 的区域内 → 美白只作用在人脸

用法：
    py probe_whiten_scope.py <照片> [裁剪x,y,w,h]
环境变量：
    BEAUTY_OFX_DLL            选插件二进制（默认装好的那个）
    BEAUTY_ENGINE_DIR         引擎目录
    BEAUTY_EXTRA_PARAMS_FILE  诊断参数文件（每帧重新读，便于扫组合）
"""
import ctypes
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import mock_host as mh  # noqa: E402  复用它的宿主套件实现

mh.DEBUG = False

W = 640
H = 394
# 夹具的 clip_get_rod / clip_get_image 读的是**模块级** W/H。
# 不设的话 clipGetRegionOfDefinition 返回 0x0，插件会算出空的渲染区域。
mh.W = W
mh.H = H


def load_frame(photo, crop=None):
    """把照片缩放/裁剪成 W×H 的 RGBA8 原始帧（宿主的 byte 深度是 RGBA）。"""
    from PIL import Image
    im = Image.open(photo).convert("RGB")
    if crop:
        x, y, w, h = crop
        im = im.crop((x, y, x + w, y + h))
    # 保持宽高比塞进 W×H，四周补黑（黑边不影响人脸检测，但要够高）
    sw, sh = im.size
    s = min(W / float(sw), H / float(sh))
    nw, nh = max(1, int(sw * s)), max(1, int(sh * s))
    im = im.resize((nw, nh), Image.LANCZOS)
    canvas = Image.new("RGBA", (W, H), (0, 0, 0, 255))
    canvas.paste(im.convert("RGBA"), ((W - nw) // 2, (H - nh) // 2))
    blob = canvas.tobytes()
    assert len(blob) == W * H * 4, (len(blob), W * H * 4)
    return blob


class Driver(object):
    def __init__(self, dll):
        self.dll = dll
        self.lib = ctypes.WinDLL(dll)
        getn = self.lib.OfxGetNumberOfPlugins
        getn.restype = ctypes.c_int
        getn.argtypes = []
        getp = self.lib.OfxGetPlugin
        getp.restype = ctypes.POINTER(mh.OfxPlugin)
        getp.argtypes = [ctypes.c_int]
        plug = getp(0)
        self.p = plug.contents
        SETHOST = ctypes.CFUNCTYPE(None, ctypes.POINTER(mh.OfxHost))
        SETHOST(self.p.setHost)(ctypes.byref(mh.HOST))
        MAIN = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_char_p, ctypes.c_void_p,
                                 ctypes.c_void_p, ctypes.c_void_p)
        self.main = MAIN(self.p.mainEntry)
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
            if nm in kw:
                mh.pset(pm, kw[nm])
            else:
                mh.pset(pm, 0)

    def render(self, src):
        mh.SRC_BUF = ctypes.create_string_buffer(src, len(src))
        mh.DST_BUF = ctypes.create_string_buffer(b"\x5a" * len(src), len(src))
        mh.DEPTH = "OfxBitDepthByte"
        st = self.main(mh.kOfxImageEffectActionRender.encode(), self.eff_h,
                       self.inargs_h, None)
        return st, mh.DST_BUF.raw


def delta_map(a, b):
    """逐像素最大通道差（0..255）。"""
    out = bytearray(len(a) // 4)
    for i in range(0, len(a) - 3, 4):
        d = max(abs(a[i] - b[i]), abs(a[i + 1] - b[i + 1]), abs(a[i + 2] - b[i + 2]))
        out[i // 4] = 255 if d >= 3 else d
    return out


def bbox(dm, thr=3):
    Wp, Hp = len(dm) // 1, 0
    minx, miny, maxx, maxy = 10 ** 9, 10 ** 9, -1, -1
    n = 0
    for y in range(H):
        row = y * W
        for x in range(W):
            if dm[row + x] >= thr:
                n += 1
                if x < minx:
                    minx = x
                if x > maxx:
                    maxx = x
                if y < miny:
                    miny = y
                if y > maxy:
                    maxy = y
    if maxx < 0:
        return None, 0
    return (minx, miny, maxx, maxy), n


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    photo = sys.argv[1]
    crop = None
    if len(sys.argv) > 2:
        crop = tuple(int(v) for v in sys.argv[2].split(","))
    dll = os.environ.get(
        "BEAUTY_OFX_DLL",
        r"C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle\Contents\Win64\DaVinciBeauty.ofx")
    src = load_frame(photo, crop)
    drv = Driver(dll)

    # --- 基线 ---
    drv.set_params(color_level=0, blur_level=0, skin_detect=1)
    st, out0 = drv.render(src)

    # --- 只开美白 ---
    drv.set_params(color_level=100, blur_level=0, skin_detect=1)
    stW, outW = drv.render(src)

    # --- 只开磨皮（skin_detect=1 → 只作用在皮肤/人脸）---
    drv.set_params(color_level=0, blur_level=100, skin_detect=1)
    stB, outB = drv.render(src)

    dw = delta_map(out0, outW)
    db = delta_map(out0, outB)
    bw, nw = bbox(dw)
    bb, nb = bbox(db)

    # 美白改动落在「皮肤/人脸区域」之外的比例 → 全局化的程度
    outside = 0
    for y in range(H):
        row = y * W
        for x in range(W):
            if dw[row + x] >= 3 and db[row + x] < 3:
                outside += 1
    # 四角是否被美白改动（背景的直接证据）
    corners = []
    for (cx, cy) in ((2, 2), (W - 3, 2), (2, H - 3), (W - 3, H - 3)):
        i = (cy * W + cx) * 4
        corners.append(max(abs(out0[i] - outW[i]), abs(out0[i + 1] - outW[i + 1]),
                           abs(out0[i + 2] - outW[i + 2])))

    res = {
        "photo": os.path.basename(photo),
        "status": [st, stW, stB],
        "whiten_changed_px": nw,
        "whiten_changed_pct": round(100.0 * nw / (W * H), 2),
        "whiten_bbox": bw,
        "skin_changed_px": nb,
        "skin_bbox": bb,
        "whiten_outside_skin_px": outside,
        "whiten_outside_skin_pct": round(100.0 * outside / max(nw, 1), 2),
        "corner_delta": corners,
        "verdict": ("GLOBAL" if (nw > W * H * 0.5 or max(corners) >= 3) else "LOCAL"),
    }
    print("RESULT " + json.dumps(res, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
