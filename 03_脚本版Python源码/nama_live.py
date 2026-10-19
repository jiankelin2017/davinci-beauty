# -*- coding: utf-8 -*-
"""NamaBeauty 实时预览 —— 常驻渲染进程。

和 nama_worker.py 的根本差别在「生命周期」：
  worker    : 跑一次就退出，每次都要付 1.8 秒的 SDK 初始化代价（所以预览要 3 秒）
  nama_live : 只启动一次，SDK / GL 上下文 / 解码器全部常驻，
              之后每帧的成本只剩「渲染 4.5ms + 编码 4ms + 贴图 17ms」

工作方式：
  * 达芬奇那侧的 Lua 面板把参数与播放头写进 live/live.json
  * 本进程轮询该文件，内容变了就重渲染并刷新窗口
  * Lua 另写 live/live.hb 做心跳，长时间没有心跳就自行退出，
    避免面板关掉后留下孤儿进程

窗口布局：左边源帧，右边美颜后，正面对比。
"""
import ctypes
import json
import os
import subprocess
import sys
import time
import traceback

import numpy as np
import tkinter as tk

ROOT = r"C:\ProgramData\NamaBeauty"
sys.path.insert(0, ROOT)
import nama_worker as W  # noqa: E402

LIVE_DIR = os.path.join(ROOT, "live")
LIVE_JSON = os.path.join(LIVE_DIR, "live.json")
LIVE_HB = os.path.join(LIVE_DIR, "live.hb")
LIVE_PID = os.path.join(LIVE_DIR, "live.pid")
LIVE_QUIT = os.path.join(LIVE_DIR, "live.quit")
PPM_PATH = os.path.join(LIVE_DIR, "preview.ppm")
LOG_PATH = os.path.join(ROOT, "logs", "live.log")

POLL_MS = 60            # 主循环间隔（毫秒）
HB_TIMEOUT = 20.0       # 心跳超时（秒）——兜底手段，正常退出走 live.quit
GAP = 4                 # 对比图中间白线宽度
MAX_PER_SIDE = 820      # 单边最大像素宽

_logf = None


def log(msg):
    global _logf
    line = "%s %s" % (time.strftime("%H:%M:%S"), msg)
    try:
        if _logf is None:
            os.makedirs(os.path.dirname(LOG_PATH), exist_ok=True)
            _logf = open(LOG_PATH, "a", encoding="utf-8")
        _logf.write(line + "\n")
        _logf.flush()
    except Exception:
        pass
    # 同时写一份到 stdout：fuscript 的 stdout 默认有缓冲，
    # 显式 flush 后从外部管道就能实时看到，排查问题时省事
    try:
        sys.stdout.write(line + "\n")
        sys.stdout.flush()
    except Exception:
        pass


# ------------------------------------------------------------------ #
#  为什么用 PPM 而不是 PNG
#
#  实测（tests\probe_tk3.py，真实视频帧，拼接后 1444x1280）：
#      PNG-b64   编码 69.4 ms  贴图 68.2 ms  合计 137.5 ms  (7 fps)
#      PNG-file  编码 65.4 ms  贴图 52.5 ms  合计 117.9 ms  (8 fps)
#      PPM-file  编码  4.6 ms  贴图 21.6 ms  合计  26.1 ms  (38 fps)
#
#  PNG 要 zlib 压真实照片，这是纯 CPU 时间；Tk 的 -data 通道还只吃
#  base64 字符串，平白多一道编解码。PPM 无压缩、Tk 原生支持，
#  而且可以直接用 ndarray.tofile 写进打开的文件句柄，一次拷贝都不做。
# ------------------------------------------------------------------ #
def pick_preview_size(sw, sh, screen_w, screen_h):
    """按源尺寸与屏幕大小，算出单边预览尺寸（偶数）。"""
    total = min(int(screen_w * 0.86), 1760)
    per = min((total - GAP) // 2, MAX_PER_SIDE)
    scale = per / float(sw)
    w = max(2, int(sw * scale) // 2 * 2)
    h = max(2, int(sh * scale) // 2 * 2)
    cap = int(screen_h * 0.80)
    if h > cap:
        h = max(2, cap // 2 * 2)
        w = max(2, int(sw * (h / float(sh))) // 2 * 2)
    return w, h


# ------------------------------------------------------------------ #
class LivePreview:
    def __init__(self):
        self.gl = None
        self.sdk = None
        self.engine_ok = False
        self.engine_msg = "正在加载美颜引擎…"

        self.src = ""          # 当前源文件
        self.src_meta = None   # probe 结果
        self.cur_pos = None    # 已抽帧的时间点
        self.fw = self.fh = 0  # 预览单边尺寸
        self.src_np = None     # 源帧 RGB (h,w,3)
        self.out_np = None     # 效果帧 RGB
        self.combo_np = None   # 拼接缓冲

        self._in_buf = None
        self._out_buf = None
        self._in_ptr = None
        self._out_ptr = None
        self._frame_id = 0

        self.last_cfg = ""     # 上次生效的配置串
        self.last_hb = time.time()
        self.renders = 0
        self.last_render_ms = 0.0
        self.frames_decoded = 0
        self.t_start = time.time()

        self.root = tk.Tk()
        self.root.title("NamaBeauty 实时预览")
        self.root.configure(bg="#1b1b1b")
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)

        self.status = tk.Label(self.root, text=self.engine_msg,
                               bg="#1b1b1b", fg="#e8e8e8",
                               font=("Microsoft YaHei UI", 10), anchor="w",
                               padx=10, pady=6)
        self.status.pack(side="top", fill="x")

        bar = tk.Frame(self.root, bg="#1b1b1b")
        bar.pack(side="top", fill="x")
        tk.Label(bar, text="原始画面", bg="#1b1b1b", fg="#8fd3ff",
                 font=("Microsoft YaHei UI", 9)).pack(side="left", expand=True)
        tk.Label(bar, text="美颜效果", bg="#1b1b1b", fg="#ffb56b",
                 font=("Microsoft YaHei UI", 9)).pack(side="right", expand=True)

        self.canvas = tk.Label(self.root, bg="#101010", bd=0)
        self.canvas.pack(side="top")

        self.foot = tk.Label(self.root, text="等待达芬奇参数…",
                             bg="#1b1b1b", fg="#9a9a9a",
                             font=("Consolas", 9), anchor="w",
                             padx=10, pady=5)
        self.foot.pack(side="bottom", fill="x")

        self.root.update()   # 先把窗口显出来，再去做 1.8 秒的引擎初始化

    # -------------------------------------------------------------- #
    def on_close(self):
        log("用户关闭窗口，退出")
        self.cleanup()
        self.root.destroy()

    def cleanup(self):
        try:
            if self.sdk:
                self.sdk.dispose()
        except Exception:
            pass
        try:
            if self.gl:
                self.gl.close()
        except Exception:
            pass
        try:
            if os.path.exists(LIVE_PID):
                os.remove(LIVE_PID)
        except Exception:
            pass

    # -------------------------------------------------------------- #
    def ensure_engine(self):
        """GL 上下文 + SDK 初始化。只做一次，之后一直复用。"""
        if self.engine_ok:
            return True
        t0 = time.perf_counter()
        try:
            self.gl = W.GLContext()
            self.sdk = W.NamaSDK(W.ENGINE_DIR, log_level=3)
            ver = self.sdk.setup(detect_mode=1, max_faces=4)
            self.engine_ok = True
            dt = (time.perf_counter() - t0) * 1000
            self.engine_msg = "引擎就绪（SDK %s，初始化 %.0f ms）" % (ver, dt)
            log("引擎就绪 %s 用时 %.0f ms" % (ver, dt))
            return True
        except Exception as e:
            self.engine_msg = "引擎不可用：%s" % e
            log("引擎初始化失败：\n" + traceback.format_exc())
            return False

    # -------------------------------------------------------------- #
    def decode_frame(self, src, pos):
        """抽一帧并按预览尺寸缩放。返回 (h,w,3) uint8 或 None。"""
        meta = self.src_meta
        if not meta or meta["src"] != src:
            meta = W.probe_video(src)
            meta["src"] = src
            self.src_meta = meta
            sw, sh = meta["w"], meta["h"]
            self.fw, self.fh = pick_preview_size(
                sw, sh, self.root.winfo_screenwidth(), self.root.winfo_screenheight())
            self.canvas.configure(width=self.fw * 2 + GAP, height=self.fh)
            self._in_buf = self._out_buf = None
            self.src_np = self.out_np = self.combo_np = None
            log("源 %dx%d -> 预览单边 %dx%d" % (sw, sh, self.fw, self.fh))

        dur = meta.get("duration") or 0.0
        t = max(0.0, float(pos or 0.0))
        if dur > 0 and t >= dur:
            t = max(0.0, dur - 0.5)

        t0 = time.perf_counter()
        cmd = [W.FFMPEG, "-v", "error", "-nostdin",
               "-ss", "%.4f" % t, "-i", src,
               "-map", "0:v:0", "-frames:v", "1",
               "-vf", "scale=%d:%d" % (self.fw, self.fh),
               "-pix_fmt", "rgb24", "-f", "rawvideo", "-"]
        raw = subprocess.run(cmd, capture_output=True).stdout
        need = self.fw * self.fh * 3
        if len(raw) < need:
            log("抽帧失败 t=%.3f 得到 %d/%d 字节" % (t, len(raw), need))
            return None
        self.cur_pos = t
        self.frames_decoded += 1
        dt = (time.perf_counter() - t0) * 1000
        log("抽帧 t=%.3f 用时 %.0f ms" % (t, dt))
        return np.frombuffer(raw[:need], dtype=np.uint8).reshape(self.fh, self.fw, 3)

    # -------------------------------------------------------------- #
    def render(self, params, blur_type, skin_detect):
        """对当前源帧跑一次美颜，结果存到 self.out_np。"""
        w, h, need = self.fw, self.fh, self.fw * self.fh * 4
        if self._in_buf is None or len(self._in_buf) != need:
            self._in_buf = bytearray(need)
            self._out_buf = bytearray(need)
            self._in_arr = (ctypes.c_char * need).from_buffer(self._in_buf)
            self._out_arr = (ctypes.c_char * need).from_buffer(self._out_buf)
            self._in_ptr = ctypes.cast(self._in_arr, ctypes.c_void_p)
            self._out_ptr = ctypes.cast(self._out_arr, ctypes.c_void_p)

        # RGB -> 带 alpha 的 RGBA（SDK 只吃四通道）
        rgba = np.empty((h, w, 4), dtype=np.uint8)
        rgba[:, :, :3] = self.src_np
        rgba[:, :, 3] = 255
        ctypes.memmove(self._in_ptr, rgba.ctypes.data, need)

        self._frame_id += 1
        self.sdk.set_params(params, blur_type=blur_type, skin_detect=skin_detect)
        ret = self.sdk.render(self._in_ptr, self._out_ptr, w, h, self._frame_id)
        if ret < 0:
            log("渲染失败 ret=%d" % ret)
            return False

        out4 = np.frombuffer(self._out_buf, dtype=np.uint8).reshape(h, w, 4)
        self.out_np = np.ascontiguousarray(out4[:, :, :3])
        return True

    # -------------------------------------------------------------- #
    def refresh(self):
        """拼接 -> 写 PPM -> 贴到窗口，返回耗时毫秒。"""
        h, w = self.fh, self.fw
        cw = w * 2 + GAP
        if self.combo_np is None or self.combo_np.shape != (h, cw, 3):
            self.combo_np = np.empty((h, cw, 3), dtype=np.uint8)
        c = self.combo_np
        c[:, :w] = self.src_np
        c[:, w:w + GAP] = 255
        c[:, w + GAP:] = self.out_np

        t0 = time.perf_counter()
        with open(PPM_PATH, "wb") as f:
            f.write(b"P6\n%d %d\n255\n" % (cw, h))
            c.tofile(f)                      # 直接落盘，不经过 Python bytes
        img = tk.PhotoImage(file=PPM_PATH)
        self.canvas.configure(image=img)
        self.canvas.image = img
        self.root.update_idletasks()
        return (time.perf_counter() - t0) * 1000

    # -------------------------------------------------------------- #
    def read_cfg(self):
        """读 Lua 写来的配置；返回 (key, cfg) 或 (None, None)。"""
        try:
            with open(LIVE_JSON, "r", encoding="utf-8-sig") as f:
                cfg = json.load(f)
        except Exception:
            return None, None
        key = json.dumps([
            cfg.get("src"), round(float(cfg.get("pos") or 0.0), 3),
            cfg.get("params"), cfg.get("blur_type"), cfg.get("skin_detect"),
        ], sort_keys=True)
        return key, cfg

    def touch_hb(self):
        try:
            self.last_hb = os.path.getmtime(LIVE_HB)
        except Exception:
            pass

    # -------------------------------------------------------------- #
    def tick(self):
        try:
            # 面板主动关闭时写的退出标志：比等心跳超时快得多
            if os.path.exists(LIVE_QUIT):
                log("收到退出标志，收工")
                self.cleanup()
                self.root.destroy()
                return

            self.touch_hb()
            if time.time() - self.last_hb > HB_TIMEOUT:
                log("心跳超时（%.0fs），退出" % HB_TIMEOUT)
                self.cleanup()
                self.root.destroy()
                return

            key, cfg = self.read_cfg()
            if cfg is None or not cfg.get("src"):
                self.root.after(POLL_MS, self.tick)
                return

            if not self.engine_ok and not self.ensure_engine():
                self.status.configure(text=self.engine_msg)
                self.root.after(POLL_MS, self.tick)
                return

            src = cfg["src"]
            pos = float(cfg.get("pos") or 0.0)
            params = cfg.get("params") or {}
            blur_type = int(cfg.get("blur_type") or 1)
            skin = 1 if cfg.get("skin_detect") else 0

            t0 = time.perf_counter()
            changed = False

            # 源变了 / 播放头挪了 -> 重新抽帧
            need_decode = (src != self.src) or (self.cur_pos is None) \
                or (abs(pos - self.cur_pos) > 0.001)
            if need_decode:
                frame = self.decode_frame(src, pos)
                if frame is None:
                    self.root.after(POLL_MS, self.tick)
                    return
                self.src = src
                self.src_np = frame
                changed = True

            if changed or key != self.last_cfg:
                t1 = time.perf_counter()
                if not self.render(params, blur_type, skin):
                    self.root.after(POLL_MS, self.tick)
                    return
                self.last_render_ms = (time.perf_counter() - t1) * 1000
                self.renders += 1
                self.last_cfg = key
                ms_img = self.refresh()
                total = (time.perf_counter() - t0) * 1000
                fps = 1000.0 / total if total > 0 else 0
                self.status.configure(
                    text="实时预览中   渲染 %.1f ms   贴图 %.1f ms   本轮 %.1f ms (%.0f fps)"
                         % (self.last_render_ms, ms_img, total, fps))
                log("刷新 #%-4d 解码%s 渲染 %5.1f ms  贴图 %5.1f ms  本轮 %6.1f ms (%.0f fps)"
                    % (self.renders, "是" if need_decode else "否",
                       self.last_render_ms, ms_img, total, fps))
                self.foot.configure(
                    text="%s   位置 %.2fs   输出 %dx%d   刷新 %d 次   抽帧 %d 次"
                         % (os.path.basename(src), self.cur_pos, self.fw, self.fh,
                            self.renders, self.frames_decoded))
            else:
                self.status.configure(
                    text="实时预览中   渲染 %.1f ms   等待参数变化" % self.last_render_ms)

        except Exception:
            log("tick 异常：\n" + traceback.format_exc())

        self.root.after(POLL_MS, self.tick)

    # -------------------------------------------------------------- #
    def run(self):
        os.makedirs(LIVE_DIR, exist_ok=True)
        with open(LIVE_PID, "w", encoding="ascii") as f:
            f.write(str(os.getpid()))
        log("实时预览进程启动 pid=%d" % os.getpid())
        self.touch_hb()
        self.status.configure(text=self.engine_msg)
        self.root.after(50, self.tick)
        try:
            self.root.mainloop()
        finally:
            self.cleanup()
            log("实时预览进程结束")


def main():
    try:
        LivePreview().run()
    except Exception:
        log("致命异常：\n" + traceback.format_exc())


if __name__ == "__main__":
    main()
