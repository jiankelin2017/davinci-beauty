#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""用假的 Resolve API 跑一遍真实脚本，验证 collect_clips / 降级 / worker 调用链。"""
import importlib.util
import os
import sys
import types

HERE = os.path.dirname(os.path.abspath(__file__))
# 测试素材用脚本自身目录定位，别再硬编码旧工作区绝对路径。
SRC = os.path.join(HERE, "test_src.mp4")
# 被测脚本路径：可被 NAMA_RESOLVE_SCRIPT 覆盖（默认指向 Resolve 的 Utility 脚本目录）。
SCRIPT = os.environ.get(
    "NAMA_RESOLVE_SCRIPT",
    r"C:\ProgramData\Blackmagic Design\DaVinci Resolve\Support\Scripts\Utility\NamaBeauty_美颜.py")


class FakeMPI:
    def GetName(self):
        return "test_src.mp4"

    def GetClipProperty(self):
        return {"File Path": SRC, "Duration": "00:00:03:00", "FPS": "25"}


class FakeItem:
    def GetMediaPoolItem(self):
        return FakeMPI()

    def GetName(self):
        return "test_src.mp4"

    def GetSourceStartTime(self):
        return 0.0

    def GetSourceEndTime(self):
        return 3.0


class FakeTimeline:
    def GetName(self):
        return "Timeline 1"

    def GetTrackCount(self, t):
        return 1

    def GetItemListInTrack(self, t, i):
        return [FakeItem()]

    def GetSetting(self, k):
        return "25"


class FakeProject:
    def GetCurrentTimeline(self):
        return FakeTimeline()

    def GetMediaPool(self):
        return None


class FakePM:
    def GetCurrentProject(self):
        return FakeProject()


class FakeMS:
    def AddItemListToMediaPool(self, path):
        print("         [Fake] AddItemListToMediaPool <- %s" % path)
        return ["mock-media-pool-item"]


class FakeResolve:
    def GetProjectManager(self):
        return FakePM()

    def GetMediaStorage(self):
        return FakeMS()

    def Fusion(self):
        return None  # 强制走「无 UI」降级路径


mod = types.ModuleType("bmd")
mod.scriptapp = lambda kind: FakeResolve() if kind == "Resolve" else None
sys.modules["bmd"] = mod

spec = importlib.util.spec_from_file_location("nama_resolve_script", SCRIPT)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
m.main()
print("\n=== harness 结束 ===")
