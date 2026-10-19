# -*- coding: utf-8 -*-
"""实时预览基准测试 2：换帧成本 + 编码提速空间。

要回答两个问题：
  1) 拖动时间轴换到新画面时，SDK 会不会重新做人脸检测？耗时多少？
     （稳态 4.5ms 快得可疑，怀疑 SDK 复用了上一帧的检测结果，
       若换帧也复用，效果就会错位——必须查清）
  2) 编码环节的 154ms 里，多少是 ffmpeg 进程启动、多少是真正的编码？
     做法：开一个常驻 ffmpeg 连续吃帧，量每帧的稳态编码耗时。
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
LIMIT = 1280
STAMPS = [5.0, 20.0, 60.0, 120.0, 300.0]


def rep(label, dt, extra=""):
    print("[B2] %-34s %8.1f ms  %s" % (label, dt * 1000, extra))


def grab(t, w, h):
    cmd = [W.FFMPEG, "-v", "error", "-nostdin", "-ss", "%.6f" % t, "-i", SRC,
           "-map", "0:v:0", "-frames:v", "1",
           "-vf", "scale=%d:%d" % (w, h), "-pix_fmt", "rgba", "-f", "rawvideo", "-"]
    raw = subprocess.run(cmd, capture_output=True).stdout
    need = w * h * 4
    assert len(raw) >= need, "抽帧失败 t=%.1f  %d/%d" % (t, len(raw), need)
    return bytearray(raw[:need])


def main():
    meta = W.probe_video(SRC)
    src_w, src_h = meta["w"], meta["h"]
    scale = LIMIT / float(src_w) if src_w > LIMIT else 1.0
    w = max(2, (int(src_w * scale) // 2) * 2)
    h = max(2, (int(src_h * scale) // 2) * 2)
    need = w * h * 4
    print("[B2] 预览尺寸 %dx%d" % (w, h))

    gl = W.GLContext()
    sdk = W.NamaSDK(W.ENGINE_DIR, log_level=3)
    sdk.setup(detect_mode=1, max_faces=4)
    sdk.set_params({"blur_level": 70, "color_level": 15, "red_level": 30}, blur_type=1)

    out_buf = bytearray(need)
    out_arr = (ctypes.c_char * need).from_buffer(out_buf)
    out_ptr = ctypes.cast(out_arr, ctypes.c_void_p)

    print("\n[B2] ==== 1. 换帧成本（每帧都是新画面）====")
    frames = []
    for t in STAMPS:
        t0 = time.perf_counter()
        buf = grab(t, w, h)
        t_dec = time.perf_counter() - t0
        frames.append((t, buf))
        rep("  抽帧 t=%.0fs" % t, t_dec)

    for t, buf in frames:
        in_arr = (ctypes.c_char * need).from_buffer(buf)
        in_ptr = ctypes.cast(in_arr, ctypes.c_void_p)
        t0 = time.perf_counter()
        sdk.render(in_ptr, out_ptr, w, h, 0)
        r0 = time.perf_counter() - t0
        t0 = time.perf_counter()
        sdk.render(in_ptr, out_ptr, w, h, 1)
        r1 = time.perf_counter() - t0
        # 同一帧再来一次（模拟只改参数）
        t0 = time.perf_counter()
        sdk.set_params({"blur_level": 55, "color_level": 25, "red_level": 30}, blur_type=1)
        sdk.render(in_ptr, out_ptr, w, h, 2)
        r2 = time.perf_counter() - t0
        rep("  换帧 t=%.0fs 首渲+次渲" % t, r0 + r1,
            "r0=%.1f r1=%.1f 稳态=%.1f 人脸=%d" % (r0 * 1000, r1 * 1000, r2 * 1000, sdk.faces()))

    # 复用第一帧，连续 8 次只改参数
    in_arr = (ctypes.c_char * need).from_buffer(frames[0][1])
    in_ptr = ctypes.cast(in_arr, ctypes.c_void_p)
    sdk.render(in_ptr, out_ptr, w, h, 100)
    print("\n[B2] ==== 2. 同帧连续改参（实时调参场景）====")
    ts = []
    for i in range(8):
        t0 = time.perf_counter()
        sdk.set_params({"blur_level": 30 + i * 5, "color_level": 15, "red_level": 30}, blur_type=1)
        sdk.render(in_ptr, out_ptr, w, h, 200 + i)
        ts.append(time.perf_counter() - t0)
    ts.sort()
    rep("  中位", ts[len(ts) // 2], "最快 %.1fms 最慢 %.1fms" % (ts[0] * 1000, ts[-1] * 1000))

    print("\n[B2] ==== 3. 编码：常驻 ffmpeg 连续吃帧 ====")
    img = os.path.join(ROOT, "logs", "bench_seq")
    p = subprocess.Popen(
        [W.FFMPEG, "-v", "error", "-nostdin", "-y",
         "-f", "rawvideo", "-pixel_format", "rgba", "-video_size", "%dx%d" % (w, h),
         "-framerate", "30", "-i", "pipe:0",
         "-f", "image2", "-q:v", "4", img + "_%02d.jpg"],
        stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    hs = None
    try:
        for i in range(6):
            t0 = time.perf_counter()
            p.stdin.write(bytes(out_buf))
            p.stdin.flush()
            if hs is None:
                rep("  首帧（含启动）", time.perf_counter() - t0)
                hs = True
            else:
                rep("  稳态第 %d 帧" % i, time.perf_counter() - t0)
    finally:
        p.stdin.close()
        p.wait(timeout=20)

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
