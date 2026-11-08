# -*- coding: utf-8 -*-
"""校验 OfxParamPropSecret：确认「锐化」已在界面上隐藏、其余七项照常显示。

直接复用 mock_host 的 OFX 宿主桩，只走到 DescribeInContext 就读参数属性，
不渲染、不建实例，几秒出结果。
"""
import ctypes
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import mock_host as mh  # noqa: E402

DLL = os.environ.get(
    "BEAUTY_OFX_DLL",
    # 默认指向**已安装**的那份：它和插件自己的运行时 DLL 放在同一目录
    # 同目录，describeInContext 不需要引擎真初始化也能读参数属性。
    r"C:\Program Files\Common Files\OFX\Plugins"
    r"\DaVinciBeauty.ofx.bundle\Contents\Win64\DaVinciBeauty.ofx")

EXPECT_SECRET = {
    "blur_level": 0, "clarity": 0, "color_level": 0, "red_level": 0,
    "sharpen": 1,                       # ← 唯一被隐藏的
    "face_threed": 0, "eye_bright": 0, "remove_pouch_strength": 0,
}

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.LoadLibraryExW.argtypes = [ctypes.c_wchar_p, ctypes.c_void_p, ctypes.c_uint32]
k32.LoadLibraryExW.restype = ctypes.c_void_p
if not k32.LoadLibraryExW(DLL, None, 0x1100):
    print("FAIL load dll err=%d" % ctypes.get_last_error())
    sys.exit(1)

lib = ctypes.WinDLL(DLL)
lib.OfxGetNumberOfPlugins.restype = ctypes.c_int
lib.OfxGetPlugin.restype = ctypes.POINTER(mh.OfxPlugin)
lib.OfxGetPlugin.argtypes = [ctypes.c_int]
n = lib.OfxGetNumberOfPlugins()
plug = lib.OfxGetPlugin(0)
p = plug.contents
print("DLL            : %s" % DLL)
print("插件版本        : %d.%d" % (p.pluginVersionMajor, p.pluginVersionMinor))

SETHOST = ctypes.CFUNCTYPE(None, ctypes.POINTER(mh.OfxHost))
SETHOST(p.setHost)(ctypes.byref(mh.HOST))

MAIN = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_char_p, ctypes.c_void_p,
                        ctypes.c_void_p, ctypes.c_void_p)
main_fn = MAIN(p.mainEntry)

eff = mh.Effect()
eff_h = mh.newh(eff)
assert main_fn(mh.kOfxActionLoad.encode(), None, None, None) == mh.kOfxStatOK
assert main_fn(mh.kOfxActionDescribe.encode(), eff_h, None, None) == mh.kOfxStatOK
assert main_fn(mh.kOfxImageEffectActionDescribeInContext.encode(),
               eff_h, None, None) == mh.kOfxStatOK

print("")
print("%-24s %-8s %-8s %-8s %s" % ("参数", "secret", "期望", "default", "结论"))
print("-" * 74)
bad = 0
for nm in EXPECT_SECRET:
    pm = eff.params.get(nm)
    if pm is None:
        print("%-24s %-8s %-8s %-8s %s" % (nm, "缺失", EXPECT_SECRET[nm], "-", "FAIL"))
        bad += 1
        continue
    got = pm.props.get("OfxParamPropSecret", 0)
    got = 0 if got is None else int(got)
    dflt = pm.props.get("OfxParamPropDefault", 0)
    exp = EXPECT_SECRET[nm]
    ok = (got == exp)
    if not ok:
        bad += 1
    print("%-24s %-8d %-8d %-8s %s" %
          (nm, got, exp, dflt, "OK 隐藏" if (ok and exp) else ("OK 显示" if ok else "FAIL")))

# 附带确认：blur_type / skin_detect 这两个非滑块参数不受影响
print("")
for nm in ("blur_type", "skin_detect"):
    pm = eff.params.get(nm)
    got = pm.props.get("OfxParamPropSecret", 0) if pm else None
    print("  %-24s secret=%s（应保持显示）" % (nm, 0 if got is None else int(got)))

print("")
print("结论：%s" % ("全部符合预期，锐化已从界面隐藏、接线保留" if bad == 0
                  else "有 %d 项不符合预期" % bad))
sys.exit(1 if bad else 0)
