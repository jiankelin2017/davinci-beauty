# 不打开 Resolve，直接 LoadLibrary 调 OFX DLL 里的 BeautySelfTest，
# 验证「加载 SDK -> 初始化 GL -> 渲染 -> 落盘」这条链路在插件二进制里是通的。
import ctypes, os, shutil, sys, tempfile

# 用脚本自身所在目录定位素材，别再硬编码旧工作区的绝对路径
# （换个机器/换个工作区就会 FileNotFoundError）。可用环境变量覆盖：
#   BEAUTY_PROBE_ROOT    素材目录（默认脚本所在目录）
#   BEAUTY_SELFTEST_IN / BEAUTY_SELFTEST_OUT   单独指定输入 / 输出文件
ROOT = os.environ.get("BEAUTY_PROBE_ROOT") or os.path.dirname(os.path.abspath(__file__))
DLL = os.environ.get(
    "BEAUTY_OFX_DLL",
    r"C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle\Contents\Win64\DaVinciBeauty.ofx")
# 引擎资源目录：可被 BEAUTY_ENGINE_DIR 覆盖；默认取插件 bundle 自带的引擎，
# 与插件运行时的定位方式一致。
ENGINE = os.environ.get("BEAUTY_ENGINE_DIR", os.path.dirname(DLL))
W = int(os.environ.get("BEAUTY_SELFTEST_W") or 640)
H = int(os.environ.get("BEAUTY_SELFTEST_H") or 394)

IN = os.environ.get("BEAUTY_SELFTEST_IN") or os.path.join(ROOT, "probe_in.raw")
OUT = os.environ.get("BEAUTY_SELFTEST_OUT") or os.path.join(ROOT, "probe_out.raw")

# 素材缺失时自造一帧确定性纹理（与 mock_host.py 的做法一致）。
# 以前这里直接 open(IN)，仓库里没有 probe_in.raw 时整个自测第一步就 FileNotFound，
# 夹具不自包含。合成帧没有五官，人脸检测会返回 0，所以美白不会生效 ——
# 这是预期行为，不是失败。
if not os.path.isfile(IN):
    _gen_dir = tempfile.mkdtemp(prefix="nama_selftest_in_")
    IN = os.path.join(_gen_dir, "probe_in_synth.raw")
    _buf = bytearray(W * H * 4)
    for _y in range(H):
        for _x in range(W):
            _i = (_y * W + _x) * 4
            _v = 96 + ((_x * 13 + _y * 7) % 64)
            _grain = 40 if ((_x ^ _y) & 3) == 0 else 0
            _buf[_i + 0] = min(255, _v + _grain)
            _buf[_i + 1] = _v
            _buf[_i + 2] = max(0, 255 - _v)
            _buf[_i + 3] = 255
    with open(IN, "wb") as _f:
        _f.write(_buf)
    print("WARN 未找到 probe_in.raw，改用合成纹理帧（无人脸，美白不会生效）")

# BeautySelfTest 的路径参数是窄字符（const char* + fopen），路径里含非 ASCII 时
# 在部分系统码页下会打不开文件。把非 ASCII 的输入/输出先中转到一个纯 ASCII 的
# 临时目录，跑完再搬回来，这样脚本放在中文目录下也能用。
_STAGE = None


def _stage_ascii(path, need_exists):
    global _STAGE
    try:
        path.encode("ascii")
        return path
    except UnicodeEncodeError:
        pass
    if _STAGE is None:
        _STAGE = tempfile.mkdtemp(prefix="nama_selftest_")
    dst = os.path.join(_STAGE, os.path.basename(path))
    if need_exists:
        shutil.copy2(path, dst)
    return dst


# 用法: selftest_ofx.py [blur] [whiten]
BLUR = float(sys.argv[1]) if len(sys.argv) > 1 else 70.0
WHITEN = float(sys.argv[2]) if len(sys.argv) > 2 else 15.0

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.LoadLibraryExW.argtypes = [ctypes.c_wchar_p, ctypes.c_void_p, ctypes.c_uint32]
k32.LoadLibraryExW.restype = ctypes.c_void_p

# LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR(0x100) | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS(0x1000)
# 让 opengl32/gdi32 及引擎 DLL 能被正确解析
h = k32.LoadLibraryExW(DLL, None, 0x1100)
if not h:
    print("FAIL 无法加载插件 DLL, err=%d" % ctypes.get_last_error())
    sys.exit(1)
print("插件 DLL 加载成功: 0x%x" % h)

lib = ctypes.WinDLL(DLL, handle=h)
lib.BeautySelfTest.restype = ctypes.c_int
lib.BeautySelfTest.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p,
                             ctypes.c_int, ctypes.c_int, ctypes.c_double, ctypes.c_double,
                             ctypes.c_char_p, ctypes.c_int]
lib.BeautyVersion.restype = ctypes.c_ulong
lib.BeautyVersion.argtypes = []

print("BeautyVersion() = %d" % lib.BeautyVersion())

src = open(IN, "rb").read()
if len(src) != W * H * 4:
    print("FAIL 输入尺寸不符: %d != %d" % (len(src), W * H * 4))
    sys.exit(1)

if not os.path.isfile(IN):
    print("FAIL 找不到输入文件: %s" % IN)
    sys.exit(1)

IN_USE = _stage_ascii(IN, True)
OUT_USE = _stage_ascii(OUT, False)

msg = ctypes.create_string_buffer(1024)
rc = lib.BeautySelfTest(ENGINE.encode("utf-8"),
                      IN_USE.encode("utf-8"),
                      OUT_USE.encode("utf-8"),
                      W, H,
                      BLUR, WHITEN,
                      msg, 1024)
print("BeautySelfTest rc=%d" % rc)
print("msg=%s" % msg.value.decode("utf-8", "replace"))

if rc != 0 or not os.path.isfile(OUT_USE):
    print("FAIL 未产出输出文件")
    sys.exit(1)
if OUT_USE != OUT:
    shutil.copy2(OUT_USE, OUT)

dst = open(OUT_USE, "rb").read()
print("输出字节数=%d (期望 %d)" % (len(dst), W * H * 4))

# 量化像素改动
n = min(len(src), len(dst))
tot = 0
cnt = 0
mx = 0
changed = 0
step = 4  # 按像素采样
for i in range(0, n - 3, step):
    a0, b0 = src[i], dst[i]
    a1, b1 = src[i + 1], dst[i + 1]
    a2, b2 = src[i + 2], dst[i + 2]
    d = max(abs(a0 - b0), abs(a1 - b1), abs(a2 - b2))
    tot += d
    cnt += 1
    if d > mx:
        mx = d
    if d >= 3:
        changed += 1

print("采样像素=%d 平均最大通道差=%.3f 最大差=%d" % (cnt, tot / max(cnt, 1), mx))
print("改动>=3 的像素占比=%.2f%%" % (100.0 * changed / max(cnt, 1)))

# 期望：给了效果就一定要改动；全 0 时必须几乎不变（透传）
if BLUR == 0 and WHITEN == 0:
    verdict = "PASS(透传)" if changed < cnt * 0.005 else "FAIL(全0却改动了画面)"
else:
    verdict = "PASS" if (mx >= 3 and changed > cnt * 0.01) else "FAIL"
print("结论: %s" % verdict)
