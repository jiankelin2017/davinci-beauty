# -*- coding: utf-8 -*-
"""解析 ofx.log 里 1.0.10.1+ 的「GPU 磨皮」行，验算分项与合计是否闭合。

用途
----
GPU 磨皮的耗时日志把它拆成「上传(Map/拷) / 算 / 读回(等GPU/拷) / 补齐 / 合计」。
这些分项是否**逐帧相加等于合计**，决定了「有没有未计量的隐藏开销」这个问题的答案。
本脚本把它算出来，并给出热状态下的分布（min/med/max/mean）与抖动倍数。

为什么需要它
------------
2026-11-07 曾凭「各项区间最小值相加」去比「合计均值」，得出「有 ~1.3ms 未计量段（29%）」
的结论——那是错的：把不同帧的最小值拼成一项再当同一帧用，等于在比较两个不同样本。
逐帧验算后缺口只有 0.01ms（纯舍入）。这种错法很容易重犯，所以把验算固化成脚本。

用法
----
    python logcheck_gpu_timing.py                    # 默认读装机日志
    python logcheck_gpu_timing.py <日志路径>

判读
----
* 「最大偏差」< 0.05ms → 分项闭合，没有隐藏开销，别再去找「未计量段」。
* 「热状态」只统计累计帧号 ≥ 60 的行：首帧与第 2 帧含着色器编译、纹理首次落实物理页等
  一次性开销，拿它们横向比较会得出相反结论（见 docs/history/history-progress-1x.md 的 1.0.10.1 节 (3)）。
* 「合计抖动 max/min」才是体感的关键：拖滑块时决定卡不卡的是尖峰，不是均值。
"""
import re
import sys
from statistics import mean, median

DEFAULT_LOG = r"C:\ProgramData\DaVinciBeauty\logs\ofx.log"

# 样例：
# 07:43:58.777 [T19992] [engine/I] GPU 磨皮[auto] roi=1600x1080@320,0 平面=800x540
#   r=2/4/2 P=3 上传=1.06(Map 0.02/拷 1.04) 算=0.16 读回=1.75(等GPU 1.31/拷 0.44)
#   补齐=0.74 合计=3.71 ms | 累计 60 帧 均合计=4.40 均平面像素=348964
PAT = re.compile(
    r"GPU 磨皮\[(?P<mode>[^\]]+)\] roi=(?P<rw>\d+)x(?P<rh>\d+)@(?P<rx>-?\d+),(?P<ry>-?\d+)"
    r"\s+平面=(?P<pw>\d+)x(?P<ph>\d+).*?"
    r"上传=(?P<up>[\d.]+)\(Map (?P<map>[\d.]+)/拷 (?P<upc>[\d.]+)\)"
    r"\s+算=(?P<comp>[\d.]+)"
    r"\s+读回=(?P<down>[\d.]+)\(等GPU (?P<wait>[\d.]+)/拷 (?P<dc>[\d.]+)\)"
    r"\s+补齐=(?P<fill>[\d.]+)"
    r"\s+合计=(?P<total>[\d.]+) ms"
    r".*?累计 (?P<frames>\d+) 帧 均合计=(?P<avg>[\d.]+)"
)
TS = re.compile(r"^(\d\d:\d\d:\d\d\.\d\d\d)")

FLOAT_KEYS = ("up", "map", "upc", "comp", "down", "wait", "dc", "fill", "total", "avg")
INT_KEYS = ("rw", "rh", "pw", "ph", "frames")


def parse(path):
    rows = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = PAT.search(line)
            if not m:
                continue
            d = {k: m.group(k) for k in m.groupdict()}
            for k in FLOAT_KEYS:
                d[k] = float(d[k])
            for k in INT_KEYS:
                d[k] = int(d[k])
            t = TS.match(line)
            d["ts"] = t.group(1) if t else "?"
            rows.append(d)
    return rows


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_LOG
    rows = parse(path)
    if not rows:
        print(f"没有解析到日志行：{path}")
        print("需要 1.0.10.1 及以后版本打出的「上传=..(Map ../拷 ..)」格式。")
        return 0

    print(f"日志：{path}")
    print(f"共解析 {len(rows)} 行\n")
    print(f"{'时间':<13}{'roi':<13}{'上传':>6}{'Map':>6}{'拷':>6}{'算':>6}"
          f"{'读回':>6}{'等GPU':>7}{'补齐':>6}{'合计':>7}{'四项和':>8}{'缺口':>7}")
    print("-" * 108)

    worst = 0.0
    for r in rows:
        s = r["up"] + r["comp"] + r["down"] + r["fill"]
        gap = r["total"] - s
        worst = max(worst, abs(gap))
        print(f"{r['ts']:<13}{r['rw']}x{r['rh']:<7}"
              f"{r['up']:>6.2f}{r['map']:>6.2f}{r['upc']:>6.2f}{r['comp']:>6.2f}"
              f"{r['down']:>6.2f}{r['wait']:>7.2f}{r['fill']:>6.2f}"
              f"{r['total']:>7.2f}{s:>8.2f}{gap:>+7.2f}")

    print(f"\n四项之和 vs 合计：最大偏差 {worst:+.3f} ms"
          f"  → {'闭合，没有隐藏开销' if worst < 0.05 else '存在未计量段，值得继续拆'}")

    hot = [r for r in rows if r["frames"] >= 60]
    if not hot:
        print("\n没有累计帧号 ≥ 60 的热状态样本，无法比较分布。")
        print("提示：让播放头连续播放几秒，等日志出现「累计 60 帧」那一行再跑。")
        return 0

    print(f"\n热状态样本 {len(hot)} 帧（累计帧号 ≥ 60）：")
    labels = (("上传", "up"), ("  其中 Map", "map"), ("  其中 拷", "upc"),
              ("算", "comp"), ("读回", "down"), ("  其中 等GPU", "wait"),
              ("  其中 拷", "dc"), ("补齐", "fill"), ("合计", "total"))
    for name, key in labels:
        vals = [r[key] for r in hot]
        print(f"  {name:<12} min={min(vals):6.2f}  med={median(vals):6.2f}  "
              f"max={max(vals):6.2f}  mean={mean(vals):6.2f}")

    tot = [r["total"] for r in hot]
    m = mean(tot)
    print(f"\n  合计抖动：max/min = {max(tot) / min(tot):.2f} 倍，极差 {max(tot) - min(tot):.2f} ms")
    print(f"  占合计（mean）：等GPU {mean([r['wait'] for r in hot]) / m * 100:.0f}% / "
          f"上传 {mean([r['up'] for r in hot]) / m * 100:.0f}% / "
          f"补齐 {mean([r['fill'] for r in hot]) / m * 100:.0f}% / "
          f"算 {mean([r['comp'] for r in hot]) / m * 100:.0f}%")

    # 由「均合计」反推各段区间均值，用于观察是否随播放时长漂移（热节流/GPU 排队）
    if len(hot) >= 2:
        print("\n  区间均值（由均合计反推，看是否随播放时长漂移）：")
        prev_f, prev_a = 0, 0.0
        for r in hot:
            span = r["frames"] - prev_f
            if span > 0:
                seg = (r["avg"] * r["frames"] - prev_a * prev_f) / span
                print(f"    1~{r['frames']:>4} 帧均值 {r['avg']:.2f}    "
                      f"其中 {prev_f + 1}~{r['frames']} 段均值 {seg:.2f}")
            prev_f, prev_a = r["frames"], r["avg"]
    return 0


if __name__ == "__main__":
    sys.exit(main())
