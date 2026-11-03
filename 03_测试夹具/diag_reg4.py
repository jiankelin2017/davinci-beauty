# -*- coding: utf-8 -*-
"""对照测试：注册表里到底有没有 OFX/ResolveFX 插件？"""
import os
import sys

API = r"C:\ProgramData\Blackmagic Design\DaVinci Resolve\Support\Developer\Scripting"
os.environ["RESOLVE_SCRIPT_API"] = API
os.environ["RESOLVE_SCRIPT_LIB"] = r"D:\DaVinci Resolve\fusionscript.dll"
sys.path.append(API + r"\Modules")

import DaVinciResolveScript as dvr  # noqa: E402

resolve = dvr.scriptapp("Resolve")
fusion = resolve.Fusion()
regs = fusion.GetRegList()
ids = list(regs.keys())

KEY = ["ofx", "nama", "beauty", "bloom", "glow", "face", "refine",
       "sharpen", "beautybox", "denoise", "film"]

hits = {k: [] for k in KEY}
scanned = 0
for rid in ids:
    try:
        reg = fusion.GetReg(rid)
        nm = ""
        try:
            nm = str(reg.Name())
        except Exception:
            pass
        s = (str(rid) + " | " + nm).lower()
        scanned += 1
        for k in KEY:
            if k in s:
                hits[k].append("%s | %s" % (rid, nm))
    except Exception:
        pass

print("扫描条目数: %d" % scanned)
print("")
for k in KEY:
    v = hits[k]
    print("%-12s 命中 %d" % (k, len(v)))
    for x in v[:6]:
        print("      %s" % x)
    if len(v) > 6:
        print("      ...(共 %d)" % len(v))
