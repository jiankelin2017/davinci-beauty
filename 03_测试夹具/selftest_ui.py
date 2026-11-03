#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""无人工介入验证 nama_ui.py：窗口能否创建、默认值是否正确、按钮能否闭环。

做法：真实地把窗口建出来（topmost 已开，会闪一下），然后用定时器自动
触发按钮，检查 sink 里拿到的数据结构。窗口句柄用 Windows API 交叉验证，
确认它是「真的存在且可见」，而不是只有 Tk 内部对象。
"""

import ctypes
import json
import os
import sys
import traceback

UI = r"C:\ProgramData\DaVinciBeauty\nama_ui.py"
ROOT = os.path.dirname(UI)
sys.path.insert(0, ROOT)

SPEC = {
    "clips": [
        {"name": "A001.mp4", "track": 1, "path": "D:\\media\\A001.mp4"},
        {"name": "访谈镜头.mov", "track": 2, "path": "D:\\media\\访谈镜头.mov"},
    ],
    "config": {
        "params": {"blur_level": 55, "color_level": 20, "red_level": 30},
        "blur_type": 0,
        "skin_detect": True,
        "preset": "libx264",
        "max_width": 1920,
        "use_range": True,
    },
}


def window_state(hwnd):
    u = ctypes.WinDLL("user32", use_last_error=True)
    u.IsWindowVisible.argtypes = [ctypes.c_void_p]
    u.GetWindowTextLengthW.argtypes = [ctypes.c_void_p]
    u.GetWindowTextW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int]
    visible = bool(u.IsWindowVisible(hwnd))
    n = u.GetWindowTextLengthW(hwnd)
    buf = ctypes.create_unicode_buffer(n + 1)
    u.GetWindowTextW(hwnd, buf, n + 1)
    return visible, buf.value


def find_child_by_text(widget, text, found):
    try:
        if str(widget.cget("text")).strip() == text:
            found.append(widget)
    except Exception:
        pass
    for ch in widget.winfo_children():
        find_child_by_text(ch, text, found)


def main():
    failures = []
    try:
        import tkinter as tk
        from tkinter import ttk
    except Exception as e:
        print("[FAIL] 无 tkinter：%s" % e)
        return 1

    try:
        import nama_ui
    except Exception:
        traceback.print_exc()
        print("[FAIL] 导入 nama_ui 失败")
        return 1

    sink = {"ok": False, "settings": None}
    root = None
    try:
        root = nama_ui.build_ui(tk, ttk, SPEC, sink)
    except Exception:
        traceback.print_exc()
        print("[FAIL] build_ui 抛异常")
        return 1
    print("[PASS] build_ui 未抛异常")

    root.update_idletasks()
    root.update()

    u = ctypes.WinDLL("user32", use_last_error=True)
    u.GetParent.argtypes = [ctypes.c_void_p]
    u.GetParent.restype = ctypes.c_void_p
    try:
        hwnd = u.GetParent(root.winfo_id())
    except Exception:
        hwnd = 0
    if hwnd:
        visible, title = window_state(hwnd)
        print("[%s] 窗口句柄 0x%x 可见=%s 标题=%s"
              % ("PASS" if visible else "FAIL", hwnd, visible, title))
        if not visible:
            failures.append("窗口不可见")
    else:
        print("[WARN] 拿不到顶层窗口句柄（无法交叉验证）")

    # 滑块默认值是否按 spec 注入
    check_targets = {"blur_level": 55, "color_level": 20, "red_level": 30,
                     "clarity": 0, "face_threed": 0}
    sliders = {}

    def walk(w):
        for ch in w.winfo_children():
            cls = ch.winfo_class()
            cls_name = str(type(ch)).lower()
            if "scale" in cls_name:
                try:
                    # ttk.Scale 没有 get(key)，需要从 children 里找
                    pass
                except Exception:
                    pass
            walk(ch)
    walk(root)

    # ---- 自动点击「开始处理」 ----
    found = []
    find_child_by_text(root, "开始处理", found)
    if not found:
        print("[FAIL] 找不到「开始处理」按钮")
        failures.append("缺 OK 按钮")
    else:
        print("[PASS] 找到「开始处理」按钮")
        def fire():
            try:
                found[0].invoke()
            except Exception:
                traceback.print_exc()
        root.after(300, fire)
        root.mainloop()

    if sink.get("ok"):
        print("[PASS] 按钮回调触发，拿到结果")
    else:
        print("[FAIL] 按钮回调没写回 sink")
        failures.append("OK 回调无效")

    st = sink.get("settings")
    if not st:
        print("[FAIL] settings 为空")
        return 1

    print("[INFO] 回传 JSON：%s" % json.dumps(st, ensure_ascii=False))

    # 断言
    def expect(name, got, want):
        if got == want:
            print("[PASS] %s = %r" % (name, got))
        else:
            print("[FAIL] %s = %r，期望 %r" % (name, got, want))
            failures.append(name)

    for key, want in check_targets.items():
        expect("params." + key, st["params"].get(key), want)
    expect("blur_type（spec=0 高斯）", st.get("blur_type"), 0)
    expect("skin_detect（spec=True）", st.get("skin_detect"), True)
    expect("preset", st.get("preset"), "libx264")
    expect("max_width（spec=1920）", st.get("max_width"), 1920)
    expect("use_range", st.get("use_range"), True)
    expect("clip_index（留第一项=全部）", st.get("clip_index"), 0)
    if set(st["params"].keys()) == set(k for k, _l, _d, _n in nama_ui.EFFECTS):
        print("[PASS] 参数键与 EFFECTS 完全一致（8 项）")
    else:
        print("[FAIL] 参数键不匹配：%s" % sorted(st["params"].keys()))
        failures.append("参数键")

    # ---- 取消路径 ----
    sink2 = {"ok": False, "settings": None}
    root2 = nama_ui.build_ui(tk, ttk, SPEC, sink2)
    found2 = []
    find_child_by_text(root2, "取消", found2)
    root2.after(200, lambda: found2[0].invoke())
    root2.mainloop()
    if not sink2.get("ok"):
        print("[PASS] 取消路径 ok=False")
    else:
        print("[FAIL] 取消路径仍然返回 ok=True")
        failures.append("取消路径")

    print("=" * 60)
    if failures:
        print("结论：有 %d 项未通过 → %s" % (len(failures), failures))
        return 1
    print("结论：面板全部检查通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
