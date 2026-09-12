#!/usr/bin/env python3
"""SteamStub header reader and dump fixer for re0hd.exe.

    python3 tools/fix_dump.py --exe "$GAME_DIR/re0hd.exe"                       # print the decoded stub header
    python3 tools/fix_dump.py --exe "$GAME_DIR/re0hd.exe" --dump "$GAME_DIR/re0hd.dumped.exe" [--out fixed.exe]

The on-disk exe's entry point sits in the .bind section (the SteamStub v3.1
wrapper); the 0xF0-byte stub header just before it is XOR-rolled with the first
dword as the seed. Decoding it yields the original entry point (OEP), the app id
and the flags. `--dump` takes the image the mod wrote (DumpImage=1: sections at
their virtual addresses, .text decrypted) and writes a copy whose
AddressOfEntryPoint is the OEP, so Ghidra starts at WinMain's CRT entry rather
than the stub. Pure stdlib.
"""
import argparse, struct


def sections(d):
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    opt = pe + 24
    opt_sz = struct.unpack_from("<H", d, pe + 20)[0]
    out = []
    for i in range(nsec):
        o = opt + opt_sz + i * 40
        name = d[o:o + 8].rstrip(b"\0").decode(errors="replace")
        vsz, va, rsz, rptr = struct.unpack_from("<IIII", d, o + 8)
        out.append((name, va, vsz, rptr, rsz))
    return pe, opt, out


def rva2off(secs, rva):
    for name, va, vsz, rptr, rsz in secs:
        if va <= rva < va + max(vsz, rsz):
            return rptr + (rva - va)
    return None


def decode_header(d):
    pe, opt, secs = sections(d)
    ep = struct.unpack_from("<I", d, opt + 16)[0]
    off = rva2off(secs, ep - 0xF0)
    hdr = bytearray(d[off:off + 0xF0])
    key = struct.unpack_from("<I", hdr, 0)[0]
    for x in range(4, 0xF0, 4):
        val = struct.unpack_from("<I", hdr, x)[0]
        struct.pack_into("<I", hdr, x, val ^ key)
        key = val
    words = [struct.unpack_from("<I", hdr, x)[0] for x in range(0, 0xF0, 4)]
    text = next((s for s in secs if s[0] == ".text"), None)
    oep = None
    if words[1] in (0xC0DEC0DF, 0xC0DEC0DE) and text:
        lo, hi = text[1], text[1] + text[2]
        image_base = words[2]
        # +0x20 is the OEP in the v3.1 layout; fall back to the first .text RVA in the header.
        cands = [words[8]] + [w for w in words[3:] if lo < w < hi and w != text[1] and w != image_base]
        oep = next((w for w in cands if lo < w < hi), None)
    return ep, words, oep, secs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True)
    ap.add_argument("--dump")
    ap.add_argument("--out")
    ap.add_argument("--oep", type=lambda s: int(s, 0), help="override the OEP RVA")
    a = ap.parse_args()
    d = open(a.exe, "rb").read()
    ep, words, oep, secs = decode_header(d)
    print(f"stub entry point RVA {ep:#x}; decoded header (signature {words[1]:#x}):")
    for i, w in enumerate(words[:0x20]):
        print(f"  +{i*4:03x}: {w:08x}")
    if a.oep:
        oep = a.oep
    print(f"OEP RVA: {oep:#x}" if oep else "OEP not identified (use --oep)")
    if a.dump:
        dd = bytearray(open(a.dump, "rb").read())
        pe, opt, dsecs = sections(dd)
        if not oep:
            raise SystemExit("no OEP")
        struct.pack_into("<I", dd, opt + 16, oep)
        out = a.out or a.dump.rsplit(".", 1)[0] + ".fixed.exe"
        open(out, "wb").write(dd)
        print(f"wrote {out} with AddressOfEntryPoint={oep:#x}; sections:")
        for name, va, vsz, rptr, rsz in dsecs:
            print(f"  {name:8s} va={va:#x} vsize={vsz:#x} raw={rptr:#x} rsize={rsz:#x}")


if __name__ == "__main__":
    main()
