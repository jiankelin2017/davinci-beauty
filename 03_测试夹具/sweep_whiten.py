# -*- coding: utf-8 -*-
"""测「美白(color_level)」的真实作用域 —— 通用双帧差分器。

**测量纪律（踩过的坑，务必保留）**
1. 插件在八项面板参数全 0 时会**直接透传，SDK 根本没跑**（beauty_engine.h 的
   allZero()）。所以「全 0 的渲染」= 原始图，不是 SDK 基准输出。
   差分必须选两帧**都真的跑了 SDK**、且只差一个变量的组合。
2. SDK 道具参数是**粘住**的，set 过就一直有效 → 每个组合独立进程测。
3. 人脸掩码**不能**从美白差分里取（循环论证）。本脚本当年改用
   **skin_detect=1 vs 0 的差分**来近似「人脸区域」——**这个假设已被推翻**：
   磨皮的掩码是**肤色**掩码，与人脸检测无关（一块没人脸的肤色块也会被磨）。
   现在要做区域测量请用 `probe_blur_region.py` / `probe_whiten_region.py`
   （合成帧 + 非肤色对照块），结论见 `PHASE0_SDK区域探测报告.md`。

用法：
    py sweep_whiten.py --mask <照片>
    py sweep_whiten.py --diff <照片> "<ref面板>" "<test面板>" "<ref额外>" "<test额外>"
    py sweep_whiten.py <照片>
"""
import ctypes
import json
import os
import subprocess
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

# 参照组合：(说明, ref面板, test面板, ref额外, test额外)
# 面板里必须至少有一项非 0，否则插件透传、SDK 不跑。
COMBOS = [
    ("A1 磨皮 skin_detect=1 vs 0（应=人脸区域）",
     {"blur_level": 100, "skin_detect": 1}, {"blur_level": 100, "skin_detect": 0}, "", ""),
    ("A2 美白 0 vs 100（legacy 通道）",
     {"blur_level": 100, "skin_detect": 1, "color_level": 0},
     {"blur_level": 100, "skin_detect": 1, "color_level": 100}, "", ""),
    ("A3 v2 通道 0 vs 1（color_level 保持 0）",
     {"blur_level": 100, "skin_detect": 1}, {"blur_level": 100, "skin_detect": 1},
     "", "color_level_v2=1"),
    ("A4 v2 通道 0 vs 0.5",
     {"blur_level": 100, "skin_detect": 1}, {"blur_level": 100, "skin_detect": 1},
     "", "color_level_v2=0.5"),
    ("A5 v2 + use_new_whiten 0 vs 1",
     {"blur_level": 100, "skin_detect": 1}, {"blur_level": 100, "skin_detect": 1},
     "", "use_new_whiten=1;color_level_v2=1"),
    ("A6 use_new_whiten 0 vs 1（color_level 固定 0）",
     {"blur_level": 100, "skin_detect": 1}, {"blur_level": 100, "skin_detect": 1},
     "", "use_new_whiten=1"),
    ("A7 whiten_v2 0 vs 1",
     {"blur_level": 100, "skin_detect": 1}, {"blur_level": 100, "skin_detect": 1},
     "", "whiten_v2=1"),
    ("A8 beauty_mask_v 0 vs 1",
     {"blur_level": 100, "skin_detect": 1}, {"blur_level": 100, "skin_detect": 1},
     "", "beauty_mask_v=1"),
    ("A9 blur_use_mask 0 vs 1",
     {"blur_level": 100, "skin_detect": 1}, {"blur_level": 100, "skin_detect": 1},
     "", "blur_use_mask=1"),
    ("A10 v2=1 下 美白 0 vs 100（v2 是否接管）",
     {"blur_level": 100, "skin_detect": 1, "color_level": 0},
     {"blur_level": 100, "skin_detect": 1, "color_level": 100},
     "color_level_v2=1", "color_level_v2=1"),
    ("A11 v2=1 + use_new_whiten=1 下 美白 0 vs 100",
     {"blur_level": 100, "skin_detect": 1, "color_level": 0},
     {"blur_level": 100, "skin_detect": 1, "color_level": 100},
     "use_new_whiten=1;color_level_v2=1", "use_new_whiten=1;color_level_v2=1"),
    ("A12 red_level 0 vs 100（红润是否也全局）",
     {"blur_level": 100, "skin_detect": 1, "red_level": 0},
     {"blur_level": 100, "skin_detect": 1, "red_level": 100}, "", ""),
    ("A13 clarity 0 vs 100（清晰是否也全局）",
     {"blur_level": 100, "skin_detect": 1, "clarity": 0},
     {"blur_level": 100, "skin_detect": 1, "clarity": 100}, "", ""),
]


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


def diff_stats(a, b, thr=3):
    n = len(a) // 4
    changed = 0
    tot = 0
    minx, miny, maxx, maxy = 10 ** 9, 10 ** 9, -1, -1
    for i in range(0, len(a) - 3, 4):
        d = max(abs(a[i] - b[i]), abs(a[i + 1] - b[i + 1]), abs(a[i + 2] - b[i + 2]))
        tot += d
        if d >= thr:
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
    return changed, round(100.0 * changed / n, 2), bb, corners, round(tot / max(n, 1), 2)


def face_split(a, b, thr=3):
    """人脸内 / 外 的改动量。人脸掩码来自 skin_detect 差分（见 --mask）。"""
    if not os.path.exists(MASK_FILE):
        return None
    fm = open(MASK_FILE, "rb").read()
    n = len(a) // 4
    if len(fm) != n:
        return None
    fi = fo = 0
    fn = on = 0
    fchg = ochg = 0
    for i in range(0, len(a) - 3, 4):
        d = max(abs(a[i] - b[i]), abs(a[i + 1] - b[i + 1]), abs(a[i + 2] - b[i + 2]))
        p = i // 4
        if fm[p]:
            fi += d; fn += 1
            if d >= thr: fchg += 1
        else:
            fo += d; on += 1
            if d >= thr: ochg += 1
    return {"face_avg": round(fi / max(fn, 1), 2), "face_px": fn,
            "face_changed": fchg, "out_avg": round(fo / max(on, 1), 2),
            "out_px": on, "out_changed": ochg}


def do_mask(photo, crop):
    """人脸掩码 = 「磨皮 skin_detect=1」与「磨皮 skin_detect=0」的差分。

    两帧都开了磨皮、都真的跑 SDK，唯一差别是 skin_detect；
    磨皮在 skin_detect=1 时只动人脸 → 差分即人脸区域。
    """
    dll = os.environ.get(
        "BEAUTY_OFX_DLL",
        r"C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle\Contents\Win64\DaVinciBeauty.ofx")
    set_extra("")
    src = load_frame(photo, crop)
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
    with open(MASK_FILE, "wb") as f:
        f.write(bytes(m))
    print("MASK pixels=%d -> %s" % (sum(m), MASK_FILE))
    return 0


def do_diff(photo, crop, ref_panel, test_panel, ref_extra, test_extra):
    dll = os.environ.get(
        "BEAUTY_OFX_DLL",
        r"C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle\Contents\Win64\DaVinciBeauty.ofx")
    src = load_frame(photo, crop)
    drv = Driver(dll)
    set_extra(ref_extra)
    drv.set_params(**ref_panel)
    _, ref = drv.render(src)
    set_extra(test_extra)
    drv.set_params(**test_panel)
    _, test = drv.render(src)
    changed, pct, bb, corners, avg = diff_stats(ref, test)
    fs = face_split(ref, test)
    verdict = "GLOBAL" if max(corners) >= 3 else ("local" if changed else "none")
    return {"changed": changed, "pct": pct, "bbox": bb, "corners": corners,
            "avg": avg, "face": fs, "verdict": verdict}


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    if args[0] == "--mask":
        return do_mask(args[1], tuple(int(v) for v in args[2].split(",")) if len(args) > 2 and args[2] else None)
    if args[0] == "--diff":
        photo = args[1]
        crop = tuple(int(v) for v in args[2].split(",")) if len(args) > 2 and args[2] else None
        rp = json.loads(args[3]); tp = json.loads(args[4])
        re_ = args[5] if len(args) > 5 else ""
        te = args[6] if len(args) > 6 else ""
        print("ONE " + json.dumps(do_diff(photo, crop, rp, tp, re_, te), ensure_ascii=False))
        return 0

    photo = args[0]
    crop = tuple(int(v) for v in args[1].split(",")) if len(args) > 1 and args[1] else None
    print("%-52s %8s %7s %8s %-18s %-20s %-10s %-10s %s" %
          ("combo", "changed", "pct", "avg", "bbox", "corners", "face_avg", "out_avg", "verdict"))
    for name, rp, tp, re_, te in COMBOS:
        cmd = [sys.executable, os.path.abspath(__file__), "--diff", photo,
               (",".join(str(v) for v in crop) if crop else ""),
               json.dumps(rp), json.dumps(tp), re_, te]
        env = dict(os.environ)
        env["PYTHONUTF8"] = "1"
        env["PYTHONIOENCODING"] = "utf-8"
        p = subprocess.run(cmd, capture_output=True, env=env)
        line = ""
        for ln in p.stdout.decode("utf-8", "replace").splitlines():
            if ln.startswith("ONE "):
                line = ln[4:]
        if not line:
            err = p.stderr.decode("utf-8", "replace")
            i = err.find("Traceback")
            msg = (err[i:i + 240] if i >= 0 else err[-200:]).replace("\n", " ")
            print("%-52s  FAILED  %s" % (name, msg))
            continue
        r = json.loads(line)
        f = r.get("face") or {}
        print("%-52s %8d %6.2f%% %8.2f %-18s %-20s %-10s %-10s %s" %
              (name, r["changed"], r["pct"], r["avg"], r["bbox"], r["corners"],
               f.get("face_avg", "-"), f.get("out_avg", "-"), r["verdict"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
