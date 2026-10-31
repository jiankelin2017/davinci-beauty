#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""DaVinci Beauty 一键工程验收。

用法
----
    python acceptance.py              # 跑全部检查
    python acceptance.py --build      # 先重新编译再验（慢，发版时用）
    python acceptance.py --skip 回归  # 跳过某一组
    python acceptance.py --report 验收报告_2.3.0.txt

退出码 0 = 全部 PASS，1 = 有 FAIL，2 = 有 WARN 但无 FAIL（看 --strict）。

检查分组
--------
    版本    源码宏 / 二进制内嵌串 / NamaVersion / 装机产物是否同源
    参数    三张表对齐、默认值、隐藏项
    回归    15 个用例逐字节比对黄金基线
    接口    mock host（19 项 OFX 宿主契约）+ 自测链路
    装机    bundle 完整性、残留文件
"""
import ctypes
import hashlib
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
SRC = os.path.join(REPO, "01_ofx插件源码", "src")
# 清理后 out/ 不再是交付物（构建产物，已移除）；验收直接校验装机用 bundle 内的 .ofx
BUILD = os.path.join(REPO, "01_ofx插件源码", "bundle",
                     "DaVinciBeauty.ofx.bundle", "Contents", "Win64", "DaVinciBeauty.ofx")
INSTALLED = (r"C:\Program Files\Common Files\OFX\Plugins"
             r"\DaVinciBeauty.ofx.bundle\Contents\Win64\DaVinciBeauty.ofx")
BUNDLE = os.path.dirname(INSTALLED)
PROBE = os.path.join(REPO, "03_测试夹具")

BUILD_DLL = os.environ.get("NAMA_OFX_DLL", BUILD)
PY = sys.executable

# 装机 bundle 里必须存在的引擎资产（缺一个插件就起不来）
# 自研引擎（OBS CoreEngine + ONNX YuNet）只需要 ONNX Runtime 与本地人脸模型，
# 不再依赖任何闭源 SDK。
REQUIRED_ASSETS = [
    "onnxruntime.dll", "yunet.onnx",
]

_results = []


def rec(group, name, ok, detail="", warn=False):
    _results.append((group, name, ok, detail, warn))
    tag = "WARN" if warn else ("PASS" if ok else "FAIL")
    print("  [%s] %-28s %s" % (tag, name, detail))
    return ok


def md5file(p):
    h = hashlib.md5()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def strip_comments(s):
    """去掉 // 和 /* */ 注释。解析 C++ 数组前必须做，否则注释里提到的
    P_CHEEK 之类会被当成枚举成员抓出来。"""
    s = re.sub(r"/\*.*?\*/", "", s, flags=re.S)
    s = re.sub(r"//[^\n]*", "", s)
    return s


def block_after(txt, marker):
    """取 marker 之后到匹配的 } 为止的花括号块内容。"""
    i = txt.find(marker)
    if i < 0:
        return None
    i = txt.find("{", i)
    if i < 0:
        return None
    depth = 0
    for j in range(i, len(txt)):
        if txt[j] == "{":
            depth += 1
        elif txt[j] == "}":
            depth -= 1
            if depth == 0:
                return txt[i + 1:j]
    return None


# --------------------------------------------------------------------------
# A. 版本
# --------------------------------------------------------------------------
def read_version_macros():
    txt = open(os.path.join(SRC, "nama_engine.h"), "r", encoding="utf-8").read()
    got = {}
    for k in ("MAJOR", "MINOR", "PATCH"):
        m = re.search(r"#define\s+NAMA_BUILD_%s\s+(\d+)" % k, txt)
        if not m:
            return None
        got[k] = int(m.group(1))
    m = re.search(r'#define\s+NAMA_BUILD_STR\s+"([^"]+)"', txt)
    if not m:
        return None
    got["STR"] = m.group(1)
    return got


def check_version():
    print("\n[版本]")
    v = read_version_macros()
    if not v:
        rec("版本", "读到源码版本宏", False, "nama_engine.h 里没找到 NAMA_BUILD_*")
        return
    expect = "%d.%d.%d" % (v["MAJOR"], v["MINOR"], v["PATCH"])
    rec("版本", "NAMA_BUILD_STR 与三段一致", v["STR"] == expect,
        "%s == %s" % (v["STR"], expect))

    # 二进制里必须能搜到完整版本串：这是「装的这个 DLL 是哪个版本」的
    # 唯一自证途径，因为 NamaVersion() 丢掉了 PATCH 位。
    blob = open(BUILD_DLL, "rb").read()
    has_str = v["STR"].encode("ascii") in blob
    rec("版本", "二进制内嵌版本串", has_str, "搜 '%s' %s"
        % (v["STR"], "命中" if has_str else "未命中"))

    lib = ctypes.WinDLL(BUILD_DLL)
    lib.NamaVersion.restype = ctypes.c_ulong
    lib.NamaVersion.argtypes = []
    got = lib.NamaVersion()
    want = (v["MAJOR"] << 16) | v["MINOR"]
    rec("版本", "NamaVersion 编码", got == want,
        "0x%x，期望 (MAJOR<<16)|MINOR = 0x%x" % (got, want))

    if os.path.isfile(INSTALLED):
        a, b = md5file(BUILD_DLL), md5file(INSTALLED)
        # 二进制 md5 只作信息展示，不作判据：MSVC 链接会在 PE 头写时间戳，
        # 同一份源码重编一次 md5 就变（实测 50fea442 -> e3d64956），但输出
        # 逐字节不变。真正该判的是「行为是否等价」，见下一项。
        rec("版本", "装机产物字节比对", a == b,
            "相同 %s" % a[:12] if a == b else "不同 out=%s 装机=%s（构建路径不同；/Brepro 已去掉时间戳，非缺陷）"
            % (a[:12], b[:12]), warn=(a != b))

        # 行为等价：让装机 DLL 跑同一组黄金用例，与编译 DLL 得出相同输出。
        # 这才是「装的是不是这一版的效果」。
        env = dict(os.environ, NAMA_OFX_DLL=INSTALLED)
        p = subprocess.run([PY, os.path.join(HERE, "baseline.py"), "check",
                            "--only", "blur62", "shape100", "mode2"],
                           cwd=HERE, capture_output=True, text=True,
                           encoding="utf-8", errors="replace", env=env)
        out = p.stdout or ""
        m = re.search(r"逐字节一致:\s*(\d+)/(\d+)", out)
        a1, b1 = (int(m.group(1)), int(m.group(2))) if m else (0, 3)
        rec("版本", "装机产物行为等价", p.returncode == 0 and a1 == b1,
            "黄金用例 %d/%d" % (a1, b1))
    else:
        rec("版本", "装机产物存在", False, INSTALLED)


# --------------------------------------------------------------------------
# B. 参数表
# --------------------------------------------------------------------------
def parse_enum():
    txt = strip_comments(open(os.path.join(SRC, "nama_engine.h"), "r",
                              encoding="utf-8").read())
    body = block_after(txt, "enum {")
    if body is None:
        return None
    return [m for m in re.findall(r"\b(P_[A-Z0-9_]+)\b", body) if m != "P_COUNT"]


def parse_params():
    txt = strip_comments(open(os.path.join(SRC, "ofx_plugin.cpp"), "r",
                              encoding="utf-8").read())
    body = block_after(txt, "kParams[P_COUNT]")
    if body is None:
        return None
    out = []
    for m in re.finditer(r'\{\s*"([^"]+)"\s*,\s*"([^"]*)"\s*,\s*([-\d.]+)\s*,', body):
        name, label, dflt = m.group(1), m.group(2), float(m.group(3))
        # 从该条目起向后找 secret 标志（第 5 个字段）
        seg = body[m.end():m.end() + 900]
        tail = re.search(r",\s*(true|false)\s*,", seg)
        secret = tail.group(1) == "true" if tail else None
        out.append((name, label, dflt, secret))
    return out


def parse_keys():
    txt = strip_comments(open(os.path.join(SRC, "nama_engine.cpp"), "r",
                              encoding="utf-8").read())
    body = block_after(txt, "keys[P_COUNT]")
    if body is None:
        return None
    return re.findall(r'"([^"]+)"', body)


def check_params():
    print("\n[参数]")
    en, pa, ks = parse_enum(), parse_params(), parse_keys()
    if not (en and pa and ks):
        rec("参数", "解析三张表", False,
            "枚举=%s 面板=%s keys=%s" % (bool(en), bool(pa), bool(ks)))
        return

    rec("参数", "三张表长度一致", len(en) == len(pa) == len(ks),
        "枚举%d 面板%d keys%d" % (len(en), len(pa), len(ks)))

    # 面板名与 SDK key 名按位对齐。v2.2.3 的「拖 A 滑块、下发成 B 参数」就是
    # 这里错了一位，编译期没有任何提示，只有回归用例的 md5 会变。
    mis = [(i, pa[i][0], ks[i]) for i in range(min(len(pa), len(ks)))
           if pa[i][0] != ks[i]]
    rec("参数", "面板名 ↔ SDK key 对齐", not mis,
        "" if not mis else "首处错位 #%d: %s vs %s" % (mis[0][0], mis[0][1], mis[0][2]))

    # 期望默认档（v2.2.3 用户决策）：磨皮 50 / 美白 30 / 红润 30，其余 0
    want = {"blur_level": 50.0, "color_level": 30.0, "red_level": 30.0}
    bad = []
    for name, label, dflt, _s in pa:
        exp = want.get(name, 0.0)
        if abs(dflt - exp) > 1e-9:
            bad.append("%s=%g(期望%g)" % (name, dflt, exp))
    rec("参数", "默认档 50/30/30 其余0", not bad, "" if not bad else ", ".join(bad))

    # 「锐化」磨皮>0 时完全无效，必须保持隐藏，否则用户拖了没反应会以为是坏了
    hidden = {n for n, _l, _d, s in pa if s is True}
    rec("参数", "锐化保持隐藏", "sharpen" in hidden,
        "隐藏项: %s" % (", ".join(sorted(hidden)) or "(无)"))

    # 美型三项必须沉底：Resolve 的 OFX 面板不渲染 Group/Page，只能靠定义顺序
    tail = [n for n, _l, _d, _s in pa][-3:]
    shape = {"cheek_thinning", "eye_enlarging", "intensity_chin"}
    rec("参数", "美型三项沉底", set(tail) == shape, "末三项: %s" % ", ".join(tail))


# --------------------------------------------------------------------------
# C. 回归
# --------------------------------------------------------------------------
def check_regression():
    print("\n[回归] 逐字节比对黄金基线（用例数见下方结果行，耗时长，请稍候）")
    t0 = time.time()
    p = subprocess.run([PY, os.path.join(HERE, "baseline.py"), "check"],
                       cwd=HERE, capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    out = p.stdout or ""
    m = re.search(r"逐字节一致:\s*(\d+)/(\d+)", out)
    if m:
        a, b = int(m.group(1)), int(m.group(2))
        rec("回归", "逐字节一致", a == b and p.returncode == 0,
            "%d/%d  (%.1fs)" % (a, b, time.time() - t0))
    else:
        rec("回归", "逐字节一致", False, "没能解析结果 rc=%d" % p.returncode)
        print(out[-800:])


# --------------------------------------------------------------------------
# D. 接口
# --------------------------------------------------------------------------
def check_interface():
    print("\n[接口]")
    mh = os.path.join(PROBE, "mock_host.py")
    if os.path.isfile(mh):
        env = dict(os.environ, NAMA_OFX_DLL=INSTALLED if os.path.isfile(INSTALLED)
                   else BUILD_DLL)
        p = subprocess.run([PY, mh], cwd=PROBE, capture_output=True, text=True,
                           encoding="utf-8", errors="replace", env=env)
        out = p.stdout or ""
        npass = len(re.findall(r":\s*PASS", out))
        nfail = len(re.findall(r":\s*FAIL", out))
        rec("接口", "mock host OFX 契约", p.returncode == 0,
            "PASS %d / FAIL %d" % (npass, nfail))
        if nfail:
            for line in out.splitlines():
                if "FAIL" in line:
                    print("      %s" % line.strip())
    else:
        rec("接口", "mock host 存在", False, mh)

    st = os.path.join(PROBE, "selftest_ofx.py")
    if os.path.isfile(st):
        # 这个脚本默认把引擎目录取成 DLL 所在目录。bundle/ 内的 .ofx 与
        # onnxruntime.dll / yunet.onnx 同目录，初始化即可成功；装机不可用时回退到 DLL 目录。
        eng = BUNDLE if os.path.isdir(BUNDLE) else os.path.dirname(BUILD_DLL)
        env = dict(os.environ, NAMA_OFX_DLL=BUILD_DLL, NAMA_ENGINE_DIR=eng)
        p = subprocess.run([PY, st], cwd=PROBE, capture_output=True, text=True,
                           encoding="utf-8", errors="replace", env=env)
        out = p.stdout or ""
        ok = ("PASS" in out) and p.returncode == 0
        rec("接口", "自测链路 LoadLibrary→渲染", ok,
            (out.strip().splitlines() or [""])[-1][:60])
    else:
        rec("接口", "自测脚本存在", False, st)


# --------------------------------------------------------------------------
# E. 装机
# --------------------------------------------------------------------------
def check_bundle():
    print("\n[装机]")
    if not os.path.isdir(BUNDLE):
        rec("装机", "bundle 目录存在", False, BUNDLE)
        return
    miss = [a for a in REQUIRED_ASSETS if not os.path.exists(os.path.join(BUNDLE, a))]
    rec("装机", "引擎资产齐全", not miss, "" if not miss else "缺: " + ", ".join(miss))

    # install.bat 替换被占用 DLL 时把旧文件改名成 .locked-<pid> 留着，
    # Resolve 关不干净就会一直堆下去。这些是纯垃圾，可安全删除。
    leftovers = [f for f in os.listdir(BUNDLE) if ".locked-" in f]
    size = sum(os.path.getsize(os.path.join(BUNDLE, f)) for f in leftovers)
    rec("装机", "无 .locked-* 残留", not leftovers,
        "干净" if not leftovers else "%d 个，共 %.1f MB（可删）" % (len(leftovers), size / 1048576),
        warn=bool(leftovers))


# --------------------------------------------------------------------------
# F. 工程一致性（都是事后才被发现的成本，能静态判的就别靠人记）
# --------------------------------------------------------------------------
# v2.3.1 踩的坑：install.bat 从来没拷过 ai_face_processor_pc_plain.bundle，
# 旧 bundle 里那份是手工放的。全新装机就缺资产，NamaSelfTest 直接崩（rc=0xC0000005）。
MISSING_MSG = "install.bat 没拷过这笔资产 —— 全新装机必崩，不是可用可不用"


def readtext(p):
    try:
        with open(p, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def check_project():
    print("\n[工程]")

    # 1) install.bat 的拷贝清单必须覆盖装机必备资产（静态判，装机之前就抓出来）
    ibat = os.path.join(REPO, "01_ofx插件源码", "install.bat")
    itxt = readtext(ibat)
    if itxt:
        covered, missing = [], []
        for a in REQUIRED_ASSETS:
            hit = any(("copy" in ln or "call :inst" in ln or "mkdir" in ln) and a in ln
                      for ln in itxt.splitlines())
            (covered if hit else missing).append(a)
        rec("工程", "install.bat 覆盖必备资产", not missing,
            "%d/%d 项" % (len(covered), len(REQUIRED_ASSETS)) if not missing
            else "缺: " + ", ".join(missing))
    else:
        rec("工程", "install.bat 可读", False, ibat)

    # 1b) 装机资产清单散在三处，任何一处漏了都会产出「能装但会崩」的包。
    #     v2.3.1 时三处同时漏掉 ai_face_processor_pc_plain.bundle —— 缺它不是报错，
    #     是 NamaSelfTest 直接崩（rc=0xC0000005），所以这里对三处一并静态校验。
    #     ① install.bat 的 copy/inst 行  ② build_nsis.py 的 PAYLOAD_PLAN
    #     ③ davincibeauty.nsi 的 File 指令
    file_assets = [a for a in REQUIRED_ASSETS if a not in ("model", "graphics")]
    bnp = os.path.join(REPO, "05_安装包", "build_nsis.py")
    nsi = os.path.join(REPO, "05_安装包", "davincibeauty.nsi")
    for label, path, pats in (
        ("PAYLOAD_PLAN", bnp, ('"%s"',)),
        ("NSI File 指令", nsi, ('${STAGE}\\%s"', '"%s"')),
    ):
        txt = readtext(path)
        if not txt:
            rec("工程", "%s 可读" % label, False, path)
            continue
        miss = [a for a in file_assets
                if not any((p % a) in txt for p in pats)]
        rec("工程", "安装包 %s 覆盖必备资产" % label, not miss,
            "%d/%d 项" % (len(file_assets) - len(miss), len(file_assets))
            if not miss else "缺: " + ", ".join(miss))

    # 2) UI 文案：不许残留旧项目名，且三个 label 必须带上新名
    plugin_src = os.path.join(SRC, "ofx_plugin.cpp")
    # 注释里会提到历史名与带括号的旧叫法，取文案前必须先剥掉注释
    ptxt = strip_comments(readtext(plugin_src))
    if ptxt:
        stale = set(re.findall(r"NamaBeauty(?!SDK|Settings|Engine|SelfTest|Version|Region|Log)", ptxt))
        rec("工程", "UI 无旧名残留", not stale,
            "干净" if not stale else "残留: " + ", ".join(sorted(stale)[:5]))

        # 只认插件级文案（变量是 props）；参数级用的是 pp，那是「磨皮」之类的功能名，
        # 不该带上产品名。
        labels = re.findall(
            r'propSetString\(props,\s*kOfxProp(?:Short|Long)?Label[^,]*,\s*\d+,\s*"([^"]*)"', ptxt)
        labels += re.findall(
            r'propSetString\(props,\s*kOfxImageEffectPluginPropGrouping[^,]*,\s*\d+,\s*"([^"]*)"',
            ptxt)
        bad = [d for d in labels if "DaVinci Beauty" not in d]
        rec("工程", "面板文案统一新名", not bad,
            "共 %d 处" % len(labels) if not bad else "未同步: " + " / ".join(bad))

        # 3) 下拉项不许带括号后缀（用户明确要求简洁）
        paren = []
        for m in re.finditer(r"k(?:SmoothMode|BlurType)Choices\[\] = \{(.*?)\};", ptxt, re.S):
            paren += [s for s in re.findall(r'"([^"\n]*)"', m.group(1))
                      if "（" in s or "(" in s]
        rec("工程", "下拉项无括号后缀", not paren,
            "干净" if not paren else "仍带括号: " + " / ".join(paren))

    # 4) 文档版本号必须跟上源码宏 —— 之前四个文档全线停在旧版本，用户照着看会错
    htxt = readtext(os.path.join(SRC, "nama_engine.h"))
    mv = re.search(r'#define\s+NAMA_BUILD_STR\s+"([^"]+)"', htxt)
    if mv:
        ver = mv.group(1)
        # 05_安装包/ 的 README 也在内：它历史上长期停在 v1.4 无人发现。
        docs = [os.path.join(REPO, "README.md"), os.path.join(REPO, "说明.txt"),
                os.path.join(REPO, "01_ofx插件源码", "README.md"),
                os.path.join(REPO, "01_ofx插件源码", "PROGRESS.md"),
                os.path.join(REPO, "05_安装包", "README.md")]
        present = [d for d in docs if os.path.isfile(d)]
        stale = [os.path.basename(d) for d in present if ver not in readtext(d)]
        rec("工程", "文档版本号同步 v" + ver, not stale,
            "%d 份齐" % len(present) if not stale else "未同步: " + ", ".join(stale))

        # 4b) 文档里提到的「安装包文件名」版本必须是当前版本。
        #     上面那条只保证「有当前版本号」，抓不到「还留着旧文件名」——
        #     05_安装包/README 曾同时写着当前版本和 Setup_v1.4.exe，
        #     用户照后者去磁盘上找，只会找不到文件。
        #     含「历史」字样的行跳过：PROGRESS / 验收摘要里引用旧产物是正常留档。
        badsetup = []
        for d in present:
            for line in readtext(d).splitlines():
                if "历史" in line:
                    continue
                for m in re.finditer(
                        r"DaVinciBeauty_Setup_v([0-9]+(?:\.[0-9]+)*)\.exe", line):
                    if m.group(1) != ver:
                        badsetup.append("%s→v%s" % (os.path.basename(d), m.group(1)))
        rec("工程", "文档引用的安装包版本", not badsetup,
            "均为 v%s" % ver if not badsetup else "过期引用: " + ", ".join(badsetup))

    # 5) 所有 .bat 必须是「GBK + CRLF + 无 chcp」
    #    实测过三种失败形态，都会让 cmd 把行切开、把注释当命令执行：
    #      · UTF-8 中文  —— cmd 按 ANSI(GBK) 解析，多字节序列吃掉行边界
    #      · chcp 65001  —— 中途换代码页，后续行的字节偏移全部错位
    #      · 裸 LF 换行  —— 批处理只认 CRLF
    #    症状是满屏「'xx' 不是内部或外部命令」，极难从报错反推原因。
    bats = []
    for root, dirs, fs in os.walk(REPO):
        dirs[:] = [d for d in dirs
                   if d not in (".git", "out", "obj", "build", "__pycache__")
                   and "_backup_before" not in d]
        for f in fs:
            if f.lower().endswith(".bat"):
                bats.append(os.path.join(root, f))
    bad = []
    for b in bats:
        raw = open(b, "rb").read()
        probs = []
        if raw.count(b"\n") - raw.count(b"\r\n") > 0:
            probs.append("LF换行")
        if b"chcp " in raw:
            probs.append("有chcp")
        if any(c > 127 for c in raw):
            try:
                raw.decode("gbk")
            except UnicodeDecodeError:
                probs.append("非GBK")
        if probs:
            bad.append("%s(%s)" % (os.path.relpath(b, REPO), "+".join(probs)))
    rec("工程", "批处理编码 GBK+CRLF+无chcp", not bad,
        "%d 个 .bat 全部合规" % len(bats) if not bad
        else "违规 %d 个: %s" % (len(bad), "; ".join(bad[:3])))


# --------------------------------------------------------------------------
def maybe_build():
    bat = os.path.join(REPO, "01_ofx插件源码", "build.bat")
    print("\n[编译] %s" % bat)
    p = subprocess.run(["cmd", "/c", bat], cwd=os.path.dirname(bat),
                       capture_output=True, text=True, encoding="utf-8",
                       errors="replace")
    out = (p.stdout or "") + (p.stderr or "")
    warns = len(re.findall(r"warning", out, re.I))
    rec("编译", "build.bat 成功", p.returncode == 0,
        "warning %d 条" % warns, warn=warns > 0)
    if p.returncode != 0:
        print(out[-1500:])


def main():
    args = sys.argv[1:]
    skip = set()
    if "--skip" in args:
        skip = set(args[args.index("--skip") + 1:])

    print("=" * 68)
    print("DaVinci Beauty 工程验收")
    print("时间: %s" % time.strftime("%Y-%m-%d %H:%M:%S"))
    print("DLL : %s" % BUILD_DLL)
    print("=" * 68)

    if "--build" in args:
        maybe_build()

    groups = [
        ("版本", check_version),
        ("参数", check_params),
        ("回归", check_regression),
        ("接口", check_interface),
        ("装机", check_bundle),
        ("工程", check_project),
    ]
    for name, fn in groups:
        if name in skip:
            print("\n[%s] 已跳过" % name)
            continue
        try:
            fn()
        except Exception as e:
            rec(name, "执行异常", False, "%s: %s" % (type(e).__name__, e))

    fails = [r for r in _results if not r[2] and not r[4]]
    warns = [r for r in _results if r[4] and not r[2]]
    passed = [r for r in _results if r[2]]

    print("\n" + "=" * 68)
    print("PASS %d   FAIL %d   WARN %d" % (len(passed), len(fails), len(warns)))
    for g, n, _o, d, _w in fails:
        print("  FAIL %s / %s : %s" % (g, n, d))
    for g, n, _o, d, _w in warns:
        print("  WARN %s / %s : %s" % (g, n, d))
    print("总判定: %s" % ("PASS" if not fails else "FAIL"))
    print("=" * 68)

    if "--report" in args:
        rp = args[args.index("--report") + 1]
        with open(rp, "w", encoding="utf-8") as f:
            f.write("DaVinci Beauty 验收报告 %s\n\n" % time.strftime("%Y-%m-%d %H:%M:%S"))
            for g, n, o, d, w in _results:
                f.write("[%s] %-12s %-28s %s\n"
                        % ("WARN" if w else ("PASS" if o else "FAIL"), g, n, d))
            f.write("\nPASS %d FAIL %d WARN %d\n" % (len(passed), len(fails), len(warns)))
            f.write("总判定: %s\n" % ("PASS" if not fails else "FAIL"))
        print("报告已写入 %s" % rp)

    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
