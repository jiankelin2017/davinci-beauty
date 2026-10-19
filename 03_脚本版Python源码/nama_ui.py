#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""NamaBeauty 参数面板 —— 独立子进程版。

为什么单独一个进程：
    Resolve 内置的脚本环境并行跑 GUI 有两个已知问题：一是 Fusion 的 UIManager
    在通用 Scripts 目录下的 Python 环境里并不总能拿到，二是窗口若建在 Resolve
    自己的消息泵上容易把界面卡住。这里让面板跑在它自己的进程里，
    消息循环独立，崩溃也不会带崩 Resolve。

调用方式：
    fuscript.exe -l python3 nama_ui.py --spec <spec.json>

输出约定（stdout 最后一行，前面会混入少量 SDK/环境杂讯，故用前缀标记）：
    #NBUI     {json}   用户点了「开始处理」
    #NBCANCEL          用户点了「取消」或直接关窗
    #NBERROR  {json}   面板本身起不来（例如没有 tkinter）
"""

import json
import os
import sys

# (参数键, 中文标签, 默认刻度, blur_type 标定说明, 实测量化结论)
EFFECTS = [
    ("blur_level", "磨皮", 70, "平均差异 2.579，改动像素 26.36%"),
    ("clarity", "清晰", 0, "平均差异 6.536，改动像素 89.29%"),
    ("color_level", "美白", 15, "平均差异 21.586，改动像素 99.37%"),
    ("red_level", "红润", 30, "平均差异 3.625，改动像素 57.78%"),
    ("sharpen", "锐化", 0, "平均差异 1.898，改动像素 8.17%"),
    ("face_threed", "五官立体", 0, "平均差异 1.474，改动像素 15.61%"),
    ("eye_bright", "亮眼", 0, "平均差异 0.086，改动像素 0.65%"),
    ("remove_pouch_strength", "去黑眼圈", 0, "平均差异 0.075，改动像素 0.93%"),
]

BLUR_TYPES = [
    ("双边滤波（最强·推荐）", 1),
    ("高斯（中等）", 0),
    ("弱磨皮", 2),
]

PRESETS = [
    ("H.264 / 显卡加速（NVENC）", "h264_nvenc"),
    ("H.264 / CPU 通用", "libx264"),
    ("Apple ProRes 422 HQ", "prores422"),
]

PRESETS_QUICK = [
    ("自然", {"blur_level": 60, "clarity": 0, "color_level": 12, "red_level": 25,
              "sharpen": 0, "face_threed": 0, "eye_bright": 0, "remove_pouch_strength": 0}),
    ("明显", {"blur_level": 80, "clarity": 0, "color_level": 28, "red_level": 45,
              "sharpen": 0, "face_threed": 25, "eye_bright": 100, "remove_pouch_strength": 100}),
    ("全部归零", {k: 0 for k, _l, _d, _n in EFFECTS}),
]

WARN_SHARPEN = "注意：实测只要磨皮不为 0，锐化会被完全盖掉。要用锐化，请把磨皮调到 0。"
WARN_WEAK = "提示：亮眼、去黑眼圈实测很弱（改动像素不足 1%），要放大画面才看得出。"
WARN_RANGE = "勾选下面这一项时，只处理片段在时间线上真正用到的那一段，输出会短很多。"


def emit(line):
    try:
        sys.stdout.write(line + "\n")
        sys.stdout.flush()
    except Exception:
        pass


def load_spec(path):
    """读调用方塞进来的 spec：可选。读不到就用内置默认值。"""
    if not path or not os.path.isfile(path):
        return {}
    try:
        with open(path, "r", encoding="utf-8-sig") as f:
            return json.load(f)
    except Exception as e:
        emit("#NBWARN 读 spec 失败：%s" % e)
        return {}


def index_of(choices, value, fallback=0):
    """按「值」在 (标签, 值) 列表里定位索引。"""
    try:
        return [v for _l, v in choices].index(value)
    except Exception:
        return fallback


def index_of_label(choices, label, fallback=0):
    """按「显示文本」定位索引。

    下拉框回读的是标签文本而不是值，所以要单独按标签查。
    之前 collect() 直接拿标签去 index_of() 里按值比对，必然查不到，
    于是静默退回第 0 项——表现就是磨皮算法、编码格式总被改回默认值。
    """
    try:
        return [l for l, _v in choices].index(label)
    except Exception:
        return fallback


def build_ui(tk, ttk, spec, sink):
    """搭窗口。sink 收集结果：{\"ok\": bool, \"settings\": {...}}。"""
    cfg = spec.get("config") or {}
    clips = spec.get("clips") or []
    params = dict(cfg.get("params") or {})
    for key, _l, dflt, _n in EFFECTS:
        params.setdefault(key, dflt)

    root = tk.Tk()
    root.title("NamaBeauty 美颜参数")
    root.geometry("470x660")
    try:
        root.attributes("-topmost", True)
    except Exception:
        pass
    try:
        root.tk.call("tk", "scaling", 1.0)
    except Exception:
        pass

    base_font = None
    try:
        from tkinter import font as tkfont
        for fam in ("Microsoft YaHei UI", "微软雅黑", "Segoe UI"):
            if fam in tkfont.families():
                base_font = (fam, 9)
                break
    except Exception:
        base_font = None

    container = ttk.Frame(root)
    container.pack(fill="both", expand=True, padx=6, pady=6)

    canvas = tk.Canvas(container, highlightthickness=0)
    scrollbar = ttk.Scrollbar(container, orient="vertical", command=canvas.yview)
    inner = ttk.Frame(canvas)
    inner.bind("<Configure>", lambda e: canvas.configure(scrollregion=canvas.bbox("all")))
    canvas.create_window((0, 0), window=inner, anchor="nw")
    canvas.configure(yscrollcommand=scrollbar.set)
    canvas.pack(side="left", fill="both", expand=True)
    scrollbar.pack(side="right", fill="y")

    def _wheel(ev):
        canvas.yview_scroll(int(-1 * (ev.delta / 120)), "units")
    canvas.bind("<MouseWheel>", _wheel)
    inner.bind("<MouseWheel>", _wheel)

    row = [0]
    def next_row():
        row[0] += 1
        return row[0]

    if base_font:
        try:
            ttk.Style().configure(".", font=base_font)
        except Exception:
            pass

    def add_label(text, extra=None):
        options = {"text": text, "anchor": "w"}
        if extra:
            options.update(extra)
        lbl = ttk.Label(inner, **options)
        lbl.grid(row=next_row(), column=0, columnspan=3, sticky="ew", pady=(6, 2))
        return lbl

    inner.columnconfigure(0, weight=1)
    inner.columnconfigure(1, weight=1)
    inner.columnconfigure(2, weight=1)

    # ---- 处理目标 ----
    clip_names = []
    for i, c in enumerate(clips):
        clip_names.append("<%d> %s" % (i + 1, c.get("name", "")))
    if clip_names:
        clip_names.insert(0, "全部 %d 条视频片段（逐条处理）" % len(clips))
    else:
        clip_names = ["（没有可选片段）"]

    r = next_row()
    ttk.Label(inner, text="处理目标", anchor="w").grid(row=r, column=0, sticky="w", pady=(2, 2))
    clip_var = tk.StringVar(value=clip_names[0])
    clip_box = ttk.Combobox(inner, textvariable=clip_var, values=clip_names, state="readonly")
    clip_box.grid(row=r, column=1, columnspan=2, sticky="ew", pady=(2, 2))

    add_label("── 皮肤 ──")

    # ---- 八项效果滑块 ----
    slider_vars = {}
    value_labels = {}
    for key, label, dflt, note in EFFECTS:
        val = int(params.get(key, dflt))
        val = max(0, min(100, val))
        r = next_row()
        ttk.Label(inner, text=label, anchor="w", width=10).grid(row=r, column=0, sticky="w")
        var = tk.IntVar(value=val)
        slider_vars[key] = var

        def make_cb(k, v):
            def cb(_ev=None):
                try:
                    value_labels[k].config(text=str(int(v.get())))
                except Exception:
                    pass
            return cb

        scale = ttk.Scale(inner, from_=0, to=100, orient="horizontal",
                          variable=var, command=make_cb(key, var))
        scale.grid(row=r, column=1, sticky="ew")
        value_labels[key] = ttk.Label(inner, text=str(val), width=4, anchor="e")
        value_labels[key].grid(row=r, column=2, sticky="e")

    # ---- 算法 / 输出 ----
    add_label("── 磨皮算法 / 输出 ──")

    r = next_row()
    ttk.Label(inner, text="磨皮算法", anchor="w").grid(row=r, column=0, sticky="w")
    blur_var = tk.StringVar(value=BLUR_TYPES[index_of(BLUR_TYPES, int(cfg.get("blur_type", 1)))][0])
    ttk.Combobox(inner, textvariable=blur_var,
                 values=[l for l, _v in BLUR_TYPES], state="readonly").grid(
        row=r, column=1, columnspan=2, sticky="ew")
    skin_var = tk.BooleanVar(value=bool(cfg.get("skin_detect", False)))
    ttk.Checkbutton(inner, text="只磨皮肤区域", variable=skin_var).grid(
        row=next_row(), column=0, columnspan=3, sticky="w")

    r = next_row()
    ttk.Label(inner, text="输出编码", anchor="w").grid(row=r, column=0, sticky="w")
    preset_var = tk.StringVar(value=PRESETS[index_of(PRESETS, cfg.get("preset", "libx264"))][0])
    ttk.Combobox(inner, textvariable=preset_var,
                 values=[l for l, _v in PRESETS], state="readonly").grid(
        row=r, column=1, columnspan=2, sticky="ew")

    r = next_row()
    ttk.Label(inner, text="处理宽度上限（0=原始尺寸）", anchor="w").grid(row=r, column=0, sticky="w")
    width_var = tk.StringVar(value=str(int(cfg.get("max_width", 0) or 0)))
    ttk.Spinbox(inner, from_=0, to=100000, textvariable=width_var, width=8).grid(
        row=r, column=1, sticky="w")

    range_var = tk.BooleanVar(value=bool(cfg.get("use_range", True)))
    ttk.Checkbutton(inner, text="只处理时间线上用到的那一段", variable=range_var).grid(
        row=next_row(), column=0, columnspan=3, sticky="w")

    # ---- 快捷档位 ----
    def apply_quick(preset_map):
        for key, v in preset_map.items():
            if key in slider_vars:
                slider_vars[key].set(int(v))
                value_labels[key].config(text=str(int(v)))

    r = next_row()
    qfrm = ttk.Frame(inner)
    qfrm.grid(row=r, column=0, columnspan=3, sticky="ew")
    ttk.Label(qfrm, text="快捷档位", anchor="w").pack(side="left")
    for name, mapping in PRESETS_QUICK:
        ttk.Button(qfrm, text=name, width=8,
                   command=lambda m=mapping: apply_quick(m)).pack(side="right", padx=2)

    add_label("")
    for w in (WARN_SHARPEN, WARN_WEAK, WARN_RANGE):
        lbl = ttk.Label(inner, text=w, anchor="w", wraplength=430,
                        foreground="#8a6d3b", justify="left")
        lbl.grid(row=next_row(), column=0, columnspan=3, sticky="ew")

    # ---- 底部按钮 ----
    r = next_row()
    btnfrm = ttk.Frame(inner)
    btnfrm.grid(row=r, column=0, columnspan=3, sticky="ew", pady=(10, 6))
    btnfrm.columnconfigure(0, weight=1)
    btnfrm.columnconfigure(1, weight=1)

    def collect():
        params_out = {}
        for key, _l, _d, _n in EFFECTS:
            try:
                params_out[key] = int(slider_vars[key].get())
            except Exception:
                params_out[key] = 0
        try:
            maxw = int(float(width_var.get()))
        except Exception:
            maxw = 0
        return {
            "clip_index": max(0, int(clip_box.current())),
            "params": params_out,
            "blur_type": BLUR_TYPES[index_of_label(BLUR_TYPES, blur_var.get(), 0)][1],
            "skin_detect": bool(skin_var.get()),
            "preset": PRESETS[index_of_label(PRESETS, preset_var.get(), 0)][1],
            "max_width": maxw,
            "use_range": bool(range_var.get()),
        }

    def on_ok():
        sink["ok"] = True
        sink["settings"] = collect()
        try:
            root.quit()
            root.destroy()
        except Exception:
            pass

    def on_cancel():
        sink["ok"] = False
        try:
            root.quit()
            root.destroy()
        except Exception:
            pass

    ok_btn = ttk.Button(btnfrm, text="开始处理", command=on_ok)
    ok_btn.grid(row=0, column=0, sticky="ew", padx=(0, 4))
    ttk.Button(btnfrm, text="取消", command=on_cancel).grid(row=0, column=1, sticky="ew", padx=(4, 0))
    root.protocol("WM_DELETE_WINDOW", on_cancel)
    return root


def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

    spec_path = ""
    argv = sys.argv[1:]
    for i, a in enumerate(argv):
        if a == "--spec" and i + 1 < len(argv):
            spec_path = argv[i + 1]
    spec = load_spec(spec_path)

    try:
        import tkinter as tk
        from tkinter import ttk
    except Exception as e:
        print("#NBERROR " + json.dumps({"error": "起不来面板：本机 Python 没有 tkinter（%s）" % e},
                                       ensure_ascii=False))
        return

    sink = {"ok": False, "settings": None}
    try:
        root = build_ui(tk, ttk, spec, sink)
        root.mainloop()
    except Exception as e:
        import traceback
        emit("#NBERROR " + json.dumps({"error": str(e),
                                       "trace": traceback.format_exc().splitlines()[-3:]},
                                      ensure_ascii=False))
        return

    if sink.get("ok") and sink.get("settings"):
        print("#NBUI " + json.dumps(sink["settings"], ensure_ascii=False))
    else:
        print("#NBCANCEL")


if __name__ == "__main__":
    main()
