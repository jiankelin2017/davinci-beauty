# -*- coding: utf-8 -*-
"""在真实照片里找出人脸框，供「美白作用范围」测量脚本裁剪用。

为什么需要它：
    美白要验证「是否只作用于人脸」，必须有一张**脸足够大**的素材。
    整幅 3936x2624 的照片里脸只占几个百分点，SDK 的人脸蒙版在那种比例下
    很容易整帧失效，量出来的「全局」结论不可信。
    所以先把脸抠出来、放大到画面里占比可观，再喂给插件。

实现：
    scikit-image 自带一份 OpenCV 格式的 LBP 人脸级联
    (lbpcascade_frontalface_opencv.xml)，skimage.feature.Cascade 能直接读。
    先缩到最长边 1600 检测（快），再把坐标换算回原图。

用法：
    py scan_faces.py [图片目录] [最大张数]
输出（stdout，每行一张）：
    <文件名> <x> <y> <w> <h> <原图宽> <原图高> <脸数>
"""
import os
import sys

import numpy as np
from PIL import Image

DEFAULT_DIR = os.path.expanduser("~") + r"\Pictures"


def _cascade_path():
    import skimage
    d = os.path.join(os.path.dirname(skimage.__file__), "data")
    return os.path.join(d, "lbpcascade_frontalface_opencv.xml")


def detect(path):
    """返回 (原图尺寸, [(x,y,w,h), ...])，坐标在原图尺度。"""
    try:
        im = Image.open(path).convert("L")
    except Exception:
        return None, []
    w0, h0 = im.size
    scale = 1600.0 / max(w0, h0)
    if scale < 1.0:
        small = im.resize((max(1, int(w0 * scale)), max(1, int(h0 * scale))),
                          Image.BILINEAR)
    else:
        small = im
    inv = 1.0 / (scale if scale < 1.0 else 1.0)
    arr = np.asarray(small, dtype=np.uint8)
    from skimage.feature import Cascade
    cascade = Cascade(_cascade_path())
    # skimage 的级联签名是位置参数：img, scale_factor, step_ratio, min_size,
    # max_size, min_neighbor_number, intersection_score_threshold
    boxes = cascade.detect_multi_scale(arr, 1.12, 0.06, (40, 40), None, 4, 0.15)
    out = []
    for b in boxes:
        out.append((int(b["c"] * inv), int(b["r"] * inv),
                    int(b["width"] * inv), int(b["height"] * inv)))
    return (w0, h0), out


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_DIR
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    files = sorted(f for f in os.listdir(d)
                   if f.lower().endswith((".jpg", ".jpeg", ".png")))
    if limit:
        files = files[:limit]
    nface = 0
    for f in files:
        p = os.path.join(d, f)
        size, faces = detect(p)
        if not faces:
            continue
        x, y, fw, fh = max(faces, key=lambda r: r[2] * r[3])
        nface += 1
        print("%s %d %d %d %d %d %d %d"
              % (f, x, y, fw, fh, size[0], size[1], len(faces)))
    print("--- 共 %d/%d 张检出人脸 ---" % (nface, len(files)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
