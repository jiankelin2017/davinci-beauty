# 扫描 CNamaSDK.dll / fuai.dll 里出现的参数名字符串。
# 目的：找出 SDK 到底支持哪些效果参数，是否存在「只对人脸生效」的美白开关。
#
# 用法: py probe_sdk_strings.py
import os
import re
import sys

ENGINE = os.environ.get(
    "NAMA_ENGINE_DIR",
    r"C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle\Contents\Win64",
)

TARGETS = [
    os.path.join(ENGINE, "CNamaSDK.dll"),
    os.path.join(ENGINE, "fuai.dll"),
]

# 形如 snake_case 的标识符，且含有效果/范围相关关键字
ID_RE = re.compile(rb"[A-Za-z_][A-Za-z0-9_]{2,48}")
KEY_RE = re.compile(
    r"skin|face|white|level|blur|clarity|sharpen|eye|red|color|colour|"
    r"makeup|landmark|detect|body|shape|filter|beaut",
    re.IGNORECASE,
)


def main():
    for path in TARGETS:
        if not os.path.exists(path):
            print("MISSING %s" % path)
            continue
        blob = open(path, "rb").read()
        seen = []
        known = set()
        for m in ID_RE.finditer(blob):
            s = m.group().decode("ascii", "replace")
            if s in known:
                continue
            known.add(s)
            seen.append(s)
        hits = [s for s in seen if KEY_RE.search(s)]
        print("=" * 74)
        print("%s  (%d bytes, %d unique idents, %d keyword hits)"
              % (os.path.basename(path), len(blob), len(seen), len(hits)))
        print("=" * 74)
        for s in hits:
            print("   %s" % s)


if __name__ == "__main__":
    sys.exit(main())
