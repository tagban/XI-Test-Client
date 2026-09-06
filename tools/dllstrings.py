"""Static analysis of the unpacked client DLL, without Ghidra.

Ghidra's decompiler is the right tool for reading logic, but a lot of useful
work is just strings, constants and cross-references, and that needs no
decompiler and no running Ghidra. This does that part against the DLL that
tools/unpackmain.py produces.

    tools/dllstrings.py <FFXiMain.unpacked.dll> strings <regex>
    tools/dllstrings.py <FFXiMain.unpacked.dll> xref <hex-va>      # who points at this address
    tools/dllstrings.py <FFXiMain.unpacked.dll> float <value>      # where a float constant lives
    tools/dllstrings.py <FFXiMain.unpacked.dll> at <hex-va>        # what string is at this address

Cross-references are absolute-pointer matches, which is what a 32-bit PE with a
fixed image base uses for string and vtable references. A string that no
pointer names is either dead or reached through a computed address; the DLL has
both, so a zero result is a fact, not a failure.
"""
import struct, sys, re


def load(path):
    d = open(path, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    base = struct.unpack_from("<I", d, pe + 24 + 28)[0]
    nsec, = struct.unpack_from("<H", d, pe + 6)
    opt, = struct.unpack_from("<H", d, pe + 20)
    secs = []
    for k in range(nsec):
        s = d[pe + 24 + opt + k * 40:][:40]
        name = s[:8].rstrip(b"\0").decode("latin1", "replace")
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", s, 8)
        secs.append((name, vaddr, vsize, rawsize, rawptr))
    return d, base, secs


def where(d, base, secs, off):
    for name, va0, vs, rs, rp in secs:
        if rp <= off < rp + rs:
            return name, base + va0 + (off - rp)
    return "?", 0


def va_to_off(base, secs, va):
    r = va - base
    for name, va0, vs, rs, rp in secs:
        if va0 <= r < va0 + max(vs, rs):
            return rp + (r - va0)
    return None


def cstr(d, base, secs, va, n=128):
    o = va_to_off(base, secs, va)
    if o is None:
        return None
    e = d.find(b"\0", o, o + n)
    return d[o:e].decode("latin1") if e > 0 else None


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    d, base, secs = load(argv[1])
    cmd = argv[2]

    if cmd == "strings":
        rx = re.compile(argv[3], re.I)
        seen = set()
        for m in re.finditer(rb"[\x20-\x7e]{4,}", d):
            s = m.group().decode("latin1")
            if rx.search(s) and s not in seen:
                seen.add(s)
                name, va = where(d, base, secs, m.start())
                print(f"{va:08x} {name:<8} {s}")

    elif cmd == "xref":
        va = int(argv[3], 16)
        needle = struct.pack("<I", va)
        i = 0
        while True:
            j = d.find(needle, i)
            if j < 0:
                break
            name, refva = where(d, base, secs, j)
            print(f"{refva:08x} {name}")
            i = j + 1

    elif cmd == "float":
        needle = struct.pack("<f", float(argv[3]))
        i = 0
        while True:
            j = d.find(needle, i)
            if j < 0:
                break
            name, va = where(d, base, secs, j)
            if name in (".text", ".rdata", ".data"):
                print(f"{va:08x} {name}")
            i = j + 1

    elif cmd == "at":
        print(cstr(d, base, secs, int(argv[3], 16)))

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
