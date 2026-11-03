# -*- coding: utf-8 -*-
"""隔离验证 nama_region.cpp 的调用契约（不牵扯 旧版闭源 SDK）。

背景：把 NamaRegion 接进 NamaEngine::process 之后，mock_host 在第一次 render
就整进程退出（EXIT=127），日志停在 fuai 的 `Perform SetUseFaceLandmark`。
在宿主进程里跑 fuai 和单独跑 fuai 有两处不同：
  1) 旧版闭源 SDK 内部**也**加载了 fuai.dll（同一个模块），我们已经又开了一个
     FaceProcessor 实例；
  2) 我们是先 fuSetup + 建道具，之后才第一次调 FUAI_...Process。

这个脚本只做第 1 步排查：**完全不含 旧版闭源 SDK**，只按 nama_region.cpp 里的
参数组合调 fuai，看是「契约本身错」还是「和 旧版闭源 SDK 冲突」。

用法（每个变体必须单独起进程，崩了才看得出来）：
    python probe_region_contract.py <照片> --variant A
    python probe_region_contract.py <照片> --variant B

变体：
  A = extract_face_assets.py 的已知可用组合（RGB / stride=w*3 / max5 / head+hair 开）
  B = nama_region.cpp 的组合（RGBA / stride=w*4 / max2 / head+hair 关）
  C = B 但 fmt 退回 RGB（定位是不是 fmt=3 的问题）
  D = B 但 head/hair 打开（定位是不是关掉分段器的问题）
"""
import argparse
import ctypes
import os
import sys
import time
from ctypes import POINTER, c_char_p, c_float, c_int, c_void_p

P = c_void_p
I = c_int
F = c_float

DLL = r"C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle\Contents\Win64\fuai.dll"
BUNDLE = r"C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle\Contents\Win64\ai_face_processor_pc_plain.bundle"

VARIANTS = {
    # name: (fmt, bytes_per_px, max_faces, use_head, use_hair)
    "A": (1, 3, 5, 1, 1),
    "B": (3, 4, 2, 0, 0),
    "C": (1, 3, 2, 0, 0),
    "D": (3, 4, 2, 1, 1),
}


def bind(d, name, restype, argtypes):
    fn = getattr(d, name)
    fn.restype = restype
    fn.argtypes = argtypes
    return fn


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("photo")
    ap.add_argument("--variant", default="B")
    args = ap.parse_args()

    fmt, bpp, maxf, use_head, use_hair = VARIANTS[args.variant]
    print("[契约] variant=%s fmt=%d bpp=%d maxFaces=%d head=%d hair=%d"
          % (args.variant, fmt, bpp, maxf, use_head, use_hair), flush=True)

    from PIL import Image
    im = Image.open(args.photo).convert("RGB")
    w, h = im.size
    print("[输入] %dx%d" % (w, h), flush=True)

    # 按变体准备缓冲：A/C 走 RGB，B/D 走 RGBA（把 alpha 填 255）
    if bpp == 3:
        buf = im.tobytes()
    else:
        rgba = Image.new("RGBA", im.size)
        rgba.paste(im, (0, 0))
        buf = rgba.tobytes()
    stride = w * bpp
    print("[输入] buf=%d 字节 stride=%d" % (len(buf), stride), flush=True)

    if os.name == "nt":
        os.add_dll_directory(os.path.dirname(DLL))
    d = ctypes.WinDLL(DLL)

    NewProc = bind(d, "FUAI_NewFaceProcessorFromBundle", P, [c_char_p, I])
    NewResult = bind(d, "FUAI_NewFaceProcessorResult", P, [])
    NewView = bind(d, "FUAI_NewImageView", P, [])
    InitView = bind(d, "FUAI_ImageViewInit", None, [P, I, I, I, I, P, I, P, I, P, I])
    SetViewData = bind(d, "FUAI_ImageViewSetData", None, [P, I, P])
    Process = bind(d, "FUAI_FaceProcessorProcess2Result", None, [P, P, P])
    GetNum = bind(d, "FUAI_FaceProcessorGetNumFromResult", I, [P])
    GetSkinSeg = bind(d, "FUAI_FaceProcessorGetSkinSegResult",
                      P, [P, P, POINTER(I), POINTER(I), P])
    SetMaxFaces = bind(d, "FUAI_FaceProcessorSetMaxFaces", None, [P, I])
    SetDetectMode = bind(d, "FUAI_FaceProcessorSetDetectMode", None, [P, I])
    SetUseLandmark = bind(d, "FUAI_FaceProcessorSetUseFaceLandmark", None, [P, I])
    SetUseHead = bind(d, "FUAI_FaceProcessorSetUseHeadSegmenter", None, [P, I])
    SetUseHair = bind(d, "FUAI_FaceProcessorSetUseHairSegmenter", None, [P, I])
    SetUseSkin = bind(d, "FUAI_FaceProcessorSetUseSkinSeg", None, [P, I])

    t0 = time.time()
    blob = open(BUNDLE, "rb").read()
    print("[bundle] %d 字节 读盘 %.0f ms" % (len(blob), (time.time() - t0) * 1000), flush=True)

    t0 = time.time()
    proc = NewProc(blob, len(blob))
    print("[init] NewFaceProcessorFromBundle -> %s  %.0f ms"
          % (bool(proc), (time.time() - t0) * 1000), flush=True)
    if not proc:
        print("FAIL: proc 为空", flush=True)
        return 2

    # 顺序照 nama_region.cpp：先配参数再分配 result/view
    SetMaxFaces(proc, maxf)
    SetDetectMode(proc, 0)
    SetUseLandmark(proc, 1)
    SetUseHead(proc, use_head)
    SetUseHair(proc, use_hair)
    SetUseSkin(proc, 1)
    print("[init] 参数配置完成", flush=True)

    res = NewResult()
    view = NewView()
    print("[init] result=%s view=%s" % (bool(res), bool(view)), flush=True)

    # 关键：InitView 只给 stride，缓冲指针留空，再 SetViewData
    cbuf = ctypes.create_string_buffer(buf, len(buf))
    print("[run] InitView ...", flush=True)
    InitView(view, fmt, w, h, 0, None, stride, None, 0, None, 0)
    SetViewData(view, 0, ctypes.cast(cbuf, P))
    print("[run] Process ...", flush=True)
    t0 = time.time()
    Process(proc, view, res)
    ms = (time.time() - t0) * 1000
    print("[run] Process 返回  %.2f ms" % ms, flush=True)

    n = GetNum(res)
    print("[out] faces=%d" % n, flush=True)

    mw, mh = I(0), I(0)
    extra = (c_int * 4)()
    p = GetSkinSeg(proc, view, ctypes.byref(mw), ctypes.byref(mh), ctypes.cast(extra, P))
    if not p or not mw.value or not mh.value:
        print("[out] 皮肤分割: 空 (p=%s %dx%d)" % (bool(p), mw.value, mh.value), flush=True)
        return 1

    cnt = mw.value * mh.value
    arr = ctypes.cast(p, POINTER(F))
    hit = 0
    mx = 0.0
    for i in range(cnt):
        v = arr[i]
        if v >= 0.5:
            hit += 1
        if v > mx:
            mx = v
    print("[out] 皮肤分割: %dx%d  覆盖=%.1f%%  max=%.3f"
          % (mw.value, mh.value, 100.0 * hit / cnt, mx), flush=True)
    print("OK variant=%s" % args.variant, flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
