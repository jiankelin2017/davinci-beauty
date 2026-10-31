# -*- coding: utf-8 -*-
"""
连接到正在运行的 DaVinci Resolve，诊断 DaVinci Beauty 插件是否可用。
在 Resolve 外部运行（系统 Python），通过 Resolve 脚本 API 连接。
"""
import os
import sys

API = r"C:\ProgramData\Blackmagic Design\DaVinci Resolve\Support\Developer\Scripting"
LIB = r"D:\DaVinci Resolve\fusionscript.dll"

os.environ["RESOLVE_SCRIPT_API"] = API
os.environ["RESOLVE_SCRIPT_LIB"] = LIB
if API + r"\Modules" not in sys.path:
    sys.path.append(API + r"\Modules")


def main():
    print("=" * 62)
    print("DaVinci Beauty 插件诊断")
    print("=" * 62)

    try:
        import DaVinciResolveScript as dvr
    except Exception as e:
        print("[FAIL] 无法导入 Resolve 脚本模块: %s" % e)
        return 2

    resolve = dvr.scriptapp("Resolve")
    if not resolve:
        print("[FAIL] 无法连接到 Resolve。请确认：")
        print("       1. Resolve 已启动并停留在项目管理器或已进入项目")
        print("       2. Resolve 设置里已开启外部脚本访问")
        print("          Preferences > System > General > "
              "External Control Using > 允许 (或选网络并设端口)")
        return 3

    print("[OK] 已连接到 Resolve")
    try:
        ver = resolve.GetVersion()
        print("     Resolve 版本: %s" % (ver,))
    except Exception:
        pass

    pm = resolve.GetProjectManager()
    proj = pm.GetCurrentProject()
    if not proj:
        print("[WARN] 当前没有打开的项目，无法继续片段级测试。")
        print("       请先打开或新建一个项目和时间线，再运行本诊断。")
        return 4

    print("[OK] 当前项目: %s" % proj.GetName())

    timeline = proj.GetCurrentTimeline()
    if not timeline:
        print("[WARN] 当前没有激活的时间线，无法测试添加效果。")
        return 5

    print("[OK] 当前时间线: %s" % timeline.GetName())

    # ---- 列出视频轨道上的片段 ----
    total = 0
    first_item = None
    tracks = timeline.GetTrackCount("video")
    print("     视频轨道数: %s" % tracks)
    for ti in range(1, tracks + 1):
        items = timeline.GetItemListInTrack("video", ti) or []
        for it in items:
            total += 1
            if first_item is None:
                first_item = it
    print("     片段总数: %d" % total)
    if not first_item:
        print("[WARN] 时间线上没有视频片段，请放一段素材再测。")
        return 6

    # ---- 关键一步：让 Resolve 自己列出可用的 OpenFX ----
    print("")
    print("--- 尝试读取 Resolve 认识的 OpenFX 效果列表 ---")
    found = []
    try:
        # Resolve 19+ 的 Fusion 全局对象可枚举 OFX 注册项
        fusion = resolve.Fusion()
        if fusion:
            for attr in ("GetRegList", "GetReg", "GetPluginList"):
                if hasattr(fusion, attr):
                    print("     Fusion 支持 %s()" % attr)
    except Exception as e:
        print("     (Fusion 对象读取受限: %s)" % str(e)[:60])

    # 用 AddEffect 直接试探插件名是否被 Resolve 接受
    print("")
    print("--- 用 AddEffect 试探插件是否可加载 ---")
    for nm in ("DaVinci Beauty 美颜", "NamaBeauty", "com.nama.davinci.beauty"):
        try:
            ok = first_item.AddEffect(nm)
            print("     AddEffect(%-24s) -> %s" % (nm, ok))
            if ok:
                found.append(nm)
                break
        except Exception as e:
            print("     AddEffect(%-24s) -> 异常 %s" % (nm, str(e)[:50]))

    print("")
    if found:
        print("[OK] 插件在 Resolve 中可用，生效名: %s" % found[0])
        return 0
    print("[FAIL] Resolve 未识别到该插件名。")
    return 1


if __name__ == "__main__":
    sys.exit(main())
