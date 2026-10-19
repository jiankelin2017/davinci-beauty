# -*- coding: utf-8 -*-
"""探针 2：找出「内存图像 -> Tk 窗口」最快的可行路径。

上一版失败在 PhotoImage(data=bytes) 不被 Tk 接受。这里把候选路径都试一遍：
  A  PPM base64（str）
  B  PPM 临时文件
  C  PNG 临时文件（numpy 造数据 + zlib）
  D  PNG base64（str）
并顺带量出 numpy 做 RGBA→RGB / 缩放 的耗时。
"""
import base64
import os
import struct
import sys
import time
import traceback
import zlib

import numpy as np
import tkinter as tk

LOG = r"C:\ProgramData\NamaBeauty\logs\probe_tk2.txt"
TMP = r"C:\ProgramData\NamaBeauty\logs"
_lf = open(LOG, "w", encoding="utf-8")


def P(*a):
    _lf.write(" ".join(str(x) for x in a) + "\n")
    _lf.flush()


def ms(t):
    return (time.perf_counter() - t) * 1000


def png_encode(rgb, w, h, level=1):
    """rgb: (h, w, 3) uint8 -> PNG bytes"""
    raw = np.empty((h, w * 3 + 1), dtype=np.uint8)
    raw[:, 0] = 0
    raw[:, 1:] = rgb.reshape(h, w * 3)
    comp = zlib.compress(raw.tobytes(), level)

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
            + chunk(b"IDAT", comp) + chunk(b"IEND", b""))


def main():
    P("=== numpy 操作耗时（1080p，RGBA uint8）===")
    W, H = 1080, 1920
    rgba = (np.arange(W * H * 4, dtype=np.uint64) * 2654435761 & 0xFF).astype(np.uint8)
    rgba = rgba.reshape(H, W, 4)

    t = time.perf_counter(); rgbv = rgba[:, :, :3]; t1 = ms(t)
    t = time.perf_counter(); rgbc = np.ascontiguousarray(rgbv); t2 = ms(t)
    P("  切 RGB 视图 %.1f ms   转连续内存 %.1f ms" % (t1, t2))

    t = time.perf_counter(); small = rgba[::2, ::2]; t3 = ms(t)
    P("  隔行隔列降采样(1/4面积) %.1f ms" % t3)

    P("\n=== PNG 编码耗时（不同分辨率 / 压缩级别）===")
    for (w, h) in [(640, 1138), (810, 1440), (1080, 1920)]:
        a = np.zeros((h, w, 3), dtype=np.uint8)
        for lv in (0, 1, 3, 6):
            t = time.perf_counter()
            p = png_encode(a, w, h, lv)
            P("  %4dx%-5d level=%d  %7.1f ms   %6.1f MB" % (w, h, lv, ms(t), len(p) / 1048576.0))

    P("\n=== 三条显示路径实测（含窗口重绘）===")
    root = tk.Tk()
    root.title("NamaBeauty 探针 2")
    root.geometry("700x460+100+100")
    lbl = tk.Label(root, text="...")
    lbl.pack()
    root.update()

    for (w, h) in [(640, 1138), (1080, 1920)]:
        rgb = np.zeros((h, w, 3), dtype=np.uint8)
        rgb[:, :, 0] = np.arange(w, dtype=np.uint8)[None, :]
        rgb[:, :, 1] = np.arange(h, dtype=np.uint8)[:, None]

        # --- A: PPM base64 str ---
        try:
            t = time.perf_counter()
            ppm = b"P6\n%d %d\n255\n" % (w, h) + rgb.tobytes()
            b64s = base64.b64encode(ppm).decode("ascii")
            t_prep = ms(t)
            t = time.perf_counter()
            im = tk.PhotoImage(data=b64s)
            P("  A %4dx%-5d PPM-b64-str  准备 %6.1f  建图 %7.1f ms  b64 %.1f MB"
              % (w, h, t_prep, ms(t), len(b64s) / 1048576.0))
        except Exception as e:
            P("  A %4dx%-5d 失败: %s" % (w, h, e))

        # --- B: PPM 文件 ---
        try:
            fp = os.path.join(TMP, "_probe.ppm")
            t = time.perf_counter()
            with open(fp, "wb") as f:
                f.write(b"P6\n%d %d\n255\n" % (w, h)); f.write(rgb.tobytes())
            t_prep = ms(t)
            t = time.perf_counter()
            im = tk.PhotoImage(file=fp)
            P("  B %4dx%-5d PPM-文件      准备 %6.1f  建图 %7.1f ms"
              % (w, h, t_prep, ms(t)))
        except Exception as e:
            P("  B %4dx%-5d 失败: %s" % (w, h, e))

        # --- C: PNG 文件 ---
        try:
            fp = os.path.join(TMP, "_probe.png")
            t = time.perf_counter()
            p = png_encode(rgb, w, h, 1)
            with open(fp, "wb") as f:
                f.write(p)
            t_prep = ms(t)
            t = time.perf_counter()
            im = tk.PhotoImage(file=fp)
            P("  C %4dx%-5d PNG-文件      准备 %6.1f  建图 %7.1f ms  png %.2f MB"
              % (w, h, t_prep, ms(t), len(p) / 1048576.0))
        except Exception as e:
            P("  C %4dx%-5d 失败: %s" % (w, h, e))

        # --- D: PNG base64 str ---
        try:
            t = time.perf_counter()
            b64s = base64.b64encode(png_encode(rgb, w, h, 1)).decode("ascii")
            t_prep = ms(t)
            t = time.perf_counter()
            im = tk.PhotoImage(data=b64s)
            P("  D %4dx%-5d PNG-b64-str   准备 %6.1f  建图 %7.1f ms"
              % (w, h, t_prep, ms(t)))
        except Exception as e:
            P("  D %4dx%-5d 失败: %s" % (w, h, e))

    # 端到端：连续刷新 30 次，看真实帧率（含窗口重绘）
    P("\n=== 端到端连续刷新（640x1138，PNG 文件路径）===")
    w, h = 640, 1138
    fp = os.path.join(TMP, "_probe_live.png")
    base = np.zeros((h, w, 3), dtype=np.uint8)
    frames = 20
    t0 = time.perf_counter()
    for i in range(frames):
        a = base.copy()
        a[:, :, 0] = (i * 12) & 0xFF
        with open(fp, "wb") as f:
            f.write(png_encode(a, w, h, 1))
        im = tk.PhotoImage(file=fp)
        lbl.configure(image=im)
        lbl.image = im
        root.update()
    total = ms(t0)
    P("  %d 帧共 %.1f ms -> 平均 %.1f ms/帧 = %.1f fps"
      % (frames, total, total / frames, 1000.0 * frames / total))

    root.after(1200, root.destroy)
    root.mainloop()
    P("窗口正常")


try:
    main()
except Exception:
    P("!!! 异常\n" + traceback.format_exc())
finally:
    _lf.close()
