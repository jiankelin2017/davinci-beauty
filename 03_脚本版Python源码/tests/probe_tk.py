# -*- coding: utf-8 -*-
"""探针：fuscript 自带 Python 的 tkinter 显示能力与耗时。

实时窗能不能做，取决于「一帧图像从内存到屏幕」要多少毫秒。
这里把候选路径都量一遍：
  * 不同分辨率下 PPM+base64 -> PhotoImage 的构造耗时
  * RGBA -> RGB 的 array 技巧耗时
  * 窗口是否真的能显示出来
日志写文件（fuscript 的 stdout 没有 flush，不能依赖）。
"""
import base64
import os
import sys
import time
import traceback

import tkinter as tk

LOG = r"C:\ProgramData\NamaBeauty\logs\probe_tk.txt"
os.makedirs(os.path.dirname(LOG), exist_ok=True)
_lf = open(LOG, "w", encoding="utf-8")


def P(*a):
    _lf.write(" ".join(str(x) for x in a) + "\n")
    _lf.flush()


def tick():
    return time.perf_counter()


def ms(t):
    return (time.perf_counter() - t) * 1000


P("=== 环境 ===")
P("python :", sys.version.replace("\n", " "))
P("exe    :", sys.executable)
P("tk     :", tk.TkVersion, "tcl", tk.TclVersion)

try:
    import array
    import zlib
    P("array / zlib : OK")
except Exception as e:
    P("array / zlib :", e)

try:
    import numpy
    P("numpy :", numpy.__version__)
except Exception as e:
    P("numpy : 无 (%s)" % e)

try:
    from PIL import Image, ImageTk
    P("PIL   :", Image.__version__)
except Exception as e:
    P("PIL   : 无 (%s)" % e)


# ------------------------------------------------------------------ #
def prep_rgba_to_rgb(rgba, w, h):
    """RGBA -> RGB，走 array 切片删除，避开 Python 逐像素循环。"""
    a = array.array("B", rgba)
    del a[3::4]
    return a.tobytes()


def make_ppm(rgb, w, h):
    return b"P6\n%d %d\n255\n" % (w, h) + rgb


def main():
    P("\n=== 各分辨率：内存 -> PhotoImage 耗时 ===")
    root = tk.Tk()
    root.title("NamaBeauty 探针")
    root.geometry("760x480+80+80")
    lbl = tk.Label(root, text="probe")
    lbl.pack()

    sizes = [(480, 854), (640, 1138), (810, 1440), (1080, 1920)]
    results = {}
    for (w, h) in sizes:
        n = w * h * 4
        rgba = bytes(bytearray((i * 7 + (i >> 8) * 13) & 0xFF for i in range(min(n, 1 << 16))))
        rgba = (rgba * (n // len(rgba) + 1))[:n]

        t = tick()
        rgb = prep_rgba_to_rgb(rgba, w, h)
        t_rgb = ms(t)

        t = tick()
        ppm = make_ppm(rgb, w, h)
        t_ppm = ms(t)

        t = tick()
        b64 = base64.b64encode(ppm)
        t_b64 = ms(t)

        t = tick()
        img = tk.PhotoImage(data=b64)
        t_img = ms(t)

        t = tick()
        img2 = tk.PhotoImage(data=b64)
        t_img2 = ms(t)

        t = tick()
        lbl.configure(image=img2)
        lbl.image = img2
        root.update_idletasks()
        t_set = ms(t)

        results[(w, h)] = t_rgb + t_ppm + t_b64 + t_img
        P("%4dx%-5d rgba→rgb %6.1f  ppm %5.1f  b64 %6.1f  建图 %7.1f  "
          "重建 %7.1f  贴图 %6.1f  → 合计 %7.1f ms"
          % (w, h, t_rgb, t_ppm, t_b64, t_img, t_img2, t_set, results[(w, h)]))
        P("          数据量: rgb %.1f MB  ppm %.1f MB  b64 %.1f MB"
          % (len(rgb) / 1048576.0, len(ppm) / 1048576.0, len(b64) / 1048576.0))

    P("\n=== 等效帧率 ===")
    for k in sorted(results, key=lambda z: results[z]):
        v = results[k]
        P("  %4dx%-5d  %6.1f ms  ->  %5.1f fps" % (k[0], k[1], v, 1000.0 / v))

    # 窗口真能显示吗：显示 3.5 秒，中间切换一次画面
    P("\n=== 显示测试：窗口停留 3.5 秒，中途切图 ===")
    w, h = 640, 1138
    n = w * h * 4
    rgba = (bytes(bytearray((i * 3) & 0xFF for i in range(65536))) * (n // 65536 + 1))[:n]
    rgb = prep_rgba_to_rgb(rgba, w, h)
    imgA = tk.PhotoImage(data=base64.b64encode(make_ppm(rgb, w, h)))
    lbl.configure(image=imgA, text="")
    lbl.image = imgA

    def flip():
        P("  切图 @", time.strftime("%H:%M:%S"))
        imgB = tk.PhotoImage(data=base64.b64encode(
            make_ppm(bytes(len(rgb)), w, h)))
        lbl.configure(image=imgB)
        lbl.image = imgB

    root.after(1500, flip)
    root.after(3500, root.destroy)
    root.mainloop()
    P("窗口正常打开并关闭")


try:
    main()
except Exception:
    P("!!! 异常")
    P(traceback.format_exc())
finally:
    _lf.close()
