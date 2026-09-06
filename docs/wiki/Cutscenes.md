# Cutscenes: what they are, and what it takes to play one

A cutscene in FFXI is an **event**: the server says "play event N on entity E",
the client runs a script that moves the camera, poses NPCs, shows dialogue and
offers choices, and then tells the server it is done. The character is
`InEvent` the whole time and invisible to everyone else until it ends.

MogHouse today **skips** every cutscene. When a `0x032` event start arrives -
or when the login reply says the character was parked in one - the client sends
`EVENTEND` straight back and prints `[cutscene N was skipped]`. That is correct
and deliberate: skipping is better than freezing in an event the client cannot
run. Making them *play* is the work below.

## The packet flow

| packet | dir | meaning | MogHouse |
|---|---|---|---|
| `0x032` event | s2c | start event N (the id is `EventPara`; `EventNum` is the *zone*) | parsed (`FfxiEventStart`) |
| `0x033` eventstr | s2c | string parameters for the script (names to slot into text) | not read |
| `0x034` eventnum | s2c | number parameters | not read |
| `0x036` talknum | s2c | a single NPC line, no cutscene | — |
| `0x038` schedulor | s2c | a scheduler command tagged by FourCC | not read |
| `0x052` eventucoff | s2c | release the update-lock during an event | not read |
| `0x05b` eventend | c2s | "the event finished" | sent (to skip) |
| `0x05c` eventendxzy | c2s | event end, plus where the player ended up | sent by position |

So the transport is mostly understood and half-built. The gap is not the
packets; it is what happens between the start and the end.

## The missing piece: the event VM

The script itself lives in the zone's **event DAT** (`5820 + zone`), which
[Events](Events.md) documents and `FfxiEventTable` already segments into
per-entity code blocks. Each block is **bytecode** for a little virtual machine
the client runs: a sequence of opcodes that do things like

- point the camera here, move it there over N frames
- turn an NPC to face the player, play an animation on it
- print a message (by id, into the zone's message DAT), with the `0x033`/`0x034`
  parameters substituted
- offer a menu and wait for the player's choice
- wait, fade, end

**These opcodes are not decoded yet.** The earlier note in the summary - that
dialogue is not reachable by a plain `u16` message id - is the symptom: the
message shown is chosen *by the bytecode*, not by a fixed offset, so nothing
plays until the bytecode is read.

This is the one real blocker. Everything else is plumbing.

## What the DLL shows

Traced in the unpacked client with Ghidra headless (see below). Addresses are
at the 0x10000000 image base. This is the event system's shape, down to the one
layer still to decode.

### Loading the files

`FUN_100aeb90(zone)` loads the event **bytecode** file. The file id is:

| zone range | file id |
|---|---|
| `< 256` | `zone + 0x16bc` (= `zone + 5820`) |
| `256..999` | `zone + 0x14aff` |
| `1000..1999` | `zone + 0xde31` |
| `>= 2000` | `zone + 0x1004b` |

The first row is exactly the `5820 + zone` MogHouse already uses, now confirmed
and extended to the expansion ranges. `FUN_100ae1f0(zone)` loads the **message**
(text) file, its base id from a category table (`FUN_1025b570(0x6b..0x6e)`).

### Starting an event

`FUN_100aed10(entity, eventId, p3, p4)` is the start: it clears a large state
block, then loads the bytecode and the message for the event.

### The bytecode buffer

`DAT_10489944` points at the loaded bytecode. Its first dword is a **count** in
the low 30 bits with two flag bits on top (`& 0x3fffffff`); the entries follow.
This is the same container `FfxiEventTable` already parses.

Each entry carries a **target list** and a script. `FUN_100bd140` walks the
list (count at `+4`, target ids at `+8`) and matches an entry to the entity the
event fired on (`DAT_1048871c`); `0xfffe` is a wildcard that matches anyone.

`FUN_100af190` decodes an entry's target reference. Values `0x7fffffc0` to
`0x7fffffff` are **dynamic references** - the player, the last actor, and so on,
resolved through helpers - and any other value is a plain entry whose low ten
bits are the entity index. This is why a message is not reachable by a bare
`u16`: the target, and the script that runs, are chosen by these tagged values,
not by a fixed offset.

### Setup to running

`FUN_100aeeb0` matches the triggered entity to an entry, sets a per-actor
"has an active event" bit in the `DAT_10488020` bitmap (`FUN_100af2d0` sets,
`FUN_100af300` tests), and allocates a per-actor **event task object of type
0x16** (`FUN_100aec80` -> `FUN_100fc3d0(0x16, ...)`). `FUN_100ae400` is the
per-frame update that walks every actor and drives its event position, facing,
animation and flags.

Load progress lives in `_DAT_1048994c` (states 0..7); the debug logger
`FUN_100ad58c` prints `CSTAT/SSTAT/UC/EF/Slock/ETask` with task-state names from
a small table at `0x1035af5f` (`init`, `ini1`, `ini2`, `ini3`).

### The message lookup is a format MogHouse already has

`FUN_100962cf(id)` calls `FUN_10096230(messageBuffer, id)`, which is the
event-text lookup, and it is the same offset-table shape as the zone dialogue
table `FfxiDialogueTable` already parses:

```c
count = (u32 at buffer+4 - 4) / 4;                 // FUN_10096260
text  = buffer + 4 + (u32 at buffer + 4 + id*4);   // FUN_10096230
```

Compare [Dialogue](Dialogue.md): a dword at 0, an offset table at +4, text after,
`count = firstOffset / 4`. Identical. So the **text a cutscene shows is already
solved** - MogHouse can look up any event message by id today. What it does not
know is *which* id, and when, because that is chosen by the bytecode. This is
the precise shape of the remaining gap: not the text, not the transport, just
the opcode that names a message.

### The run loop and the per-actor dispatch

`FUN_10096650` is the event system's per-frame driver. It picks up a queued
event (from `DAT_104dfdd8 + 0x40d90`) and calls the start `FUN_100aed10`; while
one is running it calls the init `FUN_100aea50` until it reports done, then
walks the actor list and dispatches each active actor through a virtual method,
`FUN_100973c0` -> `(*(actor+0xa0)->vtable[0x30])(...)`.

**That virtual method is where the opcode stepping lives, and it is a virtual
call.** Resolving it means resolving the class of the object at `actor+0xa0`
and its vtable - another object-system layer - which is why the opcode set is
not in reach from the event module alone. This is the exact spot the next dive
starts from, rather than re-deriving the surrounding system.

### The bytecode, as it really sits in the file

Dumped and confirmed against a live zone file (S. Sandoria, `5820+230`):

```
file:   count(u32), then count lengths(u32), then that many blocks back to back
block:  entityId(u32), capacity(u32), one u16, then the index and the code
index:  capacity u16 offsets (the last is 0xFFFF), then capacity u16 event ids
code:   event i is code[offset[i-1] .. offset[i]]; event 0 is a 1-byte placeholder
```

This is exactly what `FfxiEventTable` parses, now checked byte for byte. A real
script begins with a `0x00` byte and then instruction-and-operand runs, e.g.
`00 be 75 01 00 77 5e ff ff e8 03 00 00` - but the run lengths are set by the
opcode table, so the bytes cannot be split into instructions until that table
is decoded. That, and only that, is what stands between here and a cutscene
that plays.

### The one layer left: the opcode VM

The type-0x16 event task's own update is the bytecode interpreter - the switch
over opcodes that moves the camera, poses an actor, prints a message, offers a
menu, waits, fades and ends. It is the largest single thing left to reverse in
the client, and it is what actually plays a cutscene. The entry to it is the
type-0x16 object created in `FUN_100aec80`; following the task framework's
dispatch for that type reaches the opcode switch.

For the dialogue-only milestone, only two of those opcodes matter - print a
message by id, and end - so that switch does not have to be decoded whole
before something plays.

## Driving Ghidra without the GUI

The decompiler runs headless against the analysed project, no MCP and no
CodeBrowser:

```
JAVA_HOME=/opt/homebrew/opt/openjdk@21 \
  /opt/homebrew/opt/ghidra/libexec/support/analyzeHeadless \
  <projectDir> <projectName> -process FFXiMain.unpacked.dll -noanalysis \
  -scriptPath ~/ghidra-scripts -postScript DecompDump.java
```

`DecompDump.java` reads a list of hex addresses from `$MOGHOUSE_TARGETS` and
writes their decompiled C to `$MOGHOUSE_OUT`. `tools/dllstrings.py` finds the
addresses - strings, cross-references and float constants - so the two together
are the whole loop: find an address, decompile what is there, follow it.
