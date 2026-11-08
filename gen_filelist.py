#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""重新生成 FILELIST.txt（每个文件的 SHA256 + 大小）。

为什么需要这个脚本
------------------
FILELIST.txt 以前是手工维护的，结果很快就和实际文件脱节：里面还留着一个早就删掉的
`01_ofx插件源码/t.h`，05_安装包/ 整个目录没被收录，被改动过的文件哈希也没更新。
与其靠人肉同步，不如把规则写成脚本，随时可重放。

收录规则
--------
* 收录：本目录下所有「源码 / 文档 / 脚本」类文件；若本目录是 git 仓库，
  则只收录 **git 已跟踪** 的文件（未跟踪的本地资产不写进清单，
  这样清单与本仓库实际发布的内容一致）。
* 排除：
    - 构建产物目录 out/ 、obj/ ，以及 .git / __pycache__ ；
    - 本地回滚副本目录（名字以 _backup_before 开头，见 README 的备份约定）；
    - 二进制 / 媒体 / 产物类扩展名（dll、ofx、exe、bundle、bin、raw、mp4 ……）。
* 排除清单写在下面的常量里，一眼可查。

用法
----
    python gen_filelist.py
输出 UTF-8（无 BOM）的 FILELIST.txt，按相对路径升序排列。
"""

import hashlib
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "FILELIST.txt")

# bundle/ 是 install.bat 的打包中间产物（含引擎 DLL 副本），与 out/ obj/ 同类，
# 不该进清单。_locked残留备份* 是删 .locked-* 前的临时备份（约 139 MB）。
EXCLUDE_DIR_NAMES = {"out", "obj", "bundle", ".git", "__pycache__", ".vs", "_archive"}
# 历史遗留目录（脚本版 / Lua 版 / 旧模型数据）现已归入 _archive/ 内，已被
# EXCLUDE_DIR_NAMES 整体排除，无需再用 03_/04_/05_ 前缀排除——否则会误伤重命名后的
# 活动目录 03_测试夹具 / 04_验收工具 / 05_安装包。_dbgtest 是调试产物目录。清单只描述
# 「会被交付的东西」。
EXCLUDE_DIR_PREFIXES = ("_backup_before", "_locked残留备份", "_dbgtest")
# 一次性诊断叠加图（每次探针都会重生成），不算源码/文档。
# ★ 2026-10-31：工作区与交付包合一后，根目录多了三个**由 package.py 生成**的交付件，
#   它们不是源码，且 MANIFEST 与 FILELIST 互相引用会形成自引用哈希，故排除。
EXCLUDE_NAMES = {"_phase1_overlay.png", "_phase2_overlay.png", "probe_out.raw",
                 "MANIFEST.txt", "安装说明.txt", "stage_engine.bat"}
EXCLUDE_EXT = {
    ".raw", ".mp4", ".mov", ".avi", ".mkv",
    ".dll", ".ofx", ".exe", ".bundle", ".bin", ".sys",
    ".pdb", ".lib", ".exp", ".obj", ".ilk", ".res",
    ".log", ".zip", ".7z", ".rar", ".tmp",
}


def git_tracked():
    """返回 git 已跟踪的相对路径列表；不是 git 仓库时返回 None。"""
    if not os.path.isdir(os.path.join(HERE, ".git")):
        return None
    try:
        out = subprocess.run(
            ["git", "-c", "core.quotepath=false", "ls-files", "-z"],
            cwd=HERE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            check=True,
        ).stdout
    except (OSError, subprocess.CalledProcessError):
        return None
    return [p for p in out.decode("utf-8", "replace").split("\0") if p]


def keep(rel):
    """按排除规则判断某个相对路径是否收录。"""
    parts = rel.split("/")
    if any(p in EXCLUDE_DIR_NAMES for p in parts[:-1]):
        return False
    if any(p.startswith(EXCLUDE_DIR_PREFIXES) for p in parts[:-1]):
        return False
    if parts[-1] in EXCLUDE_NAMES:
        return False
    if os.path.splitext(parts[-1])[1].lower() in EXCLUDE_EXT:
        return False
    return rel != "FILELIST.txt"


def iter_files():
    tracked = git_tracked()
    if tracked is not None:
        for rel in sorted(tracked):
            if keep(rel) and os.path.isfile(os.path.join(HERE, rel)):
                yield rel, os.path.join(HERE, rel)
        return

    for dirpath, dirnames, filenames in os.walk(HERE):
        # 原地过滤目录名，避免 os.walk 继续下潜
        dirnames[:] = [
            d for d in dirnames
            if d not in EXCLUDE_DIR_NAMES
            and not d.startswith(EXCLUDE_DIR_PREFIXES)
        ]
        for name in filenames:
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, HERE).replace("\\", "/")
            if keep(rel):
                yield rel, full


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    entries = []
    total = 0
    for rel, full in iter_files():
        try:
            size = os.path.getsize(full)
        except OSError:
            continue
        entries.append((rel, size, sha256(full)))
        total += size
    entries.sort(key=lambda e: e[0])

    stamp = time.strftime("%Y-%m-%d %H:%M")
    lines = [
        "# 文件清单（SHA256）",
        "# 生成时间: %s CST" % stamp,
        "# 由 gen_filelist.py 生成；收录规则见脚本头部注释。",
        "# 文件数: %d  合计: %.1f KB" % (len(entries), total / 1024.0),
        "",
    ]
    for rel, size, digest in entries:
        lines.append("%s%10d  %s" % (digest, size, rel))
    lines.append("")

    # 原 FILELIST.txt 用 CRLF；保持一致的换行风格，避免整文件 diff。
    with open(OUT, "w", encoding="utf-8", newline="\r\n") as f:
        f.write("\n".join(lines))

    print("已写入 %s" % OUT)
    print("文件数: %d  合计: %.1f KB" % (len(entries), total / 1024.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
