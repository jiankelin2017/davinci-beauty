# -*- coding: utf-8 -*-
"""生成 mock_host.py 用的测试帧 probe_in.raw。

为什么需要这一步
----------------
v1.4 时 mock_host 在找不到 probe_in.raw 时会现场合成一张纹理帧，也能跑通。
v1.5 接了语义皮肤分割之后不行了：合成帧里没有皮肤，模型判定「整帧无皮肤」，
插件按契约什么都不做 —— 于是 8bit / float / B5 全部 FAIL。那不是回归，
是夹具不再具备代表性。

所以从一张**有真人脸**的照片生成固定尺寸（640×394 RGBA 裸字节）的测试帧。

用法：
    python make_probe_frame.py <照片路径> [--size 640x394] [--out probe_in.raw]
"""
import os
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    photo = sys.argv[1]
    w, h = 640, 394
    if "--size" in sys.argv:
        s = sys.argv[sys.argv.index("--size") + 1]
        w, h = [int(v) for v in s.lower().split("x")]
    out = os.path.join(HERE, "probe_in.raw")
    if "--out" in sys.argv:
        out = sys.argv[sys.argv.index("--out") + 1]

    im = Image.open(photo).convert("RGBA")
    # 等比缩放后居中贴到黑底上，避免拉伸改变人脸比例
    sw, sh = im.size
    k = min(w / float(sw), h / float(sh))
    nw, nh = max(1, int(sw * k)), max(1, int(sh * k))
    im = im.resize((nw, nh), Image.LANCZOS)
    canvas = Image.new("RGBA", (w, h), (0, 0, 0, 255))
    canvas.paste(im, ((w - nw) // 2, (h - nh) // 2))
    blob = canvas.tobytes()
    need = w * h * 4
    assert len(blob) == need, (len(blob), need)
    with open(out, "wb") as f:
        f.write(blob)
    print("已写出 %s  (%dx%d, %d 字节)" % (out, w, h, len(blob)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
