#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Phase 0 补测：人脸几何数据到底能不能拿到（决定「区域是否能按人脸」）。

背景
----
probe_sdk_caps.py 里 `fuGetFaceInfo("face_rect")` 的结果很"哑"：ret=0、缓冲全 0、
SDK 一条日志都没打；而 `landmarks` 明确报 `certificate don't have access to this parameter!`。
「沉默的全 0」有两种可能，必须区分开：

    (1) 该键被证书拒绝但 SDK 忘了打日志  -> 永远拿不到
    (2) 该键其实是许可的，只是我们查询的时机/上下文不对 -> 可以拿到

已知的正确用法约束（FaceUnity 一贯要求）：
    * 必须在「本帧已经渲染过」之后查询，且要连着渲染；
    * 必须在**同一个进程 / 同一个 SDK 实例**里（跨进程拿不到跟踪状态）；
    * 人脸跟踪有延迟，前几帧本来就可能是空的。

所以这里做一件事：加载插件跑一次 NamaSelfTest（SDK 完成 init+render）后，
自己再用 `fuRenderItemsEx` 连渲若干帧，**每帧渲染完立刻**查一遍人脸键，
把「是不是时机问题」这个可能性彻底排除。每一步都丢进独立子进程。

用法
----
    python probe_face_avail.py              # 跑全部步骤
    python probe_face_avail.py --step a     # 只跑某一步

依赖：PIL、已安装/已编译的 DaVinciBeauty.ofx（复用 probe_sdk_caps 的素材与初始化）。
"""
import ctypes
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import probe_sdk_caps as CAPS  # noqa: E402  (make_raw / load_and_render / load_sdk / _sig / buf_f)

W, H = CAPS.W, CAPS.H
RAW = CAPS.RAW

STEPS = ["a", "b", "c", "d", "e"]

KEYS = ["face_count", "face_rect", "landmarks", "landmarks_ar", "landmarks_origin",
        "rotation", "rotation_euler", "expression", "pupil_pos", "eye_rotation_xy"]


def hdr(title):
    print("")
    print("-" * 60)
    print("[%s]" % title)


def query_keys(sdk, item, tag):
    """按 fuGetFaceInfo(int item, const char* name, float* ret, int size) 查一遍。"""
    f = CAPS._sig(sdk.fuGetFaceInfo,
                  [ctypes.c_int, ctypes.c_char_p, ctypes.POINTER(ctypes.c_float),
                   ctypes.c_int])
    n = CAPS._sig(sdk.fuFaceProcessorGetNumResults, [])
    try:
        faces = n()
    except Exception:
        faces = -1
    print("  (%s) fuFaceProcessorGetNumResults=%d" % (tag, faces), flush=True)
    for key in KEYS:
        b = CAPS.buf_f(256)
        # @@KEY 紧贴 SDK 自己的日志，便于把「哪条日志属于哪个键」分清
        sys.stderr.write("@@Q %s | %s\n" % (tag, key))
        sys.stderr.flush()
        try:
            ret = f(item, key.encode(), b, 256 * 4)
        except Exception as e:
            print("    %-16s 抛异常: %s" % (key, e), flush=True)
            continue
        nz = sum(1 for i in range(256) if abs(b[i]) > 1e-6)
        head = [round(float(b[i]), 3) for i in range(min(nz, 6))]
        print("    %-16s ret=%-4d 非零=%d %s" % (key, ret, nz, head), flush=True)


def render_frame(sdk, item, frame_id, in_bytes, out_buf):
    """按插件引擎同样的形状调 fuRenderItemsEx（fmt=0 即 RGBA buffer）。"""
    f = CAPS._sig(sdk.fuRenderItemsEx,
                  [ctypes.c_int, ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p,
                   ctypes.c_int, ctypes.c_int, ctypes.c_int,
                   ctypes.POINTER(ctypes.c_int), ctypes.c_int])
    items = (ctypes.c_int * 1)(item)
    src = ctypes.create_string_buffer(in_bytes, len(in_bytes))
    return f(0, ctypes.cast(out_buf, ctypes.c_void_p), 0,
             ctypes.cast(src, ctypes.c_void_p),
             W, H, frame_id, items, 1)


# ---------------------------------------------------------------------------
def step_a(lib, item, faces):
    """紧跟渲染查人脸键 —— 排除「时机不对」。"""
    sdk = CAPS.load_sdk()
    in_bytes = open(RAW, "rb").read()
    out = ctypes.create_string_buffer(W * H * 4)

    hdr("a1 紧跟 NamaSelfTest（内部已渲染过）")
    query_keys(sdk, item, "selftest 后")

    hdr("a2 自己连渲 4 帧，每帧后立刻查")
    for i in range(4):
        try:
            ret = render_frame(sdk, item, 100 + i, in_bytes, out)
        except Exception as e:
            print("  fuRenderItemsEx 抛异常: %s" % e)
            return False
        print("  fuRenderItemsEx(frame=%d) ret=%d" % (100 + i, ret), flush=True)
        query_keys(sdk, item, "自渲染 frame=%d" % (100 + i))
    return True


def step_b(lib, item, faces):
    """别的「人脸存在性/身份」类导出。"""
    sdk = CAPS.load_sdk()
    hdr("b1 存在性类")
    if hasattr(sdk, "fuIsTracking"):
        f = CAPS._sig(sdk.fuIsTracking, [])
        try:
            print("  fuIsTracking() = %d" % f())
        except Exception as e:
            print("  fuIsTracking 抛异常: %s" % e)
    if hasattr(sdk, "fuHasFace"):
        f = CAPS._sig(sdk.fuHasFace, [])
        try:
            print("  fuHasFace() = %d" % f())
        except Exception as e:
            print("  fuHasFace 抛异常: %s" % e)

    hdr("b2 fuGetFaceIdentifier —— 试一参 / 零参两种形状")
    if hasattr(sdk, "fuGetFaceIdentifier"):
        v = ctypes.c_int(0)
        f1 = CAPS._sig(sdk.fuGetFaceIdentifier, [ctypes.POINTER(ctypes.c_int)])
        try:
            print("  一参 ret=%d id=%d" % (f1(ctypes.byref(v)), v.value))
        except Exception as e:
            print("  一参抛异常: %s" % e)
        f2 = CAPS._sig(sdk.fuGetFaceIdentifier, [])
        try:
            print("  零参 ret=%d" % f2())
        except Exception as e:
            print("  零参抛异常: %s" % e)

    hdr("b3 fuFaceProcessorGetConfidenceScore(item, &score)")
    v = ctypes.c_float(-1.0)
    f = CAPS._sig(sdk.fuFaceProcessorGetConfidenceScore,
                  [ctypes.c_int, ctypes.POINTER(ctypes.c_float)])
    try:
        print("  ret=%d score=%.4f" % (f(item, ctypes.byref(v)), v.value))
    except Exception as e:
        print("  抛异常: %s" % e)
    return True


def step_c(lib, item, faces):
    """fuGetFaceInfoRotated —— 与 fuGetFaceInfo 并行的一套接口。"""
    sdk = CAPS.load_sdk()
    hdr("c1 fuGetFaceInfoRotated 渲染后查询")
    in_bytes = open(RAW, "rb").read()
    out = ctypes.create_string_buffer(W * H * 4)
    for i in range(2):
        render_frame(sdk, item, 200 + i, in_bytes, out)
    if not hasattr(sdk, "fuGetFaceInfoRotated"):
        print("  不存在")
        return False
    f = CAPS._sig(sdk.fuGetFaceInfoRotated,
                  [ctypes.c_int, ctypes.c_char_p, ctypes.POINTER(ctypes.c_float),
                   ctypes.c_int])
    for key in KEYS:
        b = CAPS.buf_f(256)
        sys.stderr.write("@@Q rotated | %s\n" % key)
        sys.stderr.flush()
        try:
            ret = f(item, key.encode(), b, 256 * 4)
        except Exception as e:
            print("  %-16s 抛异常: %s" % (key, e))
            continue
        nz = sum(1 for i in range(256) if abs(b[i]) > 1e-6)
        head = [round(float(b[i]), 3) for i in range(min(nz, 6))]
        print("  %-16s ret=%-4d 非零=%d %s" % (key, ret, nz, head), flush=True)
    return True


def step_d(lib, item, faces):
    """老接口路线：fuTrackFace(裸帧) -> fuGetFaceInfo。签名靠猜，独立进程跑。"""
    sdk = CAPS.load_sdk()
    hdr("d1 fuSetMaxFaces / fuTrackFace 后再查")
    if hasattr(sdk, "fuSetMaxFaces"):
        f = CAPS._sig(sdk.fuSetMaxFaces, [ctypes.c_int])
        try:
            print("  fuSetMaxFaces(4) ret=%d" % f(4))
        except Exception as e:
            print("  fuSetMaxFaces 抛异常: %s" % e)
    if not hasattr(sdk, "fuTrackFace"):
        print("  fuTrackFace 不存在")
        return False
    in_bytes = open(RAW, "rb").read()
    src = ctypes.create_string_buffer(in_bytes, len(in_bytes))
    f = CAPS._sig(sdk.fuTrackFace,
                  [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int])
    # 试两种 format 值：0（本 SDK 的 RGBA buffer=0）与 1
    for fmt in (0, 1):
        for i in range(3):
            try:
                ret = f(ctypes.cast(src, ctypes.c_void_p), W, H, fmt)
            except Exception as e:
                print("  fmt=%d fuTrackFace 抛异常: %s" % (fmt, e))
                break
            print("  fmt=%d frame=%d fuTrackFace ret=%d" % (fmt, i, ret), flush=True)
        query_keys(sdk, item, "fuTrackFace fmt=%d 后" % fmt)
    return True


def step_e(lib, item, faces):
    """result 句柄类接口（fuGetFaceProcessorResult / occlusion）。"""
    sdk = CAPS.load_sdk()
    hdr("e1 fuGetFaceProcessorResult(item, buf, size) 试探")
    if not hasattr(sdk, "fuGetFaceProcessorResult"):
        print("  不存在")
    else:
        f = CAPS._sig(sdk.fuGetFaceProcessorResult,
                      [ctypes.c_int, ctypes.c_void_p, ctypes.c_int])
        b = ctypes.create_string_buffer(4096)
        try:
            print("  ret=%d" % f(item, ctypes.cast(b, ctypes.c_void_p), 4096))
            print("  head:", [b[i] for i in range(8)])
        except Exception as e:
            print("  抛异常: %s" % e)

    hdr("e2 fuFaceProcessorGetResultFaceOcclusion(item, float*, size)")
    f = CAPS._sig(sdk.fuFaceProcessorGetResultFaceOcclusion,
                  [ctypes.c_int, ctypes.POINTER(ctypes.c_float), ctypes.c_int])
    b = CAPS.buf_f(64)
    try:
        ret = f(item, b, 256)
        nz = sum(1 for i in range(64) if abs(b[i]) > 1e-6)
        print("  ret=%d 非零=%d %s" % (ret, nz, [round(float(b[i]), 3) for i in range(min(nz, 6))]))
    except Exception as e:
        print("  抛异常: %s" % e)
    return True


STEPS_FN = {"a": step_a, "b": step_b, "c": step_c, "d": step_d, "e": step_e}
LOG_DIR = os.path.join(HERE, "_probe_face_logs")


def run_step(name):
    CAPS.make_raw()
    lib, item, faces = CAPS.load_and_render()
    print("NamaSelfTest 报告 faces=%d item=%d" % (faces, item))
    ok = STEPS_FN[name](lib, item, faces)
    print("STEP %s -> %s" % (name, "OK" if ok else "FAIL"))
    return 0 if ok else 1


def orchestrate():
    CAPS.make_raw()
    os.makedirs(LOG_DIR, exist_ok=True)
    print("输入帧 : %s (%d 字节)" % (RAW, os.path.getsize(RAW)))
    print("插件   : %s" % CAPS.DLL)
    print("引擎   : %s" % CAPS.ENGINE)
    results = {}
    for s in STEPS:
        print("")
        print("=" * 72)
        print(">> step %s" % s)
        cmd = [sys.executable, os.path.abspath(__file__), "--step", s]
        t0 = time.time()
        try:
            p = subprocess.run(cmd, capture_output=True, timeout=240)
            out = p.stdout.decode("utf-8", "replace")
            err = p.stderr.decode("utf-8", "replace")
            if out:
                sys.stdout.write(out if out.endswith("\n") else out + "\n")
            # SDK 自己的日志（含 certificate 拒绝）落在 stderr，落盘以便对照 @@Q 标记
            log = os.path.join(LOG_DIR, "step_%s.stderr.txt" % s)
            with open(log, "w", encoding="utf-8") as fh:
                fh.write(err)
            cert = sum(1 for l in err.splitlines() if "certificate" in l.lower()
                       or "permission" in l.lower() or "not supported" in l.lower())
            print("   [stderr] 行数=%d 其中「证书/权限/不支持」相关=%d -> %s"
                  % (len(err.splitlines()), cert, log))
            crashed = p.returncode not in (0, 1)
            results[s] = ("CRASH(rc=%d)" % p.returncode) if crashed else (
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
    for s in STEPS:
        print("  step %-4s %s" % (s, results.get(s, "?")))
    print("")
    print("SDK 原始日志目录: %s" % LOG_DIR)
    return 0


def main():
    if "--step" in sys.argv:
        return run_step(sys.argv[sys.argv.index("--step") + 1])
    return orchestrate()


if __name__ == "__main__":
    sys.exit(main())
