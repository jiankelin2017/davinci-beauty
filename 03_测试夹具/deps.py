#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""解析 PE 导入表，定位 CNamaSDK.dll / fuai.dll 的缺失依赖。"""
import os, struct, ctypes, sys

K32 = ctypes.WinDLL("kernel32", use_last_error=True)
K32.LoadLibraryExW.argtypes = [ctypes.c_wchar_p, ctypes.c_void_p, ctypes.c_uint32]
K32.LoadLibraryExW.restype = ctypes.c_void_p


def pe_imports(path):
    d = open(path, "rb").read()
    e_lfanew = struct.unpack_from("<I", d, 0x3C)[0]
    nto = e_lfanew + 4
    mach, nsec, _, _, _, optsz, chars = struct.unpack_from("<HHIIIHH", d, nto)
    assert mach == 0x8664, "not x64: %X" % mach
    opt = nto + 20
    magic = struct.unpack_from("<H", d, opt)[0]
    assert magic == 0x20B, "not PE32+"
    # data directory offset: opt + 112 (NumberOfRvaAndSizes at opt+108)
    nrva = struct.unpack_from("<I", d, opt + 108)[0]
    import_rva = struct.unpack_from("<I", d, opt + 112 + 8 * 1)[0]
    import_sz = struct.unpack_from("<I", d, opt + 112 + 8 * 1 + 4)[0]
    if import_rva == 0:
        return []
    secs = []
    so = opt + optsz
    for i in range(nsec):
        base = so + i * 40
        name = d[base:base + 8].rstrip(b"\0").decode("ascii", "replace")
        vsz, vaddr, rsize, raddr = struct.unpack_from("<IIII", d, base + 8)
        secs.append((name, vaddr, vsz, raddr, rsize))

    def r2o(rva):
        for n, va, vs, ra, rs in secs:
            if va <= rva < va + max(vs, rs):
                return ra + (rva - va)
        return None

    out = []
    off = r2o(import_rva)
    if off is None:
        return out
    while True:
        ilt_rva, ts, fc, name_rva, fat = struct.unpack_from("<IIIII", d, off)
        if ilt_rva == 0 and name_rva == 0:
            break
        noff = r2o(name_rva)
        nm = ""
        if noff is not None:
            end = d.index(b"\0", noff)
            nm = d[noff:end].decode("ascii", "replace")
        # 首个函数名
        j = 0
        toff = r2o(ilt_rva) if ilt_rva else None
        if toff is not None:
            j = 0
        out.append(nm)
        off += 20
    return out


def try_load(dll):
    h = K32.LoadLibraryExW(dll, None, 0x00001100)  # LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|DEFAULT_DIRS
    if h:
        return True, 0
    return False, ctypes.get_last_error()


if __name__ == "__main__":
    BIN = sys.argv[1] if len(sys.argv) > 1 else r"C:\Users\Administrator\AppData\Local\Temp\e2e_beauty\bin"
    for nm in ("CNamaSDK.dll", "fuai.dll", "w32-pthreads.dll"):
        p = os.path.join(BIN, nm)
        if not os.path.exists(p):
            print("%-20s <missing file>" % nm); continue
        ok, err = try_load(p)
        print("%-20s load=%s err=%d" % (nm, ok, err))
        if not ok:
            for dep in pe_imports(p):
                if not dep:
                    continue
                system_dir = os.path.join(os.environ.get("WINDIR", "C:\\Windows"), "System32", dep)
                exists_sys = os.path.exists(system_dir)
                exists_loc = os.path.exists(os.path.join(BIN, dep))
                print("      dep %-46s inSystem32=%-5s inLocalDir=%s" % (dep, exists_sys, exists_loc))
