#!/usr/bin/env python3
"""Static probe for the decrypted re0hd image: MT Framework DTI classes, their
vtables, and the event-script command table.

Works on the image the mod dumps (DumpImage=1 -> re0hd.dumped.exe, sections at
their virtual addresses, static initialisers already run so the MtDTI objects
exist in .data). Pure stdlib.

    python3 tools/find_dti.py --image re0hd.dumped.exe --list                # every class: name, parent, size, id, vtable
    python3 tools/find_dti.py --image ... --class sGamePause --class sPlayer  # one class, with its vtable entries
    python3 tools/find_dti.py --image ... --script                            # the command table
    python3 tools/find_dti.py --image ... --xref 0xCD7158                    # code that embeds this immediate (string/object refs)
    python3 tools/find_dti.py --image ... --strref mPause                     # code that references this C string
    python3 tools/find_dti.py --image ... --slot 4                            # getDTI vtable slot (default 4)
"""
import argparse, re, struct, sys


class Image:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        d = self.d
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        nsec = struct.unpack_from("<H", d, pe + 6)[0]
        opt = pe + 24
        self.base = struct.unpack_from("<I", d, opt + 28)[0]
        opt_sz = struct.unpack_from("<H", d, pe + 20)[0]
        self.secs = {}
        for i in range(nsec):
            o = opt + opt_sz + i * 40
            name = d[o:o + 8].rstrip(b"\0").decode(errors="replace")
            vsz, va, rsz, rptr = struct.unpack_from("<IIII", d, o + 8)
            self.secs[name] = (self.base + va, vsz, rptr, rsz)

    def range(self, name):
        va, vsz, rptr, rsz = self.secs[name]
        return va, va + vsz

    def off(self, va):
        for name, (sva, vsz, rptr, rsz) in self.secs.items():
            if sva <= va < sva + max(vsz, rsz):
                return rptr + (va - sva)
        return None

    def u32(self, va):
        o = self.off(va)
        return struct.unpack_from("<I", self.d, o)[0] if o is not None and o + 4 <= len(self.d) else None

    def cstr(self, va, n=96):
        o = self.off(va)
        if o is None:
            return None
        e = self.d.find(b"\0", o, o + n)
        if e < 0:
            return None
        s = self.d[o:e]
        return s.decode() if all(0x20 <= c < 0x7F for c in s) else None

    def in_sec(self, name, va):
        lo, hi = self.range(name)
        return lo <= va < hi

    def find_all(self, needle, sec):
        va, vsz, rptr, rsz = self.secs[sec]
        blob = self.d[rptr:rptr + max(vsz, rsz)]
        return [va + m.start() for m in re.finditer(re.escape(needle), blob)]

    def find_cstring(self, s, sec=".rdata"):
        hits = self.find_all(b"\0" + s.encode() + b"\0", sec)
        return [h + 1 for h in hits]


def classes(img, slot):
    """All MtDTI objects, keyed by name."""
    root_names = img.find_cstring("MtObject")
    root = None
    for s in root_names:
        for hit in img.find_all(struct.pack("<I", s), ".data"):
            base = hit - 4
            vt = img.u32(base)
            if vt and img.in_sec(".rdata", vt) and img.in_sec(".text", img.u32(vt) or 0):
                root = base
                break
        if root:
            break
    if not root:
        raise SystemExit("MtObject DTI not found - is this the decrypted dump with static init done?")
    dti_vt = img.u32(root)
    out = {}
    for hit in img.find_all(struct.pack("<I", dti_vt), ".data"):
        name = img.cstr(img.u32(hit + 4) or 0)
        if not name:
            continue
        parent = img.u32(hit + 0x10)
        stub_hits = img.find_all(b"\xb8" + struct.pack("<I", hit) + b"\xc3", ".text")
        vt = 0
        if stub_hits:
            slots = img.find_all(struct.pack("<I", stub_hits[0]), ".rdata")
            if slots:
                vt = slots[0] - 4 * slot
        out[name] = dict(dti=hit, parent=parent, size=img.u32(hit + 0x18), id=img.u32(hit + 0x1C),
                         stub=stub_hits[0] if stub_hits else 0, vtable=vt, nslots=len(slots) if stub_hits else 0)
    for c in out.values():
        c["parent_name"] = img.cstr(img.u32(c["parent"] + 4) or 0) if c["parent"] else "-"
    return out


def chain(out, name):
    names = [name]
    while True:
        c = out.get(names[-1])
        if not c or c["parent_name"] in ("-", None) or c["parent_name"] in names:
            return names
        names.append(c["parent_name"])


def script_table(img):
    anchors = img.find_cstring("PlayerMutekiSet")
    row = None
    for s in anchors:
        for hit in img.find_all(struct.pack("<I", s), ".rdata"):
            row = hit - 4
            break
    if row is None:
        return []

    def ok(r):
        a, b, c = img.u32(r), img.u32(r + 4), img.u32(r + 8)
        if a is None or b is None or c is None:
            return False
        sig = img.cstr(a)
        return sig is not None and re.fullmatch(r"[A-Za-z0-9]{0,31}", sig) is not None and img.cstr(b) and img.in_sec(".text", c)

    b = row
    while ok(b - 12):
        b -= 12
    e = row
    while ok(e + 12):
        e += 12
    rows = []
    for r in range(b, e + 12, 12):
        rows.append((img.cstr(img.u32(r + 4)), img.cstr(img.u32(r)), img.u32(r + 8), r))
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image", required=True)
    ap.add_argument("--slot", type=int, default=4)
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--class", dest="cls", action="append", default=[])
    ap.add_argument("--script", action="store_true")
    ap.add_argument("--xref", action="append", default=[], type=lambda s: int(s, 0))
    ap.add_argument("--strref", action="append", default=[])
    a = ap.parse_args()
    img = Image(a.image)
    rva = lambda va: va - img.base if va else 0
    if a.list or a.cls:
        out = classes(img, a.slot)
        print(f"{len(out)} classes (getDTI slot {a.slot})")
        names = sorted(out) if a.list else a.cls
        for n in names:
            c = out.get(n)
            if not c:
                print(f"  {n}: not found")
                continue
            print(f"  {n:28s} dti=exe+{rva(c['dti']):06X} parent={c['parent_name']:20s} size=0x{c['size']:08X} id=0x{c['id']:08X} "
                  f"stub=exe+{rva(c['stub']):06X} vtable=exe+{rva(c['vtable']):06X} slots={c['nslots']}")
            if a.cls:
                print("    chain: " + " -> ".join(chain(out, n)))
                if c["vtable"]:
                    for i in range(0, 48):
                        fn = img.u32(c["vtable"] + 4 * i)
                        if fn is None or not img.in_sec(".text", fn):
                            break
                        print(f"    vtable[{i:2d}] = exe+{rva(fn):06X}")
    if a.script:
        rows = script_table(img)
        print(f"{len(rows)} script commands")
        for name, sig, fn, r in rows:
            print(f"  {name:32s} {sig:12s} handler=exe+{rva(fn):06X} row=exe+{rva(r):06X}")
    for s in a.strref:
        for sva in img.find_cstring(s):
            print(f'"{s}" at exe+{rva(sva):06X}:')
            for h in img.find_all(struct.pack("<I", sva), ".text"):
                print(f"  code ref at exe+{rva(h):06X}")
            for h in img.find_all(struct.pack("<I", sva), ".rdata") + img.find_all(struct.pack("<I", sva), ".data"):
                print(f"  data ref at exe+{rva(h):06X}")
    for v in a.xref:
        print(f"references to {v:#x}:")
        for sec in (".text", ".rdata", ".data"):
            for h in img.find_all(struct.pack("<I", v), sec):
                print(f"  {sec} exe+{rva(h):06X}")


if __name__ == "__main__":
    main()
