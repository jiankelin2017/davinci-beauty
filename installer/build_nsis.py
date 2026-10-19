#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
NamaBeauty 安装包构建器（NSIS 版）。

产物：单个真正的 .exe（NamaBeauty_Setup_v1.3.exe），双击即装、一键卸载。

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
  2. 调 makensis 编译 namabeauty.nsi -> 单文件 exe

安装包**不**捆绑 Python。理由：
  * 安装/卸载逻辑完全由 NSIS 原生实现，不需要解释器；
  * Contents\\Win64 是 Resolve 的插件扫描目录，塞进一个 30 MB 的
    解释器既无必要，也可能干扰 OFX 扫描。
namabeauty_setup.py 保留为「无 GUI / 脚本化」场景的等价实现与调试工具。

用法
----
    python build_nsis.py
    python build_nsis.py --version 1.4
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
NSI = os.path.join(HERE, "namabeauty.nsi")
SETUP_PY = os.path.join(HERE, "namabeauty_setup.py")

# 引擎资源的默认来源：源码仓库里那份 bundle 是权威副本。
# （过去默认指向 C:\ProgramData\NamaBeauty\engine，但那个目录会被卸载器全清掉，
#   拿它当构建源会在「卸载后重构建」时失败。）
DEFAULT_ENGINE_DIR = os.path.normpath(os.path.join(
    HERE, "..", "01_ofx插件源码", "bundle", "NamaBeauty.ofx.bundle",
    "Contents", "Win64"))
# 已编译的插件二进制
DEFAULT_OFX_BIN = os.path.join(HERE, "..", "01_ofx插件源码", "out", "NamaBeauty.ofx")

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
    ("NamaBeauty.ofx", None),
    ("CNamaSDK.dll", None),
    ("fuai.dll", None),
    ("w32-pthreads.dll", None),
    ("api-ms-win-downlevel-shlwapi-l1-1-0.dll", None),
    ("auth_package.bin", None),
    ("ai_face_processor_pc.bundle", "model"),
    ("ai_human_processor_pc.bundle", "model"),
    ("face_beautification.bundle", "graphics"),
    ("face_makeup.bundle", "graphics"),
    ("body_slim.bundle", "graphics"),
    ("background_blur.bundle", "graphics"),
]


def collect_payload(stage, engine_dir, ofx_bin):
    """把引擎与插件按 bundle/Win64 的布局收进 stage。返回 (文件数, 字节数, 缺失)。"""
    nfiles = 0
    nbytes = 0
    missing = []

    for name, sub in PAYLOAD_PLAN:
        if sub is None:
            src = ofx_bin if name == "NamaBeauty.ofx" else os.path.join(engine_dir, name)
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


def collect_python(stage, python_exe):
    """（已弃用）把便携 Python 收进 stage/python。

    保留这个函数只是为了兼容旧调用；现在安装包不再捆绑解释器。
    Contents\\Win64 是 Resolve 的插件扫描目录，不该放无关文件。
    """
    return 0, 0


# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description="构建 NamaBeauty 一键安装包（NSIS）")
    ap.add_argument("--engine", default=DEFAULT_ENGINE_DIR, help="引擎资源目录")
    ap.add_argument("--ofx", default=os.path.normpath(DEFAULT_OFX_BIN), help="插件二进制")
    ap.add_argument("--version", default="1.3", help="版本号")
    ap.add_argument("--out", default=None, help="输出 exe 路径")
    args = ap.parse_args()

    print("")
    print("=" * 62)
    print("  NamaBeauty 安装包构建器 (NSIS)")
    print("=" * 62)
    print("")

    mk = find_makensis()
    if not mk:
        print("  [XX] 找不到 makensis.exe")
        print("       安装：winget install --id NSIS.NSIS -e")
        return 1
    print("  makensis: %s" % mk)
    print("")

    if not os.path.isfile(NSI):
        print("  [XX] 找不到 %s" % NSI)
        return 1
    if not os.path.isfile(args.ofx):
        print("  [XX] 找不到插件二进制: %s" % args.ofx)
        print("       请先跑 01_ofx插件源码\\build.bat")
        return 1
    if not os.path.isdir(args.engine):
        print("  [XX] 找不到引擎目录: %s" % args.engine)
        return 1

    out_exe = args.out or os.path.join(HERE, "build",
                                       "NamaBeauty_Setup_v%s.exe" % args.version)
    os.makedirs(os.path.dirname(os.path.abspath(out_exe)), exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="nama_nsis_") as stage:
        # ---- 1. 资源 ----
        print("[1/2] 收集插件与引擎资源 ...")
        nf, nb, missing = collect_payload(stage, args.engine, args.ofx)
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
            "/DVERSION=%s" % args.version,
            "/DVI_VERSION=%s" % vi_product_version(args.version),
            "/DOUTFILE=%s" % os.path.abspath(out_exe),
            NSI,
        ]
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
    print("  卸载：控制面板「应用和功能」里找「NamaBeauty 美颜」，")
    print("        或运行 %s --uninstall" % os.path.basename(out_exe))
    print("")
    return 0


if __name__ == "__main__":
    sys.exit(main())
