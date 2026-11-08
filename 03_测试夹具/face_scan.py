# -*- coding: utf-8 -*-
"""扫一遍照片目录，报告每张能检出多少人脸像素（skin_detect 差分掩码）。

用法: py face_scan.py [目录]
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import sweep_v2 as sv  # noqa: E402  (复用 Driver/load_frame/掩码构建)

PHOTO_DIR = os.environ.get(
    "PHOTO_DIR", os.path.expanduser("~") + r"\Pictures")


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else PHOTO_DIR
    dll = os.environ.get(
        "BEAUTY_OFX_DLL",
        r"C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle"
        r"\Contents\Win64\DaVinciBeauty.ofx")
    photos = sorted(
        os.path.join(d, f) for f in os.listdir(d)
        if f.lower().endswith((".jpg", ".jpeg")))
    drv = None
    hits = []
    for p in photos:
        if drv is None:
            drv = sv.Driver(dll)
        try:
            src = sv.load_frame(p)
            mask = sv.build_mask(drv, src)
            n = sum(mask)
        except Exception as e:
            print("ERR %s %s" % (os.path.basename(p), e))
            continue
        if n > 0:
            hits.append((n, p))
            print("FACE %-40s %d px" % (os.path.basename(p), n))
        else:
            print("none %-40s" % os.path.basename(p))
    print("")
    print("有脸照片 %d / %d" % (len(hits), len(photos)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
