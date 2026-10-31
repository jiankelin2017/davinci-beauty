# -*- coding: utf-8 -*-
"""枚举 Resolve 注册表里所有 OpenFX 相关条目，确认 DaVinci Beauty 是否已登记。"""
import os
import sys

API = r"C:\ProgramData\Blackmagic Design\DaVinci Resolve\Support\Developer\Scripting"
LIB = r"D:\DaVinci Resolve\fusionscript.dll"

os.environ["RESOLVE_SCRIPT_API"] = API
os.environ["RESOLVE_SCRIPT_LIB"] = LIB
if API + r"\Modules" not in sys.path:
    sys.path.append(API + r"\Modules")

import DaVinciResolveScript as dvr  # noqa: E402

resolve = dvr.scriptapp("Resolve")
if not resolve:
    print("[FAIL] 无法连接 Resolve")
    sys.exit(3)

print("[OK] 已连接 Resolve %s" % (resolve.GetVersion(),))

fusion = resolve.Fusion()
if not fusion:
    print("[FAIL] 拿不到 Fusion 对象")
    sys.exit(4)

print("[OK] Fusion 对象: %r" % (fusion,))

# ---- GetRegList ----
try:
    regs = fusion.GetRegList()
    print("")
    print("=== GetRegList() 返回类型: %s" % type(regs))
    try:
        print("    元素总数: %d" % len(regs))
    except Exception:
        pass
except Exception as e:
    print("GetRegList() 调用失败: %s" % e)
    regs = None

if regs is not None:
    try:
        items = list(regs)
    except Exception as e:
        print("无法遍历: %s" % e)
        items = []

    # 统计包含关键字的条目
    nama = [x for x in items if "nama" in str(x).lower()]
    ofx = [x for x in items if "ofx" in str(x).lower()]

    print("")
    print("--- 含 nama 的注册项 (%d) ---" % len(nama))
    for x in nama[:30]:
        print("    %s" % x)

    print("")
    print("--- 含 ofx 的注册项 (共 %d，显示前 25) ---" % len(ofx))
    for x in ofx[:25]:
        print("    %s" % x)

    print("")
    print("--- 全部条目前 15 个（看结构）---")
    for x in items[:15]:
        print("    %s" % x)

# ---- GetPluginList ----
print("")
print("=== GetPluginList() ===")
try:
    pl = fusion.GetPluginList()
    print("返回类型: %s" % type(pl))
    try:
        print("元素总数: %d" % len(pl))
        for k in list(pl)[:20]:
            print("    %s" % (k,))
    except Exception as e:
        print("遍历失败: %s" % e)
        print("值: %r" % (pl,))
except Exception as e:
    print("GetPluginList() 失败: %s" % e)
