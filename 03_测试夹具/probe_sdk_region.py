# 按文件偏移区间打印 CNamaSDK.dll 里的 ASCII 串。
# 用途：SDK 的参数名集中在某一段字符串池里，整段 dump 出来看它到底认哪些参数。
#
# 用法: py probe_sdk_region.py <start_hex> <end_hex> [file]
import os
import re
import sys

ENGINE = os.environ.get(
    "NAMA_ENGINE_DIR",
    r"C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle\Contents\Win64")
STR_RE = re.compile(rb"[\x20-\x7e]{3,160}")


def main():
    start = int(sys.argv[1], 16)
    end = int(sys.argv[2], 16)
    fn = sys.argv[3] if len(sys.argv) > 3 else "CNamaSDK.dll"
    blob = open(os.path.join(ENGINE, fn), "rb").read()
    for m in STR_RE.finditer(blob, start, end):
        s = m.group().decode("ascii", "replace")
        print("0x%08x  %s" % (m.start(), s))
    return 0


if __name__ == "__main__":
    sys.exit(main())
