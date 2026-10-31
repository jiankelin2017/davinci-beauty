# 扫描 SDK graphics bundle 里的可见字符串，看看它到底支持哪些参数名。
# 目的：如果 face_beautification.bundle 内部有类似 "skin_only" / "face_only"
# 之类的开关，就能用它把美白限制在人脸，而不是自己做掩膜。
#
# 用法: py probe_bundle_strings.py
import os
import re
import sys

ENGINE = os.environ.get(
    "NAMA_ENGINE_DIR",
    r"C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle\Contents\Win64",
)

TARGETS = [
    os.path.join(ENGINE, "graphics", "face_beautification.bundle"),
    os.path.join(ENGINE, "graphics", "face_makeup.bundle"),
]

# 已知的 SDK 参数名，用来判断哪些字符串是参数、哪些是别的东西
KNOWN = [
    "blur_level", "blur_type", "skin_detect", "color_level", "red_level",
    "clarity", "sharpen", "face_threed", "eye_bright",
    "remove_pouch_strength", "face_shape", "face_shape_level",
    "filter_name", "filter_level", "heavy_blur", "use_landmark",
    "is_opengl_es", "white", "whiten",
]

# 看起来像「局部/范围」语义的关键字
SCOPE_HINT = re.compile(
    r"skin|face|local|region|mask|scope|body|hair|eye|mouth|nose|"
    r"landmark|human|hand|head|neck|forehead|cheek|blur",
    re.IGNORECASE,
)


def strings(blob, minlen=4):
    out = []
    for m in re.finditer(rb"[\x20-\x7e]{%d,}" % minlen, blob):
        out.append(m.group().decode("ascii", "replace"))
    return out


def main():
    rc = 0
    for path in TARGETS:
        if not os.path.exists(path):
            print("MISSING %s" % path)
            rc = 1
            continue
        blob = open(path, "rb").read()
        ss = strings(blob)
        print("=" * 70)
        print("%s  (%d bytes, %d strings)" % (os.path.basename(path), len(blob), len(ss)))
        print("=" * 70)
        uniq = []
        seen = set()
        for s in ss:
            if s in seen:
                continue
            seen.add(s)
            uniq.append(s)

        hits = [s for s in uniq if any(k in s for k in KNOWN)]
        print("-- 已知参数名命中 (%d) --" % len(hits))
        for s in hits[:120]:
            print("   %s" % s)

        scope = [s for s in uniq if SCOPE_HINT.search(s)]
        print("-- 范围/局部语义候选 (%d) --" % len(scope))
        for s in scope[:200]:
            print("   %s" % s)
    return rc


if __name__ == "__main__":
    sys.exit(main())
