#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
DaVinci Beauty 安装包构建器（NSIS 版）。

产物：单个真正的 .exe（DaVinciBeauty_Setup_v<版本>.exe），双击即装、一键卸载。
版本号默认从 src\nama_engine.h 的 NAMA_BUILD_* 宏自动读取。

为什么是 NSIS
-------------
本机工具链排查结论：
  * NSIS / 7-Zip 原本没有  ->  已用 winget 装上 NSIS 3.13
  * .NET 的 csc.exe 被安全策略禁用（连命令行出现这几个字母都被拦）
  * dotnet 只装了运行时、没有 SDK，不能 dotnet build
  * iexpress.exe 存在但 SED 配置调不通（静默失败）

NSIS 是唯一能产出**真正的 PE 单文件 exe**、且自带标准安装/卸载界面的工具。
它的产物是货真价实的 exe，不会被 Windows 的 PE 加载器拒绝。

流程
----
  1. 把资源（插件 + 引擎 + 模型）按 bundle/Win64 布局 stage 到临时目录
  2. 调 makensis 编译 davincibeauty.nsi -> 单文件 exe

安装包**不**捆绑 Python。理由：
  * 安装/卸载逻辑完全由 NSIS 原生实现，不需要解释器；
  * Contents\\Win64 是 Resolve 的插件扫描目录，塞进一个 30 MB 的
    解释器既无必要，也可能干扰 OFX 扫描。
davincibeauty_setup.py 保留为「无 GUI / 脚本化」场景的等价实现与调试工具。

用法
----
    python build_nsis.py
    python build_nsis.py --version 1.4
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
NSI = os.path.join(HERE, "davincibeauty.nsi")
SETUP_PY = os.path.join(HERE, "davincibeauty_setup.py")

# 版本号的唯一出处是源码头的 NAMA_BUILD_* 宏。
# 本脚本不再维护第二份版本号 —— 历史上 --version 默认写死 "1.4"，
# 导致产物名（DaVinciBeauty_Setup_v1.4.exe）与实际版本长期脱节。
SRC_HEADER = os.path.normpath(os.path.join(
    HERE, "..", "01_ofx插件源码", "src", "nama_engine.h"))

# 引擎资源的候选来源，按优先级回落。
# 说明：仓库里的 bundle/ 是 install.bat 的 stage 中间产物，会被 cleanup.py 判为
# 构建垃圾清掉，所以它必须排在后面且作为「可选」；
# C:\ProgramData\DaVinciBeauty\engine 虽然会被卸载器清空，但在卸载之前是可用源。
ENGINE_CANDIDATES = [
    # 仓库自带的一份，让打包在本机自包含：不再依赖 ProgramData 是否已 stage
    os.path.normpath(os.path.join(HERE, "..", "02_引擎资产")),
    os.path.normpath(r"C:\ProgramData\DaVinciBeauty\engine"),
    os.path.normpath(os.path.join(
        HERE, "..", "01_ofx插件源码", "bundle", "DaVinciBeauty.ofx.bundle",
        "Contents", "Win64")),
    os.path.normpath(os.path.join(
        os.environ.get("COMMONPROGRAMFILES", r"C:\Program Files\Common Files"),
        "OFX", "Plugins", "DaVinciBeauty.ofx.bundle", "Contents", "Win64")),
]
DEFAULT_ENGINE_DIR = ENGINE_CANDIDATES[0]
# 已编译的插件二进制
DEFAULT_OFX_BIN = os.path.join(HERE, "..", "01_ofx插件源码", "out", "DaVinciBeauty.ofx")

MAKENSIS_CANDIDATES = [
    r"C:\Program Files (x86)\NSIS\makensis.exe",
    r"C:\Program Files\NSIS\makensis.exe",
    os.path.join(os.environ.get("LOCALAPPDATA", ""), "Programs", "NSIS", "makensis.exe"),
]


def find_makensis():
    for p in MAKENSIS_CANDIDATES:
        if os.path.isfile(p):
            return p
    # 兜底：PATH 上找
    w = shutil.which("makensis")
    return w


def vi_product_version(v):
    """把 --version（如 1.3 / 1.3.1）规整成 NSIS VIProductVersion 需要的四段 x.x.x.x。"""
    nums = []
    for part in str(v).replace("-", ".").split("."):
        if part.isdigit():
            nums.append(int(part))
    nums = nums[:4]
    while len(nums) < 4:
        nums.append(0)
    return ".".join(str(n) for n in nums)


def ensure_utf8_bom(path):
    """`Unicode true` 的 NSIS 脚本必须是 UTF-8 带 BOM，否则 makensis 报
    "Bad text encoding"。这里在编译前统一补上，保证可重复构建。"""
    with open(path, "rb") as f:
        data = f.read()
    if data.startswith(b"\xef\xbb\xbf"):
        return False
    with open(path, "wb") as f:
        f.write(b"\xef\xbb\xbf" + data)
    return True


# ---------------------------------------------------------------------------
# 资源收集
# ---------------------------------------------------------------------------
PAYLOAD_PLAN = [
    ("DaVinciBeauty.ofx", None),
    # 自研引擎（OBS CoreEngine + ONNX YuNet）的全部运行时资产：
    # ONNX Runtime 的 CPU 构建版 DLL，以及本地人脸检测模型。不再依赖任何闭源 SDK。
    ("onnxruntime.dll", None),
    ("yunet.onnx", None),
]


def collect_payload(stage, engine_dir, ofx_bin):
    """把引擎与插件按 bundle/Win64 的布局收进 stage。返回 (文件数, 字节数, 缺失)。"""
    nfiles = 0
    nbytes = 0
    missing = []

    for name, sub in PAYLOAD_PLAN:
        if sub is None:
            src = ofx_bin if name == "DaVinciBeauty.ofx" else os.path.join(engine_dir, name)
            dst = os.path.join(stage, name)
        else:
            src = os.path.join(engine_dir, sub, name)
            dst = os.path.join(stage, sub, name)
        if not os.path.isfile(src):
            missing.append(src)
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(src, dst)
        nfiles += 1
        nbytes += os.path.getsize(src)

    return nfiles, nbytes, missing


def read_source_version():
    """从 nama_engine.h 的 NAMA_BUILD_* 宏读版本号，让版本号只有一个来源。"""
    try:
        with open(SRC_HEADER, encoding="utf-8") as f:
            txt = f.read()
    except OSError:
        return None

    def macro(name):
        m = re.search(r"#define\s+%s\s+(\d+)\b" % name, txt)
        return int(m.group(1)) if m else None

    major, minor = macro("NAMA_BUILD_MAJOR"), macro("NAMA_BUILD_MINOR")
    if major is None or minor is None:
        return None
    return "%d.%d.%d" % (major, minor, macro("NAMA_BUILD_PATCH") or 0)


def resolve_engine_dir(explicit=None):
    """按候选列表找出第一个同时具备核心引擎文件的目录。"""
    cands = [explicit] if explicit else []
    cands += ENGINE_CANDIDATES
    for c in cands:
        if not c or not os.path.isdir(c):
            continue
        if os.path.isfile(os.path.join(c, "onnxruntime.dll")) \
                and os.path.isfile(os.path.join(c, "yunet.onnx")):
            return c
    return None


# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description="构建 DaVinci Beauty 一键安装包（NSIS）")
    ap.add_argument("--engine", default=DEFAULT_ENGINE_DIR, help="引擎资源目录")
    ap.add_argument("--ofx", default=os.path.normpath(DEFAULT_OFX_BIN), help="插件二进制")
    ap.add_argument("--version", default=None,
                    help="版本号（默认从 nama_engine.h 的 NAMA_BUILD_* 宏读取）")
    ap.add_argument("--out", default=None, help="输出 exe 路径")
    ap.add_argument("--sandbox", action="store_true",
                    help="沙箱构建：用隔离的 APPID / bundle 名安装到旁处，"
                         "用来验证安装与卸载而不影响正式装机")
    args = ap.parse_args()

    print("")
    print("=" * 62)
    print("  DaVinci Beauty 安装包构建器 (NSIS)")
    print("=" * 62)
    print("")

    mk = find_makensis()
    if not mk:
        print("  [XX] 找不到 makensis.exe")
        print("       安装：winget install --id NSIS.NSIS -e")
        return 1
    print("  makensis: %s" % mk)

    version = args.version or read_source_version()
    if not version:
        print("  [XX] 无法确定版本号：既没传 --version，也读不到 %s" % SRC_HEADER)
        return 1
    print("  版本号 : %s%s" % (version, "（显式指定）" if args.version else "（源码宏自动）"))
    print("")

    if not os.path.isfile(NSI):
        print("  [XX] 找不到 %s" % NSI)
        return 1
    if not os.path.isfile(args.ofx):
        print("  [XX] 找不到插件二进制: %s" % args.ofx)
        print("       请先跑 01_ofx插件源码\\build.bat")
        return 1
    engine_dir = resolve_engine_dir(args.engine if args.engine != DEFAULT_ENGINE_DIR else None)
    if not engine_dir:
        print("  [XX] 找不到引擎资源目录，以下候选都不可用：")
        for c in ENGINE_CANDIDATES:
            print("       %s%s" % (c, "" if os.path.isdir(c) else "（不存在）"))
        print("       可用 --engine <目录> 指定；或先跑 install.bat 重建 stage。")
        return 1
    print("  引擎源 : %s" % engine_dir)
    print("")

    # 沙箱包必须与正式包**不同名**。曾因同名同路径，正式包覆盖掉沙箱包后
    # 验证脚本拿正式包当沙箱包装了一遍，把正式安装刷了 —— 检查全报「缺文件」，
    # 因为查的是 Sandbox 路径而包装的是正式路径。
    suffix = "_sandbox" if args.sandbox else ""
    out_exe = args.out or os.path.join(
        HERE, "build", "DaVinciBeauty_Setup_v%s%s.exe" % (version, suffix))
    os.makedirs(os.path.dirname(os.path.abspath(out_exe)), exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="nama_nsis_") as stage:
        # ---- 1. 资源 ----
        print("[1/2] 收集插件与引擎资源 ...")
        nf, nb, missing = collect_payload(stage, engine_dir, args.ofx)
        print("       %d 个文件，%.1f MB" % (nf, nb / 1048576.0))
        if missing:
            print("  [!!] 以下资源缺失，无法继续：")
            for m in missing:
                print("       " + m)
            return 1

        # ---- 2. makensis ----
        print("[2/2] 编译安装包（makensis）...")
        ensure_utf8_bom(NSI)
        cmd = [
            mk,
            "/V2",
            "/DSTAGE=%s" % stage,
            "/DVERSION=%s" % version,
            "/DVI_VERSION=%s" % vi_product_version(version),
            "/DOUTFILE=%s" % os.path.abspath(out_exe),
        ]
        if args.sandbox:
            # 隔离 ID：卸载 / 注册表 / bundle 目录全部走旁的具体情况
            cmd += ["/DAPPID=DaVinciBeauty.SandboxTest",
                    "/DBUNDLE_NAME=DaVinciBeautySandbox.ofx.bundle"]
            print("       沙箱模式：APPID=DaVinciBeauty.SandboxTest")
        cmd.append(NSI)
        r = subprocess.run(cmd, capture_output=True, text=True,
                           encoding="mbcs", errors="replace",
                           cwd=os.path.dirname(NSI), timeout=1800)
        out = (r.stdout or "").strip()
        err = (r.stderr or "").strip()
        if out:
            for line in out.splitlines():
                print("       " + line)
        if r.returncode != 0:
            print("  [XX] makensis 失败 (rc=%d)" % r.returncode)
            if err:
                for line in err.splitlines():
                    print("       " + line)
            return 1

    if not os.path.isfile(out_exe):
        print("  [XX] 编译成功但没找到产物：%s" % out_exe)
        return 1

    size = os.path.getsize(out_exe)
    print("")
    print("=" * 62)
    print("  完成：%s" % out_exe)
    print("  体积：%.1f MB" % (size / 1048576.0))
    print("=" * 62)
    print("")
    print("  双击安装（会自动请求管理员权限）。")
    print("  卸载：控制面板「应用和功能」里找「DaVinci Beauty 美颜」，")
    print("        或运行 %s --uninstall" % os.path.basename(out_exe))
    print("")
    return 0


if __name__ == "__main__":
    sys.exit(main())
