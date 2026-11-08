# -*- coding: utf-8 -*-
"""Phase 0c：SDK 自带美白到底能不能限制到人脸/肤色 —— 用同一套人工帧量。

背景
----
插件现在的美白是自己在 CPU 上做的（`applyWhiten` + YCbCr 肤色阈值），因为
早先测出 SDK 的 `color_level` 是**整帧提亮**（人脸 11.25 / 背景 11.17 无差别）。
但 DLL 字符串池里还有一组没公开的美白参数：

    use_new_whiten / color_level_v2 / use_new_red / remove_pouch_strength_v2 ...

Phase 0b 已经证明这套「诊断参数注入口」是能用的（并排掉了 7 个无效开关）。
所以这里把美白那组也量一遍：**只要有一组能让非肤色区域逐字节不变，美白就能
甩掉我们自己的启发式掩码**。

测试帧与 probe_blur_region 完全一致（左半真人脸，右半 HF 黑白 / BLUE 蓝 /
PALE 苍白肤色 / SKINTONE 标准肤色，四块都没有人脸）。

注意两个坑（都是实测踩出来的）
  * 引擎每次下发面板参数都会把 `color_level` 强制为 0（SDK 美白恒关），所以
    测试用例里**面板参数必须保持全 0**，否则 `changed` 为真会先写 0 再被
    附加参数覆盖 —— 一帧内同一个参数写两次，SDK 只认第一次。
  * 因此基线与用例的面板参数完全相同，只有附加参数文件不同。

用法
    python probe_whiten_region.py <照片>
    python probe_whiten_region.py <照片> --noface
"""
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import probe_blur_region as pbr  # noqa: E402  复用合成帧与区域统计

ROI = pbr.ROI
W, H = pbr.W, pbr.H

# (标签, 附加道具参数)；面板参数恒为全 0，只靠附加参数开美白
CASES = [
    ("wl",              "color_level=1"),
    ("wl_new",          "color_level=1;use_new_whiten=1"),
    ("wl_v2_1",         "color_level_v2=1"),
    ("wl_v2_2",         "color_level_v2=2"),
    ("wl_new_v2_1",     "use_new_whiten=1;color_level_v2=1"),
    ("wl_new_v2_2",     "use_new_whiten=1;color_level_v2=2"),
    ("wl_new_v2_05",    "use_new_whiten=1;color_level_v2=0.5"),
    ("wl_red_new",      "use_new_red=1;red_level=1"),
    ("wl_new_red_new",  "use_new_whiten=1;use_new_red=1;color_level=1;red_level=1"),
]


def run_case(label, photo):
    extra = dict(CASES)[label]
    with open(pbr.EXTRA_FILE, "wb") as f:
        f.write(b"")
    src = pbr.build_frame(photo)
    dll = os.environ.get(
        "BEAUTY_OFX_DLL",
        r"C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle\Contents\Win64\DaVinciBeauty.ofx")
    drv = pbr.pws.Driver(dll)
    # 面板固定：磨皮开（历史 sweep 的条件）+ 美白 0。
    # 引擎在面板变化时会把 SDK 的 color_level 强制写 0，而「一帧内同一参数写两次」
    # 时 SDK 只认第一次；所以基线与用例的面板必须完全一致，只让附加参数不同。
    drv.set_params(blur_level=100, skin_detect=1, color_level=0)
    drv.render(src)
    st0, base = drv.render(src)

    # 面板参数不变 → 引擎走 else 分支，附加参数每帧生效（不会一帧内写两次）
    with open(pbr.EXTRA_FILE, "wb") as f:
        f.write(extra.encode("ascii"))
    drv.render(src)
    st1, out = drv.render(src)

    row = {"case": label, "extra": extra, "status": [st0, st1]}
    for name, box in ROI.items():
        d, ch = pbr.delta(base, out, box)
        row[name] = {"delta": round(d, 2), "changed_pct": round(ch, 1)}
    print("RESULT " + json.dumps(row, ensure_ascii=False), flush=True)
    return 0


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    photo = sys.argv[1]
    if "--case" in sys.argv:
        return run_case(sys.argv[sys.argv.index("--case") + 1], photo)

    noface = "--noface" in sys.argv
    if noface:
        print(">>> 无脸模式：画面里没有人脸 <<<")
    rows = []
    for label, _ in CASES:
        t0 = time.time()
        cmd = [sys.executable, os.path.abspath(__file__), photo, "--case", label]
        if noface:
            cmd.append("--noface")
        p = subprocess.run(cmd, capture_output=True, timeout=300)
        row = None
        for line in p.stdout.decode("utf-8", "replace").splitlines():
            if line.startswith("RESULT "):
                row = json.loads(line[7:])
        if row:
            rows.append(row)
            print("  %-16s %5.1fs" % (label, time.time() - t0), flush=True)
        else:
            print("  %-16s FAIL rc=%d" % (label, p.returncode), flush=True)

    print("")
    print("%-16s %-52s | %s" % ("用例", "附加参数", "各区域 Δ / 改动像素%"))
    print("-" * 118)
    for r in rows:
        parts = []
        for name in ("FACE", "HF", "BLUE", "PALE", "SKINTONE"):
            d = r[name]
            parts.append("%s %5.1f %5.1f%%" % (name, d["delta"], d["changed_pct"]))
        print("%-16s %-52s | %s" % (r["case"], r["extra"], " ".join(parts)))
    print("")
    print("判读：HF/BLUE 是非肤色。它们逐字节不变（Δ≈0）才说明该组合把美白限到了肤色；")
    print("      FACE 里含人脸，但里面有头发/背景等非肤色像素，所以 Δ 不会很高是正常的。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
