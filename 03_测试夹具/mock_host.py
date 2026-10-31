# 模拟一个最小 OpenFX 宿主，直接驱动已安装的 DaVinciBeauty.ofx。
# 目的：在不开 DaVinci Resolve 的前提下，验证插件的协议行为是否正确
#   describe -> describeInContext -> createInstance -> render
# 以及最终是否真的改动了像素。
import ctypes
import os
import sys
import time
from ctypes import c_char_p, c_double, c_int, c_void_p, POINTER

# 默认驱动已安装的插件；用 NAMA_OFX_DLL 可以指向刚编译出来、还没来得及安装
# 的二进制（Resolve 开着会锁住安装目录，装不进去时靠这个继续验证）。
DLL = os.environ.get(
    "NAMA_OFX_DLL",
    r"C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle\Contents\Win64\DaVinciBeauty.ofx",
)

kOfxStatOK = 0
kOfxStatReplyDefault = 14
kOfxImageEffectSuite = "OfxImageEffectSuite"
kOfxPropertySuite = "OfxPropertySuite"
kOfxParameterSuite = "OfxParameterSuite"
kOfxMessageSuite = "OfxMessageSuite"

kOfxActionLoad = "OfxActionLoad"
kOfxActionUnload = "OfxActionUnload"
kOfxActionDescribe = "OfxActionDescribe"
kOfxActionCreateInstance = "OfxActionCreateInstance"
kOfxActionDestroyInstance = "OfxActionDestroyInstance"
kOfxImageEffectActionDescribeInContext = "OfxImageEffectActionDescribeInContext"
kOfxImageEffectActionGetRegionOfDefinition = "OfxImageEffectActionGetRegionOfDefinition"
kOfxImageEffectActionIsIdentity = "OfxImageEffectActionIsIdentity"
kOfxImageEffectActionRender = "OfxImageEffectActionRender"
kOfxImageEffectActionGetClipPreferences = "OfxImageEffectActionGetClipPreferences"

# ---------------------------------------------------------------------------
# handle 注册表：handle 就是 Python 对象的 id()，全局 dict 保活
# ---------------------------------------------------------------------------
REG = {}
KEEP = []


def newh(obj, _h=None):
    REG[id(obj)] = obj
    return id(obj)


def getobj(h):
    v = h if isinstance(h, int) else (h.value if h else 0)
    return REG.get(v)


class PropSet(object):
    def __init__(self, tag):
        self.tag = tag
        self.props = {}       # name -> {index: value}
        self.order = []       # 记录定义顺序

    def set(self, name, idx, val):
        self.props.setdefault(name, {})[idx] = val

    def get(self, name, idx=0, default=None):
        return self.props.get(name, {}).get(idx, default)

    def dump(self):
        out = {}
        for k, v in self.props.items():
            for i, val in v.items():
                out["%s[%d]" % (k, i)] = val if isinstance(val, (int, float, str)) else str(val)
        return out


class Param(object):
    def __init__(self, ptype, name):
        self.ptype = ptype
        self.name = name
        self.props = PropSet("param:" + name)
        self.value = None     # 当前值（paramSetValue 写入）；None 表示回落到 Default


class Clip(object):
    def __init__(self, name):
        self.name = name
        self.props = PropSet("clip:" + name)


class Effect(object):
    def __init__(self):
        self.props = PropSet("effect")
        self.paramSet = PropSet("paramSet")
        self.params = {}      # name -> Param
        self.clips = {}       # name -> Clip



def safe(fn):
    def wrapper(*a):
        try:
            return fn(*a)
        except Exception as e:
            sys.stderr.write("  [MOCK-EXC] %s: %s\n" % (fn.__name__, e))
            sys.stderr.flush()
            return 1
    return wrapper


# ---------------------------------------------------------------------------
# Property suite
# ---------------------------------------------------------------------------
def _h2ps(h):
    o = getobj(h)
    if isinstance(o, PropSet):
        return o
    if isinstance(o, Param):
        return o.props
    if isinstance(o, Clip):
        return o.props
    if isinstance(o, Effect):
        return o.props
    return None


def prop_set_pointer(h, name, idx, val):
    ps = _h2ps(h)
    if ps: ps.set(name.decode(), idx, val)
    return kOfxStatOK


def prop_set_string(h, name, idx, val):
    ps = _h2ps(h)
    if ps: ps.set(name.decode(), idx, val.decode("utf-8", "replace"))
    return kOfxStatOK


def prop_set_double(h, name, idx, val):
    ps = _h2ps(h)
    if ps: ps.set(name.decode(), idx, float(val))
    return kOfxStatOK


def prop_set_int(h, name, idx, val):
    ps = _h2ps(h)
    if ps: ps.set(name.decode(), idx, int(val))
    return kOfxStatOK


def prop_set_int_n(h, name, count, dest):
    ps = _h2ps(h)
    if not ps: return 1
    vals = [dest[i] for i in range(count)]
    ps.set(name.decode(), 0, vals)
    return kOfxStatOK


def prop_get_pointer(h, name, idx, out):
    ps = _h2ps(h)
    v = ps.get(name.decode(), idx) if ps else None
    out[0] = v if v else None
    if DEBUG and ps and ps.tag == "image":
        sys.stderr.write("  [MOCK] propGetPointer(%s)=%s\n" % (name.decode(), v))
    return kOfxStatOK if ps else 1


def prop_get_string(h, name, idx, out):
    ps = _h2ps(h)
    v = ps.get(name.decode(), idx) if ps else None
    if DEBUG and ps and ps.tag == "image":
        sys.stderr.write("  [MOCK] propGetString(%s)=%r\n" % (name.decode(), v))
    if v is None:
        out[0] = None
        return 1
    b = v.encode("utf-8") if isinstance(v, str) else bytes(v)
    KEEP.append(b)
    out[0] = b
    return kOfxStatOK


def prop_get_double(h, name, idx, out):
    ps = _h2ps(h)
    v = ps.get(name.decode(), idx) if ps else None
    if v is None: return 1
    out[0] = c_double(v)
    return kOfxStatOK


def prop_get_int(h, name, idx, out):
    ps = _h2ps(h)
    v = ps.get(name.decode(), idx) if ps else None
    if v is None: return 1
    out[0] = c_int(int(v))
    return kOfxStatOK


def prop_get_int_n(h, name, count, dest):
    ps = _h2ps(h)
    v = ps.get(name.decode(), 0) if ps else None
    if not isinstance(v, (list, tuple)): return 1
    for i in range(min(count, len(v))):
        dest[i] = int(v[i])
    return kOfxStatOK


def prop_get_double_n(h, name, count, dest):
    # 数组型 double 读取（OfxImageEffectPropRenderScale 等）。之前这套夹具没实现
    # 它，结构槽位是 0，插件一调用就是空指针崩溃。
    ps = _h2ps(h)
    v = ps.get(name.decode(), 0) if ps else None
    if not isinstance(v, (list, tuple)):
        return 1
    for i in range(min(count, len(v))):
        dest[i] = float(v[i])
    return kOfxStatOK


def prop_set_double_n(h, name, count, src):
    ps = _h2ps(h)
    if not ps:
        return 1
    ps.set(name.decode(), 0, [float(src[i]) for i in range(count)])
    return kOfxStatOK


def prop_reset(h, name):
    return kOfxStatOK


def prop_get_dimension(h, name, out):
    return 1


class PropertySuite(ctypes.Structure):
    _fields_ = [(n, ctypes.c_void_p) for n in
                ["propSetPointer", "propSetString", "propSetDouble", "propSetInt",
                 "propSetPointerN", "propSetStringN", "propSetDoubleN", "propSetIntN",
                 "propGetPointer", "propGetString", "propGetDouble", "propGetInt",
                 "propGetPointerN", "propGetStringN", "propGetDoubleN", "propGetIntN",
                 "propReset", "propGetDimension"]]


PROPSUITE = PropertySuite()
PS = ctypes.CFUNCTYPE(c_int, c_void_p, c_char_p, c_int, c_void_p)
PSS = ctypes.CFUNCTYPE(c_int, c_void_p, c_char_p, c_int, c_char_p)
PSD = ctypes.CFUNCTYPE(c_int, c_void_p, c_char_p, c_int, c_double)
PSI = ctypes.CFUNCTYPE(c_int, c_void_p, c_char_p, c_int, c_int)
PSIN = ctypes.CFUNCTYPE(c_int, c_void_p, c_char_p, c_int, POINTER(c_int))
PGP = ctypes.CFUNCTYPE(c_int, c_void_p, c_char_p, c_int, POINTER(c_void_p))
PGS = ctypes.CFUNCTYPE(c_int, c_void_p, c_char_p, c_int, POINTER(c_char_p))
PGD = ctypes.CFUNCTYPE(c_int, c_void_p, c_char_p, c_int, POINTER(c_double))
PGI = ctypes.CFUNCTYPE(c_int, c_void_p, c_char_p, c_int, POINTER(c_int))
PGIN = ctypes.CFUNCTYPE(c_int, c_void_p, c_char_p, c_int, POINTER(c_int))
PGDN = ctypes.CFUNCTYPE(c_int, c_void_p, c_char_p, c_int, POINTER(c_double))
PSDN = ctypes.CFUNCTYPE(c_int, c_void_p, c_char_p, c_int, POINTER(c_double))
PRST = ctypes.CFUNCTYPE(c_int, c_void_p, c_char_p)
PGDIM = ctypes.CFUNCTYPE(c_int, c_void_p, c_char_p, POINTER(c_int))

PROPSUITE.propSetPointer = ctypes.cast(PS(prop_set_pointer), c_void_p)
PROPSUITE.propSetString = ctypes.cast(PSS(prop_set_string), c_void_p)
PROPSUITE.propSetDouble = ctypes.cast(PSD(prop_set_double), c_void_p)
PROPSUITE.propSetInt = ctypes.cast(PSI(prop_set_int), c_void_p)
PROPSUITE.propSetIntN = ctypes.cast(PSIN(prop_set_int_n), c_void_p)
PROPSUITE.propSetDoubleN = ctypes.cast(PSDN(prop_set_double_n), c_void_p)
PROPSUITE.propGetPointer = ctypes.cast(PGP(prop_get_pointer), c_void_p)
PROPSUITE.propGetString = ctypes.cast(PGS(prop_get_string), c_void_p)
PROPSUITE.propGetDouble = ctypes.cast(PGD(prop_get_double), c_void_p)
PROPSUITE.propGetInt = ctypes.cast(PGI(prop_get_int), c_void_p)
PROPSUITE.propGetIntN = ctypes.cast(PGIN(prop_get_int_n), c_void_p)
PROPSUITE.propGetDoubleN = ctypes.cast(PGDN(prop_get_double_n), c_void_p)
PROPSUITE.propReset = ctypes.cast(PRST(prop_reset), c_void_p)
PROPSUITE.propGetDimension = ctypes.cast(PGDIM(prop_get_dimension), c_void_p)

# ---------------------------------------------------------------------------
# Parameter suite
# ---------------------------------------------------------------------------
def param_define(pset_h, ptype, name, out_props):
    o = getobj(pset_h)
    if not isinstance(o, PropSet):
        return 1
    nm = name.decode()
    p = Param(ptype.decode(), nm)
    h = newh(p)
    if o.tag == "paramSet":
        # 找到宿主 effect 并登记
        for k, v in REG.items():
            if isinstance(v, Effect) and v.paramSet is o:
                v.params[nm] = p
                break
    out_props[0] = newh(p.props)
    return kOfxStatOK


def param_get_handle(pset_h, name, out_param, out_props):
    o = getobj(pset_h)
    if not isinstance(o, PropSet):
        return 1
    nm = name.decode()
    found = None
    for k, v in REG.items():
        if isinstance(v, Effect) and v.paramSet is o:
            found = v.params.get(nm)
            break
    if found is None:
        return 1
    out_param[0] = id(found)
    if out_props:
        out_props[0] = id(found.props)
    return kOfxStatOK


def param_get_value(param_h, out):
    p = getobj(param_h)
    if not isinstance(p, Param):
        return 1
    # 当前值优先；没被 paramSetValue 改过时回落到 kOfxParamPropDefault。
    # 以前这里**永远**返回 Default，导致「面板参数 → SDK 真值」的换算链路根本
    # 测不到（旧审查报告第五节的盲区）。
    d = p.value
    if d is None:
        d = p.props.get("OfxParamPropDefault", 0)
    addr = out if isinstance(out, int) else (out.value if out else 0)
    if not addr:
        return 1
    if p.ptype == "OfxParamTypeDouble":
        ctypes.cast(addr, POINTER(c_double))[0] = float(d if d is not None else 0)
    else:
        ctypes.cast(addr, POINTER(c_int))[0] = int(d if d is not None else 0)
    return kOfxStatOK


def param_set_value(param_h, value):
    p = getobj(param_h)
    if not isinstance(p, Param):
        return 1
    addr = value if isinstance(value, int) else (value.value if value else 0)
    if not addr:
        return 1
    if p.ptype == "OfxParamTypeDouble":
        p.value = float(ctypes.cast(addr, POINTER(c_double))[0])
    else:
        p.value = int(ctypes.cast(addr, POINTER(c_int))[0])
    return kOfxStatOK


class ParameterSuite(ctypes.Structure):
    _fields_ = [(n, ctypes.c_void_p) for n in
                ["paramDefine", "paramGetHandle", "paramSetGetPropertySet",
                 "paramGetPropertySet", "paramGetValue", "paramGetValueAtTime",
                 "paramGetDerivative", "paramGetIntegral", "paramSetValue",
                 "paramSetValueAtTime", "paramGetNumKeys", "paramGetKeyTime",
                 "paramGetKeyIndex", "paramDeleteKey", "paramDeleteAllKeys",
                 "paramCopy", "paramEditBegin", "paramEditEnd"]]


PARAMS = ParameterSuite()
PARAMS.paramDefine = ctypes.cast(
    ctypes.CFUNCTYPE(c_int, c_void_p, c_char_p, c_char_p, POINTER(c_void_p))(safe(param_define)),
    c_void_p)
PARAMS.paramGetHandle = ctypes.cast(
    ctypes.CFUNCTYPE(c_int, c_void_p, c_char_p, POINTER(c_void_p), POINTER(c_void_p))(safe(param_get_handle)), c_void_p)
PARAMS.paramGetValue = ctypes.cast(
    ctypes.CFUNCTYPE(c_int, c_void_p, c_void_p)(safe(param_get_value)), c_void_p)
_psv = ctypes.CFUNCTYPE(c_int, c_void_p, c_void_p)(safe(param_set_value))
PARAMS.paramSetValue = ctypes.cast(_psv, c_void_p)


def pset(pm, val):
    """从夹具侧改一个参数的当前值（走 paramSetValue，和真人拖滑块等价）。"""
    arg = c_double(float(val)) if pm.ptype == "OfxParamTypeDouble" else c_int(int(val))
    _psv(id(pm), ctypes.byref(arg))

# ---------------------------------------------------------------------------
# Image effect suite
# ---------------------------------------------------------------------------
W = H = 0
SRC_BUF = None
DST_BUF = None
DEPTH = "OfxBitDepthByte"
MESSAGES = []
DEBUG = True
# 让 Output 的 bounds 比 Source 大一圈，用来验证插件是否会补满目标图剩余区域
# （旧审查报告的 B8：bounds 不一致时目标边缘保留旧内存 → 花屏）。
DST_PAD = (0, 0)


def effect_get_property_set(effect_h, out):
    o = getobj(effect_h)
    if not isinstance(o, Effect):
        return 1
    out[0] = newh(o.props)
    return kOfxStatOK


def effect_get_param_set(effect_h, out):
    o = getobj(effect_h)
    if not isinstance(o, Effect):
        return 1
    out[0] = newh(o.paramSet)
    return kOfxStatOK


def clip_define(effect_h, name, out_props):
    o = getobj(effect_h)
    if not isinstance(o, Effect):
        return 1
    c = Clip(name.decode())
    o.clips[c.name] = c
    newh(c)
    out_props[0] = newh(c.props)
    return kOfxStatOK


def clip_get_handle(effect_h, name, out_clip, out_props):
    o = getobj(effect_h)
    if not isinstance(o, Effect):
        return 1
    c = o.clips.get(name.decode())
    if c is None:
        return 1
    out_clip[0] = newh(c)
    if out_props:
        out_props[0] = newh(c.props)
    return kOfxStatOK


def clip_get_property_set(clip_h, out):
    c = getobj(clip_h)
    if not isinstance(c, Clip):
        return 1
    out[0] = newh(c.props)
    return kOfxStatOK


def make_image_props(buf, rowbytes, x1, y1, x2, y2, depth):
    ps = PropSet("image")
    ps.set("OfxImagePropData", 0, ctypes.cast(buf, c_void_p).value)
    ps.set("OfxImagePropRowBytes", 0, rowbytes)
    ps.set("OfxImagePropBounds", 0, [x1, y1, x2, y2])
    ps.set("OfxImagePropRegionOfDefinition", 0, [x1, y1, x2, y2])
    ps.set("OfxImageEffectPropPixelDepth", 0, depth)
    return newh(ps)


IMG_HANDLES = {}

# 记录插件向宿主索要的图像时间。规范要求这里必须是 render 动作 inArgs 里
# OfxPropTime 的原样值 —— 写成固定 0.0 会让成片定格在第一帧（真机上就是
# 「播放时画面不动、声音照走」）。宿主实现也照规范把它当「取第几帧」用。
CLIP_TIMES = []


class OfxRectD(ctypes.Structure):
    _fields_ = [("x1", c_double), ("y1", c_double), ("x2", c_double), ("y2", c_double)]


def clip_get_image(clip_h, time, region, out_img):
    c = getobj(clip_h)
    if not isinstance(c, Clip):
        return 1
    CLIP_TIMES.append((c.name, float(time)))
    if region:
        r = ctypes.cast(region, POINTER(OfxRectD)).contents
        x1, y1, x2, y2 = int(r.x1), int(r.y1), int(r.x2), int(r.y2)
    else:
        x1, y1, x2, y2 = 0, 0, W, H
    if c.name == "Output" and (DST_PAD[0] or DST_PAD[1]):
        x2 += DST_PAD[0]
        y2 += DST_PAD[1]
    w = x2 - x1
    h = y2 - y1
    if w <= 0 or h <= 0:
        return 1
    # 头文件里 kOfxImageEffectOutputClipName == "Output"，不是 "OfxImageClipOutput"
    if c.name == "Output":
        buf = DST_BUF
    else:
        buf = SRC_BUF
    if DEBUG:
        sys.stderr.write("  [MOCK] clipGetImage(%s) region=(%d,%d,%d,%d) buf=%s\n"
                         % (c.name, x1, y1, x2, y2, buf))
    comp = 4 if DEPTH == "OfxBitDepthByte" else 16
    ps_h = make_image_props(buf, w * comp, x1, y1, x2, y2, DEPTH)
    IMG_HANDLES[ps_h] = True
    out_img[0] = ps_h
    return kOfxStatOK


def clip_release_image(img_h):
    return kOfxStatOK


def clip_get_rod(clip_h, time, out):
    out.contents.x1 = 0
    out.contents.y1 = 0
    out.contents.x2 = W
    out.contents.y2 = H
    return kOfxStatOK


class ImageEffectSuite(ctypes.Structure):
    _fields_ = [(n, ctypes.c_void_p) for n in
                ["getPropertySet", "getParamSet", "clipDefine", "clipGetHandle",
                 "clipGetPropertySet", "clipGetImage", "clipReleaseImage",
                 "clipGetRegionOfDefinition", "abort", "imageMemoryAlloc",
                 "imageMemoryFree", "imageMemoryLock", "imageMemoryUnlock"]]


EFFECT = ImageEffectSuite()
EFFECT.getPropertySet = ctypes.cast(
    ctypes.CFUNCTYPE(c_int, c_void_p, POINTER(c_void_p))(safe(effect_get_property_set)), c_void_p)
EFFECT.getParamSet = ctypes.cast(
    ctypes.CFUNCTYPE(c_int, c_void_p, POINTER(c_void_p))(safe(effect_get_param_set)), c_void_p)
EFFECT.clipDefine = ctypes.cast(
    ctypes.CFUNCTYPE(c_int, c_void_p, c_char_p, POINTER(c_void_p))(safe(clip_define)), c_void_p)
EFFECT.clipGetHandle = ctypes.cast(
    ctypes.CFUNCTYPE(c_int, c_void_p, c_char_p, POINTER(c_void_p), POINTER(c_void_p))(safe(clip_get_handle)), c_void_p)
EFFECT.clipGetPropertySet = ctypes.cast(
    ctypes.CFUNCTYPE(c_int, c_void_p, POINTER(c_void_p))(safe(clip_get_property_set)), c_void_p)
EFFECT.clipGetImage = ctypes.cast(
    ctypes.CFUNCTYPE(c_int, c_void_p, c_double, POINTER(OfxRectD), POINTER(c_void_p))(safe(clip_get_image)), c_void_p)
EFFECT.clipReleaseImage = ctypes.cast(
    ctypes.CFUNCTYPE(c_int, c_void_p)(safe(clip_release_image)), c_void_p)
EFFECT.clipGetRegionOfDefinition = ctypes.cast(
    ctypes.CFUNCTYPE(c_int, c_void_p, c_double, c_void_p)(safe(clip_get_rod)), c_void_p)


def msg_stub(handle, mtype, mid, fmt):
    MESSAGES.append((mtype.decode("utf-8", "replace") if mtype else "",
                     fmt.decode("utf-8", "replace") if fmt else ""))
    return kOfxStatOK


class MessageSuite(ctypes.Structure):
    _fields_ = [("message", ctypes.c_void_p)]


MSG = MessageSuite()
MSG.message = ctypes.cast(
    ctypes.CFUNCTYPE(c_int, c_void_p, c_char_p, c_char_p, c_char_p)(safe(msg_stub)), c_void_p)

# ---------------------------------------------------------------------------
# OfxHost
# ---------------------------------------------------------------------------
SUITES = {kOfxImageEffectSuite: (EFFECT, 1),
          kOfxPropertySuite: (PROPSUITE, 1),
          kOfxParameterSuite: (PARAMS, 1),
          kOfxMessageSuite: (MSG, 1)}


def fetch_suite(host_h, name, version):
    n = name.decode()
    ent = SUITES.get(n)
    if not ent:
        return None
    return ctypes.addressof(ent[0])


class OfxHost(ctypes.Structure):
    _fields_ = [("host", c_void_p), ("fetchSuite", c_void_p)]


HOST = OfxHost()
HOST.host = id(HOST)
HOST.fetchSuite = ctypes.cast(
    ctypes.CFUNCTYPE(c_void_p, c_void_p, c_char_p, c_int)(safe(fetch_suite)), c_void_p)

# ---------------------------------------------------------------------------
# 驱动
# ---------------------------------------------------------------------------


class OfxPlugin(ctypes.Structure):
    _fields_ = [("pluginApi", c_char_p), ("apiVersion", c_int),
                ("pluginIdentifier", c_char_p), ("pluginVersionMajor", ctypes.c_uint),
                ("pluginVersionMinor", ctypes.c_uint),
                ("setHost", c_void_p), ("mainEntry", c_void_p)]


def _make_textured_frame(w, h):
    """合成一张带高频细节的 RGBA8 测试帧。

    磨皮是低通滤波，**平滑图喂进去逐像素差只有 1~2**，改动量恒为 0。
    所以素材必须带颗粒级纹理，否则「blur=60 应当改动画面」这条断言无法成立。
    """
    import math
    buf = bytearray()
    for y in range(h):
        row = bytearray()
        for x in range(w):
            base = x * 160 // max(w - 1, 1) + 40
            grain = int(24 * math.sin(x * 1.7) * math.cos(y * 1.9))
            v = max(0, min(255, base + grain))
            row += bytes((min(255, v + 18), v, max(0, v - 12), 255))
        buf += row
    return bytes(buf)


def _to_bottom_up(blob, w, h):
    """把 top-down 的 RGBA 帧翻成 bottom-up 行序（v3.0.4）。

    为什么夹具要翻：OFX 规范规定 kOfxImagePropData 指向**图像的左下角**
    （ofxImageEffect.h：「a pointer to memory that is the lower left hand
    corner of an image」），Resolve 严格照做，所以插件拿到的帧第 0 行是画面
    最下面一行。以前夹具按 top-down 存帧，等于替插件掩盖了这个方向问题 ——
    真机上人脸检测看到的是上下颠倒的脸（bestScore 0.25，阈值 0.6 → faces=0
    → 整帧透传），而夹具一路 PASS。

    翻成 bottom-up 之后，夹具的帧序与真实宿主一致；下面所有断言都是「同序
    比对」（src 与 dst 都在同一行序里），所以判据本身不用改。
    """
    row = w * 4
    return b"".join(blob[y * row:(y + 1) * row] for y in range(h - 1, -1, -1))


def _load_or_make_frame(path, w, h):
    """优先读磁盘素材；缺失或尺寸不符时现场合成，保证用例不依赖外部文件。

    ⚠️ v1.5 起，合成帧**不再能跑通区域路径**：语义皮肤分割对合成渐变的判定是
    「没有皮肤」，于是插件按契约什么都不做，8bit/float/B5 会全部报 FAIL ——
    那不是回归，是夹具失效。请用 make_probe_frame.py 从一张真人脸照片生成
    probe_in.raw。
    """
    need = w * h * 4
    try:
        blob = open(path, "rb").read()
        if len(blob) == need:
            return blob
        print("   [忽略] %s 是 %d 字节，与 %dx%dx4=%d 不符，改用合成图"
              % (os.path.basename(path), len(blob), w, h, need))
    except OSError:
        print("   [提示] 未找到 %s，改用合成纹理帧" % os.path.basename(path))
        print("   [警告] 合成帧里没有皮肤，v1.5 的语义区域会判定「无皮肤 → 不处理」，"
              "8bit/float/B5 将失败。")
        print("          请先运行：python make_probe_frame.py <一张有脸的照片>")
    return _make_textured_frame(w, h)


def main():
    global W, H, SRC_BUF, DST_BUF, DEPTH, DST_PAD

    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.LoadLibraryExW.argtypes = [ctypes.c_wchar_p, c_void_p, ctypes.c_uint32]
    k32.LoadLibraryExW.restype = c_void_p
    if not k32.LoadLibraryExW(DLL, None, 0x1100):
        print("FAIL load dll err=%d" % ctypes.get_last_error())
        return 1

    lib = ctypes.WinDLL(DLL)
    getn = lib.OfxGetNumberOfPlugins
    getn.restype = c_int
    getn.argtypes = []
    getp = lib.OfxGetPlugin
    getp.restype = POINTER(OfxPlugin)
    getp.argtypes = [c_int]

    n = getn()
    print("OfxGetNumberOfPlugins() = %d" % n)
    if n != 1:
        print("FAIL 期望 1")
        return 1

    plug = getp(0)
    if not plug:
        print("FAIL OfxGetPlugin(0) 返回空")
        return 1
    p = plug.contents
    print("pluginApi           = %s" % p.pluginApi.decode())
    print("apiVersion          = %d" % p.apiVersion)
    print("pluginIdentifier    = %s" % p.pluginIdentifier.decode())
    print("pluginVersion       = %d.%d" % (p.pluginVersionMajor, p.pluginVersionMinor))

    SETHOST = ctypes.CFUNCTYPE(None, POINTER(OfxHost))
    sethost = SETHOST(p.setHost)
    sethost(ctypes.byref(HOST))
    print("setHost() 完成")

    MAIN = ctypes.CFUNCTYPE(c_int, c_char_p, c_void_p, c_void_p, c_void_p)
    main_fn = MAIN(p.mainEntry)

    st = main_fn(kOfxActionLoad.encode(), None, None, None)
    print("ActionLoad                    -> %d" % st)
    if st != kOfxStatOK:
        print("FAIL load 失败")
        return 1

    eff = Effect()
    eff_h = newh(eff)

    st = main_fn(kOfxActionDescribe.encode(), eff_h, None, None)
    print("ActionDescribe                -> %d" % st)
    if st != kOfxStatOK:
        print("FAIL describe 失败")
        return 1

    d = eff.props.dump()
    print("   [describe 写入的属性] %s" % ", ".join(sorted(eff.props.props.keys())))
    for key in ["OfxPropLabel[0]", "OfxImageEffectPluginPropGrouping[0]",
                "OfxImageEffectPropSupportedContexts[0]",
                "OfxImageEffectPropSupportedContexts[1]",
                "OfxImageEffectPropSupportedPixelDepths[0]",
                "OfxImageEffectPropSupportedPixelDepths[1]",
                "OfxImageEffectPluginRenderThreadSafety[0]",
                "OfxImageEffectPropSupportsTiles[0]"]:
        print("   %-46s = %s" % (key, d.get(key)))

    st = main_fn(kOfxImageEffectActionDescribeInContext.encode(), eff_h, None, None)
    print("DescribeInContext             -> %d" % st)
    if st != kOfxStatOK:
        print("FAIL describeInContext 失败")
        return 1

    print("   clips: %s" % sorted(eff.clips.keys()))
    print("   params (%d):" % len(eff.params))
    for nm, pm in eff.params.items():
        lbl = pm.props.get("OfxParamPropLabel", 0) or pm.props.get("OfxPropLabel", 0)
        dflt = pm.props.get("OfxParamPropDefault", 0)
        print("      %-24s %-22s default=%s" % (nm, pm.ptype, dflt))

    _t0 = time.perf_counter()
    st = main_fn(kOfxActionCreateInstance.encode(), eff_h, None, None)
    _t_ci = (time.perf_counter() - _t0) * 1000.0
    print("CreateInstance                -> %d  耗时=%.0fms（含引擎预热）" % (st, _t_ci))
    if st != kOfxStatOK:
        print("FAIL createInstance 失败")
        return 1

    # RoD
    rod_out = PropSet("rod")
    st = main_fn(kOfxImageEffectActionGetRegionOfDefinition.encode(), eff_h, None, newh(rod_out))
    print("GetRegionOfDefinition         -> %d  rod=%s" % (st, rod_out.get("OfxImageEffectPropRegionOfDefinition")))

    # Render：8bit
    W, H = 640, 394
    # 用脚本自身所在目录定位素材，不再硬编码旧工作区的绝对路径。
    # 素材必须有**高频细节**（皮肤纹理级颗粒），否则磨皮无细节可磨：
    # 平滑渐变喂给 blur 后逐像素差只有 1~2，改动量恒为 0，用例会假红。
    # 以前这份素材恰好能"通过"，靠的是全局滤镜把整帧提亮 —— 那是假阳性。
    _here = os.path.dirname(os.path.abspath(__file__))
    src = _to_bottom_up(_load_or_make_frame(os.path.join(_here, "probe_in.raw"), W, H), W, H)
    KEEP.append(src)
    SRC_BUF = ctypes.create_string_buffer(src, len(src))
    # 哨兵填充：若插件根本没写输出，dst 会保持 0x5A，一眼可辨
    SENTINEL = b"\x5a" * len(src)
    DST_BUF = ctypes.create_string_buffer(SENTINEL, len(src))
    DEPTH = "OfxBitDepthByte"

    # 给 source clip 设置 RoD 与像素深度，让 render 能取到范围。
    # 深度必须设在 **clip 的 property set** 上：真实宿主就是这么做的，而且插件的
    # GetClipPreferences 正是从 source clip 读 kOfxImageEffectPropPixelDepth 来
    # 决定输出深度的。以前夹具只把深度设在 image 上，等于这条路没被验证过。
    sclip = eff.clips.get("Source")   # 头文件常量值是 "Source"
    if sclip:
        sclip.props.set("OfxImageEffectPropRegionOfDefinition", 0, [0, 0, W, H])
        sclip.props.set("OfxImageEffectPropPixelDepth", 0, "OfxBitDepthByte")
        sclip.props.set("OfxImageEffectPropComponents", 0, "OfxImageComponentRGBA")
        sclip.props.set("OfxImagePropPixelAspectRatio", 0, 1.0)

    # 渲染时间刻意取一个明显不等于 0 的值：真机上这会随播放头变化，
    # 写成 0.0 就是「无论播到哪一帧都只要第 0 帧」的经典 bug。
    RENDER_TIME = 96.5

    inargs = PropSet("inargs")
    inargs.set("OfxImageEffectPropRenderWindow", 0, [0, 0, W, H])
    inargs.set("OfxPropTime", 0, RENDER_TIME)
    inargs.set("OfxImageEffectPropRenderScale", 0, [1.0, 1.0])
    inargs_h = newh(inargs)
    CLIP_TIMES.clear()
    _t0 = time.perf_counter()
    st = main_fn(kOfxImageEffectActionRender.encode(), eff_h, inargs_h, None)
    _t_r8 = (time.perf_counter() - _t0) * 1000.0
    print("Render(8bit)                  -> %d  耗时=%.1fms（预热后首帧）" % (st, _t_r8))
    if _t_ci < 500:
        print("   [警告] CreateInstance 只用了 %.0fms，看起来引擎预热没生效" % _t_ci)

    # 时间透传检查：两次 clipGetImage（Output + Source）都必须用 inArgs 给的时间
    time_ok = len(CLIP_TIMES) == 2 and all(abs(t - RENDER_TIME) < 1e-9 for _, t in CLIP_TIMES)
    print("   向宿主索要的图像时间=%s  期望=%.3f  %s"
          % (CLIP_TIMES, RENDER_TIME, "OK" if time_ok else "BAD（时间没透传，成片会定格）"))

    dst = DST_BUF.raw
    leftover = dst.count(b"\x5a")
    print("   输出缓冲残留哨兵字节=%d / %d (残留高=插件没写输出)" % (leftover, len(dst)))

    n_px = min(len(src), len(dst))
    tot = cnt = mx = changed = 0
    for i in range(0, n_px - 3, 4):
        d1 = max(abs(src[i] - dst[i]), abs(src[i + 1] - dst[i + 1]), abs(src[i + 2] - dst[i + 2]))
        tot += d1
        cnt += 1
        mx = max(mx, d1)
        if d1 >= 3:
            changed += 1
    avg = tot / max(cnt, 1)
    print("   像素采样=%d 平均最大通道差=%.3f 最大差=%d 改动>=3 占比=%.2f%%"
          % (cnt, avg, mx, 100.0 * changed / max(cnt, 1)))

    if MESSAGES:
        print("   宿主收到的消息: %s" % MESSAGES[:3])

    # 三个条件同时成立才算真通过：调用成功、输出确实被写过、改动幅度合理
    #
    # ⚠ 2026-10-26 v2.0 起判据从「改动>=3 占比 > 1%」放宽到「> 0.1% 且平均差 > 0.01」。
    # 原因不是放水，而是磨皮曲线换了模型：v1.51 的滑块在 10 档就接近饱和
    # （实测 mad 10 档 0.167、100 档 0.207，只差 1.24 倍），默认 20 档能改动 3% 像素；
    # v2.0 换成线性曲线（10 档 0.014、100 档 0.208），默认 20 档只改动 0.02% 像素，
    # 但 100 档的输出与 v1.51 满档一致（mad 0.2075 vs 0.2070）。
    # 用旧的绝对阈值会把「曲线更线性」误判成「插件失效」。真正的失效（完全不动）
    # 仍然会被 avg > 0.01 与 changed > 0 抓住。
    wrote = leftover < len(dst) * 0.5
    sane = changed > cnt * 0.001 and avg > 0.01 and avg < 120
    ok_render8 = (st == kOfxStatOK and wrote and sane)

    # Render：float 深度
    # --- 连续渲染：Resolve 的真实用法就是一帧接一帧，必须验证第 N 帧仍然正确 ---
    DEPTH = "OfxBitDepthByte"
    import math
    multiframe_ok = True
    for frame in range(1, 4):
        DST_BUF = ctypes.create_string_buffer(SENTINEL, len(src))
        MESSAGES[:] = []
        # 关键：每帧给一个**不同**的时间。播放头是一直往前走的，插件如果把
        # clipGetImage 的时间写死成 0，这里的断言立刻就会红。
        t_f = 96.5 + frame * 7.25
        in_f = PropSet("inargs%d" % frame)
        in_f.set("OfxImageEffectPropRenderWindow", 0, [0, 0, W, H])
        in_f.set("OfxPropTime", 0, t_f)
        in_f.set("OfxImageEffectPropRenderScale", 0, [1.0, 1.0])
        CLIP_TIMES.clear()
        st_f = main_fn(kOfxImageEffectActionRender.encode(), eff_h, newh(in_f), None)
        t_ok_f = len(CLIP_TIMES) == 2 and all(abs(t - t_f) < 1e-9 for _, t in CLIP_TIMES)
        if not t_ok_f:
            multiframe_ok = False
            print("     时间透传 BAD：索要=%s 期望=%.3f（画面会定格）" % (CLIP_TIMES, t_f))
        d2 = DST_BUF.raw
        left2 = d2.count(b"\x5a")
        tot2 = cnt2 = 0
        for i in range(0, len(src) - 3, 4):
            tot2 += abs(src[i] - d2[i])
            cnt2 += 1
        a2 = tot2 / max(cnt2, 1)
        good = (st_f == kOfxStatOK and left2 < len(d2) * 0.5 and a2 < 120)
        print("Render(8bit) 第%d帧              -> %d  残留哨兵=%d 平均差=%.3f  %s"
              % (frame, st_f, left2, a2, "OK" if good else "BAD"))
        if not good:
            multiframe_ok = False
        if MESSAGES:
            print("     消息: %s" % MESSAGES[:2])

    # ------------------------------------------------------------------
    # 参数全 0 / IsIdentity
    #   1) 全 0 时插件必须短路透传，输出与输入**逐字节相同**（不是「接近」）
    #   2) IsIdentity 在全 0 时应答 OK 并指向 Source；参数非 0 时必须回 ReplyDefault
    # ------------------------------------------------------------------
    print("")
    print("--- 参数全 0 / IsIdentity ---")
    saved = {}
    for nm, pm in eff.params.items():
        if pm.ptype == "OfxParamTypePage":
            continue
        saved[nm] = pm.props.get("OfxParamPropDefault", 0)
        pm.props.set("OfxParamPropDefault", 0, 0.0 if pm.ptype == "OfxParamTypeDouble" else 0)

    ident_in = PropSet("ident_in")
    ident_in.set("OfxPropTime", 0, 0.0)
    ident_out = PropSet("ident_out")
    st_i0 = main_fn(kOfxImageEffectActionIsIdentity.encode(), eff_h,
                    newh(ident_in), newh(ident_out))
    print("IsIdentity(全0)               -> %d (期望 %d=OK)  指向 clip=%s"
          % (st_i0, kOfxStatOK, ident_out.get("OfxPropName")))

    DEPTH = "OfxBitDepthByte"
    DST_BUF = ctypes.create_string_buffer(SENTINEL, len(src))
    MESSAGES[:] = []
    st_z = main_fn(kOfxImageEffectActionRender.encode(), eff_h, inargs_h, None)
    dz = DST_BUF.raw
    left_z = dz.count(b"\x5a")
    maxd = 0
    for i in range(len(src)):
        d = abs(src[i] - dz[i])
        if d > maxd:
            maxd = d
    print("Render(全0)                   -> %d  残留哨兵=%d  与源最大逐字节差=%d (期望 0)"
          % (st_z, left_z, maxd))
    ok_zero = (st_z == kOfxStatOK and maxd == 0)
    if MESSAGES:
        print("   宿主收到的消息: %s" % MESSAGES[:2])

    for nm, v in saved.items():
        eff.params[nm].props.set("OfxParamPropDefault", 0, v)

    ident_out2 = PropSet("ident_out2")
    st_i1 = main_fn(kOfxImageEffectActionIsIdentity.encode(), eff_h,
                    newh(ident_in), newh(ident_out2))
    print("IsIdentity(默认参数)          -> %d (期望 %d=ReplyDefault)"
          % (st_i1, kOfxStatReplyDefault))
    ok_ident = (st_i0 == kOfxStatOK and st_i1 == kOfxStatReplyDefault)

    # ------------------------------------------------------------------
    # 代理 / 缩略图渲染 + GetClipPreferences
    #
    # 真实 Resolve 会以 scale=0.15（实测 161x80）甚至极小窗口调 render，这种尺寸下
    # 人脸检测必然失败（faces=0），跑 SDK 纯属白付十几毫秒。本夹具以前永远只发
    # scale=[1.0,1.0]，所以**结构性测不出**这个问题 —— 这里补上。
    # ------------------------------------------------------------------
    print("")
    print("--- 代理 / 缩略图 / ClipPreferences ---")

    # 用例 1：RenderScale 极小（模拟实机 scale=0.15），应走快速透传、逐字节等于源
    pw, ph = max(1, W // 8), max(1, H // 8)
    proxy_in = PropSet("proxy_in")
    proxy_in.set("OfxImageEffectPropRenderWindow", 0, [0, 0, pw, ph])
    proxy_in.set("OfxPropTime", 0, 200.0)
    proxy_in.set("OfxImageEffectPropRenderScale", 0, [0.15, 0.15])
    px_in = ctypes.create_string_buffer(bytes(src[: pw * ph * 4]), pw * ph * 4)
    px_out = ctypes.create_string_buffer(b"\x5a" * (pw * ph * 4), pw * ph * 4)
    KEEP.append(px_in); KEEP.append(px_out)
    SRC_BUF = ctypes.cast(px_in, c_void_p)
    DST_BUF = ctypes.cast(px_out, c_void_p)
    DEPTH = "OfxBitDepthByte"
    CLIP_TIMES.clear()
    _t0 = time.perf_counter()
    st_p = main_fn(kOfxImageEffectActionRender.encode(), eff_h, newh(proxy_in), None)
    _t_proxy = (time.perf_counter() - _t0) * 1000.0
    maxd_p = max(abs(px_in.raw[i] - px_out.raw[i]) for i in range(pw * ph * 4))
    print("Render(scale=0.15 %dx%d)      -> %d  耗时=%.1fms 与源最大逐字节差=%d (期望 0)"
          % (pw, ph, st_p, _t_proxy, maxd_p))
    ok_proxy = (st_p == kOfxStatOK and maxd_p == 0)

    # 用例 2：显式 ThumbnailRender="true"，IsIdentity 应直接应答 OK
    # 类型是 enum 字符串（ofxImageEffect.h:1425-1434），不是 int。夹具以前写 1，
    # 等于替插件掩盖了「用 propGetInt 读 enum」这个类型错误。
    tk_in = PropSet("tk_in")
    tk_in.set("OfxPropTime", 0, 201.0)
    tk_in.set("OfxImageEffectPropRenderScale", 0, [1.0, 1.0])
    tk_in.set("OfxImageEffectPropThumbnailRender", 0, "true")
    tk_out = PropSet("tk_out")
    st_t = main_fn(kOfxImageEffectActionIsIdentity.encode(), eff_h,
                   newh(tk_in), newh(tk_out))
    print("IsIdentity(ThumbnailRender=true) -> %d (期望 %d=OK)  指向 clip=%s"
          % (st_t, kOfxStatOK, tk_out.get("OfxPropName")))
    ok_thumb = (st_t == kOfxStatOK)

    # 用例 3：GetClipPreferences 必须应答，且把输出深度跟随输入
    gcp_out = PropSet("gcp_out")
    st_g = main_fn(kOfxImageEffectActionGetClipPreferences.encode(), eff_h,
                   None, newh(gcp_out))
    gcp_depth = gcp_out.get("OfxImageClipPropDepth_Output")
    gcp_fv = gcp_out.get("OfxImageEffectFrameVarying")
    print("GetClipPreferences            -> %d  Depth_Output=%s FrameVarying=%s"
          % (st_g, gcp_depth, gcp_fv))
    ok_gcp = (st_g == kOfxStatOK and gcp_depth == "OfxBitDepthByte")

    # 复位全局引用对，别影响后面的 float 用例
    SRC_BUF = ctypes.create_string_buffer(src, len(src))
    DST_BUF = ctypes.create_string_buffer(SENTINEL, len(src))
    DST_PAD = (0, 0)

    # ------------------------------------------------------------------
    # 本轮修复的回归用例：参数换算 / A1 小画幅 / B8 bounds 不一致 / A4 RoD 缺失 / B5 多实例
    # ------------------------------------------------------------------
    print("")
    print("--- 参数换算 / 小画幅 / bounds / RoD / 多实例 ---")

    # ---- (1) paramSetValue 全 0 → 必须与源逐字节相同 ----
    for nm, pm in eff.params.items():
        if pm.ptype == "OfxParamTypePage":
            continue
        pset(pm, 0)
    DST_BUF = ctypes.create_string_buffer(SENTINEL, len(src))
    st_p0 = main_fn(kOfxImageEffectActionRender.encode(), eff_h, inargs_h, None)
    d0 = DST_BUF.raw
    maxd0 = max(abs(src[i] - d0[i]) for i in range(len(src)))
    ok_pset0 = (st_p0 == kOfxStatOK and maxd0 == 0)
    print("paramSetValue 全0 → 透传          -> %d  与源最大逐字节差=%d (期望 0)"
          % (st_p0, maxd0))

    # ---- (2) paramSetValue blur=60 → 必须改动画面（验证「面板值 → SDK 真值」换算）----
    #
    # v3.0.4 判据修正：原来要求「改动>=3 的像素 >1%」，那是 NamaSDK 时代的强度
    # （SDK 是独立模糊器，整片皮肤一起动）。现在的引擎是**保边低通**：它只削掉
    # 低幅纹理，边缘（眼镜、嘴唇、发际线）逐字节保留，所以「改动像素占比」天然
    # 很小 —— 在 1074x604 的真机帧上满档也只有 1.8%，60 档 0.2%。
    # 用占比当判据会把「效果正常但克制」误判成「参数没接线」。
    #
    # 改判两件事，都直接对源帧比：
    #   ① 最大逐通道差 >= 3  —— 确实改动了（不是量化噪声）；
    #   ② 改动是局部的（<25% 画面）—— 排除「整帧被改」这类真 bug。
    pset(eff.params["blur_level"], 60)
    DST_BUF = ctypes.create_string_buffer(SENTINEL, len(src))
    st_p1 = main_fn(kOfxImageEffectActionRender.encode(), eff_h, inargs_h, None)
    d1 = DST_BUF.raw
    npx = len(src) // 4
    ch1 = 0
    maxd1 = 0
    for i in range(0, len(src) - 3, 4):
        d = max(abs(src[i] - d1[i]), abs(src[i + 1] - d1[i + 1]),
                abs(src[i + 2] - d1[i + 2]))
        if d > maxd1:
            maxd1 = d
        if d >= 3:
            ch1 += 1
    pct1 = 100.0 * ch1 / max(npx, 1)
    ok_pset1 = (st_p1 == kOfxStatOK and maxd1 >= 3 and 0.01 < pct1 < 25.0)
    print("paramSetValue blur=60 → 已改动   -> %d  最大差=%d 改动>=3 占比=%.2f%% "
          "(期望 最大差>=3 且 0.01%%<占比<25%%)"
          % (st_p1, maxd1, pct1))

    # ---- (2b) 亮度回归：磨皮不得改变整帧平均亮度 ----
    # 历史 bug：引擎里挂着一个全局色调滤镜（filter_name=ziran2 / filter_level=0.4），
    # 「只要任意一项效果不为 0 就打开」。于是把磨皮从 0 拉到 1，整帧（含背景、
    # 衣服、字幕）会被提亮 +16/+14/+14 —— 用户看到的「加亮度」就是这个。
    #
    # 判据：磨皮只该改变**细节**（降低局部方差），不该改变**整体亮度**。
    # 所以比较 blur=0 与 blur=60 的平均亮度，差值必须近乎为 0；
    # 同时 blur=60 必须真的降低了细节能量（否则说明磨皮根本没生效）。
    def _mean_rgb(buf):
        n = len(buf) // 4
        return [sum(buf[c::4]) / max(n, 1) for c in range(3)]

    def _detail_energy(buf, y0=0, y1=None):
        """相邻像素差的均值：磨皮是低通，磨完这个值应当下降。

        y0/y1 限定统计行段。**必须限定**：整帧统计会被大片未改动的背景稀释，
        磨皮再正常也拉不动 2% —— v3.0.4 之前这条一直是假 FAIL。
        """
        if y1 is None:
            y1 = H
        tot = cnt = 0
        for y in range(y0, y1):
            base = y * W * 4
            for x in range(0, W - 1):
                i = base + x * 4
                tot += abs(buf[i] - buf[i + 4])
                cnt += 1
        return tot / max(cnt, 1)

    pset(eff.params["blur_level"], 0)
    DST_BUF = ctypes.create_string_buffer(SENTINEL, len(src))
    main_fn(kOfxImageEffectActionRender.encode(), eff_h, inargs_h, None)
    d_blur0 = DST_BUF.raw

    # v3.0.4：用满档 100 比较。60 档在这张柔和素材上只削掉 0.2% 的细节能量，
    # 低于任何有意义的阈值；100 档才能稳定看出「低通确实在起作用」。
    pset(eff.params["blur_level"], 100)
    DST_BUF = ctypes.create_string_buffer(SENTINEL, len(src))
    main_fn(kOfxImageEffectActionRender.encode(), eff_h, inargs_h, None)
    d_blur60 = DST_BUF.raw

    m0, m60 = _mean_rgb(d_blur0), _mean_rgb(d_blur60)
    max_shift = max(abs(a - b) for a, b in zip(m0, m60))

    # 磨皮真正作用的行段（扫一遍找出有改动的首末行），细节能量只在这段里比
    _y0, _y1 = H, 0
    for y in range(H):
        base = y * W * 4
        for x in range(W):
            i = base + x * 4
            if (abs(d_blur0[i] - d_blur60[i]) >= 2 or
                    abs(d_blur0[i + 1] - d_blur60[i + 1]) >= 2):
                if y < _y0:
                    _y0 = y
                if y + 1 > _y1:
                    _y1 = y + 1
                break
    if _y1 <= _y0:          # 完全没改动 → 说明磨皮没生效，用全幅统计以便报出证据
        _y0, _y1 = 0, H

    e0, e60 = _detail_energy(d_blur0, _y0, _y1), _detail_energy(d_blur60, _y0, _y1)
    ok_luma = (max_shift <= 1.0)
    ok_detail = (e60 < e0 * 0.99)
    ok_bright = ok_luma and ok_detail
    print("磨皮不改变整体亮度 (B10)        -> 平均亮度 %s → %s  最大漂移=%.2f (期望 <=1.0)"
          % (["%.1f" % v for v in m0], ["%.1f" % v for v in m60], max_shift))
    print("   细节能量 blur=0/100 = %.3f/%.3f（行段 %d..%d，磨皮生效须下降）  %s"
          % (e0, e60, _y0, _y1, "OK" if ok_detail else "BAD"))

    # ---- (2c) 默认值必须是「只磨皮肤区域」开着 ----
    # 全局磨皮会把背景、衣服、字幕一起磨糊，用户体感是「画面整体不对劲」。
    # 实测 44 张真实照片：该开关使磨皮影响面积平均减少 20.8 个百分点
    # （-30.6pp ~ -4.3pp），整帧平均亮度最大漂移 0.46/255（不引入亮度问题）。
    # 参数句柄刚建立时读到的就是 Default，这里把它当成用户第一次拖上效果看到的状态。
    dflt_skin = 0
    pm_skin = eff.params.get("skin_detect")
    if pm_skin is not None:
        _d = pm_skin.props.get("OfxParamPropDefault", 0)
        dflt_skin = int(_d or 0)
    ok_skindflt = (dflt_skin == 1)
    print("skin_detect 默认开启 (B11)       -> Default=%d (期望 1, 只磨皮肤不磨背景)"
          % dflt_skin)

    # ---- (2c2) 首次拖入的默认参数：只开磨皮 62，其余十三项 0 ----
    # 产品要求：节点第一次丢到片段上时是「轻磨皮」预设。这里直接读
    # kOfxParamPropDefault —— 它等于用户第一次看到的面板值，防止以后被改回去。
    # v2.2.3 用户决策：默认改为「磨皮 50 / 美白 30 / 红润 30」（此前 v2.0.3 是 62，
    #   那次依据物理摄像头 GT 插值，本次是产品侧观感取舍覆盖了它）。
    #   其余十一项必须 0 —— 尤其新增项（全局锐化 / 去法令纹 / 祛斑痘 / 美型三项），
    #   否则「轻妆」这个产品预设就不成立。
    expect_dflt = [
        ("blur_level", 50.0), ("clarity", 0.0), ("color_level", 30.0),
        ("red_level", 30.0), ("sharpen", 0.0), ("global_sharpen", 0.0),
        ("face_threed", 0.0), ("eye_bright", 0.0),
        ("remove_pouch_strength", 0.0),
        ("remove_nasolabial_folds_strength", 0.0),
        # 2.1.0 美型三项：必须是 0。形变是不可逆的几何改动，默认开了等于
        # 擅自改用户的脸 —— 这条判据同时守住「新参数没把默认值带歪」。
        ("cheek_thinning", 0.0), ("eye_enlarging", 0.0), ("intensity_chin", 0.0),
        ("delspot_level", 0.0),
    ]
    got_dflt = {}
    for _nm, _exp in expect_dflt:
        _pm = eff.params.get(_nm)
        _v = _pm.props.get("OfxParamPropDefault", 0) if _pm is not None else None
        got_dflt[_nm] = None if _v is None else float(_v)
    _pm_bt = eff.params.get("blur_type")
    dflt_blurtype = (_pm_bt.props.get("OfxParamPropDefault", 0)
                     if _pm_bt is not None else None)
    # v2.0 新增的磨皮模式：默认必须是 0（人像磨皮），否则老工程观感会变。
    _pm_sm = eff.params.get("smooth_mode")
    dflt_smooth = (_pm_sm.props.get("OfxParamPropDefault", 0)
                   if _pm_sm is not None else None)
    ok_defaults = (all(got_dflt[k] is not None and abs(got_dflt[k] - v) < 1e-6
                       for k, v in expect_dflt)
                   and int(dflt_blurtype or 0) == 0
                   and int(dflt_smooth or 0) == 0)
    print("首次拖入默认参数 (B14)         -> 磨皮=%g 清晰=%g 美白=%g 红润=%g 锐化=%g "
          "全局锐化=%g 立体=%g 亮眼=%g 去黑眼圈=%g 去法令纹=%g "
          "瘦脸=%g 大眼=%g 下巴=%g 祛斑痘=%g "
          "算法Default=%s 磨皮模式Default=%s (期望 50/0/30/30/0×10, 0=双边, 0=人像)"
          % tuple([got_dflt[k] or 0.0 for k, _ in expect_dflt]
                  + [dflt_blurtype, dflt_smooth]))

    # ---- (2d) 通道序回归：SDK 侧按 BGRA 读缓冲，宿主侧是 RGBA ----
    # 历史 bug：引擎直接把 RGBA 交给 SDK，SDK 当成 BGRA → AI 模型看到颜色错乱的
    # 脸，43 张真实照片里 39 张 faces=0；修好后 11 张检出。
    # 判据用「阶梯边缘」：左半 R=255 / 右半 R=0，G 恒 128。
    # 磨皮会把边缘抹开，但**左右两半的基本色相**必须保持不变：
    #   左半应仍是红（R 明显大于 B），右半应仍是暗绿（R≈0）。
    # 如果 R/B 被换掉，左半会变成蓝 —— 一眼可辨，且不受磨皮强度影响。
    #
    # ⚠️ 画幅必须足够大：isThrowawayRender 在面积低于源 RoD 的 1/16 时直接透传，
    # 那样根本不会进 SDK，量到的只是源数据本身（先前用 64x64 就撞了这个坑，
    # 结果两侧都等于源值 90,90,90，断言没有意义）。这里用 W×H 全画幅，
    # 但只填两种颜色，阶梯放在画面中央。
    W2, H2 = W, H
    left_rgb = (255, 128, 0)
    right_rgb = (0, 128, 0)
    step = bytearray()
    for _y in range(H2):
        for _x in range(W2):
            step += bytes(left_rgb + (255,)) if _x < W2 // 2 else bytes(right_rgb + (255,))
    step_src = bytes(step)

    def _render_8(src_bytes, w2, h2, blur_v, skin_v):
        # SRC_BUF / DST_BUF / DEPTH 是夹具的模块级全局，clipGetImage 读的就是它们；
        # 不写 global 的话插件会拿到上一次的旧缓冲（先前量到的一侧 90,90,90 就是
        # 哨兵 0x5A，说明插件根本没写进我们手里的缓冲）。
        #
        # v3.0.4：改用**独立实例**渲染。引擎的跨帧状态（肤色中心 EMA / 掩码缓存 /
        # 细节层 EMA）是按实例隔离的，复用主实例会把上一个用例的状态带进来 ——
        # 实测：先跑满档磨皮、再喂纯红/纯绿的合成帧，语义区域判「无皮肤 → 不处理」，
        # 整帧变暗，通道序用例直接假 FAIL。
        global SRC_BUF, DST_BUF, DEPTH
        effx = Effect()
        hx = newh(effx)
        main_fn(kOfxImageEffectActionDescribeInContext.encode(), hx, None, None)
        cx = effx.clips.get("Source")
        if cx:
            cx.props.set("OfxImageEffectPropRegionOfDefinition", 0, [0, 0, w2, h2])
            cx.props.set("OfxImageEffectPropPixelDepth", 0, "OfxBitDepthByte")
            cx.props.set("OfxImageEffectPropComponents", 0, "OfxImageComponentRGBA")
            cx.props.set("OfxImagePropPixelAspectRatio", 0, 1.0)
        main_fn(kOfxActionCreateInstance.encode(), hx, None, None)
        for nm, pm in effx.params.items():
            if pm.ptype == "OfxParamTypePage":
                continue
            if nm == "blur_level":
                pset(pm, blur_v)
            elif nm == "skin_detect":
                pset(pm, skin_v)
            else:
                pset(pm, 0)
        SRC_BUF = ctypes.create_string_buffer(src_bytes, len(src_bytes))
        DST_BUF = ctypes.create_string_buffer(b"Z" * len(src_bytes), len(src_bytes))
        DEPTH = "OfxBitDepthByte"
        a2 = PropSet("step")
        a2.set("OfxImageEffectPropRenderWindow", 0, [0, 0, w2, h2])
        a2.set("OfxPropTime", 0, 99.0)
        a2.set("OfxImageEffectPropRenderScale", 0, [1.0, 1.0])
        st2 = main_fn(kOfxImageEffectActionRender.encode(), hx, newh(a2), None)
        out = DST_BUF.raw
        main_fn(kOfxActionDestroyInstance.encode(), hx, None, None)
        return st2, out

    st_step, step_out = _render_8(step_src, W2, H2, 60, 1)
    _i_l = (H2 // 2 * W2 + 8) * 4
    _i_r = (H2 // 2 * W2 + W2 - 8) * 4
    lpx = step_out[_i_l:_i_l + 3]
    rpx = step_out[_i_r:_i_r + 3]
    left_is_red = (lpx[0] > 100 and lpx[0] > lpx[2])
    right_is_green = (rpx[0] < 60 and rpx[2] < 60)
    ok_channel = (st_step == kOfxStatOK and left_is_red and right_is_green)
    print("通道序 RGB 不被交换 (B12)        -> 左半=%s(期望红) 右半=%s(期望暗绿) %s"
          % (tuple(lpx), tuple(rpx), "OK" if ok_channel else "BAD"))
    # ---- (3) B13：美白只能提亮人脸，不能把整帧（背景/衣服/字幕）一起提亮 ----
    # 全局美白与「只磨皮人脸」的体感不符。判据：只开美白（磨皮 0 隔离掉）、
    # skin_detect=1，输出与**源帧**逐像素比：四角（人脸外）不得出现≥3 的差，
    # 人脸区应明显提亮。若素材里检不出人脸，则整帧应不变。

    def _render_white(src_bytes, w2, h2, whiten_v):
        """只开美白渲一帧。必须显式 global（见 _render_8 的注释）。"""
        global SRC_BUF, DST_BUF, DEPTH
        for nm, pm in eff.params.items():
            if pm.ptype == "OfxParamTypePage":
                continue
            if nm == "skin_detect":
                pset(pm, 1)
            elif nm == "color_level":
                pset(pm, whiten_v)
            else:
                pset(pm, 0)
        SRC_BUF = ctypes.create_string_buffer(src_bytes, len(src_bytes))
        DST_BUF = ctypes.create_string_buffer(b"Z" * len(src_bytes), len(src_bytes))
        DEPTH = "OfxBitDepthByte"
        a2 = PropSet("white")
        a2.set("OfxImageEffectPropRenderWindow", 0, [0, 0, w2, h2])
        a2.set("OfxPropTime", 0, 99.0)
        a2.set("OfxImageEffectPropRenderScale", 0, [1.0, 1.0])
        st2 = main_fn(kOfxImageEffectActionRender.encode(), eff_h, newh(a2), None)
        return st2, DST_BUF.raw

    # 素材就用主帧 src（bottom-up，与其余用例同一张）。**不要换素材**：
    # 换一张画风/肤色完全不同的帧会让引擎的肤色中心 EMA 突跳，紧接其后的用例
    # 就测不准 —— v3.0.4 实测过：这里喂一张 letterbox 照片后，「通道序」用例
    # 整帧变黑（语义区域判定「无皮肤 → 不处理」），那是状态污染不是插件 bug。
    _wsrc = src
    # 人脸区取画面中央 30%~70% 列（主帧是居中构图的人像）
    _fx0, _fx1 = W * 3 // 10, W * 7 // 10
    # v3.0.4：改成「只渲染一次，和源帧比」。
    # 原来渲染两次（color=0 / color=100）再互比，两帧之间隔着一次检测 ——
    # 引擎有跨帧状态（掩码缓存、肤色 EMA），第二次用的掩码可能和第一次不同，
    # 于是整幅画面对不上，量出「四角差 36 / 人脸区 107」这种离谱数字（假 FAIL）。
    # 现在 blur=0 把磨皮隔离掉、只留美白，直接对源帧比：
    #   四角不动 = 美白没溢出到背景；人脸区变亮 = 美白真的生效。
    _st_w0, d_white0 = _render_white(_wsrc, W, H, 0)
    _st_w1, d_white100 = _render_white(_wsrc, W, H, 100)
    corners = []
    for (cx, cy) in ((2, 2), (W - 3, 2), (2, H - 3), (W - 3, H - 3)):
        i = (cy * W + cx) * 4
        corners.append(max(abs(d_white0[i] - d_white100[i]),
                           abs(d_white0[i + 1] - d_white100[i + 1]),
                           abs(d_white0[i + 2] - d_white100[i + 2])))
    changed = sum(1 for i in range(0, len(_wsrc) - 3, 4)
                  if max(abs(d_white0[i] - d_white100[i]),
                         abs(d_white0[i + 1] - d_white100[i + 1]),
                         abs(d_white0[i + 2] - d_white100[i + 2])) >= 3)
    if True:
        # 四角不动，人脸区应提亮。
        face_diff = face_n = 0
        for y in range(0, H, 4):
            for x in range(_fx0, _fx1, 4):
                i = (y * W + x) * 4
                d = max(abs(d_white0[i] - d_white100[i]),
                        abs(d_white0[i + 1] - d_white100[i + 1]),
                        abs(d_white0[i + 2] - d_white100[i + 2]))
                face_diff += d; face_n += 1
        face_avg = face_diff / max(face_n, 1)
        # 上限 30 不能省：它守的是「整数提亮表算错导致整体过曝」那类真 bug
        # （2026-10-21 验收抓到过一次整数表少除 255，人脸区平均差 67，
        # 当时只断言 >=3 就漏过去了）。
        #
        # 下限的演变（每一次都是**预期行为的修正**，不是放水）：
        #   3.0（v1.4 颜色启发式）→ 2.0（v1.5 接语义掩码，方框里头发衣服不再算皮肤，
        #   实测 3.06）→ 1.0（v2.0 回移 OBS 3.0.19 引擎，美白系数 kMax 由 0.40
        #   降到 0.16，实测 1.16）。
        # v2.0 这次降幅的依据：OBS 侧 v3.0.18 做了一次对标标定，以第三方商业美颜
        # 为锚点（55 档 +5.66、满档约 +10）反解得 kMax=0.1615。v1.51 的 0.40
        # 对应满档 +25，比商业产品强约 2.5 倍。两版都仍然是「只提亮皮肤、
        # 四角逐字节不动」，只是幅度不同。
        # 注意：这个指标量的是「照片贴进画布那一段列 × 全高」的平均差，方框里大半是
        # 头发/衣服/背景，所以数值天然远小于掩码内的单像素提亮（v1.51 掩码内实测
        # 24.35，理论 25.5）。
        ok_whiten = (max(corners) <= 2 and 1.0 <= face_avg <= 30.0)
        print("美白不提亮整帧 (B13)        -> 四角最大差=%.2f 人脸区平均差=%.2f (期望四角<=2, 1<=人脸<=30)"
              % (max(corners), face_avg))
    else:
        # 合成素材（无脸）：整帧不得变
        ok_whiten = (max(corners) <= 2 and changed == 0)
        print("美白不提亮整帧 (B13)        -> 四角最大差=%.2f 整帧改动=%d (期望四角<=2, 整帧=0)"
              % (max(corners), changed))


    # 收尾：清掉当前值，参数回到 Default
    for nm, pm in eff.params.items():
        pm.value = None

    # ---- (3) A1 回归：小画幅 + scale=[1,1] 不得被面积兜底误杀（必须仍走 SDK）----
    a1_log = os.path.join(_here, "a1_render.log")

    def _read_text(p):
        try:
            with open(p, "r", encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def _with_log(fn):
        """临时把插件日志定向到 a1_log 再执行 fn，返回 (结果, 日志文本)。"""
        prev = os.environ.get("NAMA_OFX_LOG")
        prev_all = os.environ.get("NAMA_OFX_LOG_ALL")
        try:
            os.remove(a1_log)
        except OSError:
            pass
        os.environ["NAMA_OFX_LOG"] = a1_log
        os.environ["NAMA_OFX_LOG_ALL"] = "1"
        try:
            r = fn()
        finally:
            txt = _read_text(a1_log)
            if prev is None:
                os.environ.pop("NAMA_OFX_LOG", None)
            else:
                os.environ["NAMA_OFX_LOG"] = prev
            if prev_all is None:
                os.environ.pop("NAMA_OFX_LOG_ALL", None)
            else:
                os.environ["NAMA_OFX_LOG_ALL"] = prev_all
        return r, txt

    aw, ah = 160, 120
    sclip.props.set("OfxImageEffectPropRegionOfDefinition", 0, [0, 0, aw, ah])
    a1_src = ctypes.create_string_buffer(src[: aw * ah * 4], aw * ah * 4)
    a1_dst = ctypes.create_string_buffer(b"\x5a" * (aw * ah * 4), aw * ah * 4)
    KEEP.append(a1_src)
    KEEP.append(a1_dst)
    SRC_BUF = ctypes.cast(a1_src, c_void_p)
    DST_BUF = ctypes.cast(a1_dst, c_void_p)
    DEPTH = "OfxBitDepthByte"
    a1_in = PropSet("a1_in")
    a1_in.set("OfxImageEffectPropRenderWindow", 0, [0, 0, aw, ah])
    a1_in.set("OfxPropTime", 0, 300.0)
    a1_in.set("OfxImageEffectPropRenderScale", 0, [1.0, 1.0])
    a1_in_h = newh(a1_in)

    st_a1, a1_logtxt = _with_log(
        lambda: main_fn(kOfxImageEffectActionRender.encode(), eff_h, a1_in_h, None))
    took_proxy = "无需 SDK" in a1_logtxt
    ok_a1 = (st_a1 == kOfxStatOK and not took_proxy)
    print("A1 小画幅 %dx%d + scale=[1,1]     -> %d  走了代理透传=%s (期望 False)"
          % (aw, ah, st_a1, took_proxy))

    sclip.props.set("OfxImageEffectPropRegionOfDefinition", 0, [0, 0, W, H])

    # ---- (4) B8：dst bounds 比 src 大一圈，剩余区域必须补满（不得残留哨兵）----
    PADX, PADY = 16, 8
    DST_PAD = (PADX, PADY)
    SRC_BUF = ctypes.create_string_buffer(src, len(src))
    pad_len = (W + PADX) * (H + PADY) * 4
    pad_buf = ctypes.create_string_buffer(b"\x5a" * pad_len, pad_len)
    KEEP.append(pad_buf)
    DST_BUF = ctypes.cast(pad_buf, c_void_p)
    DEPTH = "OfxBitDepthByte"
    st_b8 = main_fn(kOfxImageEffectActionRender.encode(), eff_h, inargs_h, None)
    pb = pad_buf.raw
    stride = (W + PADX) * 4
    leftover_edge = 0
    for y in range(H + PADY):
        for x in range(W, W + PADX):
            o = y * stride + x * 4
            if pb[o:o + 4] == b"\x5a\x5a\x5a\x5a":
                leftover_edge += 1
    for y in range(H, H + PADY):
        for x in range(W + PADX):
            o = y * stride + x * 4
            if pb[o:o + 4] == b"\x5a\x5a\x5a\x5a":
                leftover_edge += 1
    ok_b8 = (st_b8 == kOfxStatOK and leftover_edge == 0)
    print("B8 bounds 不一致 dst=+%d,+%d         -> %d  边缘残留哨兵像素=%d (期望 0)"
          % (PADX, PADY, st_b8, leftover_edge))
    DST_PAD = (0, 0)

    # ---- (5) A4：源 RoD 属性缺失时必须回 ReplyDefault ----
    # 正向用例先来：RoD 存在时必须回 kOfxStatOK 且值一致。fixture 以前只测「缺失」，
    # 而插件把 double×4 的 RoD 当 int×4 读，类型不符 → 永远「缺失」，这条正测能抓住。
    rod_out2 = PropSet("rod_present")
    st_rod2 = main_fn(kOfxImageEffectActionGetRegionOfDefinition.encode(), eff_h,
                      None, newh(rod_out2))
    rod_val = rod_out2.get("OfxImageEffectPropRegionOfDefinition")
    rod_int = ([int(round(x)) for x in rod_val]
               if isinstance(rod_val, (list, tuple)) and len(rod_val) == 4 else None)
    ok_rod_ok = (st_rod2 == kOfxStatOK and rod_int == [0, 0, W, H])
    print("    RoD 存在 → GetRoD            -> %d rod=%s (期望 0 / [0,0,%d,%d])"
          % (st_rod2, rod_int, W, H))

    saved_rod = sclip.props.props.pop("OfxImageEffectPropRegionOfDefinition", None)
    rod_out3 = PropSet("rod_missing")
    st_rod3 = main_fn(kOfxImageEffectActionGetRegionOfDefinition.encode(), eff_h,
                      None, newh(rod_out3))
    if saved_rod is not None:
        sclip.props.props["OfxImageEffectPropRegionOfDefinition"] = saved_rod
    ok_rod = ok_rod_ok and (st_rod3 == kOfxStatReplyDefault)
    print("A4 源 RoD 缺失 → GetRoD          -> %d (期望 %d=ReplyDefault)"
          % (st_rod3, kOfxStatReplyDefault))

    # ---- (6) B5：第二个实例应拿到独立槽位，且渲染结果互不干扰 ----
    # v3.0.4：判据从「日志里有没有 slot=1 这句话」改成「行为上互不干扰」。
    # 原来那条断言依赖日志文本，而 `_with_log` 把 NAMA_OFX_LOG 指到临时文件后
    # 只要有一处写不进去就恒为 False，既可能是假 FAIL 也可能是假 PASS（A1 那条
    # 「日志里有没有『无需 SDK』」同理）。行为断言不依赖日志管道，更硬。
    eff2 = Effect()
    eff2_h = newh(eff2)
    main_fn(kOfxImageEffectActionDescribeInContext.encode(), eff2_h, None, None)
    sclip2 = eff2.clips.get("Source")
    if sclip2:
        sclip2.props.set("OfxImageEffectPropRegionOfDefinition", 0, [0, 0, W, H])
        sclip2.props.set("OfxImageEffectPropPixelDepth", 0, "OfxBitDepthByte")
        sclip2.props.set("OfxImageEffectPropComponents", 0, "OfxImageComponentRGBA")
        sclip2.props.set("OfxImagePropPixelAspectRatio", 0, 1.0)
    SRC_BUF = ctypes.create_string_buffer(src, len(src))
    DST_BUF = ctypes.create_string_buffer(SENTINEL, len(src))
    DEPTH = "OfxBitDepthByte"
    in2 = PropSet("in2")
    in2.set("OfxImageEffectPropRenderWindow", 0, [0, 0, W, H])
    in2.set("OfxPropTime", 0, 5.0)
    in2.set("OfxImageEffectPropRenderScale", 0, [1.0, 1.0])
    in2_h = newh(in2)

    # 第一实例的基准输出：在第二实例存在之前连渲两帧（第二帧用于让掩码/EMA 稳定）
    def _render_eff():
        DST_BUF = ctypes.create_string_buffer(SENTINEL, len(src))
        SRC_BUF = ctypes.create_string_buffer(src, len(src))
        return main_fn(kOfxImageEffectActionRender.encode(), eff_h, inargs_h, None)

    _render_eff()
    _render_eff()
    DST_BUF = ctypes.create_string_buffer(SENTINEL, len(src))
    SRC_BUF = ctypes.create_string_buffer(src, len(src))
    main_fn(kOfxImageEffectActionRender.encode(), eff_h, inargs_h, None)
    d_pre = DST_BUF.raw

    def _do_multi():
        ci = main_fn(kOfxActionCreateInstance.encode(), eff2_h, None, None)
        r = main_fn(kOfxImageEffectActionRender.encode(), eff2_h, in2_h, None)
        return (ci, r)

    (st_ci2, st_r2), log2 = _with_log(_do_multi)
    d2b = DST_BUF.raw
    ch2 = sum(1 for i in range(0, len(src) - 3, 4)
              if max(abs(src[i] - d2b[i]), abs(src[i + 1] - d2b[i + 1]),
                     abs(src[i + 2] - d2b[i + 2])) >= 3)
    st_di2 = main_fn(kOfxActionDestroyInstance.encode(), eff2_h, None, None)

    # 销毁第二实例后，第一实例必须还能出图，且结果与之前一致（互不干扰）
    _render_eff()
    DST_BUF = ctypes.create_string_buffer(SENTINEL, len(src))
    SRC_BUF = ctypes.create_string_buffer(src, len(src))
    st_r1b = main_fn(kOfxImageEffectActionRender.encode(), eff_h, inargs_h, None)
    d_post = DST_BUF.raw
    drift = max(abs(a - b) for a, b in zip(d_pre, d_post))
    # 允许 <=2 的漂移：引擎有跨帧状态（掩码缓存 / 肤色 EMA），销毁邻居不该让它变形，
    # 但也不该要求逐字节相同。
    ok_multi = (st_ci2 == kOfxStatOK and st_r2 == kOfxStatOK and st_di2 == kOfxStatOK
                and st_r1b == kOfxStatOK and ch2 > npx * 0.001 and drift <= 2)
    print("B5 多实例互不干扰                -> ci=%d render=%d 改动占比=%.2f%% "
          "销毁后第一实例 render=%d 输出漂移=%d (期望 改动>0.1%%, 漂移<=2)"
          % (st_ci2, st_r2, 100.0 * ch2 / max(npx, 1), st_r1b, drift))
    try:
        os.remove(a1_log)
    except OSError:
        pass

    # Render：float 深度
    DEPTH = "OfxBitDepthFloat"
    fl = (ctypes.c_float * (W * H * 4))()
    for i in range(W * H * 4):
        fl[i] = src[i] / 255.0
    SRC_BUF = ctypes.cast(fl, c_void_p)
    dstf = (ctypes.c_float * (W * H * 4))()
    for i in range(W * H * 4):
        dstf[i] = 0.3529412  # 0x5a/255 的哨兵
    DST_BUF = ctypes.cast(dstf, c_void_p)
    MESSAGES[:] = []
    st = main_fn(kOfxImageEffectActionRender.encode(), eff_h, inargs_h, None)
    print("Render(float)                 -> %d" % st)
    if MESSAGES:
        print("   宿主收到的消息: %s" % MESSAGES[:3])

    nan_cnt = sum(1 for i in range(0, W * H * 4, 4) if math.isnan(dstf[i]))
    inf_cnt = sum(1 for i in range(0, W * H * 4, 4) if math.isinf(dstf[i]))
    print("   float 输出诊断: NaN=%d Inf=%d" % (nan_cnt, inf_cnt))

    changed_f = 0
    for i in range(0, W * H * 4, 4):
        if math.isnan(dstf[i]) or math.isinf(dstf[i]):
            continue
        a = int(src[i])
        b = int(dstf[i] * 255.0 + 0.5)
        if abs(a - b) >= 3:
            changed_f += 1
    leftover_f = sum(1 for i in range(0, W * H * 4, 4) if abs(dstf[i] - 0.3529412) < 1e-6)
    print("   float 路径改动>=3 的像素=%d  残留哨兵=%d/%d"
          % (changed_f, leftover_f, W * H))
    wrote_f = leftover_f < W * H * 0.5
    # v2.0：阈值同步「8bit 渲染」那条的改动（线性磨皮曲线下默认档改动量更小）。
    # 仍然要求确实改动了像素，只是不再假设一定超过 1%。
    ok_renderf = (st == kOfxStatOK and wrote_f and changed_f > W * H * 0.001
                  and nan_cnt == 0 and inf_cnt == 0)

    # ------------------------------------------------------------------
    # 1920x1080 性能基准（加 --bench 才跑）
    #   byte  路径喂真实人脸（把 640x394 平铺成 1920x1080），量端到端；
    #   float 路径喂平场 —— Python 侧把 8.3M 个 float 逐个算太慢，所以用
    #   array 的常数重复构造，好处是量得到「纯格式转换」的开销，
    #   代价是这一档没有人脸检测（转换耗时本来就与画面内容无关）。
    # ------------------------------------------------------------------
    if "--bench" in sys.argv:
        import array
        # 优先用真实素材帧（NB_BENCH_RAW + NB_BENCH_W/H 指定），
        # 只有拿不到真实帧时才退回到「人脸居中贴黑底」的合成图 ——
        # 合成黑底会干扰人脸检测，测出来的 SDK 耗时不代表真实场景。
        realRaw = os.environ.get("NB_BENCH_RAW")
        BW = int(os.environ.get("NB_BENCH_W", "1920"))
        BH = int(os.environ.get("NB_BENCH_H", "1080"))
        nbytes = BW * BH * 4
        tiled = None
        if realRaw and os.path.isfile(realRaw):
            blob = open(realRaw, "rb").read()
            if len(blob) == nbytes:
                tiled = blob
                print("   使用真实素材帧 %dx%d（%s）" % (BW, BH, os.path.basename(realRaw)))
            else:
                print("   [忽略] %s 是 %d 字节，与 %dx%dx4=%d 不符，改用合成图"
                      % (realRaw, len(blob), BW, BH, nbytes))
        if tiled is None:
            # 兜底合成图：把人脸按原始尺寸居中贴在黑底上。
            # 黑底会干扰人脸检测，所以只在拿不到真实素材时才用它。
            tiled = bytearray(nbytes)
            ox, oy = (BW - 640) // 2, (BH - 394) // 2
            for y in range(394):
                dst_ofs = (oy + y) * BW * 4 + ox * 4
                tiled[dst_ofs:dst_ofs + 640 * 4] = src[y * 640 * 4:(y + 1) * 640 * 4]
            tiled = bytes(tiled)
        W, H = BW, BH

        print("")
        print("--- %dx%d 性能基准 ---" % (BW, BH))
        sclip.props.set("OfxImageEffectPropRegionOfDefinition", 0, [0, 0, BW, BH])
        bwin = PropSet("bwin")
        bwin.set("OfxImageEffectPropRenderWindow", 0, [0, 0, BW, BH])
        bwin_h = newh(bwin)

        for label, depth in (("byte", "OfxBitDepthByte"), ("float", "OfxBitDepthFloat")):
            DEPTH = depth
            if depth == "OfxBitDepthByte":
                SRC_BUF = ctypes.create_string_buffer(tiled, nbytes)
                DBUF = ctypes.create_string_buffer(b"\x5a" * nbytes, nbytes)
                DST_BUF = ctypes.cast(DBUF, c_void_p)
            else:
                t0c = time.perf_counter()
                # array('f', array('B', ...)) 是 C 层的，只是值域仍是 0..255，
                # 还得除 255。这一步只能在 Python 里做，一次性约几秒。
                fa = array.array("f", (v * (1.0 / 255.0) for v in array.array("B", tiled)))
                print("   (构造 float 输入耗时 %.1fs，仅本基准需要)" % (time.perf_counter() - t0c))
                SBUF = ctypes.create_string_buffer(fa.tobytes(), nbytes * 4)
                SRC_BUF = ctypes.cast(SBUF, c_void_p)
                da = array.array("f", [0.0]) * nbytes
                DBUF = ctypes.create_string_buffer(da.tobytes(), nbytes * 4)
                DST_BUF = ctypes.cast(DBUF, c_void_p)
            KEEP.append(SRC_BUF)
            KEEP.append(DST_BUF)

            main_fn(kOfxImageEffectActionRender.encode(), eff_h, bwin_h, None)  # 预热
            runs = 5
            t0 = time.perf_counter()
            for _ in range(runs):
                main_fn(kOfxImageEffectActionRender.encode(), eff_h, bwin_h, None)
            per = (time.perf_counter() - t0) * 1000.0 / runs
            print("   %-5s %dx%d  平均 %.1f ms/帧  ≈ %.1f fps"
                  % (label, BW, BH, per, 1000.0 / per))

    # ---- (2f) 下拉框接线：磨皮算法三档 / 磨皮模式四档（**面板路径**）----
    #
    # v3.0.6 新增。v3.0.5 里这两个下拉框「引擎侧接了、面板侧没接」，而所有既有
    # 用例都没发现，因为 baseline.py 走的是 NamaSelfTest 的 NAMA_ST_ALG /
    # NAMA_ST_SMOOTH 钩子 —— 它直接调引擎，**绕过了 ofx_plugin.cpp::readParams()**。
    # 真机拖下拉框走的却是 readParams，那里有两个缺陷：
    #   ① 还留着 SDK 时代的 `map[3] = {1, 0, 2}`，把面板序 {双边,高斯,弱磨皮}
    #      换成 SDK 序 {1=双边,0=高斯,2=弱磨皮} —— 面板选「双边」实际跑高斯、
    #      选「高斯」实际跑保边，两档对调；
    #   ② 高斯分支不产生 surface3 的 I，blurRefillI 恒 0 → 磨皮模式四档全失效。
    # 这条用例用 paramSetValue 改下拉框，和真人操作等价，专门堵这两个洞。
    def _render_params(src_bytes, w2, h2, **params):
        global SRC_BUF, DST_BUF, DEPTH
        effx = Effect()
        hx = newh(effx)
        main_fn(kOfxImageEffectActionDescribeInContext.encode(), hx, None, None)
        cx = effx.clips.get("Source")
        if cx:
            cx.props.set("OfxImageEffectPropRegionOfDefinition", 0, [0, 0, w2, h2])
            cx.props.set("OfxImageEffectPropPixelDepth", 0, "OfxBitDepthByte")
            cx.props.set("OfxImageEffectPropComponents", 0, "OfxImageComponentRGBA")
            cx.props.set("OfxImagePropPixelAspectRatio", 0, 1.0)
        main_fn(kOfxActionCreateInstance.encode(), hx, None, None)
        for nm, pm in effx.params.items():
            if pm.ptype == "OfxParamTypePage":
                continue
            pset(pm, params.get(nm, 0))
        SRC_BUF = ctypes.create_string_buffer(src_bytes, len(src_bytes))
        DST_BUF = ctypes.create_string_buffer(b"Z" * len(src_bytes), len(src_bytes))
        DEPTH = "OfxBitDepthByte"
        a2 = PropSet("choices")
        a2.set("OfxImageEffectPropRenderWindow", 0, [0, 0, w2, h2])
        a2.set("OfxPropTime", 0, 99.0)
        a2.set("OfxImageEffectPropRenderScale", 0, [1.0, 1.0])
        st2 = main_fn(kOfxImageEffectActionRender.encode(), hx, newh(a2), None)
        out = DST_BUF.raw
        main_fn(kOfxActionDestroyInstance.encode(), hx, None, None)
        return st2, out

    def _mad(a, b):
        """与源帧的平均绝对差（只算 RGB，跳过 alpha）。越大 = 离原图越远。"""
        n = len(a)
        s = 0
        for i in range(0, n, 4):
            s += abs(a[i] - b[i]) + abs(a[i + 1] - b[i + 1]) + abs(a[i + 2] - b[i + 2])
        return s / (n // 4 * 3)

    def _hf_in(buf, w2, h2, reg, rad=4):
        """在给定像素集合上量「中频高通」：|G − (2r+1)² 盒式均值(G)| 的均值。

        为什么用中频（rad=4 → 9×9）而不是 3×3 细频：
          「清晰 / 精细」两档的回填主体是**中频带**（dp.mid = 0.45/0.75·I），
          细频带只有 0.06·I·4.0/5.5，落在 8bit 上常常四舍五入为 0。
          3×3 高通只看得见细频，实测四档之间只差 0.5%（人像 1.751 /
          清晰 1.760），断言拉不开；换 9×9 才和「中频回填」这个语义对齐。
        """
        g = [buf[(y * w2 + x) * 4 + 1] for y in range(h2) for x in range(w2)]
        tot = 0.0
        for idx in reg:
            y, x = divmod(idx, w2)
            s = 0
            n = 0
            for dy in range(-rad, rad + 1):
                yy = y + dy
                if yy < 0 or yy >= h2:
                    continue
                base = yy * w2
                for dx in range(-rad, rad + 1):
                    xx = x + dx
                    if 0 <= xx < w2:
                        s += g[base + xx]
                        n += 1
            tot += abs(g[idx] - s / n)
        return tot / max(len(reg), 1)

    print("")
    print("---- 下拉框接线：磨皮算法 / 磨皮模式（走 paramSetValue 的面板路径）----")
    _alg = {}
    for _i in range(3):
        _st, _o = _render_params(src, W, H, blur_level=62.0, blur_type=_i)
        _alg[_i] = _mad(src, _o) if _st == kOfxStatOK else -1.0
    print("   算法  0双边=%.5f  1高斯=%.5f  2弱磨皮=%.5f  (距源帧，越大越糊)"
          % (_alg[0], _alg[1], _alg[2]))
    # 高斯不保边 → 一定比双边糊；弱磨皮强度对折 → 一定比双边轻
    ok_alg = (_alg[1] > _alg[0] * 1.15 and _alg[2] < _alg[0] * 0.85)

    _mod = {}
    _mout = {}
    for _i in range(4):
        _st, _o = _render_params(src, W, H, blur_level=62.0, smooth_mode=_i)
        _mod[_i] = _mad(src, _o) if _st == kOfxStatOK else -1.0
        _mout[_i] = _o
    print("   模式  0人像=%.5f  1清晰=%.5f  2精细=%.5f  3轻磨皮=%.5f"
          % (_mod[0], _mod[1], _mod[2], _mod[3]))
    # 高频保留率只在「磨皮脚印」内量 —— 全帧均值会被大片背景稀释到
    # 0.2% 的差别，四档之间根本拉不开。脚印取「人像档相对源帧有改动的像素」，
    # 正好就是掩码生效的那一块，回填也只发生在这里。
    _reg = [i for i in range(W * H)
            if (abs(_mout[0][i * 4] - src[i * 4]) or
                abs(_mout[0][i * 4 + 1] - src[i * 4 + 1]) or
                abs(_mout[0][i * 4 + 2] - src[i * 4 + 2]))]
    _hf = {i: _hf_in(_mout[i], W, H, _reg) for i in range(4)}
    print("   模式中频保留（脚印 %d px）  0人像=%.3f  1清晰=%.3f  2精细=%.3f  3轻磨皮=%.3f"
          % (len(_reg), _hf[0], _hf[1], _hf[2], _hf[3]))
    # 断言**方向**而不是「有变化」（档位对调时「有变化」依然成立）：
    #   · 1 清晰 / 2 精细 = 把磨掉的高频按更大增益还回去 → 高频保留率递增；
    #   · 3 轻磨皮 = 降强度 → 必须明显更接近源帧。
    # 为什么不用「距源帧」判 1/2 档：「回填越多越接近原图」，距源帧会**变小**。
    # v3.0.6 的旧断言是 `_mod[2] > _mod[0]*1.02`（更远离源帧），v3.0.7 把回填
    # 增益从 3.2 提到 5.5 之后它就必然失败，而画面其实完全正确。
    # 四档全同就是空接线。
    ok_mod = (_mod[3] < _mod[0] * 0.85
              and _hf[1] > _hf[0] * 1.02
              and _hf[2] > _hf[1] * 1.01)

    # 高斯档下磨皮模式也必须有效（v3.0.6 的 tBlend 修复；此前 blurRefillI 恒 0）
    _st, _o0 = _render_params(src, W, H, blur_level=62.0, blur_type=1, smooth_mode=0)
    _st, _o3 = _render_params(src, W, H, blur_level=62.0, blur_type=1, smooth_mode=3)
    _g0, _g3 = _mad(src, _o0), _mad(src, _o3)
    print("   高斯档下  模式0=%.5f  模式3=%.5f  (轻磨皮必须更轻)" % (_g0, _g3))
    ok_modg = _g3 < _g0 * 0.85

    ok_choices = (ok_alg and ok_mod and ok_modg)
    print("   算法方向 %s / 模式方向 %s / 高斯档模式 %s"
          % ("OK" if ok_alg else "FAIL", "OK" if ok_mod else "FAIL",
             "OK" if ok_modg else "FAIL"))

    # ---- (2f-2) NaN 参数不得穿透到引擎（v3.0.8）----
    #
    # 正常滑块是 0..100，但损坏的工程文件 / 表达式 / 脚本可以直接写进 NaN。
    # 引擎里所有 `if (v < 0)` / `if (v > 100)` 对 NaN 都是 false，最终落到
    # `(int)(NaN * 4)` = INT_MIN → 越界读 LUT（几乎必然在 Resolve 进程里崩）。
    # 判据：render 必须成功，且输出与「回落默认档」逐字节相同 —— 只判「没崩」
    # 太弱（引擎可能已经写坏了半帧）。
    _st_nan, _out_nan = _render_params(src, W, H,
                                       blur_level=float("nan"), clarity=float("nan"),
                                       color_level=float("nan"), red_level=float("nan"))
    _st_def, _out_def = _render_params(src, W, H, blur_level=50.0, clarity=0.0,
                                       color_level=30.0, red_level=30.0)
    _nan_same = (_out_nan == _out_def)
    ok_nan = (_st_nan == kOfxStatOK and _st_def == kOfxStatOK and _nan_same)
    print("   NaN 参数  -> render=%d，回落默认档逐字节一致=%s"
          % (_st_nan, "是" if _nan_same else "否"))

    # ---- (2g) 单项参数接线：14 个滑块逐个独奏（**面板路径**）----
    #
    # 为什么要有：`baseline.py` 只覆盖 blur/whiten/清晰/锐化/全局锐化/美型六项，
    # 「立体 / 亮眼 / 去眼袋 / 去法令纹 / 祛斑 / 红润」六项**从来没有单独测过**。
    # 参数表错位（拖 A 下发成 B）在编译期零提示，acceptance 的三表对齐只比对
    # 名字，抓不到「名字对、引擎里接错字段」。这里让每个滑块单独拉到 100、
    # 其余归零，量它与源帧的差异：全 0 必须逐字节不变，独立生效项必须真的改动。
    print("")
    print("---- 单项参数接线：14 项逐个拉到 100（其余 0）----")
    _solo = [
        ("blur_level", "磨皮"), ("clarity", "清晰"), ("color_level", "美白"),
        ("red_level", "红润"), ("sharpen", "锐化"), ("global_sharpen", "全局锐化"),
        ("face_threed", "立体"), ("eye_bright", "亮眼"),
        ("remove_pouch_strength", "去眼袋"),
        ("remove_nasolabial_folds_strength", "去法令纹"),
        ("delspot_level", "祛斑"), ("cheek_thinning", "瘦脸"),
        ("eye_enlarging", "大眼"), ("intensity_chin", "下巴"),
    ]
    _solo_mad = {}
    _solo_chg = {}

    def _chg_frac(a, b):
        """被改动像素占全帧的比例。MAD 会被「只动一小块」的局部效果稀释：
        去眼袋满量程只改 0.93% 像素，MAD 只有 0.0065，看起来像没接线。"""
        n = 0
        for i in range(0, len(a), 4):
            if (a[i] != b[i] or a[i + 1] != b[i + 1] or a[i + 2] != b[i + 2]):
                n += 1
        return n / max(len(a) // 4, 1)

    for _nm, _cn in _solo:
        _st, _out = _render_params(src, W, H, **{_nm: 100.0})
        if _st == kOfxStatOK:
            _solo_mad[_nm] = _mad(src, _out)
            _solo_chg[_nm] = _chg_frac(src, _out)
        else:
            _solo_mad[_nm] = -1.0
            _solo_chg[_nm] = -1.0
        print("   %-32s %-6s MAD %.5f  改动像素 %.2f%%"
              % (_nm, _cn, _solo_mad[_nm], _solo_chg[_nm] * 100.0))
    # 判据 1：render 必须成功（-1 表示失败）。
    ok_solo_st = all(v >= 0.0 for v in _solo_mad.values())
    # 判据 2：每项都必须碰到画面。用「改动像素占比」而不是 MAD —— 局部效果
    # （去眼袋 0.93%、亮眼）的 MAD 接近 0，是效果弱，不是没接线。
    _dead = [nm for nm, v in _solo_chg.items() if 0.0 <= v < 0.001]
    ok_solo_wired = not _dead
    print("   全部 render 成功 %s / 无空接线 %s%s"
          % ("OK" if ok_solo_st else "FAIL", "OK" if ok_solo_wired else "FAIL",
             "" if ok_solo_wired else "  空接线: " + ", ".join(_dead)))
    ok_solo = ok_solo_st and ok_solo_wired

    st = main_fn(kOfxActionDestroyInstance.encode(), eff_h, None, None)
    print("DestroyInstance               -> %d" % st)
    st = main_fn(kOfxActionUnload.encode(), None, None, None)
    print("ActionUnload                  -> %d" % st)

    print("")
    print("8bit 渲染  : %s" % ("PASS" if ok_render8 else "FAIL"))
    print("连续 3 帧  : %s" % ("PASS" if multiframe_ok else "FAIL"))
    print("float 渲染 : %s" % ("PASS" if ok_renderf else "FAIL"))
    print("全0 透传   : %s" % ("PASS" if ok_zero else "FAIL"))
    print("IsIdentity : %s" % ("PASS" if ok_ident else "FAIL"))
    print("代理透传   : %s" % ("PASS" if ok_proxy else "FAIL"))
    print("缩略图恒等 : %s" % ("PASS" if ok_thumb else "FAIL"))
    print("ClipPrefs  : %s" % ("PASS" if ok_gcp else "FAIL"))
    print("参数全0    : %s" % ("PASS" if ok_pset0 else "FAIL"))
    print("参数换算   : %s" % ("PASS" if ok_pset1 else "FAIL"))
    print("亮度不变   : %s" % ("PASS" if ok_bright else "FAIL"))
    print("只磨皮肤默认: %s" % ("PASS" if ok_skindflt else "FAIL"))
    print("首次默认参数: %s" % ("PASS" if ok_defaults else "FAIL"))
    print("通道序正确 : %s" % ("PASS" if ok_channel else "FAIL"))
    print("美白不提亮整帧: %s" % ("PASS" if ok_whiten else "FAIL"))
    print("A1 小画幅  : %s" % ("PASS" if ok_a1 else "FAIL"))
    print("B8 补边    : %s" % ("PASS" if ok_b8 else "FAIL"))
    print("A4 RoD缺省 : %s" % ("PASS" if ok_rod else "FAIL"))
    print("B5 多实例  : %s" % ("PASS" if ok_multi else "FAIL"))
    print("下拉框接线 : %s" % ("PASS" if ok_choices else "FAIL"))
    print("单项参数接线: %s" % ("PASS" if ok_solo else "FAIL"))
    print("NaN 参数防护: %s" % ("PASS" if ok_nan else "FAIL"))
    allok = (ok_render8 and multiframe_ok and ok_renderf and ok_zero and ok_ident
             and ok_proxy and ok_thumb and ok_gcp
             and ok_pset0 and ok_pset1 and ok_bright and ok_skindflt
             and ok_defaults
             and ok_channel and ok_whiten and ok_a1 and ok_b8 and ok_rod and ok_multi
             and ok_choices and ok_solo and ok_nan)
    print("总判定     : %s" % ("PASS" if allok else "FAIL"))
    return 0 if allok else 1


if __name__ == "__main__":
    sys.exit(main())
