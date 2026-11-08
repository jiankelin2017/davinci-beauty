"""DaVinci Beauty 工程清理：默认 dry-run，加 --apply 才真的删。

设计原则：**只允许删除白名单里的东西**，凡是代码/文档/测试用例输入一律碰不到。
目录+文件都走显式清单，不做递归通配匹配 —— 递归通配迟早会误伤。

用法：
    python cleanup.py                 # 列清单 + 体积，不动手
    python cleanup.py --apply         # 真删
    python cleanup.py --only probe    # 只处理某一类（名字见 GROUPS 的键）

已确认「不该删」的东西写在 KEEP 里，改脚本前先想清楚为什么它在里面。
"""
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

# 无论怎么清都不能动的文件（测试用例输入、当前版本产物、版本控制）
KEEP = {
    os.path.join(REPO, "03_测试夹具", "probe_in.raw"),   # 自测链路的输入人脸帧
    os.path.join(REPO, "01_ofx插件源码", "out", "DaVinciBeauty.ofx"),  # 当前版本编译产物
}

GROUPS = {}

# ---- 1. install.bat 替换被占用 DLL 留下的旧副本（含整机上次搬进来的备份目录）----
GROUPS["locked"] = {
    "desc": "install.bat 替换旧 DLL 时的残留：散落的 *.locked-* 与 *_残留备份* 目录",
    "dirs": [d for d in (os.path.join(REPO, n) for n in os.listdir(REPO))
             if os.path.isdir(d) and "残留备份" in os.path.basename(d)],
    "files": [os.path.join(REPO, n) for n in os.listdir(REPO) if ".locked-" in n],
}

# ---- 2. 构建中间产物（install.bat / build.bat 每次运行都会重新生成）----
GROUPS["build"] = {
    "desc": "构建中间产物：install.bat 每次重新 stage bundle，build.bat 每次重编译",
    "dirs": [
        os.path.join(REPO, "01_ofx插件源码", "bundle"),
        os.path.join(REPO, "01_ofx插件源码", "obj"),
    ],
    "files": [
        os.path.join(REPO, "01_ofx插件源码", "out", "NamaBeauty.ofx"),
        os.path.join(REPO, "01_ofx插件源码", "out", "NamaBeauty.ofx.v2.1.2.bak"),
    ],
}

# ---- 3. 探针/调试过程产物（脚本每次跑都会重新生成）----
_probe = os.path.join(REPO, "03_测试夹具")
GROUPS["probe"] = {
    "desc": "探针脚本的输出产物（probe_in.raw 是输入，保留）",
    "dirs": [],
    "files": [
        os.path.join(_probe, "probe_out.raw"),
        os.path.join(_probe, "_calib_out.raw"),
        os.path.join(_probe, "_np_out.raw"),
        os.path.join(_probe, "_face_avail.txt"),
        os.path.join(_probe, "_aitype_probe.log"),
        os.path.join(_probe, "_phase1_report.txt"),
        os.path.join(_probe, "_phase2_gate_report.txt"),
        os.path.join(_probe, "probe_in.raw.bak"),
    ],
}

# ---- 4. 库外的旧 bundle 迁移备份（改名前的最后一份回滚点）----
GROUPS["external"] = {
    "desc": "改名前的旧 bundle（D:\\插件 下，不在仓库内）",
    "dirs": [os.path.join(os.path.dirname(REPO), "_改名迁移备份_20261027")],
    "files": [],
}


def dsize(p):
    """目录总大小；文件直接 size。不存在返回 0。"""
    if not os.path.exists(p):
        return 0
    if os.path.isfile(p):
        return os.path.getsize(p)
    try:
        tot = 0
        for root, _, fs in os.walk(p):
            for f in fs:
                tot += os.path.getsize(os.path.join(root, f))
        return tot
    except OSError:
        return 0


def human(n):
    for u in ("B", "KB", "MB", "GB"):
        if n < 1024 or u == "GB":
            return "%.1f%s" % (n, u)
        n /= 1024.0


def collect(only=None):
    """返回 [(group, kind, path, size)]，只挑真正存在的条目。"""
    out = []
    for g in GROUPS:
        if only and g != only:
            continue
        for kind in ("dirs", "files"):
            for p in GROUPS[g][kind]:
                if os.path.normpath(p) in (os.path.normpath(k) for k in KEEP):
                    continue
                if os.path.exists(p):
                    out.append((g, kind[:-1], p, dsize(p)))
    return out


def main():
    apply = "--apply" in sys.argv
    only = None
    if "--only" in sys.argv:
        i = sys.argv.index("--only")
        only = sys.argv[i + 1] if i + 1 < len(sys.argv) else None
        if only not in GROUPS:
            print("未知分类 %r，可选：%s" % (only, "/".join(GROUPS)))
            return 1

    items = collect(only)
    if not items:
        print("没有可清理的东西。")
        return 0

    print("%s  DaVinci Beauty 工程清理" % ("执行" if apply else "预演"))
    print("仓库: %s" % REPO)
    print("=" * 68)
    total = 0
    for g in GROUPS:
        sub = [i for i in items if i[0] == g]
        if not sub:
            continue
        print("\n[%s] %s" % (g, GROUPS[g]["desc"]))
        for _, kind, path, size in sorted(sub, key=lambda x: -x[3]):
            total += size
            rel = os.path.relpath(path, REPO) if path.startswith(REPO) else path
            print("   %-6s %-10s %s" % (kind, human(size), rel))
    print("\n" + "=" * 68)
    print("合计 %s（%d 项）" % (human(total), len(items)))

    if not apply:
        print("\n这是预演，什么都没动。确认无误后加 --apply 执行。")
        print("注意：KEEP 清单里的 %d 个文件永远不会被动" % len(KEEP))
        return 0

    # 二次确认：跨盘 + 上百 MB 的删除值得再问一次
    print("\n即将删除以上 %d 项。" % len(items))
    ans = input("输入 y 继续，其它任意键取消：").strip().lower()
    if ans != "y":
        print("已取消。")
        return 0

    done, freed, bad = 0, 0, []
    for _, kind, path, size in items:
        try:
            if kind == "dir":
                shutil.rmtree(path)
            else:
                os.remove(path)
            done += 1
            freed += size
        except OSError as e:
            bad.append("%s: %s" % (os.path.basename(path), e))
    print("\n完成 %d 项，释放 %s" % (done, human(freed)))
    if bad:
        print("失败 %d 项：" % len(bad))
        for b in bad:
            print("   " + b)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
