#!/usr/bin/env python3
"""Generate a proxy-DLL export list from what the game exe imports from a DLL.

    python3 tools/gen_proxy.py --exe "$GAME_DIR/F.E.A.R. 3.exe" --dll steam_api.dll \
        --from-dll "$GAME_DIR/steam_api_orig.dll" --data g_pSteamClientGameServer \
        --def steam_api.def --inc src/proxy_exports.inc --prefix steam

Emits a .def aliasing every export name onto a plain C symbol <prefix>_<n>, and
an X-macro include listing (index, exported name) for src/proxy.cpp, which
builds one jmp thunk per entry. By default the names come from what the exe
imports; --from-dll takes the complete export table of the original DLL instead
(a superset, so anything the game resolves through GetProcAddress at runtime
is covered too). Data exports (--data NAME, repeatable) cannot be jump thunks;
they become PE forwarders to <lib>_orig.NAME. Pure stdlib.
"""
import argparse, struct


def imports_of(exe_path, dll_name):
    d = open(exe_path, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    opt = pe + 24
    opt_sz = struct.unpack_from("<H", d, pe + 20)[0]
    secs = []
    for i in range(nsec):
        o = opt + opt_sz + i * 40
        vsz, va, rsz, rptr = struct.unpack_from("<IIII", d, o + 8)
        secs.append((va, max(vsz, rsz), rptr))

    def rva2off(rva):
        for va, sz, rptr in secs:
            if va <= rva < va + sz:
                return rptr + (rva - va)
        raise ValueError(f"rva {rva:#x} not in any section")

    def cstr(off):
        e = d.find(b"\0", off)
        return d[off:e].decode("ascii")

    imp_rva = struct.unpack_from("<I", d, opt + 104)[0]  # DataDirectory[1] (imports), PE32
    o = rva2off(imp_rva)
    names = []
    while True:
        oft, _, _, name_rva, ft = struct.unpack_from("<IIIII", d, o)
        if not name_rva:
            break
        if cstr(rva2off(name_rva)).lower() == dll_name.lower():
            t = rva2off(oft or ft)
            while True:
                thunk = struct.unpack_from("<I", d, t)[0]
                if not thunk:
                    break
                if thunk & 0x80000000:
                    names.append(f"#{thunk & 0xFFFF}")  # by ordinal (not supported by the .def alias)
                else:
                    names.append(cstr(rva2off(thunk) + 2))
                t += 4
        o += 20
    return names


def exports_of(dll_path):
    """Every named export of a PE DLL, in export-table order."""
    d = open(dll_path, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    opt = pe + 24
    opt_sz = struct.unpack_from("<H", d, pe + 20)[0]
    secs = []
    for i in range(nsec):
        o = opt + opt_sz + i * 40
        vsz, va, rsz, rptr = struct.unpack_from("<IIII", d, o + 8)
        secs.append((va, max(vsz, rsz), rptr))

    def rva2off(rva):
        for va, sz, rptr in secs:
            if va <= rva < va + sz:
                return rptr + (rva - va)
        raise ValueError(f"rva {rva:#x} not in any section")

    exp_rva = struct.unpack_from("<I", d, opt + 96)[0]  # DataDirectory[0] (exports), PE32
    if not exp_rva:
        return []
    e = rva2off(exp_rva)
    n_names = struct.unpack_from("<I", d, e + 24)[0]
    names_rva = struct.unpack_from("<I", d, e + 32)[0]
    out = []
    for i in range(n_names):
        rva = struct.unpack_from("<I", d, rva2off(names_rva) + i * 4)[0]
        o = rva2off(rva)
        out.append(d[o:d.find(b"\0", o)].decode("ascii"))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True)
    ap.add_argument("--dll", required=True)
    ap.add_argument("--from-dll", help="take the export table of this DLL (the original) instead of the exe's imports")
    ap.add_argument("--data", action="append", default=[], help="export NAME as a forwarder (data exports)")
    ap.add_argument("--def", dest="def_path", required=True)
    ap.add_argument("--inc", required=True)
    ap.add_argument("--prefix", default="proxy")
    a = ap.parse_args()
    imported = imports_of(a.exe, a.dll)
    if not imported:
        raise SystemExit(f"{a.dll}: no imports found in {a.exe}")
    ordinals = [n for n in imported if n.startswith("#")]
    if ordinals:
        raise SystemExit(f"{a.dll}: ordinal imports are not supported: {ordinals}")
    if a.from_dll:
        names = exports_of(a.from_dll)
        missing = [n for n in imported if n not in names]
        if missing:
            raise SystemExit(f"{a.from_dll} lacks imports the exe needs: {missing}")
        source = f"the export table of {a.from_dll.rsplit('/', 1)[-1]} ({len(imported)} of them imported by the exe)"
    else:
        names = imported
        source = "the game's import table"
    data = [n for n in names if n in a.data]
    names = [n for n in names if n not in a.data]
    lib = a.dll.rsplit(".", 1)[0]
    with open(a.def_path, "w") as f:
        f.write(f"; Generated by tools/gen_proxy.py from {source} - do not edit by hand.\n")
        f.write(f"; {len(names)} symbols of {a.dll}, each aliased onto a jmp thunk in src/proxy.cpp that\n")
        f.write(f"; jumps into {lib}_orig.dll" + (f"; {len(data)} data export(s) forwarded there directly.\n" if data else ".\n"))
        f.write(f"LIBRARY {lib}\nEXPORTS\n")
        for i, n in enumerate(names):
            f.write(f"    {n} = {a.prefix}_{i}\n")
        for n in data:
            f.write(f"    {n} = {lib}_orig.{n} DATA\n")
    with open(a.inc, "w") as f:
        f.write("// Generated by tools/gen_proxy.py - do not edit by hand.\n")
        f.write(f"// X(index, exported name) for every symbol the exe imports from {a.dll}.\n")
        f.write("#define PROXY_EXPORTS(X) \\\n")
        for i, n in enumerate(names):
            f.write(f'  X({i}, "{n}") \\\n')
        f.write("\n")
        f.write(f"#define PROXY_EXPORT_COUNT {len(names)}\n")
    print(f"{a.dll}: {len(names)} exports -> {a.def_path}, {a.inc}")


if __name__ == "__main__":
    main()
