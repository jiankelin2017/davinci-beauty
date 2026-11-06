# -*- coding: utf-8 -*-
"""美白（color_level）CPU 掩码路径的性能基准。

目的：applyWhiten 在「检出人脸 + 美白 > 0」时会逐像素做掩码，这是 v1.4 新增的
CPU 开销。本基准用同一帧、同一实例，只切换 color_level=0/100，其余参数固定
（blur_level=1，保证一定走 SDK，不会命中全 0 透传），用耗时差衡量纯掩码成本。

用法：
    python bench_whiten.py [照片] [宽] [高] [重复次数]
默认：~/Pictures/A7300538.jpg  1920 1080  5
环境变量：NAMA_OFX_DLL / NAMA_ENGINE_DIR（同 mock_host.py）
"""
import ctypes
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mock_host as mh  # noqa: E402

mh.DEBUG = False

W = int(sys.argv[2]) if len(sys.argv) > 2 else 1920
H = int(sys.argv[3]) if len(sys.argv) > 3 else 1080
RUNS = int(sys.argv[4]) if len(sys.argv) > 4 else 5
mh.W, mh.H = W, H


def load_frame(photo):
    from PIL import Image
    im = Image.open(photo).convert("RGB")
    sw, sh = im.size
    s = min(W / float(sw), H / float(sh))
    nw, nh = max(1, int(sw * s)), max(1, int(sh * s))
    im = im.resize((nw, nh), Image.LANCZOS)
    canvas = Image.new("RGBA", (W, H), (0, 0, 0, 255))  # 黑边不参与人脸检测
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
        self.p = getp(0).contents
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
        c = self.eff.clips.get("Source")
        c.props.set("OfxImageEffectPropRegionOfDefinition", 0, [0, 0, W, H])
        c.props.set("OfxImageEffectPropPixelDepth", 0, "OfxBitDepthByte")
        c.props.set("OfxImageEffectPropComponents", 0, "OfxImageComponentRGBA")
        c.props.set("OfxImagePropPixelAspectRatio", 0, 1.0)
        self.inargs = mh.PropSet("inargs")
        self.inargs.set("OfxImageEffectPropRenderWindow", 0, [0, 0, W, H])
        self.inargs.set("OfxPropTime", 0, 10.0)
        self.inargs.set("OfxImageEffectPropRenderScale", 0, [1.0, 1.0])
        self.inargs_h = mh.newh(self.inargs)

    def params(self, color, blur):
        for nm, pm in self.eff.params.items():
            if pm.ptype == "OfxParamTypePage":
                continue
            mh.pset(pm, 0)
        mh.pset(self.eff.params["blur_level"], blur)
        mh.pset(self.eff.params["skin_detect"], 1)
        mh.pset(self.eff.params["color_level"], color)

    def render(self, src):
        mh.SRC_BUF = ctypes.create_string_buffer(src, len(src))
        mh.DST_BUF = ctypes.create_string_buffer(b"\x5a" * len(src), len(src))
        mh.DEPTH = "OfxBitDepthByte"
        return self.main(mh.kOfxImageEffectActionRender.encode(), self.eff_h,
                         self.inargs_h, None)

    def timeit(self, src, color, blur):
        self.params(color, blur)
        self.render(src)  # 预热（参数变化后的首帧含 setter 下发）
        t0 = time.perf_counter()
        for _ in range(RUNS):
            st = self.render(src)
        return (time.perf_counter() - t0) * 1000.0 / RUNS, st


def main():
    photo = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~") + r"\Pictures\A7300538.jpg"
    if not os.path.exists(photo):
        print("找不到照片：%s" % photo)
        return 2
    dll = os.environ.get(
        "NAMA_OFX_DLL",
        r"C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle\Contents\Win64\DaVinciBeauty.ofx")
    src = load_frame(photo)
    drv = Driver(dll)
    t_no, st0 = drv.timeit(src, color=0, blur=1)
    t_w, st1 = drv.timeit(src, color=100, blur=1)
    print("")
    print("--- 美白 CPU 掩码成本 %dx%d（blur=1，%d 次平均）---" % (W, H, RUNS))
    print("  color_level=0    : %6.2f ms/帧  (status=%d)" % (t_no, st0))
    print("  color_level=100  : %6.2f ms/帧  (status=%d)" % (t_w, st1))
    print("  纯掩码开销       : %6.2f ms/帧" % (t_w - t_no))
    return 0


if __name__ == "__main__":
    sys.exit(main())
