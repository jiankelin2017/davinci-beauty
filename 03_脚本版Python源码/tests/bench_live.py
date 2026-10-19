# -*- coding: utf-8 -*-
"""实时预览可行性基准测试。

目的：把「单帧预览要 3 秒」拆开，看每一段各占多少——
      A 冷启动（GL 上下文 + LoadLibrary + fuSetup + 加载模型/道具）
      B 抽帧（ffmpeg 解码 + 缩放）
      C 首次渲染（含人脸检测预热）
      D 稳态重渲染（GL/SDK 常驻 + 帧数据已缓存，只改参数重跑）
      E 编码成图（JPEG）
只有 D 够快，实时预览才有戏。
"""
import ctypes
import os
import subprocess
import sys
import time

ROOT = r"C:\ProgramData\NamaBeauty"
sys.path.insert(0, ROOT)
import nama_worker as W  # noqa: E402

SRC = sys.argv[1] if len(sys.argv) > 1 else r"D:\c\2026-10-13 20-32-17.mp4"
T = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0
LIMIT = int(sys.argv[3]) if len(sys.argv) > 3 else 1280   # 预览限宽
N = int(sys.argv[4]) if len(sys.argv) > 4 else 12         # 稳态重复次数


def tic():
    return time.perf_counter()


def rep(label, dt, extra=""):
    print("[BENCH] %-28s %8.1f ms  %s" % (label, dt * 1000, extra))


def main():
    meta = W.probe_video(SRC)
    src_w, src_h = meta["w"], meta["h"]
    scale = LIMIT / float(src_w) if src_w > LIMIT else 1.0
    w = max(2, (int(src_w * scale) // 2) * 2)
    h = max(2, (int(src_h * scale) // 2) * 2)
    print("[BENCH] 源 %dx%d -> 预览 %dx%d，时间点 %.2fs，稳态重复 %d 次"
          % (src_w, src_h, w, h, T, N))

    # ---- A1 GL 上下文 ----
    t = tic()
    gl = W.GLContext()
    t_gl = tic() - t
    rep("A1 GL 上下文", t_gl)

    # ---- A2 LoadLibrary ----
    t = tic()
    sdk = W.NamaSDK(W.ENGINE_DIR, log_level=3)
    t_lib = tic() - t
    rep("A2 LoadLibrary + 绑定", t_lib)

    # ---- A3 fuSetup + 模型 + 道具 ----
    t = tic()
    version = sdk.setup(detect_mode=1, max_faces=4)
    t_setup = tic() - t
    rep("A3 fuSetup+模型+道具", t_setup, "SDK " + str(version))

    # ---- B 抽帧 ----
    t = tic()
    cmd = [W.FFMPEG, "-v", "error", "-nostdin", "-ss", "%.6f" % T, "-i", SRC,
           "-map", "0:v:0", "-frames:v", "1",
           "-vf", "scale=%d:%d" % (w, h), "-pix_fmt", "rgba", "-f", "rawvideo", "-"]
    raw = subprocess.run(cmd, capture_output=True).stdout
    need = w * h * 4
    assert len(raw) >= need, "抽帧失败 %d/%d" % (len(raw), need)
    src_buf = bytearray(raw[:need])
    t_dec = tic() - t
    rep("B 抽一帧(解码+缩放)", t_dec)

    in_arr = (ctypes.c_char * need).from_buffer(src_buf)
    out_buf = bytearray(need)
    out_arr = (ctypes.c_char * need).from_buffer(out_buf)
    in_ptr = ctypes.cast(in_arr, ctypes.c_void_p)
    out_ptr = ctypes.cast(out_arr, ctypes.c_void_p)

    base = {"blur_level": 70, "blur_type": 1, "clarity": 0, "color_level": 15,
            "red_level": 30, "sharpen": 0, "face_lift": 0, "eye_bright": 0,
            "eye_circle": 0}
    sdk.set_params(base, blur_type=1)

    # ---- C 首次渲染（含 AI 预热）----
    t = tic()
    sdk.render(in_ptr, out_ptr, w, h, 0)
    t_r0 = tic() - t
    rep("C1 render #0 (预热)", t_r0)
    t = tic()
    sdk.render(in_ptr, out_ptr, w, h, 1)
    t_r1 = tic() - t
    rep("C2 render #1 (首效果)", t_r1, "人脸 %d" % sdk.faces())

    # ---- D 稳态：只改参数重渲染（这就是「实时」的耗时）----
    times = []
    for i in range(N):
        p = dict(base)
        p["blur_level"] = 40 + (i * 3) % 60
        p["color_level"] = 10 + (i * 2) % 40
        t = tic()
        sdk.set_params(p, blur_type=1)
        sdk.render(in_ptr, out_ptr, w, h, 2 + i)
        times.append(tic() - t)
    times.sort()
    rep("D 稳态重渲染 中位", times[len(times) // 2], "n=%d" % N)
    rep("D 稳态重渲染 最快", times[0])
    rep("D 稳态重渲染 最慢", times[-1])
    fps = 1.0 / times[len(times) // 2] if times[len(times) // 2] > 0 else 0
    print("[BENCH]   -> 等效 %.1f fps" % fps)

    # ---- E 编码 JPEG ----
    t = tic()
    p = subprocess.run([W.FFMPEG, "-v", "error", "-nostdin", "-y",
                        "-f", "rawvideo", "-pixel_format", "rgba",
                        "-video_size", "%dx%d" % (w, h), "-framerate", "1",
                        "-i", "pipe:0", "-frames:v", "1",
                        "-q:v", "4", os.path.join(ROOT, "logs", "bench_one.jpg")],
                       input=bytes(out_buf), capture_output=True)
    t_enc = tic() - t
    rep("E 编码一张 JPEG", t_enc, "rc=%d" % p.returncode)

    # ---- 汇总 ----
    cold = t_gl + t_lib + t_setup + t_dec + t_r0 + t_r1 + t_enc
    warm = times[len(times) // 2] + t_enc
    print("[BENCH] ---- 汇总 ----")
    rep("冷启动单帧总耗时", cold)
    rep("常驻后单帧总耗时", warm)
    print("[BENCH] 结论：实时上限约 %.1f fps" % (1.0 / warm if warm else 0))

    try:
        sdk.dispose()
    except Exception:
        pass
    try:
        gl.close()
    except Exception:
        pass


if __name__ == "__main__":
    main()
