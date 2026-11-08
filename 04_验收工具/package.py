"""生成 DaVinci Beauty 整合包（源码 + 文档 + 工具 + 引擎资产）。

为什么脚本化而不是手动 copy：
同一份源码手工拷第三次就会拷漏，而且「哪些该进包」是一次性决策、
写下来才有延续性。这里显式列出 COPY_DIRS / 排除规则，改 composition 只改脚本。

用法：
    python package.py                 # 生成到 D:\\插件\\DaVinciBeauty_v<版本>_整合包
    python package.py --zip           # 额外打一个同名 .zip
    python package.py --force         # 目标已存在时覆盖
    python package.py --no-engine     # 不带引擎资产（约 11MB），只出轻包
"""
import hashlib
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
SRC_H = os.path.join(REPO, "01_ofx插件源码", "src", "beauty_engine.h")

# 引擎资产候选（按顺序取第一个有效的）。仓库自带那份优先 —— 打包要自包含，
# 不能依赖 ProgramData 是否 stage 过；装机 bundle 作为最后的兜底。
ENGINE_CANDIDATES = [
    os.path.join(REPO, "02_引擎资产"),
    r"C:\ProgramData\DaVinciBeauty\engine",
    os.path.join(os.environ.get("COMMONPROGRAMFILES", r"C:\Program Files\Common Files"),
                 "OFX", "Plugins", "DaVinciBeauty.ofx.bundle", "Contents", "Win64"),
]


def resolve_engine_src():
    """返回第一个确实含引擎的目录；全都没有则返回 None（届时出不带引擎的轻包）。

    v3.0.0 起引擎是自研 CoreEngine + ONNX Runtime，目录标记是 onnxruntime.dll
    （以及人脸模型 yunet.onnx）。
    """
    for d in ENGINE_CANDIDATES:
        if os.path.isdir(d) and os.path.isfile(os.path.join(d, "onnxruntime.dll")):
            return d
    return None

# 顶层单文件
# ★ 2026-11-08 文档治理：开发指南/开发文档/PROGRESS 全部迁到 docs/，由 COPY_DIRS 整目录带走，
#   这里不再逐个列。验收报告.txt 改为每次验收现场生成（不存当前版）；安装说明.txt 由本脚本
#   在包内生成，仓库里不再留一份会过期的副本。
TOP_FILES = ["README.md", "说明.txt", "FILELIST.txt", ".gitignore",
             ".gitattributes", "gen_filelist.py",
             "诊断_抓真实帧.bat"]

# 要进包的目录
COPY_DIRS = ["01_ofx插件源码", "docs", "03_测试夹具", "04_验收工具", "05_安装包"]

# 按目录名排除（不管在哪一层）
EXCLUDE_DIRS = {"__pycache__", ".git", ".vs", "out", "obj", "bundle",
                "_backup_before_v2.0", "旧备份"}
# 按文件名后缀排除
EXCLUDE_SUFFIX = (".pyc", ".lock", ".bak", ".rar", ".zip", ".7z", ".obj", ".exp", ".lib")
# 带 FNM 风格的关键字排除
# ★ 2026-10-31：去掉 "验收报告_" —— 04_验收工具 里的历史验收报告（v2.3.0/v2.3.1/v3.0.8）
#   一直在 FILELIST.txt 里，却被这条规则挡在包外，清单与实物对不上。
EXCLUDE_KEY = (".locked-", ".bak_", "_inance")
# 按文件名排除：探针输出 / 一次性叠加图。**probe_in.raw 必须进包** —— 它是
# mock_host.py 的输入帧，缺了会退化成合成渐变（无皮肤），8bit/float/B5 全 FAIL。
# 以前一条 ".raw" 后缀规则把输入和输出一起排掉了，包内自测等于跑不起来。
EXCLUDE_NAMES = ("probe_out.raw", "_phase1_overlay.png", "_phase2_overlay.png")


def version():
    txt = open(SRC_H, encoding="utf-8", errors="replace").read()
    m = re.search(r'#define\s+BEAUTY_BUILD_STR\s+"([^"]+)"', txt)
    return m.group(1) if m else "unknown"


def wanted(path, name):
    """单个文件要不要进包。"""
    if any(k in name for k in EXCLUDE_KEY):
        return False
    if name.endswith(EXCLUDE_SUFFIX):
        return False
    if name in EXCLUDE_NAMES:
        return False
    return True


def sha256(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for c in iter(lambda: f.read(1 << 20), b""):
            h.update(c)
    return h.hexdigest()


def human(n):
    for u in ("B", "KB", "MB", "GB"):
        if n < 1024 or u == "GB":
            return "%.1f%s" % (n, u)
        n /= 1024.0


def copy_tree(src, dst, log):
    n, size = 0, 0
    for root, dirs, files in os.walk(src):
        dirs[:] = [d for d in dirs if d not in EXCLUDE_DIRS]
        rel = os.path.relpath(root, src)
        for f in files:
            sp = os.path.join(root, f)
            if not wanted(sp, f):
                continue
            tp = os.path.join(dst, rel, f) if rel != "." else os.path.join(dst, f)
            os.makedirs(os.path.dirname(tp), exist_ok=True)
            shutil.copy2(sp, tp)
            n += 1
            size += os.path.getsize(tp)
            if log:
                print("      %s" % os.path.relpath(tp, dst))
    return n, size


def main():
    args = sys.argv[1:]
    ver = version()
    dest_parent = os.path.dirname(REPO)          # D:\\插件
    # --name 自定义目录名（默认 DaVinciBeauty_v<版本>_整合包）。交付到「新项目文件夹」
    # 时用它，让目录名表达交付批次，而不是只跟源码版本绑死。
    name = None
    if "--name" in args:
        name = args[args.index("--name") + 1]
    # --setup <exe>：把已构建好的 NSIS 安装包一并放进交付目录根。
    # 安装包体积约 5MB（引擎压缩后），默认不带；显式给出才收，避免旧包混进来。
    setup_exe = None
    if "--setup" in args:
        setup_exe = args[args.index("--setup") + 1]
    dest = os.path.join(dest_parent, name or ("DaVinciBeauty_v%s_整合包" % ver))
    # ★ 2026-10-31 整合后：本仓库自己就叫「DaVinciBeauty_整合_20261031」，
    #   工作区与交付包已经合一。若目标目录 == 源仓库，MANIFEST 的 os.walk 会把
    #   .git（约 148MB）、_dbgtest、本地归档目录全哈希进清单，
    #   而 --force 更会把仓库整个删掉重来。两种都不允许，直接拒绝。
    if os.path.abspath(dest) == os.path.abspath(REPO):
        print("目标目录就是源仓库本身（工作区与交付包已合一），拒绝原地打包。")
        print("  仓库: %s" % REPO)
        print("  请用 --name 指定独立目录名，例如：--name DaVinciBeauty_v%s_交付包" % ver)
        return 1
    ENGINE_SRC = resolve_engine_src()
    with_engine = "--no-engine" not in args and ENGINE_SRC is not None

    if "--zip-only" in args:
        zp = dest + ".zip"
        if not os.path.isdir(dest):
            print("没有现成的包目录可打包：%s" % dest)
            return 1
        print("仅打包（不重建）：%s" % dest)
        shutil.make_archive(dest, "zip", dest_parent, os.path.basename(dest))
        print("已生成: %s（%s）" % (zp, human(os.path.getsize(zp))))
        return 0

    if os.path.exists(dest):
        if "--force" in args:
            shutil.rmtree(dest)
        elif "--update" in args:
            # 原地更新：只覆盖同步文件，不删整棵树（避免出现几十 MB 的批量删除）
            print("原地更新已有包目录（不同步删除，残留的旧文件请自行清理）")
        else:
            print("目标已存在。可选：--update 原地同步 / --force 重建 / --zip-only 直接打包")
            return 1
    os.makedirs(dest, exist_ok=True)

    print("=" * 68)
    print("DaVinci Beauty 整合包 v%s" % ver)
    print("源 : %s" % REPO)
    print("目标: %s" % dest)
    print("引擎资产: %s" % ("含（%s）" % ENGINE_SRC if with_engine else "不含"))
    print("=" * 68)

    total_n, total_size = 0, 0

    for f in TOP_FILES:
        sp = os.path.join(REPO, f)
        if os.path.isfile(sp):
            shutil.copy2(sp, os.path.join(dest, f))
            total_n += 1
            total_size += os.path.getsize(sp)
            print("  顶层  %s" % f)

    for d in COPY_DIRS:
        sp = os.path.join(REPO, d)
        if not os.path.isdir(sp):
            continue
        print("  目录  %s" % d)
        n, size = copy_tree(sp, os.path.join(dest, d), log=False)
        total_n += n
        total_size += size
        print("        %d 个文件，%s" % (n, human(size)))

    if with_engine:
        print("  引擎资产（较大，请稍候）")
        n, size = copy_tree(ENGINE_SRC, os.path.join(dest, "02_引擎资产"), log=False)
        total_n += n
        total_size += size
        print("        %d 个文件，%s" % (n, human(size)))

    if setup_exe:
        if not os.path.isfile(setup_exe):
            print("  [!!] --setup 指定的文件不存在：%s" % setup_exe)
            return 1
        dst_exe = os.path.join(dest, os.path.basename(setup_exe))
        shutil.copy2(setup_exe, dst_exe)
        total_n += 1
        total_size += os.path.getsize(dst_exe)
        print("  安装包  %s（%.1f MB）"
              % (os.path.basename(dst_exe), os.path.getsize(dst_exe) / 1048576.0))

    # ---- stage_engine.bat：换机时把引擎放回 install.bat 认的路径 ----
    # 编码/换行都有讲究：批处理含中文必须以 **GBK + CRLF** 落盘（cmd 按 OEM 代码页
    # 解析 .bat；UTF-8 中文会把行边界吃掉）。另外 newline 必须为 "" —— 用
    # newline="\r\n" 再写 "\r\n" 会得到 \r\r\n（Python 会把 "\n" 再翻一次）。
    bat = os.path.join(dest, "stage_engine.bat")
    with open(bat, "w", encoding="gbk", newline="") as f:
        f.write("@echo off\r\n")
        f.write("rem 把整合包自带的引擎资产放回 install.bat 认的位置。\r\n")
        f.write("rem 换到新机器、或 ProgramData 引擎被误删后先跑它，再跑 install.bat。\r\n")
        f.write('setlocal\r\ncd /d "%~dp0"\r\n')
        f.write('xcopy /y /e /i "02_引擎资产" "C:\\ProgramData\\DaVinciBeauty\\engine" >nul\r\n')
        f.write('if errorlevel 1 (\r\n    echo [ERROR] stage failed\r\n    exit /b 1\r\n)\r\n')
        f.write("echo [OK] engine staged to C:\\ProgramData\\DaVinciBeauty\\engine\r\n")
        f.write("echo next: 01_ofx插件源码\\install.bat\r\n")

    # ---- 安装说明 ----
    readme = os.path.join(dest, "安装说明.txt")
    with open(readme, "w", encoding="utf-8-sig", newline="") as f:
        f.write("DaVinci Beauty 整合包 v%s\r\n" % ver)
        f.write("=" * 60 + "\r\n\r\n")
        f.write("【三步装好】\r\n")
        f.write("  1. （可选）双击 stage_engine.bat —— 把引擎另存一份到 ProgramData\r\n")
        f.write("     install.bat 直接用包内 02_引擎资产，本步只给旧流程/换机兜底\r\n")
        f.write("  2. 先完全退出 DaVinci Resolve，再双击 01_ofx插件源码\\install.bat\r\n")
        f.write("     不退出会让被占用的 DLL 改名成 .locked-* 留存，攒多了是纯垃圾\r\n")
        f.write("  3. 重启 Resolve：Effects Library > OpenFX > DaVinci Beauty\r\n\r\n")
        if setup_exe:
            f.write("【一键安装包（含卸载器）】\r\n")
            f.write("  根目录 %s\r\n" % os.path.basename(setup_exe))
            f.write("  双击安装（会请求管理员权限）；卸载走「应用和功能」里\r\n")
            f.write("  「DaVinci Beauty 美颜」，或运行同目录的卸载器：\r\n")
            f.write("    \"...\\DaVinciBeauty.ofx.bundle\\Uninstall_DaVinciBeauty.exe\" /S\r\n")
            f.write("  静默安装：%s /S\r\n" % os.path.basename(setup_exe))
            f.write("  静默卸载：%s --uninstall /S\r\n\r\n" % os.path.basename(setup_exe))
        f.write("【重新编译（可选）】\r\n")
        f.write("  01_ofx插件源码\\build.bat —— 薄壳，实际编译在 build.ps1\r\n")
        f.write("  需要 VS C++ 工具链 + Windows SDK；脚本自动探测，不写死路径\r\n\r\n")
        f.write("【验收】\r\n")
        f.write("  04_验收工具\\verify.bat   一键验收（编译/装机/回归/接口/工程一致性）\r\n")
        f.write("  期望：28 PASS / 0 FAIL，逐字节回归 18/18、mock host 23 项\r\n")
        f.write("  本包自带 03_测试夹具\\probe_in.raw（1MB），装机前后都能自测\r\n\r\n")
        f.write("【目录】\r\n")
        f.write("  01_ofx插件源码\\   源码 + build.ps1 / build.bat / install.bat\r\n")
        f.write("  docs\\             文档体系，先看 docs\\INDEX.md\r\n")
        f.write("  docs\\guides\\      装了怎么用 / 怎么开发 / 怎么验收 / 怎么出包\r\n")
        f.write("  docs\\reference\\   架构与关键技术事实、性能实测\r\n")
        f.write("  docs\\CHANGELOG.md 每个版本用户能看出来的变化\r\n")
        f.write("  03_测试夹具\\   无宿主契约测试与自测链路\r\n")
        f.write("  04_验收工具\\      一键验收 + 回归基线 + 性能 AB + 打包工具\r\n")
        f.write("  05_安装包\\        NSIS 安装包构建脚本（需本机装 NSIS）\r\n")
        f.write("  02_引擎资产\\      %s\r\n" % ("整合包自带（build/install 直接用它）" if with_engine else "不在本包内"))
        f.write("  MANIFEST.txt      包内每个文件的 sha256\r\n\r\n")
        f.write("【注意】\r\n")
        f.write("  - 插件 ID com.nama.davinci.beauty 不要改：Resolve 按 ID 找效果，\r\n")
        f.write("    改了历史工程里已挂的效果会失效。\r\n")
        f.write("  - 分发时 LICENSE 与 THIRD-PARTY-NOTICES.md 必须一起给出（GPL-2.0-only）。\r\n")

    # ---- 清单 ----
    lines = []
    for root, dirs, files in os.walk(dest):
        dirs[:] = [d for d in dirs if d != "__pycache__"]
        for fn in sorted(files):
            p = os.path.join(root, fn)
            if os.path.abspath(p).startswith(os.path.abspath(dest)) and \
               os.path.basename(p) in ("MANIFEST.txt",):
                continue
            lines.append("%s  %s" % (sha256(p), os.path.relpath(p, dest)))
    with open(os.path.join(dest, "MANIFEST.txt"), "w", encoding="utf-8",
              newline="") as f:
        f.write("# DaVinci Beauty 整合包 v%s 文件清单（sha256）\r\n" % ver)
        f.write("# 生成时间: %s\r\n" % __import__("time").strftime("%Y-%m-%d %H:%M:%S"))
        f.write("# 文件数: %d\r\n\r\n" % len(lines))
        f.write("\r\n".join(lines) + "\r\n")

    print("\n" + "=" * 68)
    print("整包: %d 个文件，%s" % (total_n, human(total_size)))
    print("输出: %s" % dest)

    if "--zip" in args:
        zp = dest + ".zip"
        print("打包 zip（请稍候）...")
        shutil.make_archive(dest, "zip", dest_parent,
                            os.path.basename(dest))
        print("已生成: %s（%s）" % (zp, human(os.path.getsize(zp))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
