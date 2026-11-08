# -*- coding: utf-8 -*-
"""遍历 Resolve 注册表，找出所有 OpenFX / 旧插件名相关条目。"""
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
fusion = resolve.Fusion()
regs = fusion.GetRegList()
ids = list(regs.keys())
print("注册表条目总数: %d" % len(ids))

r0 = fusion.GetReg(ids[0])
print("对象类型: %s" % type(r0).__name__)
attrs = [a for a in dir(r0) if not a.startswith("_")]
print("可用属性/方法 (%d): %s" % (len(attrs), attrs[:30]))

# 常见取值方法试探
for m in ("GetAttrs", "GetID", "Name"):
    if hasattr(r0, m):
        try:
            fn = getattr(r0, m)
            v = fn() if callable(fn) else fn
            print("  %s() -> %r" % (m, str(v)[:150]))
        except Exception as e:
            print("  %s() 异常: %s" % (m, str(e)[:70]))

# 按 ID 名字特征筛选（注册表 ID 通常含厂商/插件名）
print("")
print("=== 按 ID 名筛选 ofx/nama ===")
cand = [i for i in ids if isinstance(i, str) and ("ofx" in i.lower() or "nama" in i.lower())]
print("命中 %d 个: %s" % (len(cand), cand[:20]))

# 全量遍历，用 GetAttrs 拿 TOOLB_Name / TOOLS_Name
print("")
print("=== 全量遍历取名称（找旧名 / 新名字样）===")
found = []
scanned = 0
for rid in ids:
    try:
        reg = fusion.GetReg(rid)
        ga = getattr(reg, "GetAttrs", None)
        if not ga:
            continue
        a = ga()
        scanned += 1
        s = str(a)
        if "nama" in s.lower() or "beauty" in s.lower():
            found.append((rid, s[:200]))
    except Exception:
        pass
print("扫描了 %d 个条目" % scanned)
print("命中 %d 个:" % len(found))
for rid, s in found[:25]:
    print("  [%s] %s" % (rid, s))
