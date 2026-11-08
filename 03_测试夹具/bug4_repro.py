# -*- coding: utf-8 -*-
"""bug4_repro.py —— 不开 Resolve，用最小宿主复现「两实例塌成一层」。

和 mock_host.py 的区别（这是关键）：mock_host 的 B5 用例给两个实例喂的是
**同一块源缓冲**，所以「B 的输出其实是 A 的画面」在它那里恒等于正确结果。
本脚本给两个实例喂**两张不同的帧**，并且可以控制两次 render 之间的间隔 ——
间隔是本 bug 唯一的自变量。

用法（64 位 Python）：
    python bug4_repro.py                 # 双实例交替，gap=0
    python bug4_repro.py --gap 40        # 两次 render 间隔 40ms
    python bug4_repro.py --align 0       # 关掉「掩码↔帧对齐」（= BEAUTY_MASK_ALIGN=0）

判读（脚本会自己算）：
    out_B ≈ ref_A  → B 输出的是 A 的画面 = 复现
    out_B ≈ ref_B  → 正常

ref_A / ref_B 是「同一实例单独渲染自己那一帧」的基准，在双实例阶段之前采。
"""
import argparse
import ctypes
import os
import sys
import time
from ctypes import c_char_p, c_int, c_void_p, POINTER

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "01_ofx插件源码", "out")
DLL = os.environ.get("BEAUTY_OFX_DLL") or os.path.join(OUT, "DaVinciBeauty.ofx")
LOG = os.path.join(HERE, "bug4_diag.log")

W, H = 640, 394
SENTINEL = b"\x5a"
K = 4

# 插件每次写日志都会读这个环境变量：必须在 LoadLibrary 之前设好。
os.environ["BEAUTY_OFX_LOG"] = LOG
os.environ["BEAUTY_OFX_DLL"] = DLL

sys.path.insert(0, HERE)
import mock_host as M  # noqa: E402

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass


def load_plugin():
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.LoadLibraryExW.argtypes = [ctypes.c_wchar_p, c_void_p, ctypes.c_uint32]
    k32.LoadLibraryExW.restype = c_void_p
    if not k32.LoadLibraryExW(DLL, None, 0x1100):
        raise SystemExit("LoadLibrary 失败 err=%d" % ctypes.get_last_error())
    lib = ctypes.WinDLL(DLL)
    lib.OfxGetPlugin.restype = POINTER(M.OfxPlugin)
    lib.OfxGetPlugin.argtypes = [c_int]
    plug = lib.OfxGetPlugin(0).contents
    ctypes.CFUNCTYPE(None, POINTER(M.OfxHost))(plug.setHost)(ctypes.byref(M.HOST))
    return ctypes.CFUNCTYPE(c_int, c_char_p, c_void_p, c_void_p, c_void_p)(plug.mainEntry)


def frames():
    """两张**不同**的人脸帧。内容必须不同，否则串台不可观测。"""
    raw = open(os.path.join(HERE, "probe_in.raw"), "rb").read()
    if len(raw) != W * H * K:
        raise SystemExit("probe_in.raw 尺寸不符：%d != %d" % (len(raw), W * H * K))
    a = M._to_bottom_up(raw, W, H)
    row = W * K
    # 位移要大：它同时是「掩码是不是自己的」的判据 —— 两张帧的脸位置差得越远，
    # 一旦某实例用了对方的掩码，输出与自己的基准就差分得越明显。
    shift = 60 * K
    b = bytearray(len(a))
    for y in range(H):
        s = a[y * row:(y + 1) * row]
        b[y * row:(y + 1) * row] = s[-shift:] + s[:-shift]
    for i in range(0, len(b), K):
        b[i + 1] = min(255, b[i + 1] + 12)
    return a, bytes(b)


def make_effect(main_fn, first):
    e = M.Effect()
    h = M.newh(e)
    if first:
        main_fn(M.kOfxActionDescribe.encode(), h, None, None)
    main_fn(M.kOfxImageEffectActionDescribeInContext.encode(), h, None, None)
    st = main_fn(M.kOfxActionCreateInstance.encode(), h, None, None)
    if st != M.kOfxStatOK:
        raise SystemExit("CreateInstance 失败 st=%d" % st)
    sc = e.clips.get("Source")
    sc.props.set("OfxImageEffectPropRegionOfDefinition", 0, [0, 0, W, H])
    sc.props.set("OfxImageEffectPropPixelDepth", 0, "OfxBitDepthByte")
    sc.props.set("OfxImageEffectPropComponents", 0, "OfxImageComponentRGBA")
    sc.props.set("OfxImagePropPixelAspectRatio", 0, 1.0)
    return e, h


def render(main_fn, eff_h, frame, t):
    M.SRC_BUF = ctypes.create_string_buffer(frame, len(frame))
    M.DST_BUF = ctypes.create_string_buffer(SENTINEL * len(frame), len(frame))
    M.DEPTH = "OfxBitDepthByte"
    M.DST_PAD = [0, 0]
    ia = M.PropSet("ia")
    ia.set("OfxImageEffectPropRenderWindow", 0, [0, 0, W, H])
    ia.set("OfxPropTime", 0, float(t))
    ia.set("OfxImageEffectPropRenderScale", 0, [1.0, 1.0])
    st = main_fn(M.kOfxImageEffectActionRender.encode(), eff_h, M.newh(ia), None)
    return st, M.DST_BUF.raw


def diff_pct(x, y):
    n = min(len(x), len(y))
    bad = 0
    cnt = 0
    for i in range(0, n - 3, 4):
        cnt += 1
        if max(abs(x[i] - y[i]), abs(x[i + 1] - y[i + 1]), abs(x[i + 2] - y[i + 2])) > 8:
            bad += 1
    return 100.0 * bad / max(cnt, 1)


def run(main_fn, gap_ms, frames_n):
    a, b = frames()
    M.W, M.H = W, H
    t = 96.5

    # ---- 阶段 1/2：两个实例各自单独跑，拿到「自己的正确输出」作为基准 ----
    _, h_a = make_effect(main_fn, True)
    ref_a = None
    for _ in range(frames_n):
        t += 1.0
        _, ref_a = render(main_fn, h_a, a, t)
    _, h_b = make_effect(main_fn, False)
    ref_b = None
    for _ in range(frames_n):
        t += 1.0
        _, ref_b = render(main_fn, h_b, b, t)

    # ---- 阶段 3：交替渲染（模拟宿主一帧内先后调两个实例）----
    out_a = out_b = None
    for _ in range(frames_n):
        t += 1.0
        _, out_a = render(main_fn, h_a, a, t)
        time.sleep(gap_ms / 1000.0)
        _, out_b = render(main_fn, h_b, b, t)
        time.sleep(0.033)

    d_aa = diff_pct(out_a, ref_a)   # A 还是自己的画面
    d_ab = diff_pct(out_a, ref_b)   # A 变成了 B 的画面
    d_bb = diff_pct(out_b, ref_b)
    d_ba = diff_pct(out_b, ref_a)

    print("  gap=%-3dms  A→A基准=%5.2f%%  A→B基准=%5.2f%%   B→B基准=%5.2f%%  B→A基准=%5.2f%%"
          % (gap_ms, d_aa, d_ab, d_bb, d_ba))
    if d_ba < 0.5:
        print("     → B 输出的就是 A 的画面：**串台复现**")
    elif d_bb < 0.5:
        print("     → B 输出自己的画面：正常")
    else:
        print("     → 两者都不完全一致（掩码/EMA 状态差异），看 DIAG 日志")
    return d_ba, d_aa


def diag_lines():
    try:
        with open(LOG, "r", encoding="utf-8", errors="replace") as f:
            return [l.rstrip("\n") for l in f if "DIAG" in l]
    except OSError:
        return []


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gap", type=int, default=0)
    ap.add_argument("--frames", type=int, default=8)
    ap.add_argument("--sweep", action="store_true")
    ap.add_argument("--align", default=None, help="0 = 关掉掩码↔帧对齐")
    args = ap.parse_args()

    if args.align is not None:
        os.environ["BEAUTY_MASK_ALIGN"] = str(args.align)

    if os.path.exists(LOG):
        os.remove(LOG)

    main_fn = load_plugin()
    st = main_fn(M.kOfxActionLoad.encode(), None, None, None)
    if st != M.kOfxStatOK:
        raise SystemExit("load 失败 st=%d" % st)

    print("dll=%s" % DLL)
    print("BEAUTY_MASK_ALIGN=%r  (None = 引擎默认，对齐开启)"
          % os.environ.get("BEAUTY_MASK_ALIGN"))
    print("--- 每实例 8 帧单独基准 + 8 帧交替 ---")
    if args.sweep:
        for g in (0, 5, 10, 20, 40, 80):
            run(main_fn, g, args.frames)
    else:
        run(main_fn, args.gap, args.frames)

    lines = diag_lines()
    print("\n--- DIAG 日志（共 %d 行，摘录）---" % len(lines))
    for l in (lines[:4] + lines[-4:] if len(lines) > 8 else lines):
        print("  " + l)
    return 0


if __name__ == "__main__":
    sys.exit(main())
