#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""NamaBeauty —— DaVinci Resolve 内置美颜脚本（皮肤 / 眼部效果）。

用法：
    1. 打开 Resolve 并进入一个项目，在时间线上放好要处理的片段；
    2. 菜单 Workspace → Scripts → Utility → NamaBeauty_美颜；
    3. 在弹出的面板上调参数，点「开始处理」；
    4. 处理完成后，成品会自动导入媒体池，手动拖到时间线上替换原片段即可。

实现说明（可验证事实）：
    * 面板上 8 个效果的刻度是 0..100，脚本会按「SDK 值 = 刻度 / 100 × SDK 上限」
      换算后传给 FaceUnity NamaSDK，与 OBS 插件 obs-nama-beauty 的做法一致。
    * 真正的逐帧处理在一个独立子进程里跑（Scripts/nama_worker.py），
      避免在 Resolve 主进程里创建第二个 OpenGL 上下文、也避免崩溃带崩 Resolve。
    * 子进程解释器优先使用 Resolve 自带的 fuscript.exe，因此不需要额外安装 Python。
"""

import json
import os
import subprocess
import sys

TOOL_DIR = r"C:\ProgramData\NamaBeauty"
WORKER = os.path.join(TOOL_DIR, "nama_worker.py")
CONFIG = os.path.join(TOOL_DIR, "config.json")
JOB_DIR = os.path.join(TOOL_DIR, "jobs")

# (键, 中文标签, 默认面板刻度, 实测量化结论)
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

# 磨皮算法：实测细节能量，越小越平滑；完全不磨皮为 4.184
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

WARN_SHARPEN = "注意：实测磨皮不为 0 时，锐化会被完全掩盖。要用锐化请把磨皮调到 0。"
WARN_WEAK = "提示：亮眼与去黑眼圈实测很弱（改动像素不足 1%），需要放大画面才看得出。"


def emit(msg):
    """Resolve 的 sys.stdout 不一定是完整文件对象，写输出要带保护。"""
    try:
        print(msg)
    except Exception:
        pass
    flush = getattr(sys.stdout, "flush", None)
    if callable(flush):
        try:
            flush()
        except Exception:
            pass


# ------------------------------------------------------------------ #
#  连接到 Resolve
# ------------------------------------------------------------------ #
def get_resolve():
    candidates = []
    # 进程里可能已经有一个 resolve 全局（某些宿主会注入）。用 *_a 吸收掉下面统一
    # 传入的 "Resolve" 参数 —— 旧代码写成无参 `lambda: globals()["resolve"]`，
    # 被单参调用必然 TypeError 再被 except 吞掉，是个永远走不到的死候选。
    candidates.append(lambda *_a: globals().get("resolve"))
    for mod_name in ("DaVinciResolveScript", "bmd"):
        try:
            m = __import__(mod_name)
            candidates.append(m.scriptapp)
        except Exception:
            pass
    for fn in candidates:
        try:
            r = fn("Resolve") if callable(fn) else fn
            if r:
                return r
        except Exception:
            continue
    return None


# ------------------------------------------------------------------ #
#  收集时间线上的视频片段
# ------------------------------------------------------------------ #
def pick_source_path(props):
    """GetClipProperty 返回的键名在不同版本里可能是 File Path / FilePath，这里做模糊匹配。"""
    if not isinstance(props, dict):
        return ""
    for k, v in props.items():
        if k.lower().replace(" ", "").replace("_", "") == "filepath" and isinstance(v, str) and v:
            return v
    for k, v in props.items():
        if "path" in k.lower() and isinstance(v, str) and len(v) > 3:
            return v
    return ""


def collect_clips(timeline):
    clips = []
    try:
        tracks = timeline.GetTrackCount("video")
    except Exception:
        return clips
    for ti in range(1, tracks + 1):
        try:
            items = timeline.GetItemListInTrack("video", ti) or []
        except Exception:
            continue
        for item in items:
            mpi = None
            try:
                mpi = item.GetMediaPoolItem()
            except Exception:
                pass
            path = ""
            if mpi is not None:
                try:
                    path = pick_source_path(mpi.GetClipProperty())
                except Exception:
                    path = ""
            if not path or not os.path.isfile(path):
                continue  # 生成器、复合片段等没有落地的源文件，跳过
            name = ""
            try:
                name = mpi.GetName() if mpi else item.GetName()
            except Exception:
                name = os.path.basename(path)
            clips.append({"item": item, "path": path, "name": name, "track": ti})
    return clips


def source_range_seconds(item, fps):
    """取片段在源媒体上用到的进/出点。

    官方注释只说 GetSourceStartTime/EndTime 返回 "time position"，没写明单位；
    这里加一道判断：若数值大到明显是帧（除以帧率后才落在合理时长内），按帧换算。
    即便判断失误，worker 也会把区间夹在视频真实时长内，不会越界。
    """
    try:
        start = float(item.GetSourceStartTime())
        end = float(item.GetSourceEndTime())
    except Exception:
        return 0.0, 0.0
    if not (end > start):
        return 0.0, 0.0
    if fps > 0 and end > fps * 3600:
        start /= fps
        end /= fps
    if end - start < 0.05:
        return 0.0, 0.0
    return start, end


def timeline_fps(timeline):
    try:
        v = float(timeline.GetSetting("timelineFrameRate") or 0)
        return v if v > 0 else 0.0
    except Exception:
        return 0.0


# ------------------------------------------------------------------ #
#  参数面板
# ------------------------------------------------------------------ #
def load_defaults():
    try:
        # utf-8-sig：兼容记事本保存出来的带 BOM 的 JSON
        with open(CONFIG, "r", encoding="utf-8-sig") as f:
            return json.load(f)
    except Exception:
        return {"params": {}, "blur_type": 1, "skin_detect": False,
                "preset": "libx264", "max_width": 0, "use_range": True,
                "output_suffix": "_beauty"}


def build_dialog(fusion, bmd, clips, cfg):
    ui = fusion.UIManager
    disp = bmd.UIDispatcher(ui)

    params = dict(cfg.get("params") or {})
    for key, _label, dflt, _note in EFFECTS:
        params.setdefault(key, dflt)

    clip_items = []
    if clips:
        for i, c in enumerate(clips):
            clip_items.append("<%d> %s" % (i + 1, c["name"]))
    clip_items.insert(0, "全部 %d 条视频片段（逐条处理）" % len(clips))

    rows = [
        ui.Label({"ID": "LblClip", "Text": "处理目标", "Weight": 0.1}),
        ui.ComboBox({"ID": "Clip"}),
        ui.Label({"ID": "Sep1", "Text": "── 皮肤 ──", "StyleSheet": "QLabel { color: #888; }"}),
    ]

    # 磨皮单独一行：滑块的联动提示挂在「锐化」和「磨皮」的联动逻辑上
    for key, label, dflt, note in EFFECTS:
        val = int(params.get(key, dflt))
        rows.append(ui.Label({"ID": "L_" + key, "Text": label, "Weight": 0.1}))
        rows.append(ui.VGroup({"Weight": 0.1}, [
            ui.HGroup([
                ui.Slider({"ID": "S_" + key, "Minimum": 0, "Maximum": 100,
                           "Value": val, "Weight": 0.7}),
                ui.Label({"ID": "V_" + key, "Text": str(val), "Weight": 0.15}),
            ]),
        ]))
    rows.append(ui.Label({"ID": "Sep2", "Text": "── 磨皮算法 / 输出 ──",
                          "StyleSheet": "QLabel { color: #888; }"}))
    rows.append(ui.HGroup([
        ui.Label({"ID": "L_BT", "Text": "磨皮算法", "Weight": 0.2}),
        ui.ComboBox({"ID": "BlurType", "Weight": 0.4}),
        ui.CheckBox({"ID": "SkinDetect", "Text": "只磨皮肤区域", "Weight": 0.4}),
    ]))
    rows.append(ui.HGroup([
        ui.Label({"ID": "L_PR", "Text": "输出编码", "Weight": 0.2}),
        ui.ComboBox({"ID": "Preset", "Weight": 0.8}),
    ]))
    rows.append(ui.HGroup([
        ui.Label({"ID": "L_MW", "Text": "处理宽度上限（0=原始尺寸）", "Weight": 0.2}),
        ui.SpinBox({"ID": "MaxWidth", "Min": 0, "Max": 100000, "Weight": 0.3}),
        ui.Label({"ID": "L_UR", "Text": "", "Weight": 0.5}),
    ]))
    rows.append(ui.CheckBox({"ID": "UseRange", "Text": "只处理这段片段在时间线上用到的部分"}))
    rows.append(ui.Label({"ID": "NoteFig", "Text": WARN_SHARPEN}))
    rows.append(ui.Label({"ID": "NoteWeak", "Text": WARN_WEAK}))
    rows.append(ui.HGroup([
        ui.Button({"ID": "OK", "Text": "开始处理", "Weight": 0.5}),
        ui.Button({"ID": "Cancel", "Text": "取消", "Weight": 0.5}),
    ]))

    win = disp.AddWindow({"ID": "NamaBeauty", "WindowTitle": "NamaBeauty 美颜",
                          "Geometry": [140, 120, 420, 620]},
                         rows)

    it = win.GetItems()
    for c in clip_items:
        it["Clip"].AddItem(c)
    it["Clip"].CurrentIndex = 0
    for label, value in BLUR_TYPES:
        it["BlurType"].AddItem(label)
    try:
        it["BlurType"].CurrentIndex = [v for _l, v in BLUR_TYPES].index(int(cfg.get("blur_type", 1)))
    except Exception:
        it["BlurType"].CurrentIndex = 0
    for label, value in PRESETS:
        it["Preset"].AddItem(label)
    try:
        it["Preset"].CurrentIndex = [v for _l, v in PRESETS].index(cfg.get("preset", "libx264"))
    except Exception:
        it["Preset"].CurrentIndex = 0
    it["MaxWidth"].Value = int(cfg.get("max_width", 0) or 0)
    it["SkinDetect"].Checked = bool(cfg.get("skin_detect", False))
    it["UseRange"].Checked = bool(cfg.get("use_range", True))

    state = {"result": None}

    def make_slider_cb(key):
        def cb(ev):
            try:
                v = int(it["S_" + key].Value)
                it["V_" + key].Text = str(v)
            except Exception:
                pass
        return cb

    for key, _l, _d, _n in EFFECTS:
        try:
            win.On["S_" + key].ValueChanged = make_slider_cb(key)
        except Exception:
            pass

    def on_ok(ev):
        result = {
            "clip_index": int(it["Clip"].CurrentIndex),
            "params": {},
            "blur_type": BLUR_TYPES[max(0, int(it["BlurType"].CurrentIndex))][1],
            "skin_detect": bool(it["SkinDetect"].Checked),
            "preset": PRESETS[max(0, int(it["Preset"].CurrentIndex))][1],
            "max_width": int(it["MaxWidth"].Value),
            "use_range": bool(it["UseRange"].Checked),
        }
        for key, _l, _d, _n in EFFECTS:
            result["params"][key] = int(it["S_" + key].Value)
        state["result"] = result
        disp.ExitLoop()

    def on_dismiss(ev):
        state["result"] = None
        disp.ExitLoop()

    win.On.OK.Clicked = on_ok
    win.On.Cancel.Clicked = on_dismiss
    win.On.NamaBeauty.Close = on_dismiss
    return disp, win, state


def ask_settings(fusion, bmd, clips, cfg):
    disp, win, state = build_dialog(fusion, bmd, clips, cfg)
    try:
        win.Show()
        disp.RunLoop()
        win.Hide()
    except Exception as e:
        emit("[NamaBeauty] 面板无法显示（%s），改用 config.json 里的默认参数。" % e)
        return None
    return state["result"]


def ask_settings_tk(clips, cfg):
    """第二级：把参数面板丢进独立子进程（fuscript.exe + tkinter）。

    为什么不直接用 Fusion 的 UIManager：Resolve 通用脚本目录里的 Python
    环境不一定装了 bmd 模块，实测本机就没有（import bmd 直接报错）。
    单独起进程的好处是消息循环独立，面板崩了也不会带崩 Resolve。
    """
    import tempfile

    prefix = build_cmd_prefix()
    if prefix is None:
        return None

    ui_path = os.path.join(TOOL_DIR, "nama_ui.py")
    if not os.path.isfile(ui_path):
        emit("[提示] 找不到面板脚本 nama_ui.py，改用 config.json 默认参数。")
        return None

    spec = {
        "config": cfg,
        "clips": [{"name": c["name"], "track": c["track"], "path": c["path"]}
                  for c in clips],
    }
    try:
        os.makedirs(JOB_DIR, exist_ok=True)
    except Exception:
        pass

    spec_path = os.path.join(JOB_DIR, "ui_spec.json")
    try:
        with open(spec_path, "w", encoding="utf-8") as f:
            f.write(json.dumps(spec, ensure_ascii=False, indent=2))
    except Exception as e:
        emit("[提示] 写面板参数失败（%s），改用 config.json 默认参数。" % e)
        return None

    cmd = prefix + [ui_path, "--spec", spec_path]
    try:
        kwargs = {}
        if os.name == "nt":
            si = subprocess.STARTUPINFO()
            si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            kwargs["startupinfo"] = si
        proc = subprocess.run(cmd, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE,
                              universal_newlines=True, encoding="utf-8",
                            errors="replace", **kwargs)
        out = proc.stdout or ""
    except Exception as e:
        emit("[提示] 启动面板失败（%s），改用 config.json 默认参数。" % e)
        return None

    # stdout 里会混着 SDK 杂讯，倒着扫，取最后一条有效指令
    lines = [l.strip() for l in out.splitlines() if l.strip()]
    for s in reversed(lines):
        if s.startswith("#NBUI "):
            try:
                return json.loads(s[len("#NBUI "):])
            except Exception as e:
                emit("[提示] 面板返回解析失败（%s），改用默认参数。" % e)
            return None
        if s.startswith("#NBCANCEL"):
            return {}  # 空 dict 即用户取消
        if s.startswith("#NBERROR"):
            try:
                j = json.loads(s[len("#NBERROR "):])
                emit("[提示] 面板不可用（%s），改用 config.json 默认参数。"
                     % j.get("error"))
            except Exception:
                emit("[提示] 面板不可用，改用 config.json 默认参数。")
            return None
    return None


# ------------------------------------------------------------------ #
#  调用 worker
# ------------------------------------------------------------------ #
def build_cmd_prefix():
    """优先用 Resolve 自带的 fuscript，找不到时退回系统 Python。"""
    import shutil

    cands = [
        r"D:\DaVinci Resolve\fuscript.exe",
        r"C:\Program Files\Blackmagic Design\DaVinci Resolve\fuscript.exe",
        r"C:\Program Files\Blackmagic Design\DaVinci Resolve Studio\fuscript.exe",
    ]
    for p in cands:
        if os.path.isfile(p):
            return [p, "-l", "python3"]
    for exe in ("python", "python3", "py"):
        hit = shutil.which(exe)
        if hit:
            return [hit]
    return None


def run_worker(job):
    prefix = build_cmd_prefix()
    if prefix is None:
        return {"ok": False, "error": "没有找到可用的解释器（fuscript.exe 或 python.exe）"}
    if not os.path.isfile(WORKER):
        return {"ok": False, "error": "找不到 worker：%s" % WORKER}
    try:
        os.makedirs(JOB_DIR, exist_ok=True)
    except Exception:
        pass
    job_path = os.path.join(JOB_DIR, "job.json")
    try:
        with open(job_path, "w", encoding="utf-8") as f:
            f.write(json.dumps(job, ensure_ascii=False, indent=2))
    except Exception as e:
        return {"ok": False, "error": "写作业文件失败：%s" % e}

    cmd = prefix + [WORKER, "--job", job_path]
    emit("[NamaBeauty] 调用：%s" % " ".join(cmd))
    kwargs = {}
    try:
        if os.name == "nt":
            si = subprocess.STARTUPINFO()
            si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            kwargs["startupinfo"] = si
            no_win = getattr(subprocess, "CREATE_NO_WINDOW", None) or 0x08000000
            kwargs["creationflags"] = no_win
    except Exception:
        kwargs = {}
    try:
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              universal_newlines=True, encoding="utf-8",
                              errors="replace", **kwargs)
        out = proc.stdout or ""
    except Exception as e:
        return {"ok": False, "error": "启动 worker 失败：%s" % e}

    for line in out.splitlines():
        if line.startswith("[worker]"):
            emit("         %s" % line)
    for line in reversed(out.splitlines()):
        line = line.strip()
        if line.startswith("#NBRESULT"):
            try:
                return json.loads(line[len("#NBRESULT "):])
            except Exception as e:
                return {"ok": False, "error": "结果解析失败：%s" % e}
    tail = "\n".join(out.splitlines()[-8:])
    return {"ok": False, "error": "worker 没有返回结果。输出尾部：\n%s" % tail}


# ------------------------------------------------------------------ #
#  主流程
# ------------------------------------------------------------------ #
def main():
    emit("=" * 62)
    emit("NamaBeauty —— 皮肤 / 眼部美颜")
    emit("=" * 62)

    resolve = get_resolve()
    if resolve is None:
        emit("[错误] 取不到 Resolve 对象。请确认是在 Resolve 的 Workspace → Scripts 里执行。")
        return

    pm = resolve.GetProjectManager()
    project = pm.GetCurrentProject() if pm else None
    timeline = project.GetCurrentTimeline() if project else None
    if project is None:
        emit("[错误] 当前没有打开的项目。")
        return
    if timeline is None:
        emit("[错误] 当前项目没有打开的时间线。")
        return

    clips = collect_clips(timeline)
    if not clips:
        emit("[提示] 时间线上没有找到带磁盘源文件的视频片段。")
        emit("       生成器、复合片段之类没有源文件的素材无法处理。")
        return

    emit("时间线：%s，共 %d 条可用视频片段" % (timeline.GetName(), len(clips)))
    for i, c in enumerate(clips):
        emit("  <%d> V%d  %s" % (i + 1, c["track"], c["name"]))

    cfg = load_defaults()
    settings = {"clip_index": int(cfg.get("clip_index", 0) or 0),
                "params": dict(cfg.get("params") or {}),
                "blur_type": int(cfg.get("blur_type", 1)),
                "skin_detect": bool(cfg.get("skin_detect", False)),
                "preset": cfg.get("preset", "libx264"),
                "max_width": int(cfg.get("max_width", 0) or 0),
                "use_range": bool(cfg.get("use_range", True))}
    for key, _l, dflt, _n in EFFECTS:
        settings["params"].setdefault(key, dflt)

    # ---- 三段式取参数：Fusion 面板 → Tk 独立窗口 → config.json ----
    # 注意：fusion 与 bmd 必须分开取。两者写在一个 try 里的话，
    # 只要 bmd 导入失败就会把 fusion 也置空，面板就永远弹不出来了。
    picked = None
    fusion = None
    try:
        fusion = resolve.Fusion()
    except Exception:
        fusion = None

    if fusion is not None and hasattr(fusion, "UIManager"):
        bmd_mod = None
        try:
            bmd_mod = __import__("bmd")
        except Exception:
            bmd_mod = None
        if bmd_mod is not None:
            try:
                picked = ask_settings(fusion, bmd_mod, clips, cfg)
            except Exception as e:
                emit("[提示] Fusion 面板异常（%s），改用独立窗口。" % e)
        else:
            emit("[提示] 本机 Python 没有 bmd 模块，改用独立窗口。")

    if picked is None:
        picked = ask_settings_tk(clips, cfg)

    if picked is None:
        emit("[提示] 两级面板都起不来，直接用 config.json 的默认参数处理第一条。")
        emit("       想改强度就编辑 %s 里的 params。" % CONFIG)
        settings["clip_index"] = 1
    else:
        settings = picked
        if not settings:
            emit("[已取消]")
            return

    idx = settings.get("clip_index", 0)
    targets = clips if idx == 0 else [clips[idx - 1]]

    suffix = cfg.get("output_suffix", "_beauty")
    ok_list, fail_list = [], []
    fps = timeline_fps(timeline)

    for c in targets:
        src = c["path"]
        stem = os.path.splitext(os.path.basename(src))[0]
        out_path = os.path.join(os.path.dirname(src), stem + suffix + ".mp4")
        if settings["preset"] == "prores422":
            out_path = os.path.splitext(out_path)[0] + ".mov"

        start, end = 0.0, 0.0
        if settings["use_range"]:
            start, end = source_range_seconds(c["item"], fps)

        emit("-" * 62)
        emit("处理：%s" % c["name"])
        emit("  输入 %s" % src)
        emit("  输出 %s" % out_path)
        if end > start:
            emit("  区间 %.3f ~ %.3f 秒（时间线用到的部分）" % (start, end))
        else:
            emit("  区间 整段源媒体")

        job = {
            "src": src,
            "out": out_path,
            "start": start,
            "end": end,
            "params": settings["params"],
            "blur_type": settings["blur_type"],
            "skin_detect": settings["skin_detect"],
            "preset": settings["preset"],
            "max_width": settings["max_width"],
        }
        res = run_worker(job)
        if not res.get("ok"):
            emit("  [失败] %s" % res.get("error"))
            fail_list.append((c["name"], res.get("error", "")))
            continue

        emit("  完成 %d 帧 / 耗时 %.1fs / 平均 %.1f fps / 检出人脸 %s"
             % (res.get("frames", 0), res.get("elapsed", 0), res.get("fps_avg", 0),
                res.get("faces", 0)))

        # 导回媒体池
        imported = False
        try:
            ms = resolve.GetMediaStorage()
            pool = project.GetMediaPool()
            added = ms.AddItemListToMediaPool(res.get("out", ""))
            if added:
                imported = True
        except Exception as e:
            emit("  导入媒体池时出现异常：%s" % e)
        if imported:
            emit("  已导入媒体池")
        else:
            emit("  未能自动导入媒体池，请手动把 %s 拖进去" % res.get("out"))
        ok_list.append((c["name"], res.get("out", "")))

    emit("=" * 62)
    emit("汇总：成功 %d 条，失败 %d 条" % (len(ok_list), len(fail_list)))
    for name, path in ok_list:
        emit("  [成功] %s → %s" % (name, path))
    for name, err in fail_list:
        emit("  [失败] %s → %s" % (name, err))
    emit("用法：把媒体池里的成品片段拖到时间线上，替换原片段即可。")


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        import traceback
        emit("[未捕获异常] %s" % e)
        try:
            emit(traceback.format_exc())
        except Exception:
            pass
