# 把 CNamaSDK.dll 里的全部 ASCII 串按关键字分组打印，用于判断 SDK 内部
# 「磨皮/美白」是走人脸通道还是整帧通道。
#
# 用法: py probe_sdk_strings2.py <keyword> [keyword...]
import os
import re
import sys

ENGINE = os.environ.get(
    "NAMA_ENGINE_DIR",
    r"C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle\Contents\Win64",
)

STR_RE = re.compile(rb"[\x20-\x7e]{4,120}")


def main():
    keys = [k.lower() for k in sys.argv[1:]] or ["whitten"]
    for fn in ("CNamaSDK.dll", "fuai.dll"):
        path = os.path.join(ENGINE, fn)
        if not os.path.exists(path):
            continue
        blob = open(path, "rb").read()
        out = []
        for m in STR_RE.finditer(blob):
            s = m.group().decode("ascii", "replace")
            low = s.lower()
            if any(k in low for k in keys):
                out.append((m.start(), s))
        print("=" * 74)
        print("%s: %d hits for %s" % (fn, len(out), keys))
        print("=" * 74)
        for off, s in out:
            print("0x%08x  %s" % (off, s))


if __name__ == "__main__":
    sys.exit(main())
