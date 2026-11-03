# 列出 CNamaSDK.dll 的导出名，过滤出与「参数/掩膜/局部」相关的 API。
# 用法: py probe_sdk_exports.py [keyword]
import os
import struct
import sys


def read_exports(path):
    blob = open(path, "rb").read()
    pe = struct.unpack_from("<I", blob, 0x3C)[0]
    assert blob[pe:pe + 4] == b"PE\0\0"
    coff = pe + 4
    nsec = struct.unpack_from("<H", blob, coff + 2)[0]
    optsz = struct.unpack_from("<H", blob, coff + 16)[0]
    opt = coff + 20
    magic = struct.unpack_from("<H", blob, opt)[0]
    dd = opt + (112 if magic == 0x20B else 96)
    exp_rva, exp_sz = struct.unpack_from("<II", blob, dd)
    secs = []
    so = opt + optsz
    for i in range(nsec):
        b = so + i * 40
        name = blob[b:b + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", blob, b + 8)
        secs.append((name, vaddr, vsize, raddr, rsize))

    def r2o(rva):
        for nm, va, vs, ra, rs in secs:
            if va <= rva < va + max(vs, rs):
                return ra + (rva - va)
        return None

    eo = r2o(exp_rva)
    if eo is None:
        return []
    nfun, nnam = struct.unpack_from("<II", blob, eo + 20)
    afun, anam, aords = struct.unpack_from("<III", blob, eo + 28)
    no = r2o(anam)
    out = []
    for i in range(nnam):
        r = struct.unpack_from("<I", blob, no + i * 4)[0]
        o = r2o(r)
        if o is None:
            continue
        end = blob.index(b"\0", o)
        out.append(blob[o:end].decode("ascii", "replace"))
    return sorted(out)


def main():
    engine = os.environ.get(
        "NAMA_ENGINE_DIR",
        r"C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle\Contents\Win64")
    path = os.path.join(engine, "CNamaSDK.dll")
    names = read_exports(path)
    print("total exports: %d" % len(names))
    keys = [k.lower() for k in sys.argv[1:]]
    if not keys:
        for n in names:
            print("  %s" % n)
        return 0
    for n in names:
        ln = n.lower()
        if any(k in ln for k in keys):
            print("  %s" % n)
    return 0


if __name__ == "__main__":
    sys.exit(main())
