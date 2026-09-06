"""Unpack FFXiMain.dll's code section, so it can be read at all.

The shipped DLL has no code on disk. `.text` is 3.3 MB of nothing - a section
header with a virtual size and no bytes behind it - and a stub at the tail of
the `POL1` section inflates it into place when the DLL loads. Disassembling the
file as shipped finds the stub and nothing else: 277 functions against the
14,475 that are really there.

The stub's whole job is one call,

    unpack(src = POL1 start, srcLen = 0x1e5a60, dst = .text, &dstLen, flag)

and `0x109cc000 + 0x1e5a60` is exactly the stub's own entry point, so POL1 is
the packed stream with the unpacker sitting behind it.

The stream is LZSS:

    a control byte - eight flags, high bit first
      1  ->  copy one literal byte
      0  ->  two bytes, b0 b1:
                length   = (b0 >> 4) + 3               3..18
                distance = ((b0 << 8) | b1) & 0xfff    1..4095
             distance 0 ends the stream

The copy runs a byte at a time, so a distance shorter than the length repeats
what it has just written - the usual LZSS run trick, and the reason a
block-copy implementation produces subtly wrong output rather than obviously
wrong output.

    tools/unpackmain.py <FFXiMain.dll> [text.bin] [rebuilt.dll]

With a third argument it writes a DLL with `.text` filled in, which is what to
open in Ghidra. That file is derived from a proprietary binary: keep it out of
the repository.
"""

import struct
import sys

# Nothing here is hardcoded to one DLL. The two sizes the stub is called with
# are both derivable, which is what lets this run on FFXi.dll as well:
#
#   the stream   runs from POL1's start up to the entry point, because the
#                unpacker sits directly behind the data it unpacks
#   the output   is .text's virtual size
#
# Checked against FFXiMain, where the stub's own constants are visible:
# 0xbb1a60 - 0x9cc000 = 0x1e5a60 and .text is 0x32762e. Both exact.


def entry_rva(data):
    """AddressOfEntryPoint, which is where the packed stream stops."""
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    return struct.unpack_from("<I", data, pe + 24 + 0x10)[0]


def sections(data):
    """Every section header, by name."""
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not a PE file")

    count, = struct.unpack_from("<H", data, pe + 6)
    optional, = struct.unpack_from("<H", data, pe + 20)
    table = pe + 24 + optional

    found = {}
    for i in range(count):
        at = table + i * 40
        name = data[at:at + 8].rstrip(b"\0").decode("latin1")
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, at + 8)
        found[name] = dict(vsize=vsize, vaddr=vaddr, rawsize=rawsize,
                           rawptr=rawptr, header=at)
    return found


def unpack(packed, want):
    """The LZSS above. Stops at the end marker or when `want` bytes are out."""
    out = bytearray()
    at = 0
    while at < len(packed) and len(out) < want:
        control = packed[at]
        at += 1

        for bit in range(8):
            if at >= len(packed) or len(out) >= want:
                break

            if control & (0x80 >> bit):
                out.append(packed[at])
                at += 1
                continue

            first, second = packed[at], packed[at + 1]
            distance = ((first << 8) | second) & 0xFFF
            if distance == 0:
                return bytes(out)

            at += 2
            length = (first >> 4) + 3
            start = len(out) - distance
            if start < 0:
                raise ValueError(f"back-reference {distance} before the start, at {len(out)}")

            # One byte at a time on purpose - see the module docstring.
            for step in range(length):
                out.append(out[start + step])

    return bytes(out)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1

    data = open(argv[1], "rb").read()
    found = sections(data)
    pol1, text = found["POL1"], found[".text"]

    if text["rawsize"] != 0:
        print("this .text already has bytes on disk - nothing to unpack")
        return 1

    stream = entry_rva(data) - pol1["vaddr"]
    if not 0 < stream <= pol1["rawsize"]:
        print(f"the entry point is not behind POL1 - stream would be {stream:#x}")
        return 1

    packed = data[pol1["rawptr"]:pol1["rawptr"] + stream]
    out = unpack(packed, text["vsize"])

    print(f"POL1 at {pol1['rawptr']:#x}, {len(packed):#x} bytes packed")
    print(f"unpacked {len(out):#x} of {text['vsize']:#x} declared", end="  ")
    if len(out) != text["vsize"]:
        print("MISMATCH - the section header and the stream disagree")
        return 1
    print("exact")

    if len(argv) > 2:
        open(argv[2], "wb").write(out)
        print(f"wrote {argv[2]}")

    if len(argv) > 3:
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        alignment, = struct.unpack_from("<I", data, pe + 24 + 0x24)

        # Appended rather than spliced in. Only a disassembler has to read
        # this, never Windows, so the raw data need not sit in section order -
        # and splicing would move every section after .text.
        rebuilt = bytearray(data)
        rebuilt += b"\0" * ((-len(rebuilt)) % alignment)
        where = len(rebuilt)
        body = out + b"\0" * ((-len(out)) % alignment)
        rebuilt += body

        struct.pack_into("<I", rebuilt, text["header"] + 16, len(body))
        struct.pack_into("<I", rebuilt, text["header"] + 20, where)
        open(argv[3], "wb").write(rebuilt)
        print(f"wrote {argv[3]}: .text now at {where:#x}, {len(body):#x} bytes")

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
