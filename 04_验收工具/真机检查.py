# -*- coding: utf-8 -*-
"""真机 5 点检查：解析 Resolve 的 ofx.log，给出可判定的结论。

用法
----
    python 真机检查.py                    # 用默认日志
    python 真机检查.py --log <路径>       # 指定日志
    python 真机检查.py --tail 2000        # 只看最后 N 行（默认全读）

判据（与 性能方案.md 第十九节对应）
----
  1. 引擎预热带 build/ver      —— createInstance 那几行
  2. GPU 预热在 createInstance —— 「createInstance 预热 GPU 成功」
  3. 首帧磨皮不再 170~235 ms   —— 帧分解里第一条与后续的中位数对比
  4. 稳态合计（导出性能）      —— 帧分解「合计」的中位/p90
  5. 检测复用是否生效          —— 掩码分解里「检测=<0.5ms」的占比
"""
import argparse
import os
import re
import statistics
import sys

DEFAULT_LOG = r"C:\ProgramData\DaVinciBeauty\logs\ofx.log"

RE_FRAME = re.compile(
    r"帧分解 (\d+)x(\d+) 掩码=([\d.]+) 磨皮=([\d.]+) 色调=([\d.]+) 回填=([\d.]+) "
    r"美形=[\d.]+ 合计=([\d.]+)")
RE_MASK = re.compile(
    r"掩码分解 检测=([\d.]+) memset=([\d.]+) 肤色=([\d.]+) 脸框\+脖=([\d.]+) "
    r"保护=([\d.]+) 合计=([\d.]+)")
RE_PREHEAT = re.compile(r"createInstance 预热 GPU (\S+) 耗时=([\d.]+)ms")
RE_ENGINE = re.compile(r"createInstance 预热引擎 (\S+) 耗时=([\d.]+)ms build=(\S+) ver=(\S+)")
RE_GPU = re.compile(r"GPU 磨皮\[\S+\] 失败")
RE_ERR = re.compile(r"失败|错误|FAIL|error", re.I)


def pct(vals, p):
    if not vals:
        return float("nan")
    s = sorted(vals)
    k = max(0, min(len(s) - 1, int(round((len(s) - 1) * p))))
    return s[k]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", default=DEFAULT_LOG)
    ap.add_argument("--tail", type=int, default=0, help="只分析最后 N 行")
    args = ap.parse_args()

    if not os.path.isfile(args.log):
        print("找不到日志：%s" % args.log)
        return 2
    raw = open(args.log, "r", encoding="utf-8", errors="replace").read().splitlines()
    if args.tail > 0:
        raw = raw[-args.tail:]

    frames, masks, preheat, engine = [], [], [], []
    for ln in raw:
        if m := RE_FRAME.search(ln):
            g = m.groups()
            frames.append((int(g[0]), int(g[1])) + tuple(float(x) for x in g[2:]))
        elif m := RE_MASK.search(ln):
            masks.append(tuple(float(x) for x in m.groups()))
        elif m := RE_PREHEAT.search(ln):
            preheat.append((m.group(1), float(m.group(2))))
        elif m := RE_ENGINE.search(ln):
            engine.append((m.group(3), m.group(4), float(m.group(2))))

    print("=" * 62)
    print("  DaVinci Beauty 真机检查 —— %s" % args.log)
    print("  行数 %d，帧分解 %d 条，掩码分解 %d 条" % (len(raw), len(frames), len(masks)))
    print("=" * 62)

    # 1. 引擎与版本
    if engine:
        build, ver, ms = engine[-1]
        print("[1] 引擎预热   build=%s ver=%s 耗时=%.0fms" % (build, ver, ms))
    else:
        print("[1] 引擎预热   **日志里没有 createInstance 预热引擎 行**（没打开过效果？）")

    # 2. GPU 预热位置
    if preheat:
        ok, ms = preheat[-1]
        print("[2] GPU 预热   %s 耗时=%.0fms  （预期在 createInstance，不在首帧）" % (ok, ms))
    else:
        print("[2] GPU 预热   **没有这行** —— 要么没触发，要么仍在首帧懒加载")

    # 3. 首帧 vs 稳态
    if frames:
        first = frames[0][4]
        rest = [f[4] for f in frames[1:]]
        steady = statistics.median(rest) if rest else float("nan")
        verdict = "OK" if (first < 40 or steady < 20) else "需复查"
        print("[3] 首帧磨皮   first=%.1fms  后续中位=%.1fms  → %s" %
              (first, steady, verdict))
        if first > 150:
            print("      首帧仍 >150ms：GPU 预热的日志存在但没挡住首帧，看第 2 点")

    # 4. 稳态合计
    if frames:
        tot = [f[6] for f in frames]
        print("[4] 帧合计     中位=%.2fms  p90=%.2fms  max=%.2fms  （目标 ~14ms）" %
              (statistics.median(tot), pct(tot, 0.9), max(tot)))
        print("      分项中位：掩码=%.2f 磨皮=%.2f 色调=%.2f 回填=%.2f" % (
            statistics.median([f[2] for f in frames]),
            statistics.median([f[3] for f in frames]),
            statistics.median([f[4] for f in frames]),
            statistics.median([f[5] for f in frames])))
        w, h = frames[-1][0], frames[-1][1]
        tone, fill = statistics.median([f[4] for f in frames]), statistics.median([f[5] for f in frames])
        if w < 1000:
            print("      **这一场只有 %dx%d（代理/预览），不能当导出性能用**" % (w, h))
        if tone == 0.0 and fill == 0.0:
            print("      **色调=0 且 回填=0：效果参数基本关着，这一场不算有效测试**")

    # 5. 检测复用
    if masks:
        det = [m[0] for m in masks]
        reused = sum(1 for d in det if d < 0.5)
        print("[5] 检测复用   复用帧 %d/%d (%.0f%%)  检测中位=%.2fms  （复用帧应 ≈0.03ms）"
              % (reused, len(det), 100.0 * reused / len(det), statistics.median(det)))
        if reused == 0:
            print("      一次都没复用：画面一直在动，或 BEAUTY_DET_REUSE=0")
    else:
        print("[5] 检测复用   **没有掩码分解 行** —— 需要先设环境变量 BEAUTY_MASK_PROFILE=1 再启动 Resolve")

    # 错误
    errs = [ln for ln in raw if RE_GPU.search(ln)]
    if errs:
        print("-" * 62)
        print("GPU 回落 CPU 的帧：%d 条，最后一条：" % len(errs))
        print("   " + errs[-1][:160])
    return 0


if __name__ == "__main__":
    sys.exit(main())
