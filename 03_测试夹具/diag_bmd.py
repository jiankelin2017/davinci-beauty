# -*- coding: utf-8 -*-
"""检查 Resolve 脚本环境里 bmd / fusion.UIManager 是否可用。"""
import os
import sys

API = r"C:\ProgramData\Blackmagic Design\DaVinci Resolve\Support\Developer\Scripting"
os.environ["RESOLVE_SCRIPT_API"] = API
os.environ["RESOLVE_SCRIPT_LIB"] = r"D:\DaVinci Resolve\fusionscript.dll"
sys.path.append(API + r"\Modules")
sys.path.append(r"D:\DaVinci Resolve")

try:
    import DaVinciResolveScript as dvr
except Exception as e:
    print("[FAIL] 导入 Resolve 脚本模块失败: %s" % e)
    sys.exit(2)

resolve = dvr.scriptapp("Resolve")
print("连接 Resolve:", "OK" if resolve else "FAIL")
print("")

# 1. bmd 模块
print("=== 1. bmd 模块 ===")
try:
    import bmd
    print("  import bmd -> OK")
    print("  可用成员:", [x for x in dir(bmd) if not x.startswith("_")][:15])
except Exception as e:
    print("  import bmd -> 失败: %s" % str(e)[:90])
print("")

# 2. fusion 对象及 UIManager
print("=== 2. fusion / UIManager ===")
try:
    fusion = resolve.Fusion()
    print("  resolve.Fusion() ->", fusion)
    if fusion is not None:
        print("  hasattr UIManager:", hasattr(fusion, "UIManager"))
        if hasattr(fusion, "UIManager"):
            ui = fusion.UIManager
            print("  fusion.UIManager ->", ui)
            try:
                print("  UI 可用方法:", [x for x in dir(ui) if not x.startswith("_")][:12])
            except Exception as e:
                print("  方法枚举失败:", str(e)[:60])
except Exception as e:
    print("  Fusion() 失败: %s" % str(e)[:90])
print("")

# 3. bmd.UIDispatcher
print("=== 3. bmd.UIDispatcher ===")
try:
    import bmd
    fusion = resolve.Fusion()
    disp = bmd.UIDispatcher(fusion.UIManager)
    print("  UIDispatcher ->", disp)
except Exception as e:
    print("  失败: %s" % str(e)[:90])
