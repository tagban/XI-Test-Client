# DAT replacements: modding without touching the install

A folder whose files are used instead of the game's. The retail install is
never written to, which is the point - a mod is something you can delete.

## Where it goes

`MOGHOUSE_DAT_REPLACEMENTS` names it. With the variable unset it is
**"DAT Replacements" beside the settings file** - a folder the player owns,
rather than one inside a game directory they do not.

The client sets the variable once it has found the folder, so the renderer,
the client and the python tools all read the same place and cannot disagree.

## How a mod is named

**Mirror the install's own layout.** A replacement for `ROM/1/31.DAT` goes at

    DAT Replacements/ROM/1/31.DAT

and for `ROM9/0/7.DAT` at `DAT Replacements/ROM9/0/7.DAT`.

Not by file id, though the client addresses everything by id internally and
that would have been the obvious choice. The install's layout is what every
existing FFXI tool distributes a DAT mod as, so a mod written for one of those
drops in here unchanged - and it is what [File ids](File-Ids) already prints
when it resolves something.

## What it can and cannot do

It replaces. It does not add: the id has to resolve through `VTABLE`/`FTABLE`
before there is a path to put a replacement at, so a file the install has no
entry for stays missing however many files are dropped in beside it.

Everything the client reads goes through one lookup, so everything is
replaceable by the same rule - zones, models, items, dialogue, event scripts.

## In this codebase

The lookup exists three times and all three honour the folder. **Change one,
change all three.**

- `renderer/ffxi/filetable.cpp` - `FileTable::replacementRoot()`
- `src/MogHouse.Core/Ffxi/FfxiFileTable.cs` - `FfxiFileTable.ReplacementRoot`
- `tools/filetable.py` - `FileTable.path`
