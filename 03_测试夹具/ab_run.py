# -*- coding: utf-8 -*-
"""直接在 Python 里跑一次 A/B，绕开 shell 引号地狱。

用法:
    py ab_run.py <extra_a> <extra_b> <panel_a_json> <panel_b_json>
照片和 DLL 走环境变量/固定默认值。
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import probe_ab as pa  # noqa: E402

PHOTO = os.environ.get(
    "AB_PHOTO",
    r"C:\Users\Administrator\Pictures\A7300538.jpg")


def _load_panel(arg, default):
    """argv 可以是 JSON 文本，也可以是 JSON 文件路径（shell 引号不可靠时用文件）。"""
    if not arg:
        return default
    if os.path.isfile(arg):
        # utf-8-sig：容忍 PowerShell Set-Content 写出的 BOM
        with open(arg, "r", encoding="utf-8-sig") as f:
            return json.load(f)
    return json.loads(arg)


def main():
    extra_a = sys.argv[1] if len(sys.argv) > 1 else "none"
    extra_b = sys.argv[2] if len(sys.argv) > 2 else "none"
    panel_a = _load_panel(sys.argv[3] if len(sys.argv) > 3 else "", {
        "blur_level": 100, "skin_detect": 1})
    panel_b = _load_panel(sys.argv[4] if len(sys.argv) > 4 else "", {
        "blur_level": 100, "skin_detect": 1, "color_level": 100})
    dll = os.environ.get(
        "NAMA_OFX_DLL",
        r"C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle"
        r"\Contents\Win64\DaVinciBeauty.ofx")
    src = pa.load_frame(PHOTO)

    print("panel_a=%s" % json.dumps(panel_a, ensure_ascii=False))
    print("panel_b=%s" % json.dumps(panel_b, ensure_ascii=False))

    # 自检：同一配置两帧必须完全一致
    s1 = pa.render_stable(dll, src, extra_a, panel_a)
    s2 = pa.render_stable(dll, src, extra_a, panel_a)
    print("SELFCHECK " + json.dumps(pa.diff(s1, s2), ensure_ascii=False))

    a = pa.render_stable(dll, src, extra_a, panel_a)
    b = pa.render_stable(dll, src, extra_b, panel_b)
    print("AB " + json.dumps(pa.diff(a, b), ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
