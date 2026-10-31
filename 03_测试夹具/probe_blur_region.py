# -*- coding: utf-8 -*-
"""Phase 0b：磨皮区域到底由哪些 SDK 参数决定 —— 用真实照片量出来。

背景
----
Phase 0a（probe_sdk_caps.py）证明 SDK 的人脸信息 API 拿不到东西：
`fuGetFaceInfo` 的 face_rect/landmarks 被证书挡住（"this certificate don't have
access to this parameter"），`fuFaceProcessorGetResultHeadMask` 直接
"this certificate don't have access to this api"。也就是说**区域数据不给我们**。

但 CNamaSDK.dll 的字符串池里（skin_detect 前后）还躺着一批区域相关的道具参数，
公开文档里从来没有出现过：

    skin_detect / nonskin_blur_scale / blur_use_mask / blur_blend_ratio /
    blur_radius / debug_mask / change_frames / use_landmark / heavy_blur ...

本脚本量的是这些参数**到底干什么**：喂一帧人工合成画面，里面放四块特征完全不同
的区域，看磨皮改动落在哪里。

测试帧（640x394）
    FACE      (0..319,  0..393) 真人照片（cover 裁剪填满左半边）
    HF        (330..630, 10..119) 8x8 黑白棋盘 + 噪点 —— **非肤色**的高频结构
    BLUE      (330..630,130..189) 蓝色块 + 噪点        —— **非肤色**
    SKINTONE  (330..630,200..384) 肤色噪点块           —— **像皮肤，但没有人脸**

每块区域算两个量
    delta   与基线（全 0 参数）的平均绝对差 —— 「这块被改了多少」
    energy  平均梯度，相对基线的变化百分比 —— 「细节掉了多少」
判据：如果某块是非肤色而 delta≈0、细节不变，说明它被肤色掩码排除了；
肤色块哪怕没有人脸也被改 → 说明掩码**只看颜色，不看人脸**。

每个用例都在**独立子进程**里跑：SDK 道具参数一旦下发就不会自动复位，
同进程连续扫参数会互相污染（第一版就踩了这个坑）；顺带还能隔离崩溃。

用法
    python probe_blur_region.py <照片>                    # 全跑，最后出汇总表
    python probe_blur_region.py <照片> --noface           # 画面里不放人脸（验证掩码是否依赖人脸检测）
    python probe_blur_region.py <照片> --cases skin1,skin0 # 只跑指定的几个用例
环境变量
    NAMA_OFX_DLL / NAMA_ENGINE_DIR 与其它夹具一致
"""
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import probe_whiten_scope as pws  # noqa: E402  复用它的 OFX 宿主 Driver

W, H = pws.W, pws.H
EXTRA_FILE = os.path.join(os.environ.get("TEMP", "."), "nama_blur_region_params.txt")
# 必须**在加载插件之前**设好：引擎在第一次下发参数时缓存这个路径（内容每帧重读）。
# 两条路都设：os.environ 走 CRT，SetEnvironmentVariableW 走 Win32 进程环境块；
# 插件是独立 DLL，只认后者的情况很常见（否则文件根本读不到）。
#
# `--no-file` 时不设它 —— 这时引擎改走 NAMA_EXTRA_PARAMS（进程级、只解析一次），
# 而环境变量那一路在**创建道具时**就会下发一次（见 nama_engine.cpp:783）。
# 用来区分「道具参数只在创建时读」和「运行时也认」两种参数。
NO_FILE = "--no-file" in sys.argv
if not NO_FILE:
    os.environ["NAMA_EXTRA_PARAMS_FILE"] = EXTRA_FILE
    try:
        import ctypes as _ct
        _ct.windll.kernel32.SetEnvironmentVariableW("NAMA_EXTRA_PARAMS_FILE", EXTRA_FILE)
    except Exception:
        pass

ROI = {
    "FACE": (0, 0, 319, H - 1),
    "HF": (330, 10, 630, 79),
    "BLUE": (330, 85, 630, 154),
    "PALE": (330, 160, 630, 229),
    "SKINTONE": (330, 235, 630, H - 10),
}

def _lv():
    """面板磨皮强度，默认 100（便于看区域差异）。--level 20 用来看新默认档的表现。"""
    if "--level" in sys.argv:
        return float(sys.argv[sys.argv.index("--level") + 1])
    return 100.0


# (标签, 面板参数, 附加道具参数)
CASES = [
    # ---- 面板自带的三个开关 ----
    ("skin1",            dict(blur_level=_lv(), skin_detect=1), ""),
    ("skin0",            dict(blur_level=_lv(), skin_detect=0), ""),
    ("skin1_gauss",      dict(blur_level=_lv(), skin_detect=1, blur_type=1), ""),
    ("skin0_gauss",      dict(blur_level=_lv(), skin_detect=0, blur_type=1), ""),
    ("skin1_weak",       dict(blur_level=_lv(), skin_detect=1, blur_type=2), ""),
    ("skin0_weak",       dict(blur_level=_lv(), skin_detect=0, blur_type=2), ""),
    # ---- 从 DLL 字符串池挖出来的隐藏参数 ----
    ("blend010",         dict(blur_level=100, skin_detect=1), "blur_blend_ratio=0.1"),
    ("blend025",         dict(blur_level=100, skin_detect=1), "blur_blend_ratio=0.25"),
    ("blend050",         dict(blur_level=100, skin_detect=1), "blur_blend_ratio=0.5"),
    ("blend100",         dict(blur_level=100, skin_detect=1), "blur_blend_ratio=1.0"),
    ("skin0_blend050",   dict(blur_level=100, skin_detect=0), "blur_blend_ratio=0.5"),
    ("gauss_blend050",   dict(blur_level=100, skin_detect=1, blur_type=1), "blur_blend_ratio=0.5"),
    ("nonskin0",         dict(blur_level=100, skin_detect=1), "nonskin_blur_scale=0"),
    ("nonskin1",         dict(blur_level=100, skin_detect=1), "nonskin_blur_scale=1"),
    ("usemask1",         dict(blur_level=100, skin_detect=1), "blur_use_mask=1"),
    ("debugmask",        dict(blur_level=100, skin_detect=1), "debug_mask=1"),
    ("radius5",          dict(blur_level=100, skin_detect=1), "blur_radius=5"),
    ("chgframes",        dict(blur_level=100, skin_detect=1), "change_frames=1"),
    ("uselandmark",      dict(blur_level=100, skin_detect=1), "use_landmark=1"),
    ("heavyblur",        dict(blur_level=100, skin_detect=1), "heavy_blur=1"),
    ("lowerquality",     dict(blur_level=100, skin_detect=1), "lower_quality=1"),
    ("bogus5",           dict(blur_level=100, skin_detect=1), "b1=1;b2=1;b3=1;b4=1;b5=1"),
]


def build_frame(photo):
    """左半边真人照片 + 右半边人工区域。

    加 --noface 时左半边换成绿色噪点块（画面里**没有任何人脸**）：
    用来验证「人没检出来的时候，肤色掩码还生效吗」。
    """
    from PIL import Image
    import random

    noface = "--noface" in sys.argv
    half = W // 2
    canvas = Image.new("RGB", (W, H), (0, 0, 0))
    if not noface:
        im = Image.open(photo).convert("RGB")
        sw, sh = im.size
        s = max(half / float(sw), H / float(sh))
        nw, nh = max(1, int(round(sw * s))), max(1, int(round(sh * s)))
        im = im.resize((nw, nh), Image.LANCZOS)
        im = im.crop(((nw - half) // 2, (nh - H) // 2,
                      (nw - half) // 2 + half, (nh - H) // 2 + H))
        canvas.paste(im, (0, 0))
    px = canvas.load()
    rnd = random.Random(20261021)
    for y in range(H):
        for x in range(half, W):
            if 10 <= y <= 79 and 330 <= x <= 630:
                v = 235 if ((x // 8) + (y // 8)) % 2 == 0 else 20
                c = (v + rnd.randint(-25, 25),) * 3
            elif 85 <= y <= 154 and 330 <= x <= 630:
                c = (60 + rnd.randint(-25, 25),
                     90 + rnd.randint(-25, 25),
                     200 + rnd.randint(-25, 25))
            elif 160 <= y <= 229 and 330 <= x <= 630:
                # 苍白/低饱和的「像皮肤但更灰」的色（黄种人偏白肤色常见）
                c = (210 + rnd.randint(-20, 20),
                     190 + rnd.randint(-20, 20),
                     180 + rnd.randint(-20, 20))
            elif 235 <= y <= H - 10 and 330 <= x <= 630:
                c = (205 + rnd.randint(-30, 30),
                     155 + rnd.randint(-25, 25),
                     125 + rnd.randint(-20, 20))
            else:
                c = (0, 0, 0)
            px[x, y] = tuple(max(0, min(255, v)) for v in c)
    if noface:
        # 左半边：绿色噪点（非肤色、无五官、无人脸）
        for y in range(H):
            for x in range(half):
                px[x, y] = (60 + rnd.randint(-25, 25),
                            160 + rnd.randint(-25, 25),
                            60 + rnd.randint(-25, 25))
    return canvas.convert("RGBA").tobytes()


def luma(buf, i):
    return (buf[i] * 299 + buf[i + 1] * 587 + buf[i + 2] * 114) // 1000


def stats(buf, box):
    x0, y0, x1, y1 = box
    gsum = 0
    lsum = 0
    n = 0
    for y in range(y0, y1):
        row = y * W
        for x in range(x0, x1):
            i = (row + x) * 4
            l = luma(buf, i)
            gsum += abs(l - luma(buf, (row + x + 1) * 4))
            gsum += abs(l - luma(buf, ((y + 1) * W + x) * 4))
            lsum += l
            n += 1
    return gsum / float(n), lsum / float(n)


def delta(a, b, box):
    x0, y0, x1, y1 = box
    dsum = 0
    n = 0
    changed = 0
    for y in range(y0, y1):
        row = y * W
        for x in range(x0, x1):
            i = (row + x) * 4
            d = max(abs(a[i] - b[i]), abs(a[i + 1] - b[i + 1]), abs(a[i + 2] - b[i + 2]))
            dsum += d
            if d >= 3:
                changed += 1
            n += 1
    return dsum / float(n), 100.0 * changed / n


def run_case(label, photo):
    """在子进程里跑：先渲染基线（全 0 参数），再渲染该用例。"""
    params, extra = None, None
    for lb, p, e in CASES:
        if lb == label:
            params, extra = p, e
    if params is None:
        print("未知用例 %s" % label)
        return 2
    if not NO_FILE:
        with open(EXTRA_FILE, "wb") as f:
            f.write(extra.encode("ascii"))
    src = build_frame(photo)
    dll = os.environ.get(
        "NAMA_OFX_DLL",
        r"C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle\Contents\Win64\DaVinciBeauty.ofx")
    drv = pws.Driver(dll)

    # 基线：面板全 0 + 空附加参数（肤色/磨皮都不生效）
    if not NO_FILE:
        with open(EXTRA_FILE, "wb") as f:
            f.write(b"")
    drv.set_params(blur_level=0)
    drv.render(src)                 # 第 1 帧：参数刚下发，SDK 可能还没吃到
    st0, base = drv.render(src)     # 第 2 帧：稳定态，作为基线

    drv.set_params(**params)
    drv.render(src)                 # 第 1 帧：同上，丢掉
    st1, out = drv.render(src)      # 第 2 帧：稳定态，作为测量值

    row = {"case": label, "extra": extra, "panel": params, "status": [st0, st1]}
    for name, box in ROI.items():
        be, bl = stats(base, box)
        e, l = stats(out, box)
        d, ch = delta(base, out, box)
        row[name] = {"delta": round(d, 2), "changed_pct": round(ch, 1),
                     "energy": round(e, 2), "base_energy": round(be, 2),
                     "energy_pct": round(100.0 * (e - be) / max(be, 1e-6), 1)}
    print("RESULT " + json.dumps(row, ensure_ascii=False), flush=True)
    return 0


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    photo = sys.argv[1]
    if "--case" in sys.argv:
        return run_case(sys.argv[sys.argv.index("--case") + 1], photo)

    rows = []
    noface = "--noface" in sys.argv
    want = None
    if "--cases" in sys.argv:
        want = set(sys.argv[sys.argv.index("--cases") + 1].split(","))
    if noface:
        print(">>> 无脸模式：左半边是绿色噪点块，画面里没有人脸 <<<")
    for label, _, _ in CASES:
        if want and label not in want:
            continue
        t0 = time.time()
        cmd = [sys.executable, os.path.abspath(__file__), photo, "--case", label]
        if noface:
            cmd.append("--noface")
        if "--level" in sys.argv:
            cmd += ["--level", sys.argv[sys.argv.index("--level") + 1]]
        if NO_FILE:
            cmd.append("--no-file")
        p = subprocess.run(cmd, capture_output=True, timeout=300)
        out = p.stdout.decode("utf-8", "replace")
        err = p.stderr.decode("utf-8", "replace")
        row = None
        for line in out.splitlines():
            if line.startswith("RESULT "):
                row = json.loads(line[7:])
        # 只关心 SDK 对参数的反应：拒绝 / 无权限 / 未知
        warn = [l.split("] ", 1)[-1] for l in err.splitlines()
                if ("No Access" in l or "not support" in l.lower()
                    or "unknown" in l.lower())]
        if row:
            row["warn"] = sorted(set(warn))[:4]
            rows.append(row)
            print("  %-18s %6.1fs" % (label, time.time() - t0), flush=True)
        else:
            print("  %-18s FAIL rc=%d %s" % (label, p.returncode, warn[:2]), flush=True)

    print("")
    print("%-18s %-24s | %s" % ("用例", "附加参数", "各区域 Δ / 细节变化% / 改动像素%"))
    print("-" * 118)
    for r in rows:
        parts = []
        for name in ("FACE", "HF", "BLUE", "PALE", "SKINTONE"):
            d = r[name]
            parts.append("%s %5.1f %+5.0f%% %5.1f%%" % (name, d["delta"], d["energy_pct"],
                                                        d["changed_pct"]))
        print("%-18s %-24s | %s" % (r["case"], r["extra"] or "-", " ".join(parts)))
    print("")
    print("各用例 SDK 警告：")
    for r in rows:
        if r.get("warn"):
            print("  %-18s %s" % (r["case"], " | ".join(r["warn"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
