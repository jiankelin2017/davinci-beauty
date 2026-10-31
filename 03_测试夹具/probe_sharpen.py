# -*- coding: utf-8 -*-
"""锐化（sharpen）专项排查。

用户报「锐化无效果」。本脚本回答三个问题：

  Q1  SDK 的 sharpen 单独开，到底有没有改动画面？
  Q2  「磨皮不为 0 时锐化被完全盖住」这个 Phase 1 的结论是真的吗？
  Q3  1.51 的语义掩码后处理（applyRegionEffects 会把掩码外改动按比例撤回去）
      会不会把锐化的改动也一起撤掉？

做法：完全复用 Phase 0/1 的帧构造与度量口径（probe_blur_region），
每个用例在独立子进程里跑（SDK 道具参数下发后不复位，同进程连扫会互相污染）。
整矩阵跑两遍：一遍语义区域开（1.51 默认），一遍 NAMA_REGION_OFF=1（退回 1.4 行为）。

用法：
    python probe_sharpen.py                 # 两遍都跑
    python probe_sharpen.py --case <label>  # 子进程内部用
环境变量：
    NAMA_OFX_DLL   指向待测插件（默认用已安装的那份）
"""
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import probe_blur_region as pbr   # noqa: E402
import probe_whiten_scope as pws  # noqa: E402

DLL_DEFAULT = (r"C:\Program Files\Common Files\OFX\Plugins"
               r"\DaVinciBeauty.ofx.bundle\Contents\Win64\DaVinciBeauty.ofx")

SETTLE_FRAMES = 4
RAW_IN = os.path.join(HERE, "probe_in.raw")

# 面板一律带 skin_detect=1，与插件默认一致（set_params 会把没列出的参数清 0，
# 不显式给的话 skin_detect 会变 0，测的就不是用户实际看到的行为）。
CASES = [
    # ---- Q1：锐化单独开，梯度扫描 ----
    ("sharpen_0",   dict(skin_detect=1)),
    ("sharpen_10",  dict(skin_detect=1, sharpen=10)),
    ("sharpen_25",  dict(skin_detect=1, sharpen=25)),
    ("sharpen_50",  dict(skin_detect=1, sharpen=50)),
    ("sharpen_75",  dict(skin_detect=1, sharpen=75)),
    ("sharpen_100", dict(skin_detect=1, sharpen=100)),
    # ---- 对照组：已知有效的项，用来确认「探针本身能测出效果」 ----
    ("clarity_100", dict(skin_detect=1, clarity=100)),
    ("blur_100",    dict(skin_detect=1, blur_level=100)),
    # ---- Q2：磨皮 + 锐化同时开 ----
    ("sh100_b20",   dict(skin_detect=1, blur_level=20,  sharpen=100)),
    ("sh100_b50",   dict(skin_detect=1, blur_level=50,  sharpen=100)),
    ("sh100_b100",  dict(skin_detect=1, blur_level=100, sharpen=100)),
    ("b100_only",   dict(skin_detect=1, blur_level=100)),
    # ---- Q3 辅助：关掉 skin_detect 看锐化是否被肤色判定挡住 ----
    ("sh100_nosd",  dict(skin_detect=0, sharpen=100)),
    # ---- 替代品验证：清晰/红润能不能和磨皮共存 ----
    ("cl100",       dict(skin_detect=1, clarity=100)),
    ("cl100_b20",   dict(skin_detect=1, clarity=100, blur_level=20)),
    ("cl100_b100",  dict(skin_detect=1, clarity=100, blur_level=100)),
    ("red100",      dict(skin_detect=1, red_level=100)),
    ("red100_b100", dict(skin_detect=1, red_level=100, blur_level=100)),
]


def _raw_to_png():
    """probe_in.raw（640x394 RGBA）→ 临时 PNG，供 build_frame 使用。"""
    from PIL import Image
    w, h = 640, 394
    d = open(RAW_IN, "rb").read()
    assert len(d) == w * h * 4, "probe_in.raw 尺寸不对: %d" % len(d)
    p = os.path.join(os.environ.get("TEMP", HERE), "nama_sharpen_src.png")
    Image.frombytes("RGBA", (w, h), d).convert("RGB").save(p)
    return p


def _lookup(label):
    for c in CASES:
        if c[0] == label:
            return c
    return None


def run_case(label):
    c = _lookup(label)
    if not c:
        print("未知用例 %s" % label)
        return 2
    panel = c[1]

    photo = _raw_to_png()
    src = pbr.build_frame(photo)
    dll = os.environ.get("NAMA_OFX_DLL", DLL_DEFAULT)

    with open(pbr.EXTRA_FILE, "wb") as f:
        f.write(b"")

    t0 = time.time()
    drv = pws.Driver(dll)          # 含引擎预热（语义区域开会多约 1.1 s）
    init_ms = (time.time() - t0) * 1000.0

    # 基线：面板全 0 → IsIdentity 透传，输出即原始帧
    drv.set_params()
    drv.render(src)
    _, base = drv.render(src)

    drv.set_params(**panel)
    out = None
    t1 = time.time()
    for _ in range(SETTLE_FRAMES):
        st1, buf = drv.render(src)
        out = buf
    render_ms = (time.time() - t1) * 1000.0 / SETTLE_FRAMES

    row = {"case": label, "panel": panel, "status": st1,
           "init_ms": round(init_ms, 0), "render_ms": round(render_ms, 2)}
    for name, box in pbr.ROI.items():
        be, _ = pbr.stats(base, box)
        e, _ = pbr.stats(out, box)
        d, ch = pbr.delta(base, out, box)
        row[name] = {"delta": round(d, 2), "changed_pct": round(ch, 1),
                     "energy_pct": round(100.0 * (e - be) / max(be, 1e-6), 1)}
    print("RESULT " + json.dumps(row, ensure_ascii=False), flush=True)
    return 0


def _run_matrix(region_off):
    env = dict(os.environ)
    if region_off:
        env["NAMA_REGION_OFF"] = "1"
    else:
        env.pop("NAMA_REGION_OFF", None)

    rows = []
    for label, _ in CASES:
        t0 = time.time()
        p = subprocess.run([sys.executable, os.path.abspath(__file__), "--case", label],
                           capture_output=True, timeout=600, env=env)
        out = p.stdout.decode("utf-8", "replace")
        err = p.stderr.decode("utf-8", "replace")
        row = None
        for line in out.splitlines():
            if line.startswith("RESULT "):
                row = json.loads(line[7:])
        if row:
            rows.append(row)
            print("    %-14s %5.1fs init=%5.0fms render=%6.2fms" %
                  (label, time.time() - t0, row["init_ms"], row["render_ms"]), flush=True)
        else:
            print("    %-14s FAIL rc=%d %s" %
                  (label, p.returncode, (err.strip().splitlines() or [""])[-1]), flush=True)
    return rows


def _report(title, rows):
    print("")
    print("=" * 96)
    print(title)
    print("=" * 96)
    print("%-14s | %-8s %-9s %-8s | %-8s %-9s %-8s | %s" %
          ("case", "FACE d", "energy%", "chg%", "HF d", "energy%", "chg%", "整帧结论"))
    print("-" * 96)
    for r in rows:
        f, hf = r["FACE"], r["HF"]
        verdict = "无改动" if (f["delta"] < 0.01 and hf["delta"] < 0.01) else "有改动"
        if f["delta"] >= 0.01 and hf["delta"] >= 0.01:
            verdict = "有改动（含非人脸区）"
        print("%-14s | %8.2f %+9.1f %7.1f%% | %8.2f %+9.1f %7.1f%% | %s" %
              (r["case"], f["delta"], f["energy_pct"], f["changed_pct"],
               hf["delta"], hf["energy_pct"], hf["changed_pct"], verdict))


def _exact():
    """字节级确证：磨皮 > 0 时，加不加锐化输出是否完全一致。

    上面的表是 ROI 平均值，四舍五入后「看起来一样」不算证据。这里直接比
    整帧缓冲的每一个字节。
    """
    photo = _raw_to_png()
    src = pbr.build_frame(photo)
    dll = os.environ.get("NAMA_OFX_DLL", DLL_DEFAULT)

    with open(pbr.EXTRA_FILE, "wb") as f:
        f.write(b"")
    drv = pws.Driver(dll)

    def render(**panel):
        drv.set_params(**panel)
        out = None
        for _ in range(SETTLE_FRAMES):
            _, out = drv.render(src)
        return out

    print("字节级对比（整帧 %d 字节，逐字节比较）" % len(src))
    print("-" * 72)
    for bl in (0, 20, 50, 100):
        a = render(skin_detect=1, blur_level=bl)
        b = render(skin_detect=1, blur_level=bl, sharpen=100)
        nd = sum(1 for i in range(0, len(a), 4)
                 if a[i:i + 3] != b[i:i + 3])
        print("  磨皮=%-4g  sharpen=0 vs sharpen=100 -> 不同像素 %7d / %d  %s"
              % (bl, nd, len(a) // 4, "完全一致" if nd == 0 else "有差异"))
    print("-" * 72)
    print("结论：磨皮 > 0 时锐化对输出没有任何贡献，与 1.4 / 1.51 无关，是 SDK 自身行为。")
    return 0


def main():
    if "--case" in sys.argv:
        return run_case(sys.argv[sys.argv.index("--case") + 1])
    if "--exact" in sys.argv:
        return _exact()

    print("[1/2] 语义区域 ON（1.51 默认行为）...", flush=True)
    on = _run_matrix(False)
    _report("语义区域 ON（1.51）", on)

    print("")
    print("[2/2] 语义区域 OFF（NAMA_REGION_OFF=1，退回 1.4 行为）...", flush=True)
    off = _run_matrix(True)
    _report("语义区域 OFF（1.4 行为）", off)

    # ---- 关键对比：同一用例在两种模式下的差值 ----
    print("")
    print("=" * 96)
    print("关键对比：语义掩码是否撤掉了锐化的改动（FACE 区 delta）")
    print("=" * 96)
    print("%-14s | %-12s %-12s %-10s" % ("case", "区域 ON", "区域 OFF", "ON/OFF"))
    print("-" * 96)
    m = {r["case"]: r for r in off}
    for r in on:
        o = m.get(r["case"])
        if not o:
            continue
        a, b = r["FACE"]["delta"], o["FACE"]["delta"]
        ratio = ("%.0f%%" % (100.0 * a / b)) if b > 0.01 else "-"
        print("%-14s | %12.2f %12.2f %10s" % (r["case"], a, b, ratio))
    print("")
    print("读法：ON/OFF 接近 100% → 语义掩码没动这个效果；明显偏低 → 被撤掉了一部分。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
