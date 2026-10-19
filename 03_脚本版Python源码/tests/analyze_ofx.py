# -*- coding: utf-8 -*-
"""静态分析 Beauty Box.ofx：提取 OFX 属性声明、GPU 后端、实时预览相关机制。

只做架构层面的观察（导入表 / OFX 属性字符串 / 着色器与 kernel 痕迹），
目的是理解它凭什么做到检视器里实时预览。
"""
import os
import re
import struct
import sys
from collections import Counter, OrderedDict

PATH = sys.argv[1] if len(sys.argv) > 1 else \
    r"D:\BaiduNetdiskDownload\OFX达芬奇磨皮插件 Beauty Box 6.5 Win fix1\OFX达芬奇磨皮插件 Beauty Box 6.5 Win fix1\Beauty Box.ofx"

data = open(PATH, "rb").read()
print("[A] 文件 %s  %.1f MB" % (os.path.basename(PATH), len(data) / 1048576.0))

# ---------------- PE 结构 ----------------
pe_off = struct.unpack_from("<I", data, 0x3C)[0]
assert data[pe_off:pe_off + 4] == b"PE\0\0", "不是 PE 文件"
machine, nsec, _, _, _, optsz, chars = struct.unpack_from("<HHIIIHH", data, pe_off + 4)
magic = struct.unpack_from("<H", data, pe_off + 24)[0]
print("[A] 机器码 %#x  PE32%s  节数 %d"
      % (machine, "+" if magic == 0x20B else "", nsec))

secs = []
so = pe_off + 24 + optsz
for i in range(nsec):
    name = data[so:so + 8].rstrip(b"\0").decode("ascii", "replace")
    vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", data, so + 8)
    secs.append((name, vaddr, vsize, raddr, rsize))
    so += 40
print("[A] 节区：", "  ".join("%s(%.1fMB)" % (n, rs / 1048576.0)
                            for n, _, _, _, rs in secs))
big = sorted(secs, key=lambda s: -s[4])[:3]
for n, va, vs, ra, rs in big:
    print("[A]   最大节 %-8s 原始 %.1f MB 可执行=%s"
          % (n, rs / 1048576.0, bool([c for c in []])))
    
# ---------------- 导入 DLL ----------------
def rva2off(rva):
    for n, va, vs, ra, rs in secs:
        if va <= rva < va + max(vs, rs):
            return ra + (rva - va)
    return None


dlls = []
try:
    # DataDirectory[1] = Import
    ddir = pe_off + 24 + (112 if magic == 0x20B else 96)
    imp_rva = struct.unpack_from("<I", data, ddir + 8)[0]
    o = rva2off(imp_rva)
    while o:
        desc = data[o:o + 20]
        if len(desc) < 20 or desc == b"\0" * 20:
            break
        name_rva = struct.unpack_from("<I", desc, 12)[0]
        if not name_rva:
            break
        no = rva2off(name_rva)
        nm = data[no:data.index(b"\0", no)].decode("ascii", "replace")
        dlls.append(nm)
        o += 20
except Exception as e:
    print("[A] 导入表解析失败:", e)

print("[A] 导入 DLL（%d 个）：" % len(dlls))
interesting = [d for d in dlls if re.search(
    r"cuda|nvcuda|nvapi|opencl|opengl|glu|d3d|dxgi|gdi|python|torch|onnx|openvino|mkl|tbb|ipp|avx",
    d, re.I)]
print("     关键：", ", ".join(interesting) or "(无)")
print("     全部：", ", ".join(dlls[:40]))

# ---------------- 字符串提取 ----------------
ascii_re = re.compile(rb"[\x20-\x7e]{5,}")
utf16_re = re.compile(rb"(?:[\x20-\x7e]\x00){4,}")
strs = OrderedDict()
for m in ascii_re.finditer(data):
    strs.setdefault(m.group().decode("ascii"), len(strs))
for m in utf16_re.finditer(data):
    strs.setdefault(m.group().decode("utf-16-le"), len(strs))
print("\n[S] 提取到 %d 条唯一字符串" % len(strs))

needles = {
    "OFX 属性": r"^kOfx",
    "OFX 动作/宿主": r"OfxImageEffectAction|OfxAction|OfxHost|OfxPropertySuite|OfxImageEffectSuite",
    "GPU/渲染后端": r"CUDA|cuda|OpenCL|opencl|OpenGL|GLSL|glsl|__global__|nppi|cuFFT|CUBLAS|shader",
    "实时/交互": r"interactive|Interactive|interact|Interact|proxy|Proxy|preview|Preview|draft|Draft",
    "多分辨率/平铺": r"MultiResolution|multiResolution|Tiles|tile|ROD|RegionOfDefinition|renderWindow",
    "缓存": r"cache|Cache|\.tmp|TEMP|AppData|ProgramData",
    "人脸/分析": r"face|Face|FaceDetect|landmark|Landmark|analyze|Analyze",
    "参数名样本": r"^[A-Za-z][A-Za-z0-9_ ]{2,28}$",
}
for label, pat in needles.items():
    rx = re.compile(pat)
    hits = [s for s in strs if rx.search(s)]
    print("\n[S] ==== %s ==== 命中 %d" % (label, len(hits)))
    show = hits[:45]
    for h in show:
        print("     ", h[:120])
    if len(hits) > len(show):
        print("      ... 还有 %d 条" % (len(hits) - len(show)))
