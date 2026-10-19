#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""NamaSDK 视频美颜工作进程（独立进程，不跑在 Resolve GUI 里）。

职责：加载 FaceUnity NamaSDK -> 用 ffmpeg 把源视频解成 RGBA 原始帧流 ->
      逐帧调用 fuRenderItemsEx 做美颜 -> 再用 ffmpeg 把帧流编码回视频（保留原音轨）。

设计要点（全部来自本机实测，见同目录 README）：
  * SDK 在进程内只初始化一次，逐帧循环调用渲染，避免逐帧重启进程带来的巨额开销。
  * Windows 上必须用 LoadLibraryEx + LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR，否则同目录
    的 fuai.dll 不在搜索路径里，ctypes.CDLL 会报 "找不到模块或其依赖"。
  * 面板刻度 0..100 到 SDK 真值的换算为：sdk = panel / 100 * sdk_max。
"""

import argparse
import ctypes
import json
import os
import subprocess
import sys
import time
from ctypes import wintypes

_DEFAULT_ROOT = r"C:\ProgramData\NamaBeauty"


def _find_root():
    """fuscript 以 exec 方式执行脚本，它没有 __file__，所以要从 argv 兜底定位根目录。"""
    candidates = []
    try:
        candidates.append(globals()["__file__"])
    except KeyError:
        pass
    if sys.argv:
        candidates.append(sys.argv[0])
    for c in candidates:
        if not c:
            continue
        abs_path = os.path.abspath(c)
        if os.path.isfile(abs_path):
            return os.path.dirname(abs_path)
    return _DEFAULT_ROOT


ROOT = _find_root()
ENGINE_DIR = os.path.join(ROOT, "engine")
FFMPEG = os.path.join(ROOT, "bin", "ffmpeg.exe")
FFPROBE = os.path.join(ROOT, "bin", "ffprobe.exe")
LOGS = os.path.join(ROOT, "logs")

FUAITYPE_FACEPROCESSOR = 0x100  # 逆向实测值，非公开文档里的 1<<10

# 面板参数定义：(键, 中文标签, SDK 上限, 默认面板值, 实测备注)
PARAM_DEFS = [
    ("blur_level", "磨皮", 6.0, 70.0,
     "SDK 真值 0..6。实测平均差异 2.579 / 改动像素 26.36%"),
    ("clarity", "清晰", 1.0, 0.0,
     "SDK 真值 0..1。实测平均差异 6.536 / 改动像素 89.29%"),
    ("color_level", "美白", 2.0, 15.0,
     "SDK 真值 0..2。实测平均差异 21.586 / 改动像素 99.37%"),
    ("red_level", "红润", 1.0, 30.0,
     "SDK 真值 0..1。实测平均差异 3.625 / 改动像素 57.78%"),
    ("sharpen", "锐化", 1.0, 0.0,
     "SDK 真值 0..1。实测平均差异 1.898 / 改动像素 8.17%；磨皮不为 0 时会被完全掩盖"),
    ("face_threed", "五官立体", 1.0, 0.0,
     "SDK 真值 0..1。实测平均差异 1.474 / 改动像素 15.61%"),
    ("eye_bright", "亮眼", 1.0, 0.0,
     "SDK 真值 0..1。实测平均差异 0.086 / 改动像素 0.65%，局部有效但很弱"),
    ("remove_pouch_strength", "去黑眼圈", 1.0, 0.0,
     "SDK 真值 0..1。实测平均差异 0.075 / 改动像素 0.93%，局部有效但很弱"),
]
PARAM_BY_KEY = {p[0]: p for p in PARAM_DEFS}

# 磨皮算法：实测细节能量（越小越平滑；完全不磨皮 = 4.184）
BLUR_TYPES = [
    (1, "双边滤波（最强·推荐）", 2.358),
    (0, "高斯（中等）", 2.719),
    (2, "弱磨皮", 3.762),
]

PRESETS = {
    "h264_nvenc": {
        "ext": ".mp4",
        "vcodec": ["-c:v", "h264_nvenc", "-preset", "p4", "-cq", "20", "-pix_fmt", "yuv420p"],
        "acodec": ["-c:a", "aac", "-b:a", "192k"],
        "label": "H.264 / NVENC 显卡加速",
    },
    "libx264": {
        "ext": ".mp4",
        "vcodec": ["-c:v", "libx264", "-preset", "medium", "-crf", "18", "-pix_fmt", "yuv420p"],
        "acodec": ["-c:a", "aac", "-b:a", "192k"],
        "label": "H.264 / CPU 通用",
    },
    "prores422": {
        "ext": ".mov",
        "vcodec": ["-c:v", "prores_ks", "-profile:v", "3", "-pix_fmt", "yuv422p10le"],
        "acodec": ["-c:a", "pcm_s16le"],
        "label": "Apple ProRes 422 HQ",
    },
}


PROGRESS_PATH = ""
RESULT_PATH = ""


def log(msg):
    """fuscript 把 sys.stdout 换成自定义的 fu_stdout，它没有 flush()，必须做保护。"""
    try:
        sys.stdout.write("[worker] %s\n" % msg)
    except Exception:
        pass
    flush = getattr(sys.stdout, "flush", None)
    if callable(flush):
        try:
            flush()
        except Exception:
            pass
    # 同步镜像一行到进度文件：Resolve 前端靠定时器轮询它来刷新进度条，
    # 免得界面卡死在「处理中」。每次整体覆写，前端永远读到最新一行。
    if PROGRESS_PATH:
        try:
            with open(PROGRESS_PATH, "w", encoding="utf-8") as f:
                f.write(msg + "\n")
        except Exception:
            pass


# ------------------------------------------------------------------ #
#  OpenGL 隐藏上下文（NamaSDK 内部走 GPU 渲染，宿主要提供 current context）
# ------------------------------------------------------------------ #
class PIXELFORMATDESCRIPTOR(ctypes.Structure):
    _fields_ = [
        ("nSize", ctypes.c_ushort), ("nVersion", ctypes.c_ushort), ("dwFlags", ctypes.c_uint),
        ("iPixelType", ctypes.c_ubyte), ("cColorBits", ctypes.c_ubyte),
        ("cRedBits", ctypes.c_ubyte), ("cRedShift", ctypes.c_ubyte),
        ("cGreenBits", ctypes.c_ubyte), ("cGreenShift", ctypes.c_ubyte),
        ("cBlueBits", ctypes.c_ubyte), ("cBlueShift", ctypes.c_ubyte),
        ("cAlphaBits", ctypes.c_ubyte), ("cAlphaShift", ctypes.c_ubyte),
        ("cAccumBits", ctypes.c_ubyte), ("cAccumRedBits", ctypes.c_ubyte),
        ("cAccumGreenBits", ctypes.c_ubyte), ("cAccumBlueBits", ctypes.c_ubyte),
        ("cAccumAlphaBits", ctypes.c_ubyte),
        ("cDepthBits", ctypes.c_ubyte), ("cStencilBits", ctypes.c_ubyte),
        ("cAuxBuffers", ctypes.c_ubyte), ("iLayerType", ctypes.c_ubyte),
        ("bReserved", ctypes.c_ubyte),
        ("dwLayerMask", ctypes.c_uint), ("dwVisibleMask", ctypes.c_uint),
        ("dwDamageMask", ctypes.c_uint),
    ]


class GLContext:
    def __init__(self):
        self.user32 = ctypes.WinDLL("user32", use_last_error=True)
        self.gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
        self.opengl32 = ctypes.WinDLL("opengl32", use_last_error=True)

        self.user32.CreateWindowExA.restype = wintypes.HANDLE
        self.user32.CreateWindowExA.argtypes = [
            ctypes.c_uint32, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint32,
            ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
        self.user32.GetDC.restype = wintypes.HANDLE
        self.user32.GetDC.argtypes = [ctypes.c_void_p]
        self.gdi32.ChoosePixelFormat.argtypes = [ctypes.c_void_p,
                                                 ctypes.POINTER(PIXELFORMATDESCRIPTOR)]
        self.gdi32.SetPixelFormat.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                              ctypes.POINTER(PIXELFORMATDESCRIPTOR)]
        self.gdi32.SetPixelFormat.restype = ctypes.c_bool
        self.opengl32.wglCreateContext.restype = ctypes.c_void_p
        self.opengl32.wglCreateContext.argtypes = [ctypes.c_void_p]
        self.opengl32.wglMakeCurrent.restype = ctypes.c_bool
        self.opengl32.wglMakeCurrent.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        self.user32.DestroyWindow.restype = ctypes.c_bool
        self.user32.DestroyWindow.argtypes = [ctypes.c_void_p]
        self.user32.ReleaseDC.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        self.opengl32.wglDeleteContext.restype = ctypes.c_bool
        self.opengl32.wglDeleteContext.argtypes = [ctypes.c_void_p]

        pfd = PIXELFORMATDESCRIPTOR()
        pfd.nSize = ctypes.sizeof(PIXELFORMATDESCRIPTOR)
        pfd.nVersion = 1
        pfd.dwFlags = 0x20 | 0x01 | 0x04  # SUPPORT_OPENGL | DOUBLEBUFFER | DRAW_TO_WINDOW
        pfd.iPixelType = 0                # PFD_TYPE_RGBA
        pfd.cColorBits = 32
        pfd.cAlphaBits = 8
        pfd.cDepthBits = 24
        pfd.cStencilBits = 8

        self.hwnd = self.user32.CreateWindowExA(
            0, b"STATIC", b"", 0, 0, 0, 1, 1, None, None, None, None)
        if not self.hwnd:
            raise RuntimeError("CreateWindowExA 失败，错误码 %d" % ctypes.get_last_error())
        self.dc = self.user32.GetDC(self.hwnd)
        if not self.dc:
            raise RuntimeError("GetDC 失败")
        fmt = self.gdi32.ChoosePixelFormat(self.dc, ctypes.byref(pfd))
        if not fmt:
            raise RuntimeError("ChoosePixelFormat 失败（可能没有可用的 GPU / OpenGL 驱动）")
        if not self.gdi32.SetPixelFormat(self.dc, fmt, ctypes.byref(pfd)):
            raise RuntimeError("SetPixelFormat 失败")
        self.ctx = self.opengl32.wglCreateContext(self.dc)
        if not self.ctx:
            raise RuntimeError("wglCreateContext 失败")
        if not self.opengl32.wglMakeCurrent(self.dc, self.ctx):
            raise RuntimeError("wglMakeCurrent 失败")

    def close(self):
        try:
            if self.ctx:
                self.opengl32.wglMakeCurrent(None, None)
                self.opengl32.wglDeleteContext(self.ctx)
            if self.hwnd and self.dc:
                self.user32.ReleaseDC(self.hwnd, self.dc)
            if self.hwnd:
                self.user32.DestroyWindow(self.hwnd)
        except Exception:
            pass


# ------------------------------------------------------------------ #
#  NamaSDK 封装
# ------------------------------------------------------------------ #
class NamaSDK:
    def __init__(self, engine_dir, log_level=3):
        self.engine_dir = engine_dir
        self.handle = None
        self.lib = None
        self.item = 0
        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        k32.LoadLibraryExW.argtypes = [ctypes.c_wchar_p, ctypes.c_void_p, ctypes.c_uint32]
        k32.LoadLibraryExW.restype = ctypes.c_void_p
        self._k32 = k32
        self._flags = 0x00000100 | 0x00001000  # DLL_LOAD_DIR | DEFAULT_DIRS

        for pre in ("w32-pthreads.dll", "fuai.dll"):
            p = os.path.join(engine_dir, pre)
            if os.path.exists(p):
                self._k32.LoadLibraryExW(p, None, self._flags)

        dll = os.path.join(engine_dir, "CNamaSDK.dll")
        if not os.path.exists(dll):
            raise RuntimeError("找不到引擎：%s" % dll)
        handle = k32.LoadLibraryExW(dll, None, self._flags)
        if not handle:
            raise RuntimeError("加载 CNamaSDK.dll 失败，错误码 %d" % ctypes.get_last_error())
        self.handle = handle
        lib = ctypes.CDLL(dll, handle=handle)
        self.lib = lib

        lib.fuSetup.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p,
                                ctypes.c_void_p, ctypes.c_int]
        lib.fuSetup.restype = ctypes.c_int
        lib.fuGetVersion.restype = ctypes.c_char_p
        lib.fuIsLibraryInit.restype = ctypes.c_int
        lib.fuGetSystemError.restype = ctypes.c_int
        lib.fuSetLogLevel.argtypes = [ctypes.c_int]
        lib.fuLoadAIModelFromPackage.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
        lib.fuLoadAIModelFromPackage.restype = ctypes.c_int
        lib.fuCreateItemFromPackage.argtypes = [ctypes.c_char_p, ctypes.c_int]
        lib.fuCreateItemFromPackage.restype = ctypes.c_int
        lib.fuItemSetParamd.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_double]
        lib.fuItemSetParamd.restype = ctypes.c_int
        lib.fuItemSetParams.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_char_p]
        lib.fuItemSetParams.restype = ctypes.c_int
        lib.fuFaceProcessorGetNumResults.restype = ctypes.c_int
        lib.fuSetFaceProcessorDetectMode.argtypes = [ctypes.c_int]
        lib.fuSetFaceProcessorDetectMode.restype = ctypes.c_int
        lib.fuFaceProcessorSetFaceLandmarkQuality.argtypes = [ctypes.c_int]
        lib.fuSetMaxFaces.argtypes = [ctypes.c_int]
        lib.fuSetMaxFaces.restype = ctypes.c_int
        lib.fuSetDefaultRotationMode.argtypes = [ctypes.c_int]
        lib.fuRenderItemsEx.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_int,
                                        ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                                        ctypes.c_int, ctypes.POINTER(ctypes.c_int),
                                        ctypes.c_int]
        lib.fuRenderItemsEx.restype = ctypes.c_int
        lib.fuDestroyAllItems.restype = None
        lib.fuOnDeviceLost.restype = None
        lib.fuDestroyLibData.restype = None

        lib.fuSetLogLevel(log_level)
        self._items = (ctypes.c_int * 1)(0)

    def err(self):
        return self.lib.fuGetSystemError()

    def setup(self, detect_mode=1, max_faces=4):
        auth_path = os.path.join(self.engine_dir, "auth_package.bin")
        auth = open(auth_path, "rb").read() if os.path.exists(auth_path) else b""
        buf = ctypes.create_string_buffer(auth, len(auth)) if auth else None
        ret = self.lib.fuSetup(None, 0, None, buf, len(auth))
        if ret == 0:
            raise RuntimeError("fuSetup 失败（err=%d），授权包缺失或损坏：%s"
                               % (self.err(), auth_path))
        version = (self.lib.fuGetVersion() or b"").decode("ascii", "replace")

        model = os.path.join(self.engine_dir, "model", "ai_face_processor_pc.bundle")
        if not os.path.exists(model):
            raise RuntimeError("找不到人脸模型：%s" % model)
        data = open(model, "rb").read()
        ok = self.lib.fuLoadAIModelFromPackage(data, len(data), FUAITYPE_FACEPROCESSOR)
        if ok == 0:
            raise RuntimeError("人脸 AI 模型加载失败（err=%d）" % self.err())

        self.lib.fuSetFaceProcessorDetectMode(detect_mode)
        self.lib.fuFaceProcessorSetFaceLandmarkQuality(1)
        self.lib.fuSetMaxFaces(max_faces)
        self.lib.fuSetDefaultRotationMode(0)

        pkg = os.path.join(self.engine_dir, "graphics", "face_beautification.bundle")
        b = open(pkg, "rb").read()
        self.item = self.lib.fuCreateItemFromPackage(b, len(b))
        if self.item <= 0:
            raise RuntimeError("美颜道具加载失败（err=%d）" % self.err())
        self._items[0] = self.item
        return version

    def set_params(self, panel_params, blur_type=1, skin_detect=0):
        """panel_params 里的值是 0..100 的面板刻度，这里统一换算后写入 SDK。"""
        applied = {}
        self.lib.fuItemSetParamd(self.item, b"is_opengl_es", 0.0)
        self.lib.fuItemSetParamd(self.item, b"is_beauty_on", 1.0)
        self.lib.fuItemSetParamd(self.item, b"use_landmark", 1.0)
        # 只做皮肤与眼部类效果：美型模块关闭、滤镜关闭
        self.lib.fuItemSetParamd(self.item, b"face_shape", 0.0)
        self.lib.fuItemSetParamd(self.item, b"face_shape_level", 0.0)
        self.lib.fuItemSetParams(self.item, b"filter_name", b"origin")
        self.lib.fuItemSetParamd(self.item, b"filter_level", 0.0)
        self.lib.fuItemSetParamd(self.item, b"blur_type", float(blur_type))
        self.lib.fuItemSetParamd(self.item, b"skin_detect", 1.0 if skin_detect else 0.0)
        self.lib.fuItemSetParamd(self.item, b"heavy_blur", 0.0)

        for key, _, sdk_max, default, _ in PARAM_DEFS:
            panel = float(panel_params.get(key, default))
            panel = max(0.0, min(100.0, panel))
            sdk = panel / 100.0 * sdk_max
            self.lib.fuItemSetParamd(self.item, key.encode("ascii"), sdk)
            applied[key] = sdk
        return applied

    def render(self, in_ptr, out_ptr, w, h, frame_id):
        return self.lib.fuRenderItemsEx(0, out_ptr, 0, in_ptr, w, h, frame_id,
                                        self._items, 1)

    def faces(self):
        return self.lib.fuFaceProcessorGetNumResults()

    def dispose(self):
        try:
            if self.lib:
                self.lib.fuDestroyAllItems()
                self.lib.fuOnDeviceLost()
                self.lib.fuDestroyLibData()
        except Exception:
            pass


# ------------------------------------------------------------------ #
#  ffmpeg 元数据探测
# ------------------------------------------------------------------ #
def probe_video(path):
    cmd = [FFPROBE, "-v", "error", "-select_streams", "v:0",
           "-show_entries", "stream=width,height,r_frame_rate,codec_name:format=duration",
           "-of", "json", path]
    out = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                         errors="replace").stdout
    info = json.loads(out or "{}")
    st = (info.get("streams") or [{}])[0]
    num, den = (st.get("r_frame_rate") or "30/1").split("/")
    fps = float(num) / float(den) if float(den) else 30.0
    duration = float((info.get("format") or {}).get("duration") or 0.0)
    width = int(st.get("width") or 0)
    height = int(st.get("height") or 0)
    if not width or not height:
        raise RuntimeError("ffprobe 无法读取视频尺寸，文件可能不是有效视频：%s" % path)
    # 音轨探测
    cmd_a = [FFPROBE, "-v", "error", "-select_streams", "a:0",
             "-show_entries", "stream=codec_name", "-of", "json", path]
    out_a = subprocess.run(cmd_a, capture_output=True, text=True,
                           encoding="utf-8", errors="replace").stdout
    ainfo = json.loads(out_a or "{}")
    has_audio = bool(ainfo.get("streams"))
    return {"w": width, "h": height, "fps": fps, "duration": duration,
            "codec": st.get("codec_name"), "audio": has_audio}


# ------------------------------------------------------------------ #
#  主处理流程
# ------------------------------------------------------------------ #
def process(args):
    meta = probe_video(args.src)
    src_w, src_h, fps = meta["w"], meta["h"], meta["fps"]
    duration = meta["duration"]

    start = max(0.0, float(args.start or 0.0))
    end = float(args.end) if args.end else duration
    if end <= 0:
        end = duration
    end = min(end, duration) if duration > 0 else end
    dur = end - start
    if dur <= 0:
        raise RuntimeError("处理区间无效：start=%.3f end=%.3f" % (start, end))

    # 输出尺寸（可降采样提速；宽高必须为偶数）
    if args.max_width and src_w > args.max_width:
        scale = args.max_width / float(src_w)
        w = int(src_w * scale) // 2 * 2
        h = int(src_h * scale) // 2 * 2
    else:
        w, h = src_w, src_h
    w = max(2, (w // 2) * 2)
    h = max(2, (h // 2) * 2)

    preset = PRESETS.get(args.preset, PRESETS["libx264"])
    out_path = args.out
    if not out_path:
        stem = os.path.splitext(os.path.basename(args.src))[0]
        out_path = os.path.join(os.path.dirname(os.path.abspath(args.src)),
                                stem + "_beauty" + preset["ext"])
    os.makedirs(os.path.dirname(os.path.abspath(out_path)) or ".", exist_ok=True)

    panel_params = json.loads(args.params) if args.params else {}

    log("源：%s  %dx%d %.3ffps  区间 %.2f~%.2fs  输出 %dx%d" %
        (os.path.basename(args.src), src_w, src_h, fps, start, end, w, h))

    gl = GLContext()
    sdk = NamaSDK(ENGINE_DIR, log_level=0 if args.verbose else 3)
    version = None
    dec = enc = None
    frames_done = 0
    total_frames = int(dur * fps) if fps > 0 else 0
    t0 = time.time()

    try:
        version = sdk.setup(detect_mode=1 if not args.image_mode else 0,
                            max_faces=args.max_faces)
        applied = sdk.set_params(panel_params, blur_type=args.blur_type,
                                 skin_detect=args.skin_detect)
        log("SDK %s 就绪，参数已写入" % version)
        log("生效值：" + ", ".join("%s=%.3f" % (k, v) for k, v in applied.items()))

        frame_size = w * h * 4
        in_buf = bytearray(frame_size)
        out_buf = bytearray(frame_size)
        in_arr = (ctypes.c_char * frame_size).from_buffer(in_buf)
        out_arr = (ctypes.c_char * frame_size).from_buffer(out_buf)
        in_ptr = ctypes.cast(in_arr, ctypes.c_void_p)
        out_ptr = ctypes.cast(out_arr, ctypes.c_void_p)
        in_mv = memoryview(in_buf)
        out_mv = memoryview(out_buf)

        # 解码：源 -> RGBA 原始帧流
        dec_cmd = [FFMPEG, "-v", "error", "-nostdin"]
        if start > 0:
            dec_cmd += ["-ss", "%.6f" % start]
        dec_cmd += ["-i", args.src]
        if dur > 0:
            dec_cmd += ["-t", "%.6f" % dur]
        if (w, h) != (src_w, src_h):
            dec_cmd += ["-vf", "scale=%d:%d" % (w, h)]
        dec_cmd += ["-map", "0:v:0", "-an", "-sn", "-dn",
                    "-pix_fmt", "rgba", "-f", "rawvideo", "-"]
        log("解码：" + " ".join(dec_cmd[1:]))
        dec = subprocess.Popen(dec_cmd, stdout=subprocess.PIPE,
                               stderr=subprocess.DEVNULL, bufsize=frame_size)

        # 编码：RGBA 帧流 -> 成品，音轨直接从原片对应区间取
        enc_cmd = [FFMPEG, "-v", "error", "-nostdin", "-y",
                   "-f", "rawvideo", "-pixel_format", "rgba",
                   "-video_size", "%dx%d" % (w, h), "-framerate", "%.6f" % fps,
                   "-i", "pipe:0"]
        if meta["audio"]:
            enc_cmd += ["-ss", "%.6f" % start, "-t", "%.6f" % dur, "-i", args.src,
                        "-map", "0:v:0", "-map", "1:a:0", "-shortest"]
        else:
            enc_cmd += ["-map", "0:v:0", "-an"]
        enc_cmd += preset["vcodec"]
        if meta["audio"]:
            enc_cmd += preset["acodec"] + ["-map_metadata", "1"]
        enc_cmd += ["-movflags", "+faststart", out_path]
        log("编码：" + " ".join(enc_cmd[1:]))
        enc = subprocess.Popen(enc_cmd, stdin=subprocess.PIPE,
                               stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL, bufsize=frame_size)

        frame_id = 0
        faces_seen = 0
        while True:
            got = 0
            while got < frame_size:
                chunk = dec.stdout.readinto(in_mv[got:])
                if not chunk:
                    break
                got += chunk
            if got != frame_size:
                break

            ret = sdk.render(in_ptr, out_ptr, w, h, frame_id)
            if frame_id == 0:
                # 首帧再渲染一次：SDK 的人脸检测在首帧只做预热，第二帧才带效果
                ret = sdk.render(in_ptr, out_ptr, w, h, frame_id)
            frame_id += 1
            if ret < 0:
                raise RuntimeError("第 %d 帧渲染失败，返回值 %d" % (frame_id, ret))
            if frame_id % 15 == 0:
                faces_seen = max(faces_seen, sdk.faces())

            enc.stdin.write(out_mv)
            frames_done = frame_id
            if total_frames and frame_id % 20 == 0:
                pct = frame_id * 100.0 / total_frames
                el = time.time() - t0
                fps_now = frame_id / el if el > 0 else 0
                eta = (total_frames - frame_id) / fps_now if fps_now > 0 else 0
                log("进度 %d/%d (%.1f%%) %.1f fps 预计剩余 %.0fs"
                    % (frame_id, total_frames, pct, fps_now, eta))

        try:
            enc.stdin.close()
        except Exception:
            pass
        rc_enc = enc.wait(timeout=600)
        try:
            dec.stdout.close()
        except Exception:
            pass
        dec.wait(timeout=60)
        if rc_enc != 0:
            raise RuntimeError("ffmpeg 编码失败，退出码 %d，详见渲染日志" % rc_enc)

    finally:
        for p in (dec, enc):
            if p is not None:
                try:
                    if p.poll() is None:
                        p.kill()
                except Exception:
                    pass
        try:
            sdk.dispose()
        except Exception:
            pass
        try:
            gl.close()
        except Exception:
            pass

    elapsed = time.time() - t0
    size = os.path.getsize(out_path) if os.path.exists(out_path) else 0
    log("完成 %d 帧，耗时 %.1fs（平均 %.1f fps），输出 %.1f MB"
        % (frames_done, elapsed, frames_done / elapsed if elapsed else 0,
           size / 1048576.0))
    return {
        "ok": True,
        "out": out_path,
        "frames": frames_done,
        "elapsed": round(elapsed, 2),
        "fps_avg": round(frames_done / elapsed, 2) if elapsed else 0,
        "w": w, "h": h, "fps": fps,
        "size": size,
        "sdk_version": version,
        "faces": faces_seen,
    }


def main():
    ap = argparse.ArgumentParser(description="NamaSDK 视频美颜 worker")
    ap.add_argument("--src", help="源视频路径")
    ap.add_argument("--out", default="", help="输出路径（留空则自动生成）")
    ap.add_argument("--start", type=float, default=0.0, help="起始秒")
    ap.add_argument("--end", type=float, default=0.0, help="结束秒，0=到片尾")
    ap.add_argument("--params", default="", help='JSON 面板参数，如 {"blur_level":60}')
    ap.add_argument("--blur-type", type=int, default=1, help="磨皮算法 1=双边 0=高斯 2=弱磨皮")
    ap.add_argument("--skin-detect", action="store_true", help="磨皮只作用于检测到的皮肤区域")
    ap.add_argument("--image-mode", action="store_true", help="逐帧独立检测（关掉视频跟踪）")
    ap.add_argument("--max-faces", type=int, default=4)
    ap.add_argument("--max-width", type=int, default=0, help="降采样上限宽度，0=不降")
    ap.add_argument("--preset", default="h264_nvenc", choices=sorted(PRESETS.keys()))
    ap.add_argument("--job", default="", help="JSON 作业文件路径，覆盖命令行参数")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    if args.job:
        # utf-8-sig 兼容带 BOM 的文件（记事本保存的 JSON 会带 BOM）
        job = json.loads(open(args.job, "r", encoding="utf-8-sig").read())
        # 进度 / 结果文件路径由前端指定，不属于 argparse 参数，单独取
        global PROGRESS_PATH, RESULT_PATH
        PROGRESS_PATH = str(job.get("progress") or "")
        RESULT_PATH = str(job.get("result") or "")
        for k, v in job.items():
            dest = k.replace("-", "_")
            if not hasattr(args, dest):
                continue
            if dest == "params" and not isinstance(v, str):
                args.params = json.dumps(v, ensure_ascii=False)
            else:
                setattr(args, dest, v)

    if not args.src:
        raise SystemExit("必须提供 --src 或 --job")

    try:
        result = process(args)
    except Exception as e:
        result = {"ok": False, "error": str(e)}
        log("失败：%s" % e)
    # 结构化结果走最后一行，方便调用方从混杂的 SDK 日志里稳定解析
    print("#NBRESULT " + json.dumps(result, ensure_ascii=False))
    flush = getattr(sys.stdout, "flush", None)
    if callable(flush):
        try:
            flush()
        except Exception:
            pass
    # 单独落一份结果文件：前端靠「这个文件出现了」判断 worker 已结束，
    # 不用去猜进程还在不在，也不用解析混杂的 stdout 日志
    if RESULT_PATH:
        try:
            with open(RESULT_PATH, "w", encoding="utf-8") as f:
                f.write(json.dumps(result, ensure_ascii=False))
        except Exception:
            pass
    return 0 if result.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
