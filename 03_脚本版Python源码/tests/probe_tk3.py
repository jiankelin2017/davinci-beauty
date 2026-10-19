# -*- coding: utf-8 -*-
"""探针 3：用真实视频帧，量出「拼接图 -> 窗口」最快路径。

上一轮实测贴图要 150 ms，但探针 2 里 PNG 编码只要 3~17 ms——
差别在于探针 2 用的是纯色图，PNG 几乎不用压缩。真实照片必须重测。

同时确定：预览该降到多宽，才能在 30 fps 以上。
"""
import base64
import os
import struct
import subprocess
import sys
import time
import traceback
import zlib

import numpy as np
import tkinter as tk

ROOT = r"C:\ProgramData\NamaBeauty"
sys.path.insert(0, ROOT)
import nama_worker as W  # noqa: E402

LOG = os.path.join(ROOT, "logs", "probe_tk3.txt")
SRC = sys.argv[1] if len(sys.argv) > 1 else r"D:\c\2026-10-13 20-32-17.mp4"
_lf = open(LOG, "w", encoding="utf-8")


def P(*a):
    _lf.write(" ".join(str(x) for x in a) + "\n")
    _lf.flush()


def ms(t):
    return (time.perf_counter() - t) * 1000


def png_encode(rgb, w, h, level=1):
    raw = np.empty((h, w * 3 + 1), dtype=np.uint8)
    raw[:, 0] = 0
    raw[:, 1:] = rgb.reshape(h, w * 3)
    comp = zlib.compress(raw.tobytes(), level)

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", comp) + chunk(b"IEND", b""))


def grab(w, h, t=5.0):
    cmd = [W.FFMPEG, "-v", "error", "-nostdin", "-ss", "%.4f" % t, "-i", SRC,
           "-map", "0:v:0", "-frames:v", "1", "-vf", "scale=%d:%d" % (w, h),
           "-pix_fmt", "rgb24", "-f", "rawvideo", "-"]
    raw = subprocess.run(cmd, capture_output=True).stdout
    n = w * h * 3
    assert len(raw) >= n, "抽帧失败 %d/%d" % (len(raw), n)
    # 真实照片内容
    return np.frombuffer(raw[:n], dtype=np.uint8).reshape(h, w, 3).copy()


def main():
    P("=== 真实照片内容下的「拼接 -> 窗口」成本 ===")
    root = tk.Tk()
    root.title("probe3")
    lbl = tk.Label(root, bg="#101010")
    lbl.pack()
    root.update()

    sizes = [(480, 854), (560, 996), (640, 1138), (720, 1280), (810, 1440)]
    ppm_path = os.path.join(ROOT, "logs", "_p3.ppm")
    png_path = os.path.join(ROOT, "logs", "_p3.png")

    summary = []
    for (w, h) in sizes:
        cw = w * 2 + 4
        lbl.configure(width=cw, height=h)
        root.update()
        left = grab(w, h, 5.0)
        right = grab(w, h, 30.0)          # 用另一帧当"效果图"，内容真实

        combo = np.empty((h, cw, 3), dtype=np.uint8)
        combo[:, :w] = left
        combo[:, w:w + 4] = 255
        combo[:, w + 4:] = right

        res = {}

        # --- PNG base64 ---
        t = time.perf_counter()
        p = png_encode(combo, cw, h, 1)
        b64 = base64.b64encode(p).decode("ascii")
        t_enc = ms(t)
        t = time.perf_counter()
        im = tk.PhotoImage(data=b64)
        lbl.configure(image=im); lbl.image = im
        root.update_idletasks()
        t_img = ms(t)
        res["PNG-b64"] = (t_enc, t_img, len(b64) / 1048576.0)

        # --- PNG 文件 ---
        t = time.perf_counter()
        with open(png_path, "wb") as f:
            f.write(png_encode(combo, cw, h, 1))
        t_enc = ms(t)
        t = time.perf_counter()
        im = tk.PhotoImage(file=png_path)
        lbl.configure(image=im); lbl.image = im
        root.update_idletasks()
        t_img = ms(t)
        res["PNG-file"] = (t_enc, t_img, os.path.getsize(png_path) / 1048576.0)

        # --- PPM 文件 ---
        t = time.perf_counter()
        with open(ppm_path, "wb") as f:
            f.write(b"P6\n%d %d\n255\n" % (cw, h)); f.write(combo.tobytes())
        t_enc = ms(t)
        t = time.perf_counter()
        im = tk.PhotoImage(file=ppm_path)
        lbl.configure(image=im); lbl.image = im
        root.update_idletasks()
        t_img = ms(t)
        res["PPM-file"] = (t_enc, t_img, os.path.getsize(ppm_path) / 1048576.0)

        P("\n--- 单边 %dx%d  拼接后 %dx%d ---" % (w, h, cw, h))
        best = None
        for k, (e, i, mb) in res.items():
            tot = e + i
            P("   %-9s 编码 %7.1f ms   贴图 %7.1f ms   合计 %7.1f ms   %5.2f MB"
              % (k, e, i, tot, mb))
            if best is None or tot < best[1]:
                best = (k, tot)
        P("   -> 最快: %s  %.1f ms  (%.1f fps 上限)" % (best[0], best[1], 1000.0 / best[1]))
        summary.append((w, h, best[0], best[1]))

    P("\n=== 汇总（最快路径）===")
    for w, h, k, t in summary:
        P("  单边 %4dx%-5d %-9s %7.1f ms  ->  %5.1f fps" % (w, h, k, t, 1000.0 / t))

    root.after(600, root.destroy)
    root.mainloop()


try:
    main()
except Exception:
    P("!!! 异常\n" + traceback.format_exc())
finally:
    _lf.close()
