# -*- coding: utf-8 -*-
"""用 GetReg(id) 遍历注册表，找出所有 OpenFX / Nama 条目。"""
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

print("注册表条目总数: %d, 类型: %s" % (len(regs), type(regs).__name__))

# 先看第一个条目的结构
keys = list(regs.keys())[:3] if isinstance(regs, dict) else list(regs)[:3]
for k in keys:
    print("  key=%r  value=%r" % (k, regs[k] if isinstance(regs, dict) else ""))

# 遍历所有条目
hits = []
ofx_like = []
err = 0
ids = list(regs.keys()) if isinstance(regs, dict) else list(regs)
for rid in ids:
    try:
        reg = fusion.GetReg(rid)
        s = str(reg)
        low = s.lower()
        if "nama" in low:
            hits.append((rid, s))
        elif "ofx" in low:
            ofx_like.append((rid, s))
    except Exception:
        err += 1

print("")
print("=== 含 'nama' 的条目: %d ===" % len(hits))
for rid, s in hits[:20]:
    print("  [%s] %s" % (rid, s[:160]))

print("")
print("=== 含 'ofx' 的条目: %d (显示前 15) ===" % len(ofx_like))
for rid, s in ofx_like[:15]:
    print("  [%s] %s" % (rid, s[:160]))

print("")
print("GetReg 异常次数: %d" % err)

# 抽样看几个条目的完整结构
print("")
print("=== 抽样结构 ===")
for rid in ids[:5] + ids[700:703]:
    try:
        print("  id=%s -> %r" % (rid, fusion.GetReg(rid)))
    except Exception as e:
        print("  id=%s -> 异常 %s" % (rid, str(e)[:60]))
