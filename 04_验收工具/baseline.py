#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""DaVinci Beauty 逐字节回归基线：生成 / 比对。

为什么存在
----------
性能优化（多线程、SIMD、改内存布局）**必须**保证输出逐字节不变，否则
「变快了」很可能只是「少算了」。这个脚本就是那条防线：对一组固定用例跑
插件自测入口，把输出帧的 md5 记进 manifest，以后每次改动都重跑比对。

与旧 make_baseline.py 的区别（工程化改造）
------------------------------------------
1. 只把 md5 清单入库，不入库 raw。  15 个用例 × 2.6MB ≈ 41MB，raw 每次
   现生成，仓库里只留 1KB 的 manifest。
2. 自带输入源图 input_face.png，不再依赖工作区外的绝对路径 —— 换台机器
   clone 下来就能跑。
3. manifest 记录输入指纹（md5 + 尺寸）和插件版本。输入换了、版本变了都
   会显式报出来，不会变成一堆莫名其妙的 md5 失配。
4. 支持 --repeat：同一用例连跑两次，md5 必须一致。这一条验证的是**测试
   本身是否确定性**，避免把「测量抖动」误判成「输出变了」。
5. 输出路径走 ASCII 临时目录中转。NamaSelfTest 的入参是窄字符 const char*，
   路径含中文（本仓库就在 D:\\插件\\ 下）时部分系统码页会 fopen 失败。

用法
----
    python baseline.py check              # 比对（默认，退出码 0 = 全等）
    python baseline.py update             # 重新生成基线（升版本后必做）
    python baseline.py show               # 只打印当前基线清单
    python baseline.py check --only blur62 sh50
"""
import ctypes
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

# 尺寸沿用历史基线（1074x604），改了会让所有 md5 失配 —— 那是重新标定，
# 不是 bug。要改就同步 update 基线。
W = int(os.environ.get("NAMA_SELFTEST_W") or 1074)
H = int(os.environ.get("NAMA_SELFTEST_H") or 604)

SRC_PNG = os.environ.get("NAMA_BASELINE_SRC") or os.path.join(HERE, "input_face.png")
MANIFEST = os.environ.get("NAMA_BASELINE_MANIFEST") or os.path.join(HERE, "baseline_manifest.txt")

DLL = os.environ.get(
    "NAMA_OFX_DLL",
    os.path.join(REPO, "01_ofx插件源码", "bundle",
                 "DaVinciBeauty.ofx.bundle", "Contents", "Win64", "DaVinciBeauty.ofx"))
ENGINE = os.environ.get(
    "NAMA_ENGINE_DIR",
    r"C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle\Contents\Win64")

# (用例名, 磨皮, 美白, 额外环境变量)
# 覆盖：磨皮全档 / 美白 / 锐化 / 清晰 / 全局锐化 / 美型 / 三种磨皮模式。
# shape100 是 v2.2.3 那次「换序漏改 keys[]」唯一抓出问题的用例，务必保留。
CASES = [
    ("blur00",        0.0,  0.0, {}),
    ("blur30",       30.0,  0.0, {}),
    ("blur62",       62.0,  0.0, {}),
    ("blur100",     100.0,  0.0, {}),
    ("blur62_wh30",  62.0, 30.0, {}),
    ("blur62_wh100", 62.0, 100.0, {}),
    ("sh50",         62.0, 30.0, {"NAMA_ST_SHARPEN": 50}),
    ("sh100",        62.0, 30.0, {"NAMA_ST_SHARPEN": 100}),
    ("cl50",         62.0, 30.0, {"NAMA_ST_CLARITY": 50}),
    ("sh50_cl100",   62.0, 30.0, {"NAMA_ST_SHARPEN": 50, "NAMA_ST_CLARITY": 100}),
    ("gs50",         62.0, 30.0, {"NAMA_ST_GSHARP": 50}),
    ("shape100",     62.0, 30.0, {"NAMA_ST_CHEEK": 100, "NAMA_ST_EYE": 60,
                                  "NAMA_ST_CHIN": 40}),
    ("mode1",        62.0, 30.0, {"NAMA_ST_SMOOTH": 1}),
    ("mode2",        62.0, 30.0, {"NAMA_ST_SMOOTH": 2}),
    ("mode3",        62.0, 30.0, {"NAMA_ST_SMOOTH": 3}),
    # v3.0.5：磨皮算法三档。alg0 = 默认档（双边/保边），应当与 blur62_wh30 逐字节相同；
    # alg1/alg2 必须与 alg0 不同 —— 三者全同就说明接线又断了（v3.0.5 之前正是如此）。
    ("alg0",         62.0, 30.0, {"NAMA_ST_ALG": 0}),
    ("alg1",         62.0, 30.0, {"NAMA_ST_ALG": 1}),
    ("alg2",         62.0, 30.0, {"NAMA_ST_ALG": 2}),
]

_ST_ENV = ("NAMA_ST_CHEEK", "NAMA_ST_EYE", "NAMA_ST_CHIN", "NAMA_ST_SHARPEN",
           "NAMA_ST_CLARITY", "NAMA_ST_GSHARP", "NAMA_ST_SMOOTH", "NAMA_ST_DELSPOT",
           "NAMA_ST_NASO", "NAMA_ST_ALG")


def md5file(p):
    h = hashlib.md5()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def source_version():
    """从 nama_engine.h 的宏读完整三段版本。源码是版本号的唯一出处。

    不用 NamaVersion()：它只编码 MAJOR<<16|MINOR，PATCH 位被有意丢掉了
    （保持对外 ABI 语义），程序化查询拿不到修订号。
    """
    h = os.path.join(REPO, "01_ofx插件源码", "src", "nama_engine.h")
    if not os.path.isfile(h):
        return None
    txt = open(h, "r", encoding="utf-8").read()
    got = {}
    for key in ("MAJOR", "MINOR", "PATCH"):
        import re
        m = re.search(r"#define\s+NAMA_BUILD_%s\s+(\d+)" % key, txt)
        if not m:
            return None
        got[key] = int(m.group(1))
    return "%d.%d.%d" % (got["MAJOR"], got["MINOR"], got["PATCH"])


def make_input(stage):
    """PNG -> RGBA raw。落在 ASCII 中转目录里，返回 (路径, md5)。

    通道序必须是 **R,G,B,A**：引擎把 buf[x*4+0] 当 R 读（beauty_core.cpp 的
    skinWeight(row[x*4+0..2]) 按 R,G,B 传参可证），Resolve 的 dump 也是这个序。
    1.2.5 之前这里写的是 BGRA，等于所有离线用例都在测一张反色图 —— 肤色判据
    靠自适应肤色中心才没崩，但测出来的行为和真机不是一回事。
    """
    from PIL import Image
    import numpy as np
    a = np.asarray(Image.open(SRC_PNG).convert("RGB").resize((W, H), Image.LANCZOS))
    rgba = np.dstack([a[:, :, 0], a[:, :, 1], a[:, :, 2],
                      np.full(a.shape[:2], 255, np.uint8)])
    p = os.path.join(stage, "input_%dx%d.raw" % (W, H))
    with open(p, "wb") as f:
        f.write(rgba.tobytes())
    return p, md5file(p)


def load_lib():
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.LoadLibraryExW.argtypes = [ctypes.c_wchar_p, ctypes.c_void_p, ctypes.c_uint32]
    k32.LoadLibraryExW.restype = ctypes.c_void_p
    # LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR(0x100) | SEARCH_DEFAULT_DIRS(0x1000)
    if not k32.LoadLibraryExW(DLL, None, 0x1100):
        raise SystemExit("FAIL 无法加载插件 DLL: %s (err=%d)"
                         % (DLL, ctypes.get_last_error()))
    lib = ctypes.WinDLL(DLL)
    lib.NamaSelfTest.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p,
                                 ctypes.c_int, ctypes.c_int, ctypes.c_double,
                                 ctypes.c_double, ctypes.c_char_p, ctypes.c_int]
    lib.NamaSelfTest.restype = ctypes.c_int
    lib.NamaVersion.argtypes = []
    lib.NamaVersion.restype = ctypes.c_ulong
    return lib


def run_case(lib, stage, name, blur, whiten, env):
    """跑一个用例，返回 (md5, 错误串)。

    v3.0.4：**每个用例起独立进程**（lib 参数保留只为兼容旧调用）。

    为什么必须这样：引擎是有状态的（帧序号、肤色中心 EMA、细节层时序 EMA、
    掩码缓存）。同一个进程里连跑多个用例时，第 N 个用例的结果取决于前 N-1 个
    用例留下的状态 —— 实测 gs50 在同一进程跑两遍得到两个不同 md5，而新进程
    每次都稳定。基线一旦依赖执行顺序，就等于「改了参数也可能测不出来」。
    独立进程后每个用例都是「干净引擎 + 一帧」，可复现、可乱序、可单独重跑。
    """
    out = os.path.join(stage, "out_%s.raw" % name)
    if os.path.isfile(out):
        os.remove(out)

    child_env = os.environ.copy()
    for k in _ST_ENV:
        child_env.pop(k, None)
    for k, v in env.items():
        child_env[k] = str(v)
    child_env["NAMA_ENGINE_ASSET_DIR"] = ENGINE
    child_env.setdefault("PYTHONUTF8", "1")
    child_env.setdefault("PYTHONIOENCODING", "utf-8")

    inp = os.path.join(stage, "input_%dx%d.raw" % (W, H))
    if not os.path.isfile(inp):
        return None, "缺少输入帧 %s" % inp
    r = subprocess.run(
        [sys.executable, os.path.abspath(__file__), "--case",
         name, str(blur), str(whiten), out],
        env=child_env, capture_output=True, text=True,
        encoding="utf-8", errors="replace")
    if r.returncode != 0 or not os.path.isfile(out):
        return None, (r.stdout or "").strip()[-300:] or (r.stderr or "").strip()[-300:]
    h = md5file(out)
    os.remove(out)          # raw 不落仓库，用完即删
    return h, None


def run_case_child(name, blur, whiten, out):
    """子进程入口：只跑一个用例，把 md5 打到 stdout。"""
    lib = load_lib()
    inp = os.path.join(os.path.dirname(out), "input_%dx%d.raw" % (W, H))
    buf = ctypes.create_string_buffer(4096)
    rc = lib.NamaSelfTest(ENGINE.encode("utf-8"), inp.encode("utf-8"),
                          out.encode("utf-8"), W, H, blur, whiten, buf, 4096)
    if rc != 0 or not os.path.isfile(out):
        print("FAIL rc=%d %s" % (rc, buf.value.decode("utf-8", "ignore")))
        return 1
    print(md5file(out))
    return 0


def read_manifest():
    """返回 (meta, {name: md5})；文件不存在返回 ({}, {})。"""
    if not os.path.isfile(MANIFEST):
        return {}, {}
    meta, cases = {}, {}
    with open(MANIFEST, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                if line.startswith("# "):
                    kv = line[2:].split(":", 1)
                    if len(kv) == 2:
                        meta[kv[0].strip()] = kv[1].strip()
                continue
            parts = line.split()
            if len(parts) >= 2:
                cases[parts[0]] = parts[1]
    return meta, cases


def write_manifest(meta, results):
    lines = [
        "# DaVinci Beauty 逐字节回归基线",
        "#",
        "# 每行: <用例名> <输出帧md5>",
        "# 输入帧由 04_验收工具/input_face.png 缩放到 %dx%d 后转 RGBA 生成，"
        "其 md5 记录如下；输入变了这里会失配，属预期。" % (W, H),
        "#",
    ]
    for k in ("input_md5", "plugin_version", "generated_by"):
        if k in meta:
            lines.append("# %s: %s" % (k, meta[k]))
    lines.append("#")
    for name, _b, _w, _e in CASES:
        if name in results:
            lines.append("%-14s %s" % (name, results[name]))
    with open(MANIFEST, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    cmd = args[0] if args else "check"

    if cmd == "show":
        meta, cases = read_manifest()
        for k, v in meta.items():
            print("%-16s %s" % (k, v))
        print("-" * 50)
        for name, _b, _w, _e in CASES:
            print("%-14s %s" % (name, cases.get(name, "(缺失)")))
        return 0

    only = None
    if "--only" in sys.argv:
        i = sys.argv.index("--only")
        only = set(sys.argv[i + 1:])
    repeat = 2 if cmd == "update" else 1
    if "--repeat" in sys.argv:
        repeat = int(sys.argv[sys.argv.index("--repeat") + 1])

    stage = tempfile.mkdtemp(prefix="nama_baseline_")
    try:
        inp, imd5 = make_input(stage)
        lib = load_lib()
        ver = lib.NamaVersion()
        vstr = source_version() or "(未读到 nama_engine.h)"
        # NamaVersion 只编码 MAJOR/MINOR，这里顺手校验它与源码宏一致。
        # 编码是 (MAJOR << 16) | MINOR —— 两段各占 16 位，不是每 8 位一段。
        vmaj_min = (ver >> 16, ver & 0xFFFF)
        expect_mm = tuple(int(x) for x in vstr.split(".")[:2]) if "." in vstr else None

        print("DLL    : %s" % DLL)
        print("版本   : %s (NamaVersion=0x%x → %d.%d)"
              % (vstr, ver, vmaj_min[0], vmaj_min[1]))
        if expect_mm and vmaj_min != expect_mm:
            print("WARN NamaVersion 的 MAJOR.MINOR(%d.%d) 与源码宏(%s) 不一致"
                  % (vmaj_min[0], vmaj_min[1], vstr))
        print("输入   : %s -> %dx%d  md5=%s" % (os.path.basename(SRC_PNG), W, H, imd5))
        print("用例   : %d" % len([c for c in CASES if not only or c[0] in only]))
        print("-" * 60)

        results, errs, unstable = {}, [], []
        for name, blur, whiten, env in CASES:
            if only and name not in only:
                continue
            hs = []
            for _ in range(repeat):
                r, err = run_case(lib, stage, name, blur, whiten, env)
                if err:
                    errs.append((name, err))
                    break
                hs.append(r)
            if len(hs) == repeat and len(set(hs)) != 1:
                unstable.append((name, hs))
            if hs:
                results[name] = hs[0]
                continue

        for name, err in errs:
            print("FAIL %-14s %s" % (name, err))
        for name, hs in unstable:
            print("抖动 %-14s 两次结果不一致: %s" % (name, hs))

        if cmd == "update":
            if errs:
                print("\n有 %d 个用例失败，拒绝写入基线。" % len(errs))
                return 1
            if unstable:
                print("\n有 %d 个用例不确定，拒绝写入基线。" % len(unstable))
                return 1
            write_manifest({"input_md5": imd5, "plugin_version": vstr,
                            "generated_by": os.path.basename(DLL)}, results)
            print("\n基线已更新 -> %s (%d 个用例)" % (MANIFEST, len(results)))
            return 0

        # ---- check ----
        meta, base = read_manifest()
        if not base:
            print("FAIL 没有基线清单 %s，先跑 `python baseline.py update`" % MANIFEST)
            return 1
        if meta.get("input_md5") and meta["input_md5"] != imd5:
            print("WARN 输入帧 md5 与基线记录不同：")
            print("       基线 %s" % meta["input_md5"])
            print("       当前 %s" % imd5)
            print("     所有用例都会失配，需 `update` 重新标定。")
        if meta.get("plugin_version") and meta["plugin_version"] != vstr:
            print("WARN 基线由 v%s 生成，当前插件 v%s"
                  % (meta["plugin_version"], vstr))

        same, diff, missing = [], [], []
        for name, _b, _w, _e in CASES:
            if only and name not in only:
                continue
            if name not in base:
                missing.append(name)
            elif results.get(name) == base[name]:
                same.append(name)
            else:
                diff.append((name, base[name], results.get(name)))
        for n in same:
            print("OK   %-14s %s" % (n, results[n]))
        for n in missing:
            print("缺失 %-14s 基线里没有这个用例" % n)
        for n, b, c in diff:
            print("变了 %-14s 基线 %s" % (n, b))
            print("     %-14s 当前 %s" % ("", c))
        print("-" * 60)
        print("逐字节一致: %d/%d" % (len(same), len(same) + len(diff) + len(missing)))
        ok = not diff and not missing and not errs and not unstable
        print("结论       : %s" % ("PASS" if ok else "FAIL"))
        return 0 if ok else 1
    finally:
        shutil.rmtree(stage, ignore_errors=True)


if __name__ == "__main__":
    # 子进程入口（run_case 用）：baseline.py --case <名字> <blur> <whiten> <输出路径>
    if len(sys.argv) > 1 and sys.argv[1] == "--case":
        sys.exit(run_case_child(sys.argv[2], float(sys.argv[3]),
                                float(sys.argv[4]), sys.argv[5]))
    sys.exit(main())
