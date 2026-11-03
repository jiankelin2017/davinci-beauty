# -*- coding: utf-8 -*-
"""Phase 1：磨皮/美白「区域」能否从颜色判据升级成模型级皮肤分割。

背景
----
Phase 0 的结论是：磨皮区域由 SDK 内部的**逐像素颜色掩码**决定（肤色≈1.0、
非肤色≈0.5），所以双边档下非肤色蓝块仍被改动 96.7%。美白更彻底，SDK 那几条
通道全是整帧，只能由插件自己用 Cr/Cb 启发式在 CPU 侧做。

但 `05_解密模型数据` 里能看到 `ai_face_processor_pc.bundle` **本来就带着**
`skin_small_tiny_self_1205_nama_sparse.tflite`（皮肤分割）与去痘印模型；
CNamaSDK.dll 里也有 `FUAITYPE_FACEPROCESSOR_SKINSEGMENT` / `_DELSPOT` /
`beautifyPreprocessBlurSkinSeg` / `image_choose_skin` 这些符号。

也就是说：SDK 有语义级皮肤分割，只是**没被加载**——
`fuLoadAIModelFromPackage(pkg, len, aitype)` 的 aitype 我们一直只传
`FUAITYPE_FACEPROCESSOR`（0x100）。

本脚本干两件事
--------------
1. **aitype 位扫描**：把 1<<0 … 1<<15 逐位或进 aitype，看哪一位会让磨皮的
   非肤色区域改动掉到 0%（= 皮肤分割被加载并接管）。数值在 CNamaSDK.dll 里
   反查不到（名字是连续字符串块，没有指针表），只能按行为扫。
2. **新参数生效矩阵**：`delspot_level` / `remove_nasolabial_folds_strength(_v2)` /
   `tooth_whiten` / `heavy_denoise` / `blur_scale_ratio` / `manual_scale` 等
   从参数名池里挖出来的、但 Phase 0 没测过的项，逐个看有没有非零改动。

测试帧沿用 probe_blur_region.build_frame（左半真人脸 + 右半 HF/BLUE/PALE/SKINTONE）。
基线一律是「面板全 0」——此时插件走 IsIdentity 透传，输出就是原始帧，所以
Δ 直接等于该用例的总改动量。

每个用例在**独立子进程**里跑：
  * aitype 是进程级一次性初始化（引擎里缓存），换值必须换进程；
  * SDK 道具参数下发后不复位，同进程连扫会互相污染；
  * 顺带隔离崩溃（错误 aitype 可能让 SDK 直接抛访问违例）。

用法
    python probe_phase1_region.py <照片>                    # 全跑
    python probe_phase1_region.py <照片> --cases ai,params   # 只跑某一组
    python probe_phase1_region.py <照片> --case <label>      # 子进程内部用
环境变量
    NAMA_OFX_DLL  指向待测插件（默认用已安装的那份）
"""
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

# 复用 Phase 0 的帧构造与度量，保证两次测量口径一致
import probe_blur_region as pbr  # noqa: E402
import probe_whiten_scope as pws  # noqa: E402

W, H = pbr.W, pbr.H
EXTRA_FILE = pbr.EXTRA_FILE

DLL_DEFAULT = (r"C:\Program Files\Common Files\OFX\Plugins"
               r"\DaVinciBeauty.ofx.bundle\Contents\Win64\DaVinciBeauty.ofx")

BLUR = 100.0   # 面板磨皮拉满，便于看区域差异
WEAK = 1.0     # 只想看美白时用极轻磨皮，避免磨皮本身盖住信号
SETTLE_FRAMES = 6   # 见 run_case 里的说明：AI 重载期间前几帧是空转的


def _cases():
    """(label, group, aitype_extra, panel, extra_params)"""
    cs = []

    # ---- 组 1：aitype 位扫描 ----
    # 0x100 = FUAITYPE_FACEPROCESSOR 已经恒定或进去，这里逐位试其余的。
    #
    # 判据不只是画面：额外位如果真加载了模型，初始化耗时应该从 ~300 ms 跳回 ~820 ms
    #（模型没加载时 SDK 会提前短路）。所以 init_ms 比像素差异更灵敏。
    cs.append(("ai_none", "ai", 0, dict(blur_level=BLUR, skin_detect=1), ""))
    for bit in range(0, 32):
        if bit == 8:      # 0x100 就是现有默认值
            continue
        cs.append(("ai_bit%02d" % bit, "ai", 1 << bit,
                   dict(blur_level=BLUR, skin_detect=1), ""))
    # 两个可疑位同时开（HEADSEGMENTATION + SKINSEGMENT 常成对使用）
    for a, b in [(0x200, 0x400), (0x400, 0x800), (0x2000, 0x4000),
                 (1 << 16, 1 << 17), (1 << 24, 1 << 25), (1 << 30, 1 << 31)]:
        cs.append(("ai_combo_%x_%x" % (a, b), "ai", a | b,
                   dict(blur_level=BLUR, skin_detect=1), ""))

    # ---- 组 2：参数名池里没测过的新参数 ----
    P = dict(blur_level=BLUR, skin_detect=1)
    cs += [
        ("p_delspot",    "param", 0, P, "use_delspot=1;delspot_level=1.0"),
        ("p_delspot05",  "param", 0, P, "use_delspot=1;delspot_level=0.5"),
        ("p_naso",       "param", 0, P, "remove_nasolabial_folds_strength=1.0"),
        ("p_naso_v2",    "param", 0, P, "remove_nasolabial_folds_strength_v2=1.0"),
        ("p_tooth",      "param", 0, P, "tooth_whiten=1.0"),
        ("p_denoise",    "param", 0, P, "heavy_denoise=1.0"),
        ("p_blurscale",  "param", 0, P, "blur_scale_ratio=0.5"),
        ("p_bluroffset", "param", 0, P, "blur_offset_scale=0.5"),
        ("p_manual",     "param", 0, P, "manual_scale=1.0"),
        ("p_beautymask0", "param", 0, P, "beauty_mask_v=0.0"),
        ("p_warp0",      "param", 0, P, "warp_mask_v=0.0"),
        ("p_eyev2",      "param", 0, P, "eye_bright_v2=1.0"),
    ]

    # ---- 组 3：美白是否会被皮肤分割限制（现在恒关 SDK 美白，这里只看有没有区域化） ----
    # `enable_skinseg` 是 bundle 自带 script.js 里的参数：脚本在 color_level>0 且
    # enable_skinseg>0.5 时把 FUAITYPE_FACEPROCESSOR_SKINSEGMENT 加进 aitype，
    # 让 SDK 重新加载皮肤分割模型。Phase 0 没测它，所以「美白恒整帧」的结论
    # 只在 skinseg 关闭时成立 —— 这里补上开启后的对照。
    Q = dict(blur_level=WEAK, skin_detect=1)
    cs += [
        ("w_off",        "whiten", 0, Q, ""),
        ("w_color",      "whiten", 0, Q, "color_level=1.0"),
        ("w_color_v2",   "whiten", 0, Q, "color_level_v2=1.0"),
        ("w_seg_on",     "whiten", 0, Q, "enable_skinseg=1.0"),
        ("w_seg_color",  "whiten", 0, Q, "enable_skinseg=1.0;color_level=1.0"),
        ("w_seg_v2",     "whiten", 0, Q, "enable_skinseg=1.0;color_level_v2=1.0"),
        ("w_seg_delspot", "whiten", 0, Q,
         "enable_skinseg=1.0;delspot_level=1.0;use_delspot=1.0"),
        # 磨皮 + 皮肤分割同时开：看磨皮区域会不会跟着变硬
        ("b_seg",        "whiten", 0, dict(blur_level=BLUR, skin_detect=1),
         "enable_skinseg=1.0;color_level=1.0"),
    ]
    return cs


CASES = _cases()


def _lookup(label):
    for c in CASES:
        if c[0] == label:
            return c
    return None


def run_case(label, photo):
    """子进程入口：先渲基线（面板全 0 → 透传），再渲用例，出 RESULT json。"""
    c = _lookup(label)
    if not c:
        print("未知用例 %s" % label)
        return 2
    _, group, aitype, panel, extra = c

    src = pbr.build_frame(photo)
    dll = os.environ.get("NAMA_OFX_DLL", DLL_DEFAULT)

    # aitype 必须在**加载插件之前**设好：引擎第一次初始化时缓存它。
    if aitype:
        val = str(aitype)
        os.environ["NAMA_AITYPE_EXTRA"] = val
        try:
            import ctypes as _ct
            _ct.windll.kernel32.SetEnvironmentVariableW("NAMA_AITYPE_EXTRA", val)
        except Exception:
            pass
    else:
        os.environ.pop("NAMA_AITYPE_EXTRA", None)

    t0 = time.time()
    drv = pws.Driver(dll)          # 这里含引擎预热（约 2.5 秒）
    init_ms = (time.time() - t0) * 1000.0

    # 基线：面板全 0 + 空附加参数 → 插件 IsIdentity 透传，输出即原始帧
    with open(EXTRA_FILE, "wb") as f:
        f.write(b"")
    drv.set_params(blur_level=0)
    drv.render(src)
    _, base = drv.render(src)

    # 用例：写附加参数 → 面板参数 → 渲若干帧取最后一帧。
    # 为什么要多于 2 帧：`enable_skinseg` / `delspot` 会让 bundle 里的 script.js
    # 改写 aitype 并触发 SDK **重新加载 AI 模型**，重新加载期间那一两帧是空转的。
    # Phase 0 的 2 帧口径在这里会把「正在重载」误判成「参数无效」。
    with open(EXTRA_FILE, "wb") as f:
        f.write(extra.encode("ascii"))
    drv.set_params(**panel)
    settle = []
    out = None
    t1 = time.time()
    for k in range(SETTLE_FRAMES):
        st1, buf = drv.render(src)
        d, ch = pbr.delta(base, buf, pbr.ROI["FACE"])
        settle.append([k + 1, round(d, 2), round(ch, 1)])
        out = buf
    render_ms = (time.time() - t1) * 1000.0

    row = {"case": label, "group": group, "aitype": aitype, "extra": extra,
           "panel": panel, "status": st1, "settle": settle,
           "init_ms": round(init_ms, 0),
           "render_ms": round(render_ms / SETTLE_FRAMES, 2)}
    for name, box in pbr.ROI.items():
        be, _ = pbr.stats(base, box)
        e, _ = pbr.stats(out, box)
        d, ch = pbr.delta(base, out, box)
        row[name] = {"delta": round(d, 2), "changed_pct": round(ch, 1),
                     "energy": round(e, 2), "base_energy": round(be, 2),
                     "energy_pct": round(100.0 * (e - be) / max(be, 1e-6), 1)}
    print("RESULT " + json.dumps(row, ensure_ascii=False), flush=True)
    return 0


def _fmt_row(r):
    parts = []
    for name in ("FACE", "HF", "BLUE", "PALE", "SKINTONE"):
        d = r[name]
        parts.append("%s %5.1f %+5.0f%% %5.1f%%" % (name, d["delta"], d["energy_pct"],
                                                   d["changed_pct"]))
    return " ".join(parts)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    photo = sys.argv[1]
    if "--case" in sys.argv:
        return run_case(sys.argv[sys.argv.index("--case") + 1], photo)

    want = None
    if "--cases" in sys.argv:
        want = set(sys.argv[sys.argv.index("--cases") + 1].split(","))

    rows, failed = [], []
    for label, group, _, _, _ in CASES:
        if want and group not in want:
            continue
        t0 = time.time()
        cmd = [sys.executable, os.path.abspath(__file__), photo, "--case", label]
        p = subprocess.run(cmd, capture_output=True, timeout=300)
        out = p.stdout.decode("utf-8", "replace")
        err = p.stderr.decode("utf-8", "replace")
        row = None
        for line in out.splitlines():
            if line.startswith("RESULT "):
                row = json.loads(line[7:])
        if row:
            warn = [l.split("] ", 1)[-1] for l in err.splitlines()
                    if ("No Access" in l or "not support" in l.lower()
                        or "unknown" in l.lower() or "警告" in l)]
            row["warn"] = sorted(set(warn))[:3]
            # script.js 在切换 aitype 时会打 "open/close FUAITYPE_..."，
            # 这是判断「皮肤分割到底有没有被加载」最直接的证据
            ai = [l.strip() for l in (out + "\n" + err).splitlines()
                  if "FUAITYPE" in l and ("open" in l or "close" in l)]
            row["ai_log"] = sorted(set(ai))[:3]
            rows.append(row)
            print("  %-16s %6.1fs  init=%5.0fms render=%6.2fms %s" %
                  (label, time.time() - t0, row["init_ms"], row["render_ms"],
                   row["ai_log"][0][-46:] if row["ai_log"] else ""), flush=True)
        else:
            failed.append(label)
            print("  %-16s FAIL rc=%d %s" % (label, p.returncode,
                                             err.strip().splitlines()[-1:] or ""), flush=True)

    print("")
    print("%-16s %-7s | %s" % ("case", "aitype", "region  d / energy% / changed%"))
    print("-" * 132)
    for r in rows:
        print("%-16s 0x%-5X | %s" % (r["case"], r["aitype"], _fmt_row(r)))
    if failed:
        print("")
        print("FAILED: %s" % ", ".join(failed))

    # 结论摘要：非肤色 BLUE 块的改动占比是判定「掩码是不是语义级」的唯一硬指标
    print("")
    print("BLUE (non-skin) changed% -- target is 0.0%:")
    for r in sorted(rows, key=lambda x: x["BLUE"]["changed_pct"]):
        print("  %-16s %5.1f%%   (d=%.1f)" % (r["case"], r["BLUE"]["changed_pct"],
                                              r["BLUE"]["delta"]))

    # aitype 扫描的关键判据：模型真被加载时初始化耗时会明显变长。
    # 只挑 aitype 组，按 init_ms 降序 —— 第一行就是「最像真加载了模型」的那个。
    ai_rows = [r for r in rows if r["group"] == "ai"]
    if ai_rows:
        print("")
        print("aitype sweep, sorted by init_ms (higher = a model was actually loaded):")
        for r in sorted(ai_rows, key=lambda x: -x["init_ms"]):
            print("  0x%-9X %-16s init=%6.0fms  BLUE=%5.1f%%  FACE d=%.2f"
                  % (r["aitype"], r["case"], r["init_ms"],
                     r["BLUE"]["changed_pct"], r["FACE"]["delta"]))

    # 落一份 UTF-8 报告，避免控制台编码把中文打花
    out_path = os.path.join(HERE, "_phase1_report.txt")
    with open(out_path, "w", encoding="utf-8") as f:
        f.write("%-16s %-28s %-7s | %s\n" % ("用例", "附加参数", "aitype", "区域"))
        f.write("-" * 150 + "\n")
        for r in rows:
            f.write("%-16s %-28s 0x%-5X | %s\n" %
                    (r["case"], r["extra"] or "-", r["aitype"], _fmt_row(r)))
            f.write("%-16s %-28s %-7s | settle(FACE Δ): %s\n" %
                    ("", "", "", " / ".join(str(s[1]) for s in r["settle"])))
            if r.get("ai_log"):
                f.write("%-16s %-28s %-7s | AI: %s\n" % ("", "", "", r["ai_log"]))
        if failed:
            f.write("\n失败用例：%s\n" % ", ".join(failed))
    print("")
    print("report -> %s" % out_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
