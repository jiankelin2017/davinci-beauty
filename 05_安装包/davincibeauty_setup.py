#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
DaVinci Beauty 安装器 —— 业务逻辑层。

这份模块被两种方式使用：
  1. 打包时：build_exe.py 把它和资源一起塞进自解压 exe；
  2. 调试时：直接 `python davincibeauty_setup.py install --from <资源目录>` 跑。

设计上刻意与「自解压外壳」解耦：本模块只认「一个装着资源的目录」，
不关心这个目录是 exe 自己解出来的还是手边现成的。
"""

import ctypes
import os
import shutil
import subprocess
import sys
import time
from ctypes import wintypes

# ---------------------------------------------------------------------------
# 常量
# ---------------------------------------------------------------------------
PLUGIN_ID = "com.nama.davinci.beauty"
PLUGIN_LABEL = "DaVinci Beauty 美颜"
BUNDLE_NAME = "DaVinciBeauty.ofx.bundle"

# bundle 内需要安装的文件清单（相对 Contents/Win64）
# v3.0.0 起引擎为自研 上一代宿主 CoreEngine + ONNX Runtime，仅两个运行时资产：
# onnxruntime.dll（CPU 构建版，启用 GPU 时换成 onnxruntime-directml 构建版）
# 与本地人脸模型 yunet.onnx。不再依赖任何闭源 旧版闭源 SDK 文件。
PAYLOAD_FILES = [
    "DaVinciBeauty.ofx",
    "onnxruntime.dll",
    "yunet.onnx",
]

# 安装时必须逐项校验存在的最小集合（核心文件，缺一不可）
CRITICAL_FILES = [
    "DaVinciBeauty.ofx",
    "onnxruntime.dll",
    "yunet.onnx",
]

PROGRAMDATA_DIR = r"C:\ProgramData\DaVinciBeauty"
LOGS_DIR = os.path.join(PROGRAMDATA_DIR, "logs")
DISABLED_SCRIPTS_DIR = os.path.join(PROGRAMDATA_DIR, "disabled_scripts")
ENGINE_CACHE_DIR = os.path.join(PROGRAMDATA_DIR, "engine")

# Resolve 的 OFX 扫描缓存：改过插件后必须让它失效，否则认不出新二进制
RESOLVE_CACHE = os.path.join(
    os.environ.get("APPDATA", ""),
    "Blackmagic Design", "DaVinci Resolve", "Support", "OFXPluginCacheV2.xml",
)

GREEN = "\033[92m"
YELLOW = "\033[93m"
RED = "\033[91m"
CYAN = "\033[96m"
DIM = "\033[2m"
BOLD = "\033[1m"
RESET = "\033[0m"


def _enable_ansi():
    """让 Windows 控制台认 ANSI 颜色转义。失败就算了，不致命。"""
    try:
        k = ctypes.windll.kernel32
        h = k.GetStdHandle(-11)
        mode = wintypes.DWORD()
        if k.GetConsoleMode(h, ctypes.byref(mode)):
            k.SetConsoleMode(h, mode.value | 0x0004)  # ENABLE_VIRTUAL_TERMINAL_PROCESSING
    except Exception:
        pass


def _no_color():
    return bool(os.environ.get("NO_COLOR")) or bool(os.environ.get("NAMA_SETUP_NO_COLOR"))


def c(text, color):
    if _no_color():
        return text
    return color + text + RESET


# ---------------------------------------------------------------------------
# 输出小工具
# ---------------------------------------------------------------------------
def say(msg=""):
    print(msg)
    sys.stdout.flush()


def ok(msg):
    say("  " + c("[ok]", GREEN) + "  " + msg)


def warn(msg):
    say("  " + c("[!!]", YELLOW) + "  " + msg)


def fail(msg):
    say("  " + c("[XX]", RED) + "  " + msg)


def step(n, total, msg):
    say("")
    say(c("[%d/%d] " % (n, total), CYAN) + c(msg, BOLD))


def hr():
    say(c("-" * 62, DIM))


# ---------------------------------------------------------------------------
# 权限
# ---------------------------------------------------------------------------
def is_admin():
    try:
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except Exception:
        return False


def relaunch_as_admin(extra_args=None):
    """用 UAC 重新拉起自己，返回 True 表示已成功发起提权。

    关键点：提权后是一个**全新的进程**，原来的参数一个都不会带过去。
    所以调用方必须把「资源在哪」显式传进来（extra_args），否则提权后的实例
    会自己重新解压一遍 —— 又慢又可能因为原临时目录被清理而彻底失败。

    另外坚持用 ShellExecuteW 的 "runas"，它是弹 UAC 的唯一正路；
    返回值 >32 表示 ShellExecute 成功「发起」了（不代表用户点了是）。
    """
    if is_admin():
        return False
    if getattr(sys, "frozen", False):
        exe = sys.executable
        args = list(extra_args or [])
    else:
        exe = sys.executable
        args = [os.path.abspath(__file__)] + list(extra_args or [])

    # Windows 命令行引号规则：参数含空格时用双引号包住
    params = " ".join('"%s"' % a for a in args)
    try:
        rc = ctypes.windll.shell32.ShellExecuteW(None, "runas", exe, params, None, 1)
        return rc > 32
    except Exception:
        return False


# ---------------------------------------------------------------------------
# Resolve 进程检测
# ---------------------------------------------------------------------------
def find_resolve_processes():
    """返回正在运行的 Resolve 相关进程名列表（空表示没在跑）。

    用 tasklist 而非 psutil：不想为一个检测引入第三方依赖。
    注意用 CREATE_NO_WINDOW，否则打包成 exe 后会闪一个黑框出来。
    """
    names = []
    try:
        out = subprocess.run(
            ["tasklist", "/FO", "CSV", "/NH"],
            capture_output=True, text=True, timeout=15,
            encoding="mbcs", errors="replace",
            creationflags=0x08000000,  # CREATE_NO_WINDOW
        ).stdout
    except Exception:
        return names  # 查不到就当没运行，后续靠替换失败兜底

    for line in out.splitlines():
        low = line.lower()
        if not any(tag in low for tag in ("resolve", "davinci", "fuscript", "fusion")):
            continue
        # CSV 首列带引号；进程名本身不含逗号，直接按逗号切第一段即可
        first = line.split(",", 1)[0].strip().strip('"')
        if first:
            names.append(first)
    return sorted(set(names))


# ---------------------------------------------------------------------------
# 文件操作
# ---------------------------------------------------------------------------
def copy_one(src, dst):
    """把一个文件复制到目标，目标是「被占用也能尽量装上去」。

    目标被占用时（Resolve 映射着 DLL），Windows 允许**重命名**已映射文件，
    所以先把旧文件改名到一边，再复制新的。这正是 install.bat 里那套策略。

    返回 (成功?, 原因) —— 原因取值：
      "ok"      复制成功
      "locked"  目标被占用（已把旧文件改名挪开但仍写不进去）
      "failed"  其它 IO 错误（磁盘满、路径非法等）

    只在**第一次**失败时才尝试改名挪开：如果能挪开，说明是占用问题，
    接下来立刻重试一次就够了 —— 反复 move 只会留下一堆 .locked-* 垃圾。
    """
    parent = os.path.dirname(dst)
    try:
        os.makedirs(parent, exist_ok=True)
    except OSError:
        return False, "failed"

    # 第一次尝试：直接覆盖
    try:
        shutil.copy2(src, dst)
        return True, "ok"
    except OSError:
        pass

    # 覆盖失败 —— 大概率是文件被占用。把它改名挪开再试。
    aside = "%s.locked-%d" % (dst, int(time.time() * 1000) % 1000000)
    try:
        os.replace(dst, aside)
    except OSError:
        # 连改名都做不到（或目标本来就不存在），那就真的写不进去
        return False, "locked"

    try:
        shutil.copy2(src, dst)
        return True, "ok"
    except OSError:
        return False, "locked"


def payload_source_root(payload_root):
    """自解压目录 -> 真正的 Win64 内容目录。

    打包时资源被放在 zip 的 Win64/ 下，解压后就是 <payload_root>/Win64。
    为了调试方便，如果调用方直接把 Win64 目录本身传进来，也认。
    """
    if os.path.isdir(os.path.join(payload_root, "Win64")):
        return os.path.join(payload_root, "Win64")
    return payload_root


def install_from(payload_root, target_win64, quiet=False, on_progress=None):
    """把 payload 里的文件装到 target_win64。返回 (ok, failed_list)。

    安装顺序刻意与 PAYLOAD_FILES 相反：先装「数据/模型」这类几乎不被占用的文件，
    最后才装宿主正在映射的 DLL 与插件本体。这样即使中途被占用，也已经把大部分
    内容就位了 —— 而不是一上来就卡在第一个被锁的文件上。

    on_progress(done, total, rel) 可选回调，供调用方显示进度。
    """
    src_root = payload_source_root(payload_root)
    if not os.path.isdir(src_root):
        fail("资源目录不存在: %s" % src_root)
        return False, ["<payload>"]

    # 反过来装：数据在前，宿主机体（DLL/.ofx）在后
    order = list(reversed(PAYLOAD_FILES))
    total = len(order)
    failed = []

    for i, rel in enumerate(order, 1):
        src = os.path.join(src_root, rel)
        dst = os.path.join(target_win64, rel)
        if on_progress:
            on_progress(i, total, rel)
        if not os.path.isfile(src):
            fail("资源缺失: %s" % rel)
            failed.append(rel)
            continue
        good, why = copy_one(src, dst)
        if good:
            if not quiet:
                say("       %s" % rel)
        else:
            reason = ("被占用，无法替换（请关闭 Resolve）" if why == "locked"
                      else "复制失败")
            fail("%s  -> %s" % (rel, reason))
            failed.append(rel)

    return (len(failed) == 0), failed


def clean_locked_leftovers(target_win64):
    """清掉历史安装留下的 .locked-* / .old_* 残渣。占用中的删不掉，忽略即可。"""
    removed = 0
    if not os.path.isdir(target_win64):
        return removed
    for root, _dirs, files in os.walk(target_win64):
        for fn in files:
            if ".locked-" in fn or ".old_" in fn:
                try:
                    os.remove(os.path.join(root, fn))
                    removed += 1
                except Exception:
                    pass
    return removed


# ---------------------------------------------------------------------------
# Resolve 缓存失效
# ---------------------------------------------------------------------------
def invalidate_resolve_cache():
    """删掉 Resolve 的 OFX 扫描缓存，强制它下次启动重新扫描。

    缓存以 mtime+size 判断是否失效；我们替换了二进制（size 变了）本来就会触发重扫，
    但删掉更保险 —— 尤其是卸载后不希望残影留在列表里。
    返回 True 表示确实删了一个存在的文件。
    """
    try:
        if os.path.isfile(RESOLVE_CACHE):
            os.remove(RESOLVE_CACHE)
            return True
    except Exception:
        pass
    return False


# ---------------------------------------------------------------------------
# 安装
# ---------------------------------------------------------------------------
def target_win64_path():
    common = os.environ.get("COMMONPROGRAMFILES") or r"C:\Program Files\Common Files"
    return os.path.join(common, "OFX", "Plugins", BUNDLE_NAME, "Contents", "Win64")


def do_install(payload_root, assume_yes=False):
    say("")
    hr()
    say(c("  DaVinci Beauty 美颜 —— DaVinci Resolve OpenFX 滤镜", BOLD))
    say(c("  一键安装", DIM))
    hr()

    if not is_admin():
        fail("需要管理员权限（要写入 Program Files）")
        say("")
        say("  正在请求提权 ...")
        # 把资源目录一并传过去，提权后的新进程直接复用，不必重新解压
        extra = ["--from", payload_root]
        if assume_yes:
            extra.append("--yes")
        if relaunch_as_admin(extra):
            say("  已发起提权，请在弹出的 UAC 窗口点「是」。")
            return 0
        fail("提权失败或已被取消。请右键以管理员身份运行。")
        return 1

    target = target_win64_path()
    resolve_running = find_resolve_processes()

    step(1, 4, "检查环境")
    say("       安装目标: %s" % target)
    if resolve_running:
        warn("检测到 DaVinci Resolve 正在运行: %s" % ", ".join(resolve_running))
        say("")
        say("       Resolve 会锁定插件 DLL，导致部分文件无法替换。")
        say("       建议现在关闭 Resolve 再继续，安装会更干净。")
        if not assume_yes:
            say("")
            try:
                ans = input("       仍要继续吗？(y/N) ").strip().lower()
            except EOFError:
                ans = ""
            if ans not in ("y", "yes"):
                say("")
                say("  已取消。请关闭 DaVinci Resolve 后重新运行安装包。")
                return 2
        else:
            warn("已指定自动继续，将尽力安装并在最后提示重启。")
    else:
        ok("未检测到 Resolve，可以干净安装")

    step(2, 4, "复制插件与引擎资源")
    # 先把历史遗留的 .locked-* / .old_* 清掉再装 —— 否则每装一次就多堆一份
    # 引擎资产副本（onnxruntime.dll 单个就十几 MB），几个来回就是上百 MB。
    stale = clean_locked_leftovers(target)
    if stale:
        ok("清理了 %d 个历史残留文件" % stale)
    total_mb = _payload_size_mb(payload_root)
    if total_mb:
        say("       共约 %.0f MB，请稍候 ..." % total_mb)
    good, failed = install_from(payload_root, target, quiet=False)
    if not good:
        warn("有 %d 个文件未能替换：%s" % (len(failed), ", ".join(failed)))

    step(3, 4, "校验关键文件")
    missing = [rel for rel in CRITICAL_FILES
               if not os.path.isfile(os.path.join(target, rel))]
    if missing:
        fail("以下关键文件缺失: %s" % ", ".join(missing))
        say("")
        say("  安装未完成。请关闭 DaVinci Resolve 后重新运行安装包。")
        return 3
    ok("%d 个文件全部就位" % len(PAYLOAD_FILES))

    step(4, 4, "刷新宿主缓存")
    if invalidate_resolve_cache():
        ok("已清除 Resolve 的 OFX 扫描缓存")
    else:
        ok("无需清理（缓存不存在）")

    say("")
    hr()
    say("  " + c("安装完成", GREEN + BOLD))
    hr()
    say("")
    say("  接下来：")
    say("    1. 启动 (或重启) DaVinci Resolve")
    if resolve_running:
        say("       " + c("← Resolve 之前是开着的，必须重启才能识别新插件", YELLOW))
    say("    2. 打开任意项目，进 Color 或 Edit 页面")
    say("    3. 左侧 Effects Library -> OpenFX -> 分组 %s" % c("DaVinci Beauty", CYAN))
    say("    4. 把 %s 拖到片段上，在右侧 Inspector 调参数" % c(PLUGIN_LABEL, CYAN))
    say("")
    say(c("  参数提示：磨皮不为 0 时锐化不生效；八项全 0 时画面完全不变。", DIM))
    say(c("  日志位置：%s" % LOGS_DIR, DIM))
    say("")
    return 0


def _payload_size_mb(payload_root):
    root = payload_source_root(payload_root)
    if not os.path.isdir(root):
        return 0
    total = 0
    for r, _d, fs in os.walk(root):
        for f in fs:
            try:
                total += os.path.getsize(os.path.join(r, f))
            except Exception:
                pass
    return total / (1024.0 * 1024.0)


# ---------------------------------------------------------------------------
# 卸载
# ---------------------------------------------------------------------------
def uninstall_paths():
    """返回卸载时涉及的所有路径（去重）。"""
    common = os.environ.get("COMMONPROGRAMFILES") or r"C:\Program Files\Common Files"
    bundle = os.path.join(common, "OFX", "Plugins", BUNDLE_NAME)
    paths = [bundle, PROGRAMDATA_DIR]
    out = []
    for p in paths:
        if p not in out:
            out.append(p)
    return out


def _dir_size(path):
    total = 0
    for r, _d, fs in os.walk(path):
        for f in fs:
            try:
                total += os.path.getsize(os.path.join(r, f))
            except Exception:
                pass
    return total


def do_uninstall(assume_yes=False):
    say("")
    hr()
    say(c("  DaVinci Beauty 美颜 —— 卸载", BOLD))
    hr()

    if not is_admin():
        fail("需要管理员权限（要删除 Program Files 下的插件）")
        say("")
        say("  正在请求提权 ...")
        extra = ["--uninstall"]
        if assume_yes:
            extra.append("--yes")
        if relaunch_as_admin(extra):
            say("  已发起提权，请在弹出的 UAC 窗口点「是」。")
            return 0
        fail("提权失败或已被取消。请右键以管理员身份运行。")
        return 1

    paths = uninstall_paths()
    resolve_running = find_resolve_processes()

    step(1, 3, "确认要删除的内容")
    existing = [p for p in paths if os.path.exists(p)]
    if not existing:
        warn("没有找到已安装的 DaVinci Beauty，可能本来就没装过。")
        say("")
        say("  仍然会清理 Resolve 的 OFX 缓存，避免插件残影留在列表里。")
    for p in existing:
        sz = _dir_size(p) / (1024.0 * 1024.0)
        say("       %s  (%.1f MB)" % (p, sz))

    if resolve_running:
        say("")
        warn("检测到 DaVinci Resolve 正在运行: %s" % ", ".join(resolve_running))
        say("       Resolve 锁着插件 DLL，会导致删除失败、卸载不干净。")
        say("       " + c("强烈建议先关闭 Resolve。", YELLOW))

    if existing and not assume_yes:
        say("")
        say("  " + c("以下内容将被永久删除：", RED))
        say("       - 插件本体与全部引擎/模型资源（约 162 MB）")
        say("       - 日志目录 %s" % LOGS_DIR)
        say("       - DaVinci Beauty 在 ProgramData 下的全部数据")
        say("")
        try:
            ans = input("  确认卸载？输入 yes 继续: ").strip().lower()
        except EOFError:
            ans = ""
        if ans != "yes":
            say("")
            say("  已取消，未做任何改动。")
            return 2

    step(2, 3, "删除文件")
    failures = []
    for p in existing:
        if _remove_tree(p):
            ok("已删除 %s" % p)
        else:
            fail("无法完整删除 %s（可能有文件被占用）" % p)
            failures.append(p)

    step(3, 3, "清理宿主缓存")
    if invalidate_resolve_cache():
        ok("已清除 Resolve 的 OFX 扫描缓存")
    else:
        ok("无需清理")

    say("")
    hr()
    if failures:
        say("  " + c("卸载未完成", YELLOW + BOLD))
        hr()
        say("")
        say("  以下位置仍有文件残留：")
        for p in failures:
            say("       %s" % p)
        say("")
        say("  原因通常是 DaVinci Resolve 正在运行并锁着 DLL。")
        say("  请完全退出 Resolve 后重新运行卸载。")
        say("")
        return 3

    say("  " + c("卸载完成", GREEN + BOLD))
    hr()
    say("")
    say("  DaVinci Beauty 已从系统中移除。")
    say("  重启 DaVinci Resolve 后，效果库的 OpenFX 列表里就不会再出现它了。")
    say("")
    return 0


def _remove_tree(path):
    """删除目录。先用常规方式；失败则把能删的删掉，返回是否删干净。"""
    if not os.path.exists(path):
        return True
    try:
        shutil.rmtree(path, ignore_errors=False)
        return True
    except Exception:
        pass
    # 逐文件删，容忍个别被占用的
    for root, dirs, files in os.walk(path, topdown=False):
        for f in files:
            try:
                os.remove(os.path.join(root, f))
            except Exception:
                pass
        for d in dirs:
            try:
                os.rmdir(os.path.join(root, d))
            except Exception:
                pass
    try:
        os.rmdir(path)
    except Exception:
        pass
    return not os.path.exists(path)


# ---------------------------------------------------------------------------
# CLI（调试用；打包后由自解压外壳调用）
# ---------------------------------------------------------------------------
def main(argv=None):
    _enable_ansi()
    argv = list(sys.argv[1:] if argv is None else argv)

    if not argv or argv[0] in ("-h", "--help", "/?", "help"):
        say("")
        say("  用法:")
        say("    DaVinciBeauty_Setup.exe              安装（双击即可）")
        say("    DaVinciBeauty_Setup.exe --uninstall  卸载")
        say("    DaVinciBeauty_Setup.exe --yes        静默确认（不询问）")
        say("    DaVinciBeauty_Setup.exe --from <dir> 从指定资源目录安装（调试用）")
        say("")
        return 0

    assume_yes = "--yes" in argv or "-y" in argv
    payload = None
    if "--from" in argv:
        i = argv.index("--from")
        if i + 1 < len(argv):
            payload = argv[i + 1]

    if "--uninstall" in argv or "--remove" in argv:
        rc = do_uninstall(assume_yes=assume_yes)
    else:
        if payload is None:
            # 自解压外壳会把脚本解到临时目录，资源就在脚本同级的 Win64/ 下。
            # 这里以脚本自身位置为锚点定位资源，不依赖 cwd。
            here = os.path.dirname(os.path.abspath(__file__))
            payload = here
        rc = do_install(payload, assume_yes=assume_yes)
    return rc


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        say("")
        say("  已中断。")
        sys.exit(130)
