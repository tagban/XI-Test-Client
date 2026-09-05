"""Resolve FFXI file ids to paths, the way the client does.

The retail install indexes its content by a flat file id rather than by path.
Two tables at the install root map one to the other, both with one entry per id:

    VTABLE.DAT   u8   the ROM number holding it, 0 if it is not installed
    FTABLE.DAT   u16  (directory << 7) | file

so id -> ROM{rom}/{directory}/{file}.DAT, where ROM 1 is the folder called
plain "ROM".

That pair at the install root describes the original game. Each expansion
brings a pair of its own - ROM2/VTABLE2.DAT and FTABLE2.DAT, up through ROM9 -
covering the same id range with a non-zero entry only for the files that
expansion holds. The client reads them all, later ones overriding earlier.
Reading only the base pair makes every expansion zone and model look
uninstalled: Yhoator Jungle's map is file 223, and that is in ROM2.

MogHouse.Core has the same lookup in C#, and the renderer in C++. Change one,
change all three.

Derived by trying candidate bit splits against the install and keeping the one
that resolved: shift 7 resolves 82,912 of 82,912 present ids, shift 8 manages
50.3% and shift 9 27.6%.
"""

import os
import pathlib
import struct
from pathlib import Path


class FileTable:
    def __init__(self, install_root):
        self.root = Path(install_root)
        self._vtable = bytearray((self.root / "VTABLE.DAT").read_bytes())
        self._ftable = bytearray((self.root / "FTABLE.DAT").read_bytes())
        if len(self._ftable) != len(self._vtable) * 2:
            raise ValueError("VTABLE and FTABLE disagree on how many ids there are")

        # Each expansion over the top, later ones winning. Absent is an
        # expansion that is not installed; the wrong shape is skipped rather
        # than fatal.
        for rom in range(2, 10):
            folder = self.root / f"ROM{rom}"
            vpath = folder / f"VTABLE{rom}.DAT"
            fpath = folder / f"FTABLE{rom}.DAT"
            if not (vpath.exists() and fpath.exists()):
                continue
            vtable = vpath.read_bytes()
            ftable = fpath.read_bytes()
            if len(vtable) != len(self._vtable) or len(ftable) != len(self._ftable):
                print(f"filetable: ROM{rom}'s tables do not match the base install's; ignoring them")
                continue
            for file_id, present in enumerate(vtable):
                if present:
                    self._vtable[file_id] = present
                    self._ftable[file_id * 2 : file_id * 2 + 2] = ftable[file_id * 2 : file_id * 2 + 2]

    def __len__(self):
        return len(self._vtable)

    def path(self, file_id):
        """The path for a file id, or None if it is not installed.

        A folder named by MOGHOUSE_DAT_REPLACEMENTS is searched first. It
        mirrors the install's own layout - ROM/1/31.DAT - so a file present
        there is used instead of the install's, and the retail files are never
        written to. The renderer and the client read the same variable.
        """
        rom = self._vtable[file_id]
        if rom == 0:
            return None
        packed = struct.unpack_from("<H", self._ftable, file_id * 2)[0]
        folder = "ROM" if rom == 1 else f"ROM{rom}"
        relative = os.path.join(folder, str(packed >> 7), f"{packed & 0x7F}.DAT")

        over = os.environ.get("MOGHOUSE_DAT_REPLACEMENTS")
        if over:
            candidate = pathlib.Path(over) / relative
            if candidate.exists():
                return candidate

        return self.root / relative

    def present(self):
        """Every installed file id, in order."""
        for file_id in range(len(self)):
            if self._vtable[file_id]:
                yield file_id
