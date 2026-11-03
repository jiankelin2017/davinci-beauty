#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Phase 0 探测：旧版闭源 SDK 到底暴露了哪些「区域」能力。

为什么需要它
------------
磨皮的区域目前在 SDK 内部（`skin_detect` 一个 bool），我们既看不到也调不了；
美白用的是插件自己的颜色启发式。想把两者统一成人脸/皮肤区域，先得知道
SDK 给不给这些东西：人脸框、landmarks、head/hair mask、遮挡、按掩码渲染。

导出表里这些函数是存在的（见 all_exports.txt）：
    fuGetFaceInfo / fuGetFaceInfoRotated / fuFaceProcessorGetResultHeadMask /
    fuFaceProcessorGetResultHairMask / fuFaceProcessorGetResultFaceOcclusion /
    fuRenderItemsMasked / fuSetFaceDelayLeaveEnable / fuSetFaceDelayLeaveFrameNum /
    fuFaceProcessorSetMinFaceRatio / fuFaceProcessorSetDetectSmallFace

但「存在」不等于「能用」：签名未知、是否需要 GL 上下文、数据格式未知，
所以这里逐个试调用，并且**每一步都丢进独立子进程**跑 —— 猜错签名可能直接把
进程打崩，隔离之后崩了也只是这一步 FAIL，后面的步骤照跑。

用法
----
    python probe_sdk_caps.py                 # 跑全部（推荐）
    python probe_sdk_caps.py --step faceinfo # 只跑某一步（调试用）
    python probe_sdk_caps.py --risky         # 额外尝试 fuRenderItemsMasked

依赖：PIL（生成一张有人脸的输入帧）+ 已安装/已编译的 DaVinciBeauty.ofx。
"""
import ctypes
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
DLL = os.environ.get(
    "NAMA_OFX_DLL",
    r"C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle\Contents\Win64\DaVinciBeauty.ofx")
ENGINE = os.environ.get("NAMA_ENGINE_DIR", os.path.dirname(DLL))
PHOTO = os.environ.get("NAMA_PROBE_PHOTO", r"C:\Users\Administrator\Pictures\A7300538.jpg")
W, H = 640, 394
RAW = os.path.join(tempfile.gettempdir(), "nama_caps_in_%dx%d.raw" % (W, H))

STEPS = ["misc", "faceinfo", "confidence", "headmask", "hairmask", "occlusion", "setters"]


# ---------------------------------------------------------------------------
# 输入素材：把一张有脸的相机照片按比例裁成 640x394 的裸 RGBA 帧
# ---------------------------------------------------------------------------
def make_raw():
    if os.path.isfile(RAW) and os.path.getsize(RAW) == W * H * 4:
        return RAW
    from PIL import Image
    im = Image.open(PHOTO).convert("RGB")
    sw, sh = im.size
    # cover 裁剪：保持比例、填满画幅，人脸占比尽量大（人脸太小会检不出）
    scale = max(W / float(sw), H / float(sh))
    nw, nh = int(round(sw * scale)), int(round(sh * scale))
    im = im.resize((nw, nh), Image.LANCZOS)
    left, top = (nw - W) // 2, (nh - H) // 2
    im = im.crop((left, top, left + W, top + H)).convert("RGBA")
    with open(RAW, "wb") as f:
        f.write(im.tobytes())
    return RAW


# ---------------------------------------------------------------------------
# 子进程侧：跑一次 NamaSelfTest（让 SDK 完成 init + render），再探测
# ---------------------------------------------------------------------------
def load_and_render():
    """返回 (lib_plugin, item_id, faces)。SDK 在 NamaSelfTest 里已经初始化过。"""
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.LoadLibraryExW.argtypes = [ctypes.c_wchar_p, ctypes.c_void_p, ctypes.c_uint32]
    k32.LoadLibraryExW.restype = ctypes.c_void_p
    h = k32.LoadLibraryExW(DLL, None, 0x1100)
    if not h:
        raise RuntimeError("插件 DLL 加载失败 err=%d" % ctypes.get_last_error())
    lib = ctypes.WinDLL(DLL, handle=h)
    lib.NamaSelfTest.restype = ctypes.c_int
    lib.NamaSelfTest.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p,
                                 ctypes.c_int, ctypes.c_int, ctypes.c_double,
                                 ctypes.c_double, ctypes.c_char_p, ctypes.c_int]
    out_raw = os.path.join(tempfile.gettempdir(), "nama_caps_out.raw")
    msg = ctypes.create_string_buffer(1024)
    rc = lib.NamaSelfTest(ENGINE.encode("utf-8"), RAW.encode("utf-8"),
                          out_raw.encode("utf-8"), W, H, 70.0, 15.0, msg, 1024)
    text = msg.value.decode("utf-8", "replace")
    print("NamaSelfTest rc=%d  %s" % (rc, text))
    if rc != 0:
        raise RuntimeError("NamaSelfTest 失败")
    item = 1
    faces = 0
    for tok in text.split():
        if tok.startswith("item="):
            item = int(tok.split("=", 1)[1])
        elif tok.startswith("faces="):
            faces = int(tok.split("=", 1)[1])
    return lib, item, faces


def load_sdk():
    """CNamaSDK.dll 此时已被插件加载，按名字拿到同一个模块。"""
    return ctypes.WinDLL("CNamaSDK.dll")


def _sig(fn, argtypes, restype=ctypes.c_int):
    fn.argtypes = argtypes
    fn.restype = restype
    return fn


def buf_f(n):
    return (ctypes.c_float * n)()


def show(buf, n, label=""):
    vals = [round(float(buf[i]), 4) for i in range(n)]
    print("      %s%s" % (label, vals))


# ---------------------------------------------------------------------------
# 各探测步骤（在子进程里执行）
# ---------------------------------------------------------------------------
def step_misc(lib, item, faces):
    sdk = load_sdk()
    print("[misc] 导出可用性与基础状态")
    num = _sig(sdk.fuFaceProcessorGetNumResults, [])
    print("  fuFaceProcessorGetNumResults() = %d (NamaSelfTest 里 faces=%d)" % (num(), faces))
    for name in ("fuHasFace",):
        if hasattr(sdk, name):
            f = _sig(getattr(sdk, name), [])
            try:
                print("  %s() = %d" % (name, f()))
            except Exception as e:
                print("  %s() 抛异常: %s" % (name, e))
    n = ctypes.c_int(0)
    f = _sig(sdk.fuGetFaceProcessorFov, [ctypes.POINTER(ctypes.c_float)])
    fv = ctypes.c_float(0.0)
    try:
        print("  fuGetFaceProcessorFov() ret=%d fov=%s" % (f(ctypes.byref(fv)), fv.value))
    except Exception as e:
        print("  fuGetFaceProcessorFov 失败: %s" % e)
    return True


FACE_KEYS = ["face_count", "face_rect", "landmarks", "rotation", "rotation_euler",
             "expression", "pupil_pos", "face_identifier", "face_ord",
             "human_rect", "landmarks_ar", "landmarks_origin", "eye_rotation_xy"]


def step_faceinfo(lib, item, faces):
    sdk = load_sdk()
    print("[faceinfo] fuGetFaceInfo(key) —— 人脸框 / 关键点在不在？")
    # 常见签名 int fuGetFaceInfo(int item, const char* name, float* ret, int size)
    # size 按字节传；缓冲给得很大，避免 SDK 写越界。
    f = _sig(sdk.fuGetFaceInfo,
             [ctypes.c_int, ctypes.c_char_p, ctypes.POINTER(ctypes.c_float), ctypes.c_int])
    for key in FACE_KEYS:
        b = buf_f(256)
        # 先把 key 名刷到 stderr：SDK 内部日志也走 stderr，标记紧贴它自己的输出，
        # 这样「Not supported / certificate」到底属于哪个 key 一目了然（否则日志是糊在一起的）。
        sys.stderr.write("@@KEY %s\n" % key)
        sys.stderr.flush()
        try:
            ret = f(item, key.encode(), b, 256 * 4)
        except Exception as e:
            print("  %-18s 抛异常: %s" % (key, e), flush=True)
            continue
        nz = sum(1 for i in range(256) if abs(b[i]) > 1e-6)
        print("  %-18s ret=%-4d 非零元素=%d" % (key, ret, nz), flush=True)
        if nz:
            show(b, min(nz, 8), "前几个：")
    return True


def step_confidence(lib, item, faces):
    """fuFaceProcessorGetConfidenceScore(item, float*) —— 人脸置信度，可用于区域闸门。"""
    sdk = load_sdk()
    print("[confidence] fuFaceProcessorGetConfidenceScore(item, &score)")
    if not hasattr(sdk, "fuFaceProcessorGetConfidenceScore"):
        print("  不存在")
        return False
    f = _sig(sdk.fuFaceProcessorGetConfidenceScore,
             [ctypes.c_int, ctypes.POINTER(ctypes.c_float)])
    v = ctypes.c_float(0.0)
    try:
        ret = f(item, ctypes.byref(v))
        print("  ret=%d score=%.4f" % (ret, v.value))
    except Exception as e:
        print("  抛异常: %s" % e)
        return False
    # 再试按「人脸序号」取（多脸场景的常见形状）
    f2 = _sig(sdk.fuFaceProcessorGetConfidenceScore,
              [ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_float)])
    v2 = ctypes.c_float(0.0)
    try:
        ret2 = f2(item, 0, ctypes.byref(v2))
        print("  三参变体 ret=%d score=%.4f" % (ret2, v2.value))
    except Exception as e:
        print("  三参变体抛异常: %s" % e)
    return True


def _mask_probe(sdk, fname, label):
    if not hasattr(sdk, fname):
        print("  %s 不存在" % fname)
        return False
    print("  %s 存在，试调用 int %s(unsigned char*, int*, int*)" % (fname, fname))
    f = _sig(getattr(sdk, fname),
             [ctypes.POINTER(ctypes.c_ubyte), ctypes.POINTER(ctypes.c_int),
              ctypes.POINTER(ctypes.c_int)])
    mb = (ctypes.c_ubyte * (4 * 1024 * 1024))()
    mw, mh = ctypes.c_int(0), ctypes.c_int(0)
    try:
        ret = f(mb, ctypes.byref(mw), ctypes.byref(mh))
    except Exception as e:
        print("  调用抛异常: %s" % e)
        return False
    print("  ret=%d  w=%d h=%d" % (ret, mw.value, mh.value))
    if mw.value > 0 and mh.value > 0 and mw.value * mh.value <= len(mb):
        n = mw.value * mh.value
        s = bytes(mb[:n])
        nz = sum(1 for v in s if v > 8)
        print("  %s 非零占比=%.1f%% 最大值=%d" % (label, 100.0 * nz / n, max(s)))
    return True


def step_headmask(lib, item, faces):
    print("[headmask] fuFaceProcessorGetResultHeadMask —— 最理想的区域掩码")
    return _mask_probe(load_sdk(), "fuFaceProcessorGetResultHeadMask", "head mask")


def step_hairmask(lib, item, faces):
    print("[hairmask] fuFaceProcessorGetResultHairMask")
    return _mask_probe(load_sdk(), "fuFaceProcessorGetResultHairMask", "hair mask")


def step_occlusion(lib, item, faces):
    sdk = load_sdk()
    print("[occlusion] fuFaceProcessorGetResultFaceOcclusion(item, buf, size)")
    if not hasattr(sdk, "fuFaceProcessorGetResultFaceOcclusion"):
        print("  不存在")
        return False
    f = _sig(sdk.fuFaceProcessorGetResultFaceOcclusion,
             [ctypes.c_int, ctypes.POINTER(ctypes.c_float), ctypes.c_int])
    b = buf_f(32)
    try:
        ret = f(item, b, 32 * 4)
    except Exception as e:
        print("  调用抛异常: %s" % e)
        return False
    print("  ret=%d" % ret)
    show(b, 8, "前 8 个：")
    return True


def step_setters(lib, item, faces):
    """这些是「稳定性」相关的开关：能不能调、返回值是什么。"""
    sdk = load_sdk()
    print("[setters] 稳定性相关开关（小脸检出 / 人脸丢失延迟 / 每次检测间隔）")
    calls = [
        ("fuFaceProcessorSetDetectSmallFace", ctypes.c_int, 1),
        ("fuFaceProcessorSetMinFaceRatio", ctypes.c_float, 0.05),
        ("fuSetFaceDelayLeaveEnable", ctypes.c_int, 1),
        ("fuSetFaceDelayLeaveFrameNum", ctypes.c_int, 10),
        ("fuSetFaceProcessorDetectEveryNFramesWhenNoFace", ctypes.c_int, 5),
        ("fuSetFaceProcessorDetectEveryNFramesWhenFace", ctypes.c_int, 1),
    ]
    for name, t, v in calls:
        if not hasattr(sdk, name):
            print("  %-46s 不存在" % name)
            continue
        f = _sig(getattr(sdk, name), [t])
        try:
            ret = f(t(v))
            print("  %-46s ret=%d" % ("%s(%s)" % (name, v), ret))
        except Exception as e:
            print("  %-46s 抛异常: %s" % (name, e))
    return True


def step_risky(lib, item, faces):
    """fuRenderItemsMasked —— 签名完全未知，盲调可能崩，只在 --risky 时跑。"""
    sdk = load_sdk()
    print("[risky] fuRenderItemsMasked 盲试（签名未知，崩了就是这一步 FAIL）")
    if not hasattr(sdk, "fuRenderItemsMasked"):
        print("  不存在")
        return False
    # 按 fuRenderItemsEx 的形状猜：多出来的很可能是 mask（uchar*）+ 尺寸
    f = _sig(sdk.fuRenderItemsMasked,
             [ctypes.c_int, ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p,
              ctypes.c_int, ctypes.c_int, ctypes.c_int,
              ctypes.POINTER(ctypes.c_int), ctypes.c_int,
              ctypes.c_void_p, ctypes.c_int, ctypes.c_int])
    out = ctypes.create_string_buffer(W * H * 4)
    inp = ctypes.create_string_buffer(open(RAW, "rb").read())
    mask = ctypes.create_string_buffer(b"\xff" * (W * H))
    items = (ctypes.c_int * 1)(item)
    try:
        ret = f(0, out, 0, inp, W, H, 0, items, 1, mask, W, H)
        print("  ret=%d" % ret)
    except Exception as e:
        print("  抛异常: %s" % e)
    return True


STEPS_FN = {
    "misc": step_misc,
    "faceinfo": step_faceinfo,
    "confidence": step_confidence,
    "headmask": step_headmask,
    "hairmask": step_hairmask,
    "occlusion": step_occlusion,
    "setters": step_setters,
    "risky": step_risky,
}


def run_step(name):
    make_raw()
    lib, item, faces = load_and_render()
    fn = STEPS_FN[name]
    ok = fn(lib, item, faces)
    print("STEP %s -> %s" % (name, "OK" if ok else "FAIL"))
    return 0 if ok else 1


# ---------------------------------------------------------------------------
# 主进程侧：编排 + 隔离
# ---------------------------------------------------------------------------
def orchestrate(extra_risky):
    make_raw()
    print("输入帧: %s (%d 字节)" % (RAW, os.path.getsize(RAW)))
    print("插件:   %s" % DLL)
    print("引擎:   %s" % ENGINE)
    print("")
    steps = list(STEPS)
    if extra_risky:
        steps.append("risky")
    results = {}
    for s in steps:
        print("=" * 72)
        print(">> step %s" % s)
        cmd = [sys.executable, os.path.abspath(__file__), "--step", s]
        t0 = time.time()
        try:
            p = subprocess.run(cmd, capture_output=True, timeout=180)
            out = p.stdout.decode("utf-8", "replace")
            err = p.stderr.decode("utf-8", "replace")
            if out:
                sys.stdout.write(out if out.endswith("\n") else out + "\n")
            tail = [l for l in err.splitlines() if l.strip()][-3:]
            if tail:
                print("   [stderr] %s" % " | ".join(tail))
            crashed = p.returncode not in (0, 1)
            results[s] = "CRASH(rc=%d)" % p.returncode if crashed else (
                "OK" if p.returncode == 0 else "FAIL")
            if crashed:
                print("   *** 子进程异常退出 rc=%d，疑似签名不对 ***" % p.returncode)
        except subprocess.TimeoutExpired:
            results[s] = "TIMEOUT"
            print("   *** 超时 ***")
        print("   (%.1fs)" % (time.time() - t0))

    print("")
    print("=" * 72)
    print("汇总")
    for s in steps:
        print("  %-10s %s" % (s, results.get(s, "?")))
    return 0


def main():
    if "--step" in sys.argv:
        return run_step(sys.argv[sys.argv.index("--step") + 1])
    return orchestrate("--risky" in sys.argv)


if __name__ == "__main__":
    sys.exit(main())
