# The client binary: why it looks empty

`FFXiMain.dll` ships with no code in it.

This matters the moment anybody points a disassembler at the retail client to
settle a question the DAT files will not answer. The file looks like a normal
2.8 MB DLL, analysis runs and completes without complaint, and the result is
**277 functions** - none of them the game. Every answer taken from that state
is wrong, and nothing warns you.

## What the sections say

```
.text   10001000..1032862d   3,307,054 bytes   executable   initialized: FALSE
POL1    109cc000..10bb1bff   1,989,632 bytes   executable   initialized: true
entry   10bb1a60                                            (inside POL1)
```

`.text` is a section header with a virtual size and **no bytes behind it** -
reading any address in it fails outright. The entry point is not in `.text` at
all; it is in `POL1`, which is PlayOnline's own section, and that is where the
277 functions live.

`FFXi.dll` is packed the same way. `FFXiResource`, `FFXiVersions`, the two
`ImeUi` DLLs and `xinputdll` are not, but they are all small utility DLLs and
hold none of the rendering, skeleton or effect code.

## The stub is one call

```c
if (reason == DLL_PROCESS_ATTACH) {
    size = 0x32762e;
    unpack(POL1_start, 0x1e5a60, text_start, &size, 0);
}
```

`0x109cc000 + 0x1e5a60` is `0x10bb1a60`, which is the stub's own entry point.
So `POL1` is the packed stream with the unpacker sitting directly behind it.

## The stream is LZSS

A control byte carries eight flags, high bit first:

| flag | meaning |
|---|---|
| `1` | copy one literal byte |
| `0` | a two-byte back-reference, `b0 b1` |

For a back-reference:

```
length   = (b0 >> 4) + 3              3..18
distance = ((b0 << 8) | b1) & 0xfff   1..4095
```

A distance of zero ends the stream.

The copy runs **one byte at a time**, so a distance shorter than the length
repeats what it has just written. That is the usual LZSS run trick and it is
the one detail worth being careful about: a block-copy implementation produces
output that is subtly wrong rather than obviously wrong.

## Doing it

```
tools/unpackmain.py <FFXiMain.dll> [text.bin] [rebuilt.dll]
```

With a third argument it writes a DLL with `.text` filled in, which is the file
to open in Ghidra. It refuses to guess: the stream length and output size are
the ones the stub is called with, and it stops if what comes out does not match
the section header exactly.

It does match - `0x32762e` bytes, to the byte - and the result begins

```
8a 44 24 04    mov  al, [esp+4]
56             push esi
a8 02          test al, 2
8b f1          mov  esi, ecx
```

which is a function prologue rather than noise. Loaded into Ghidra the rebuilt
DLL gives **14,475 functions** against the packed file's 277.

No Windows, no debugger and no runtime memory dump are needed, which is worth
saying because all three are the usual answer to a packed binary and none of
them is necessary here.

## What is in there: the engine is called "dancer"

The unpacked binary carries the source paths of its own memory allocation
sites - `free(pointer, file, line)` keeps the file and line - so the engine's
module layout is readable without any guessing. It is a middleware engine
called **dancer**, built from `sq*` modules:

| module | files seen | what it is |
|---|---|---|
| `sqOpcode` | `sqopOpcode.c`, `sqopTrack.c`, `sqopSystem.c` | almost certainly the `0x21`-`0x2f` block in [Effect Generators](Effect-Generators.md) |
| `sqXform` | `sqxfXformTD.c`, `sqxfXformM.c` | transforms - where a placement is composed |
| `sqSkeleton` | `sqskSkeleton.c`, `sqskJoint.c` | bones; see [Skeletons](Skeletons.md) |
| `sqSkin` | `sqinSkin.c`, `sqinBuild.c` | skinning - where a weapon's bone gets resolved |
| `sqMotion` | `sqmoChannel.c`, `sqmoKeyChannel.c`, `sqmoMixerMotion.c`, … | animation channels and the mixer |
| `sqModel` | `sqmdModel.c`, `sqmdIO.c`, `sqmdDMB.c`, `sqmdScript.c`, `sqmdSnap.c` | models, and a script system |
| `sqHierarchy` | `sqhiNode.c` | the node tree a transform inherits through |
| `sqScene`, `sqRend`, `sqShape`, `sqShader`, `sqGrafix` | many | scene, camera, lights, materials, DX8 back end |
| `sqBase` | `sqMatrix4.c`, `sqMatrix3.c`, `sqQuat.c`, `sqVtx.c`, `sqArray.c` | the maths and containers |
| `sqImage` | `sqimImageTM2.c`, `TIFF`, `SGI`, `PPM` | image formats - TM2 is the PS2 lineage showing |
| `sqConstraint` | `sqcoConnector.c` | constraints and connectors |

The game's own layer sits above it as `D:\build0001\FFXi_Win\Main\...`, with
a `dancer/` subdirectory of adapters (`StModel.cpp`, `StChannel.cpp`,
`StTrigger.cpp`, `StAvatar.cpp`), and its classes are the `CMo*` and `CXi*`
names in the RTTI - `CMoResourceMng`, `CMoGeneratorClone`, `CXiDancerActor`.

Two consequences worth knowing before hunting anything:

- **Module code is contiguous.** An allocation site pins a module to an address
  neighbourhood, and the rest of that compilation unit is around it. `sqOpcode`
  is near `0x10289xxx`.
- **Most of these calls are through vtables**, so a function with no callers is
  normal rather than dead. Walking outward from a caller list stops early;
  walking from RTTI to a vtable to its methods does not.

## Keep the output out of the repository

The rebuilt DLL is derived from a proprietary binary. `*.unpacked.dll` is in
`.gitignore`; keep the file somewhere outside the checkout.

The same rule that governs LandSandBoat governs this: read it to learn the
rule, then implement the rule independently and write down how it was measured.
Findings arrived at that way - the weapon window at `+1320`, say - are facts
about a file format and are safe to publish. Decompiler output is not, and does
not go in.
