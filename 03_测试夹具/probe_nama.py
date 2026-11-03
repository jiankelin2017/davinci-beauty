#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""可行性验证：用纯 stdlib + ctypes 直接驱动 旧版闭源 SDK（不经 BeautyCore.exe）。

目的：确认能否在单个 Python 进程内保持 SDK 常驻，从而对视频逐帧美颜。
"""
import os, sys, ctypes, zlib, struct, struct as _s
from ctypes import wintypes

BIN = os.path.expanduser("~") + r"\AppData\Local\Temp\e2e_beauty\bin"
ASSETS = os.path.expanduser("~") + r"\AppData\Local\Temp\e2e_beauty\assets"


# ---------- 极简 PNG 解码（纯 stdlib，仅支持 8bit / 非隔行） ----------
def png_read(path):
    d = open(path, "rb").read()
    assert d[:8] == b"\x89PNG\r\n\x1a\n", "not png"
    pos, idat, pal = 8, b"", None
    w = h = bitd = ct = None
    while pos < len(d):
        ln = struct.unpack(">I", d[pos:pos + 4])[0]
        typ = d[pos + 4:pos + 8]
        body = d[pos + 8:pos + 8 + ln]
        if typ == b"IHDR":
            w, h, bitd, ct, cm, filt, itl = struct.unpack(">IIBBBBB", body)
            assert bitd == 8 and itl == 0, f"unsupported png bitdepth={bitd} interlace={itl}"
        elif typ == b"PLTE":
            pal = body
        elif typ == b"IDAT":
            idat += body
        elif typ == b"IEND":
            break
        pos += 12 + ln
    raw = zlib.decompress(idat)
    nch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ct]
    stride = w * nch
    out = bytearray(h * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        f = raw[p]; p += 1
        line = bytearray(raw[p:p + stride]); p += stride
        if f == 1:
            for i in range(nch, stride):
                line[i] = (line[i] + line[i - nch]) & 255
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride):
                a = line[i - nch] if i >= nch else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 255
        elif f == 4:
            for i in range(stride):
                a = line[i - nch] if i >= nch else 0
                b = prev[i]
                c = prev[i - nch] if i >= nch else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        out[y * stride:(y + 1) * stride] = line
        prev = line
    # -> RGBA
    rgba = bytearray(w * h * 4)
    for i in range(w * h):
        s = i * nch
        if ct == 2:
            r, g, b, a = out[s], out[s + 1], out[s + 2], 255
        elif ct == 6:
            r, g, b, a = out[s], out[s + 1], out[s + 2], out[s + 3]
        elif ct == 0:
            r = g = b = out[s]; a = 255
        elif ct == 4:
            r = g = b = out[s]; a = out[s + 1]
        else:  # palette
            idx = out[s]
            r, g, b, a = pal[idx * 3], pal[idx * 3 + 1], pal[idx * 3 + 2], 255
        d4 = i * 4
        rgba[d4] = r; rgba[d4 + 1] = g; rgba[d4 + 2] = b; rgba[d4 + 3] = a
    return w, h, bytes(rgba)


# ---------- GL 上下文 ----------
class PFD(ctypes.Structure):
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
        ("dwLayerMask", ctypes.c_uint), ("dwVisibleMask", ctypes.c_uint), ("dwDamageMask", ctypes.c_uint),
    ]


class GLHost:
    def __init__(self):
        self.u32 = ctypes.WinDLL("user32", use_last_error=True)
        self.gdi = ctypes.WinDLL("gdi32", use_last_error=True)
        self.gl = ctypes.WinDLL("opengl32", use_last_error=True)
        self.u32.CreateWindowExA.restype = wintypes.HANDLE
        self.u32.CreateWindowExA.argtypes = [ctypes.c_uint32, ctypes.c_char_p, ctypes.c_char_p,
                                             ctypes.c_uint32, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                             ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p,
                                             ctypes.c_void_p, ctypes.c_void_p]
        self.u32.GetDC.restype = wintypes.HANDLE
        self.u32.GetDC.argtypes = [ctypes.c_void_p]
        self.gdi.ChoosePixelFormat.argtypes = [ctypes.c_void_p, ctypes.POINTER(PFD)]
        self.gdi.SetPixelFormat.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(PFD)]
        self.gdi.SetPixelFormat.restype = ctypes.c_bool
        self.gl.wglCreateContext.restype = ctypes.c_void_p
        self.gl.wglCreateContext.argtypes = [ctypes.c_void_p]
        self.gl.wglMakeCurrent.restype = ctypes.c_bool
        self.gl.wglMakeCurrent.argtypes = [ctypes.c_void_p, ctypes.c_void_p]

        pfd = PFD()
        pfd.nSize = ctypes.sizeof(PFD)
        pfd.nVersion = 1
        pfd.dwFlags = 0x20 | 0x1 | 0x4   # SUPPORT_OPENGL | DOUBLEBUFFER | DRAW_TO_WINDOW
        pfd.iPixelType = 0
        pfd.cColorBits = 32
        pfd.cAlphaBits = 8
        pfd.cDepthBits = 24
        pfd.cStencilBits = 8
        self.hwnd = self.u32.CreateWindowExA(0, b"EDIT", b"", 0, 0, 0, 1, 1, None, None, None, None)
        if not self.hwnd:
            self.hwnd = self.u32.CreateWindowExA(0, b"STATIC", b"", 0, 0, 0, 1, 1, None, None, None, None)
        if not self.hwnd:
            raise RuntimeError("CreateWindowExA failed: %d" % ctypes.get_last_error())
        self.dc = self.u32.GetDC(self.hwnd)
        fmt = self.gdi.ChoosePixelFormat(self.dc, ctypes.byref(pfd))
        if not fmt:
            raise RuntimeError("ChoosePixelFormat failed")
        if not self.gdi.SetPixelFormat(self.dc, fmt, ctypes.byref(pfd)):
            raise RuntimeError("SetPixelFormat failed")
        self.ctx = self.gl.wglCreateContext(self.dc)
        if not self.ctx:
            raise RuntimeError("wglCreateContext failed: %d" % ctypes.get_last_error())
        if not self.gl.wglMakeCurrent(self.dc, self.ctx):
            raise RuntimeError("wglMakeCurrent failed")
        print("[GL] ok hwnd=0x%X" % (self.hwnd or 0))


FACEPROCESSOR = 0x100


def main():
    gl = GLHost()
    # 关键：Windows 上必须显式指定 DLL_LOAD_DIR，否则同目录的 fuai.dll 不会被搜索到
    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR = 0x00000100
    LOAD_LIBRARY_SEARCH_DEFAULT_DIRS = 0x00001000
    DLLFLAGS = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.LoadLibraryExW.argtypes = [ctypes.c_wchar_p, ctypes.c_void_p, ctypes.c_uint32]
    k32.LoadLibraryExW.restype = ctypes.c_void_p
    handles = {}
    for pre in ("w32-pthreads.dll", "fuai.dll"):
        p = os.path.join(BIN, pre)
        if os.path.exists(p):
            h = k32.LoadLibraryExW(p, None, DLLFLAGS)
            handles[pre] = h
            print("[preload] %s -> 0x%X (err=%d)" % (pre, h or 0, ctypes.get_last_error()))
    hnama = k32.LoadLibraryExW(os.path.join(BIN, "CNamaSDK.dll"), None, DLLFLAGS)
    if not hnama:
        raise RuntimeError("LoadLibrary 旧版闭源 SDK failed: %d" % ctypes.get_last_error())
    nama = ctypes.CDLL(os.path.join(BIN, "CNamaSDK.dll"), handle=hnama)
    nama.fuSetup.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]
    nama.fuSetup.restype = ctypes.c_int
    nama.fuGetVersion.restype = ctypes.c_char_p
    nama.fuIsLibraryInit.restype = ctypes.c_int
    nama.fuGetSystemError.restype = ctypes.c_int
    nama.fuLoadAIModelFromPackage.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
    nama.fuLoadAIModelFromPackage.restype = ctypes.c_int
    nama.fuCreateItemFromPackage.argtypes = [ctypes.c_char_p, ctypes.c_int]
    nama.fuCreateItemFromPackage.restype = ctypes.c_int
    nama.fuItemSetParamd.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_double]
    nama.fuItemSetParams.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_char_p]
    nama.fuFaceProcessorGetNumResults.restype = ctypes.c_int
    nama.fuRenderItemsEx.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p,
                                     ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                     ctypes.POINTER(ctypes.c_int), ctypes.c_int]
    nama.fuRenderItemsEx.restype = ctypes.c_int
    nama.fuDestroyAllItems.restype = None
    nama.fuOnDeviceLost.restype = None
    nama.fuDestroyLibData.restype = None
    nama.fuSetFaceProcessorDetectMode.argtypes = [ctypes.c_int]
    nama.fuFaceProcessorSetFaceLandmarkQuality.argtypes = [ctypes.c_int]
    nama.fuSetMaxFaces.argtypes = [ctypes.c_int]
    nama.fuSetDefaultRotationMode.argtypes = [ctypes.c_int]

    auth = open(os.path.join(BIN, "auth_package.bin"), "rb").read()
    pauth = ctypes.create_string_buffer(auth, len(auth))
    r = nama.fuSetup(None, 0, None, pauth, len(auth))
    print("[fuSetup] ret=%d err=%d ver=%s init=%d" % (r, nama.fuGetSystemError(),
                                                     nama.fuGetVersion(), nama.fuIsLibraryInit()))
    if r == 0:
        return 3

    face_model = open(os.path.join(ASSETS, "model", "ai_face_processor_pc.bundle"), "rb").read()
    a = nama.fuLoadAIModelFromPackage(face_model, len(face_model), FACEPROCESSOR)
    print("[fuLoadAIModel FACEPROCESSOR] ret=%d err=%d" % (a, nama.fuGetSystemError()))

    nama.fuSetFaceProcessorDetectMode(0)
    nama.fuFaceProcessorSetFaceLandmarkQuality(1)
    nama.fuSetMaxFaces(4)
    nama.fuSetDefaultRotationMode(0)

    beauty_pkg = open(os.path.join(ASSETS, "graphics", "face_beautification.bundle"), "rb").read()
    hb = nama.fuCreateItemFromPackage(beauty_pkg, len(beauty_pkg))
    print("[createItem face_beautification] handle=%d err=%d" % (hb, nama.fuGetSystemError()))
    if hb <= 0:
        return 4

    for k, v in [("is_opengl_es", 0), ("is_beauty_on", 1), ("use_landmark", 1),
                 ("filter_level", 0.4), ("blur_level", 4.2), ("blur_type", 2),
                 ("color_level", 0.3), ("red_level", 0.3), ("sharpen", 0.2),
                 ("face_shape", 4), ("face_shape_level", 1.0),
                 ("eye_enlarging", 0.4), ("cheek_thinning", 0.5),
                 ("heavy_blur", 0), ("skin_detect", 1)]:
        nama.fuItemSetParamd(hb, k.encode(), float(v))
    nama.fuItemSetParams(hb, b"filter_name", b"ziran2")

    src = os.path.join(os.path.dirname(os.path.abspath(__file__)), "face_in.png")
    w, h, rgba = png_read(src)
    print("[png] %dx%d bytes=%d" % (w, h, len(rgba)))

    buf_in = ctypes.create_string_buffer(rgba, len(rgba))
    buf_out = ctypes.create_string_buffer(len(rgba))
    items = (ctypes.c_int * 1)(hb)
    fid = ctypes.c_int(0)
    for i in range(2):  # 第一帧预热人脸检测
        ret = nama.fuRenderItemsEx(0, buf_out, 0, buf_in, w, h, fid, items, 1)
        fid.value += 1
    faces = nama.fuFaceProcessorGetNumResults()
    dst = buf_out.raw[:len(rgba)]
    diff = sum(1 for i in range(0, len(rgba), 997) if rgba[i] != dst[i])
    print("[render] ret=%d faces=%d diffSample=%d" % (ret, faces, diff))

    # 输出为 PPM 便于肉眼核对
    outp = os.path.join(os.path.dirname(os.path.abspath(__file__)), "probe_out.ppm")
    with open(outp, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, h))
        f.write(bytearray(rgba[i * 4:i * 4 + 3] for i in range(w * h)))
    print("[out] %s" % outp)

    nama.fuDestroyAllItems(); nama.fuOnDeviceLost(); nama.fuDestroyLibData()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:
        import traceback; traceback.print_exc()
        sys.exit(99)
