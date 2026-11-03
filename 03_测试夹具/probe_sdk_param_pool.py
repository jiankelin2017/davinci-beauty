# 打印 CNamaSDK.dll 里 skin_detect 附近的字符串池 —— 用来确认
# face_beautification_v2 这个 item 到底认哪些参数（以及有没有区域相关的键）。
import os
import re

E = os.environ.get(
    "NAMA_ENGINE_DIR",
    r"C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle\Contents\Win64")
b = open(os.path.join(E, "CNamaSDK.dll"), "rb").read()
SPAN = 2600
for m in re.finditer(b"skin_detect", b):
    s = m.start()
    print("=== offset 0x%08x ===" % s)
    for t in re.finditer(rb"[\x20-\x7e]{4,64}", b[max(0, s - SPAN):s + SPAN]):
        print("   ", t.group().decode())
