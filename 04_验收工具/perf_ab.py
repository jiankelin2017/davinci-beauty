#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""单 DLL 性能测量 —— 供 A/B 交替比较用。

为什么非要「两个 DLL 交替 + 取最小值」
--------------------------------------
这台机器上后台常驻进程（游戏、浏览器、同步工具）会周期性抢 CPU 和 GPU，
**同一个二进制在不同时段能差 15%，连 GPU 段都会变慢**。所以：
  - 绝对数值不可跨时段比较；
  - 正确做法是 A/B 两个 DLL 交替跑多轮，各自取最小值。

用法
----
    python perf_ab.py <dll路径> [dll2 dll3 ...]
    python perf_ab.py --res 1080p <dll>
    python perf_ab.py --frames 9 <dll>

输出：每行 `<dll名> <process中位数ms> <sdk中位数ms>`

本脚本只负责测一个 DLL；交替是靠 shell 循环反复调用实现的：
    for i in 1 2 3 4 5; do python perf_ab.py A.ofx B.ofx; done
取各列最小值即可。
"""
import ctypes
import os
import re
import shutil
import statistics
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.environ.get(
    "BEAUTY_ENGINE_DIR",
    r"C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle\Contents\Win64")
SRC = os.path.join(HERE, "input_face.png")

RES = {"1080p": (1920, 1080), "4k": (3840, 2160), "720p": (1280, 720),
       "540p": (960, 540)}


def main():
    # 带值选项（--res/--frames）的取值不能当成 DLL 路径收进来
    valued = {"--res", "--frames"}
    args, skip = [], False
    for a in sys.argv[1:]:
        if skip:
            skip = False
            continue
        if a in valued:
            skip = True
            continue
        if a.startswith("--"):
            continue
        args.append(a)
    res = "4k"
    if "--res" in sys.argv:
        res = sys.argv[sys.argv.index("--res") + 1]
    nframe = 7
    if "--frames" in sys.argv:
        nframe = int(sys.argv[sys.argv.index("--frames") + 1])
    W, H = RES.get(res, RES["4k"])
    if not args:
        print(__doc__)
        return 1

    import numpy as np
    from PIL import Image

    # 中文路径中转：BeautySelfTest 的入参是窄字符 const char*，本仓库在
    # D:\插件\ 下，直接传会在部分系统码页 fopen 失败。
    stage = tempfile.mkdtemp(prefix="nama_perf_")
    try:
        a = np.asarray(Image.open(SRC).convert("RGB").resize((W, H), Image.LANCZOS))
        rgba = np.dstack([a[:, :, 0], a[:, :, 1], a[:, :, 2],
                          np.full(a.shape[:2], 255, np.uint8)])
        ip = os.path.join(stage, "in.raw")
        op = os.path.join(stage, "out.raw")
        with open(ip, "wb") as f:
            f.write(rgba.tobytes())

        # 全开档：磨皮62 / 美白30 / 锐化50 / 美型三项
        os.environ["BEAUTY_ST_SHARPEN"] = "50"
        os.environ["BEAUTY_ST_CHEEK"] = "60"
        os.environ["BEAUTY_ST_EYE"] = "40"
        os.environ["BEAUTY_ST_CHIN"] = "30"
        os.environ["BEAUTY_ENGINE_ASSET_DIR"] = ENGINE

        print("# res=%s %dx%d frames=%d" % (res, W, H, nframe))
        for dll in args:
            if not os.path.isfile(dll):
                print("%-28s (文件不存在)" % dll)
                continue
            lib = ctypes.WinDLL(dll)
            lib.BeautySelfTest.argtypes = [ctypes.c_char_p, ctypes.c_char_p,
                                         ctypes.c_char_p, ctypes.c_int,
                                         ctypes.c_int, ctypes.c_double,
                                         ctypes.c_double, ctypes.c_char_p,
                                         ctypes.c_int]
            lib.BeautySelfTest.restype = ctypes.c_int
            buf = ctypes.create_string_buffer(4096)
            proc, sdk = [], []
            for i in range(nframe + 2):     # 前 2 帧预热丢弃
                lib.BeautySelfTest(ENGINE.encode(), ip.encode(), op.encode(),
                                 W, H, 62.0, 30.0, buf, 4096)
                msg = buf.value.decode("utf-8", "ignore")
                m = re.search(r"process=([\d.]+)ms", msg)
                s = re.search(r"sdk=([\d.]+)ms", msg)
                if m and i >= 2:
                    proc.append(float(m.group(1)))
                    if s:
                        sdk.append(float(s.group(1)))
            if not proc:
                print("%-28s (无有效样本)" % os.path.basename(dll))
                continue
            print("%-28s %7.1f %7.1f"
                  % (os.path.basename(dll), statistics.median(proc),
                     statistics.median(sdk) if sdk else 0.0))
        return 0
    finally:
        shutil.rmtree(stage, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
